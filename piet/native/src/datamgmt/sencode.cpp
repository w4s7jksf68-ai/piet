// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// sencode: encode a string variable in order of first appearance or of a gsort() key.
// Shared helpers: column_write_support.h. The family note for this command:
//
//   sencode      implements sencode per its help file (Newson): codes in order of first
//                appearance (or of a gsort() key), "" missing, an existing value label reused
//                and extended at its largest code + 1 (encode's own extension rule, which
//                runEncode documents). The label-store writes are runEncode's shape.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/data_values.h"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "column_write_support.h"

// integ's spline must round every product and sum separately, as StataBE's expression
// evaluator does; a fused multiply-add changes the last bit (integ_basic is pinned to it).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch. The aliases are the minimum
// abbreviations the commands' own syntax lines declare (Generate -> g, Trapezoid -> t,
// Initial -> i, Label -> l, Nquantiles -> n), which the 2-character prefix floor cannot reach.
namespace {

// sencode: `fast` only skips the package's preserve/restore on Break, so it changes nothing
// in the answer and is read as a no-op. `manyto1` (one code per observation group rather than
// per string) and `noextend` (refuse strings the label lacks) change the codes and are not
// served.
const piet::eval::OptVocab& kVocabSencode = piet::eval::registerOptVocab(
    {"sencode"}, {{"generate", "replace", "label", "gsort", "fast"},
                  {"manyto1", "noextend"},
                  {{"g", "generate"}, {"l", "label"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace column_write_detail;

namespace {

// ===========================================================================
// sencode
// ===========================================================================

struct GsortKey {
  std::string var;
  bool descending = false;
};

// The value-label set `lbl` with `var` attached, and every variable carrying the set given
// its new {code: text} map -- the label-store writes runEncode makes.
void attachLabelSet(SessionState& session, const std::string& var, const std::string& lbl,
                    const std::map<std::string, std::string>& codeMap) {
  session.labelDefs[lbl] = codeMap;
  session.stampLabelSet(lbl);
  session.varLblname[var] = lbl;
  for (const auto& [v, lname] : session.varLblname) {
    if (lname != lbl) continue;
    std::map<int, std::string> numeric;
    for (const auto& [codeStr, text] : codeMap) {
      try {
        numeric[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    if (numeric.empty())
      session.valueLabels.erase(v);
    else
      session.valueLabels[v] = numeric;
  }
}

// What `compress` leaves a positive integer code in.
std::string compressedCodeType(long long maxCode) {
  if (maxCode <= 100) return "byte";
  if (maxCode <= 32740) return "int";
  return "long";
}

}  // namespace

// `sencode strvar [if] [in], generate(newvar) | replace [label(name[, replace]) gsort(keys)
// fast]`. Implements sencode per its help file (Newson): one code per distinct non-empty
// string, numbered in order of first appearance (community log sencode_basic: b a c -> 1 2 3)
// or in the order of a gsort() key list (sencode_gsort / _gsortdesc); "" and rows outside
// if/in are missing and are not labelled (probe p4 sin). The value label is label() or the
// new variable's name; an existing set keeps its codes and new strings take its largest code
// + 1 in that order (probe p3 slab: pre {7 c} -> b 8, a 9, c 7). The variable is stored as
// `compress` leaves it (byte for <= 100 codes), %8.0g, with the string's variable label.
// `replace` puts the codes in the string variable's place, label named after it.
ojson runSencode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSencode.known,
                                            kVocabSencode.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  if (vars.size() > 1) return rcError("too many variables specified", 103);
  const std::string src = vars.front();
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(src).dtype != cecon::DType::Factor)  // probe p3 snum
      return rcError("numeric variables not allowed in varlist;\n" + src + " is a numeric variable",
                     109);
  }
  const bool replaceOpt = hasFlag(parsed.opts, "replace");
  const std::optional<std::string> genOpt = optStr(parsed.opts, "generate");
  const bool haveGen = genOpt.has_value() && !genOpt->empty();
  if (replaceOpt && haveGen)  // probe p3 sboth
    return syntaxError("options generate() and replace are mutually exclusive");
  if (!replaceOpt && !haveGen)  // sencode_noopt
    return syntaxError("must specify either generate() or replace option");

  // label(name [, replace]).
  std::string lbl;
  bool lreplace = false;
  if (auto lo = optStr(parsed.opts, "label")) {
    std::string spec = *lo;
    const size_t comma = spec.find(',');
    if (comma != std::string::npos) {
      const std::string sub = strutil::lower(strutil::trim(spec.substr(comma + 1)));
      if (sub == "replace") {
        lreplace = true;
      } else if (!sub.empty()) {
        return syntaxError("option " + sub + " not allowed");
      }
      spec = spec.substr(0, comma);
    }
    lbl = strutil::trim(spec);
    if (!lbl.empty() && !strutil::isValidStataIdentifier(lbl))
      return syntaxError(lbl + " invalid name");
  }

  std::string target = src;
  if (haveGen) {
    const std::vector<std::string> names = splitWs(*genOpt);
    // A generate() that is not one name reaches the package's label lookup as the label name
    // and fails there (probe p4 sbadname / stwonames: st_vlexists() 3300).
    if (names.size() != 1 || !strutil::isValidStataIdentifier(names.front())) {
      if (lbl.empty()) return rcError("st_vlexists():  3300  argument out of range", 3300);
      return syntaxError(*genOpt + " invalid name");
    }
    target = names.front();
    if (columnExists(session, target))  // sencode_exists r(110)
      return rcError("variable " + target + " already defined", 110);
  }
  if (lbl.empty()) lbl = target;

  // gsort(keylist [, mfirst]).
  std::vector<GsortKey> gkeys;
  bool mfirst = false;
  if (auto go = optStr(parsed.opts, "gsort")) {
    std::string spec = *go;
    const size_t comma = spec.find(',');
    if (comma != std::string::npos) {
      for (const auto& w : splitWs(spec.substr(comma + 1))) {
        if (strutil::lower(w) == "mfirst" || strutil::lower(w) == "mf")
          mfirst = true;
        else
          return ojson(eval::unsupportedOption(
              "gsort(" + *go + ")", "sencode: gsort()'s option `" + w + "' is not served"));
      }
      spec = spec.substr(0, comma);
    }
    for (std::string tok : splitWs(spec)) {
      GsortKey k;
      while (!tok.empty() && (tok[0] == '-' || tok[0] == '+')) {
        k.descending = tok[0] == '-';
        tok.erase(0, 1);
      }
      if (tok.empty()) continue;  // a bare sign belongs to the next name
      std::vector<std::string> one;
      if (auto e = resolveVars(session, {tok}, &one)) return *e;
      for (const auto& v : one) gkeys.push_back(GsortKey{v, k.descending});
    }
  }

  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const std::vector<std::string> text = columnCells(*ds, src);
  const size_t n = text.size();
  std::vector<KeyCol> keyCols;
  for (const auto& k : gkeys) keyCols.push_back(readKeyCol(*ds, k.var));

  // The order rows are visited in: the gsort() keys, then the string itself; with no gsort()
  // the row order. Each string's place is its first row in that order.
  std::vector<size_t> order;
  for (size_t i = 0; i < n; ++i)
    if (mask[i] && !text[i].empty()) order.push_back(i);
  if (!gkeys.empty()) {
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      for (size_t j = 0; j < gkeys.size(); ++j) {
        const KeyCol& k = keyCols[j];
        const bool ma = keyMissing(k, a), mb = keyMissing(k, b);
        if (!k.isString && ma != mb) {
          // gsort: missing last ascending; descending keeps it last unless mfirst.
          const bool aFirst = gkeys[j].descending && mfirst ? ma : mb;
          return aFirst;
        }
        int c = keyCompare(k, a, b);
        if (gkeys[j].descending) c = -c;
        if (c != 0) return c < 0;
      }
      return text[a] < text[b];
    });
  }
  std::vector<std::string> sequence;
  {
    std::set<std::string> seen;
    for (size_t i : order)
      if (seen.insert(text[i]).second) sequence.push_back(text[i]);
  }

  // The label: an existing set (unless label(, replace)) keeps every code it has; a string it
  // lacks takes the set's largest code + 1.
  std::map<std::string, std::string> codeMap;
  if (!lreplace) {
    auto it = session.labelDefs.find(lbl);
    if (it != session.labelDefs.end()) codeMap = it->second;
  }
  std::map<std::string, long long> codeOfText;
  long long maxCode = 0;
  for (const auto& [codeStr, t] : codeMap) {
    long long c = 0;
    try {
      c = std::stoll(codeStr);
    } catch (const std::exception&) {
      continue;
    }
    maxCode = std::max(maxCode, c);
    auto ins = codeOfText.emplace(t, c);
    if (!ins.second && c < ins.first->second) ins.first->second = c;
  }
  std::map<std::string, long long> codeFor;
  long long maxUsed = 0;
  for (const auto& s : sequence) {
    long long code = 0;
    auto known = codeOfText.find(s);
    if (known != codeOfText.end()) {
      code = known->second;
    } else {
      code = ++maxCode;
      codeMap[std::to_string(code)] = s;
      codeOfText.emplace(s, code);
    }
    codeFor[s] = code;
    maxUsed = std::max(maxUsed, code);
  }

  std::vector<std::string> cells;
  cells.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!mask[i] || text[i].empty()) {
      cells.emplace_back();
      continue;
    }
    cells.push_back(std::to_string(codeFor[text[i]]));
  }
  std::optional<std::string> srcLabel;
  {
    auto it = session.varMeta.find(src);
    if (it != session.varMeta.end() && it->second.label.has_value()) srcLabel = it->second.label;
  }
  if (auto e = setColumn(session, target, cells, false, haveGen, "")) return *e;

  if (lreplace) {
    for (const auto& [v, lname] : session.varLblname)
      if (lname == lbl) session.valueLabels.erase(v);
  }
  attachLabelSet(session, target, lbl, codeMap);
  VarMeta& vm = session.varMeta[target];
  vm.stataType = compressedCodeType(maxUsed);
  vm.format = std::string("%8.0g");
  if (haveGen && srcLabel.has_value() && !srcLabel->empty()) vm.label = srcLabel;
  session.implicitFloatVars.erase(target);
  session.userFormattedVars.erase(target);
  if (haveGen) return mutationResult(session, "gen", {target});
  return mutationResult(session, "gen", {}, {}, {target});
}

}  // namespace piet::handlers
