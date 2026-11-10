// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// mvdecode: each listed numeric value becomes missing (.), written back in place.
// Shared helpers: column_write_support.h. The family note for this command:
//
//   mvdecode     reads each column, writes it back IN PLACE through `cobra setcol` (the
//                write shape runCarryforward/runMvencode use). The only arithmetic is the
//                equality test, which follows mvdecode's documented float rule: a float
//                variable is compared with float(#) (StataBE probe p2 mvfloat).
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

// mvdecode's syntax line has exactly one option, mv().
const piet::eval::OptVocab& kVocabMvdecode =
    piet::eval::registerOptVocab({"mvdecode"}, {{"mv"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace column_write_detail;

namespace {

// ===========================================================================
// mvdecode
// ===========================================================================

// Stata's abbrev(name, 12): longer names keep their first ten characters, a `~`, and their
// last character.
std::string abbrev12(const std::string& name) {
  if (name.size() <= 12) return name;
  return name.substr(0, 10) + "~" + name.substr(name.size() - 1);
}

std::string rightAlign(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// A generated numlist element as Stata's numlist hands it back: as text with 15 significant
// digits, so 0(0.1)1 yields .3 itself and not 0.30000000000000004 (review gi I1/I3, gg G1).
double numlistElement(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  return std::strtod(buf, nullptr);
}

// The elements from `a` to `b` in steps of `d`, the first one excluded when `skipFirst`.
bool numlistRun(double a, double d, double b, bool skipFirst, std::vector<double>* out) {
  if (d == 0.0 || (b - a) / d < 0) return false;
  for (int64_t k = skipFirst ? 1 : 0;; ++k) {
    const double v = numlistElement(a + static_cast<double>(k) * d);
    if (d > 0 ? v > b : v < b) break;
    out->push_back(v);
    if (out->size() > 1000000) return false;
  }
  return true;
}

// A numlist, Stata's spellings ([U] 11.1.8): #, #/#, #(#)#, #[#]#, and `#1 #2 to #3` /
// `#1 #2 : #3` (the step is #2 - #1). Anything else is r(121) with the whole numlist named,
// as mvdecode reports it (probe p2 mvbad).
std::optional<ojson> parseNumlist(const std::string& text, std::vector<double>* out) {
  const std::string bad = text + " is not a valid numlist";
  std::string spec;
  for (char c : text) {
    if (c == ',')
      spec += ' ';
    else if (c == ':')
      spec += " : ";
    else
      spec += c;
  }
  const std::vector<std::string> toks = splitWs(spec);
  if (toks.empty()) return rcError(bad, 121);
  int plainRun = 0;  // how many of the last elements were plain numbers
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& tok = toks[i];
    double a = 0, b = 0, d = 0;
    if (tok == "to" || tok == ":") {
      if (plainRun < 2 || i + 1 >= toks.size() || !parseStrictDouble(toks[i + 1], &b))
        return rcError(bad, 121);
      ++i;
      const double a1 = (*out)[out->size() - 2], a2 = out->back();
      if (!numlistRun(a2, a2 - a1, b, true, out)) return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    const size_t slash = tok.find('/', 1);
    const size_t lp = tok.find_first_of("([", 1);
    if (slash != std::string::npos) {
      if (!parseStrictDouble(tok.substr(0, slash), &a) ||
          !parseStrictDouble(tok.substr(slash + 1), &b) ||
          !numlistRun(a, a <= b ? 1.0 : -1.0, b, false, out))
        return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    if (lp != std::string::npos) {
      const size_t rp = tok.find(tok[lp] == '(' ? ')' : ']', lp);
      if (rp == std::string::npos || !parseStrictDouble(tok.substr(0, lp), &a) ||
          !parseStrictDouble(tok.substr(lp + 1, rp - lp - 1), &d) ||
          !parseStrictDouble(tok.substr(rp + 1), &b) || !numlistRun(a, d, b, false, out))
        return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    if (!parseStrictDouble(tok, &a)) return rcError(bad, 121);
    out->push_back(a);
    ++plainRun;
  }
  return std::nullopt;
}

// mv(): a bare numlist (every value -> .), or rules `numlist = code [\ numlist = code ...]`.
// The grammar and its three messages are mvdecode's own (official log mvdecode_*; probe p2
// mvbad r(121), mvbadcode r(198)). An extended-missing code (.a-.z) cannot be stored by this
// build, which has one missing value, so that rule is refused by name.
std::optional<ojson> parseMv(const std::string& spec, std::vector<double>* values) {
  if (spec.find_first_of("=\\") == std::string::npos) return parseNumlist(spec, values);
  std::vector<std::string> toks;
  std::string cur;
  auto flush = [&]() {
    const std::string t = strutil::trim(cur);
    if (!t.empty()) toks.push_back(t);
    cur.clear();
  };
  for (char c : spec) {
    if (c == '=' || c == '\\') {
      flush();
      toks.emplace_back(1, c);
    } else {
      cur += c;
    }
  }
  flush();
  size_t i = 0;
  auto next = [&]() -> std::string { return i < toks.size() ? toks[i++] : std::string(); };
  std::string tok = next();
  while (!tok.empty()) {
    std::vector<double> lhs;
    if (auto e = parseNumlist(tok, &lhs)) return e;
    if (next() != "=") return syntaxError("= expected");
    const std::string code = next();
    const bool sysmiss = code == ".";
    const bool extended = code.size() == 2 && code[0] == '.' && code[1] >= 'a' && code[1] <= 'z';
    if (!sysmiss && !extended) return syntaxError(code + " is not a missing value code");
    if (extended)
      return ojson(eval::unsupportedOption(
          "mv(" + spec + ")",
          "mvdecode: the rule `= " + code + "' stores an extended missing value; this version "
          "of PIET stores one missing value (.), so .a-.z cannot be kept apart. Use `= .`."));
    values->insert(values->end(), lhs.begin(), lhs.end());
    const std::string sep = next();
    if (!sep.empty() && sep != "\\") return syntaxError(sep + " found, where \\ expected");
    tok = next();
  }
  return std::nullopt;
}

}  // namespace

// `mvdecode varlist [if] [in], mv(numlist | numlist=. [\ numlist=. ...])`. Implements
// mvdecode per [D] mvencode: each listed value becomes `.`; a float variable is compared with
// float(#); string variables are reported and skipped; each variable that changed prints
// `<name>: N missing value(s) generated` (official log mvdecode_single/_rules/_withstr/_all).
ojson runMvdecode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMvdecode.known,
                                            kVocabMvdecode.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  const std::optional<std::string> mv = optStr(parsed.opts, "mv");
  if (!mv.has_value()) return syntaxError("option mv() required");  // mvdecode_nomv r(198)
  // An empty sample exits before mv() is read, as mvdecode does (review gg G4: `if 0, mv(bad)`
  // is rc 0).
  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  if (std::find(mask.begin(), mask.end(), 1) == mask.end()) return ojson::array();
  std::vector<double> values;
  if (auto e = parseMv(*mv, &values)) return *e;

  std::string text;
  std::vector<std::string> rewritten;
  for (const auto& v : vars) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(v).dtype == cecon::DType::Factor) {
      text += rightAlign(abbrev12(v), 12) + ": string variable ignored\n";
      continue;
    }
    const bool isFloat = storage::resolveReplaceType(session, v) == "float";
    std::vector<double> targets;
    for (double t : values) targets.push_back(isFloat ? toFloat(t) : t);
    std::vector<double> col = columnDoubles(*ds, v);
    int64_t changed = 0;
    for (size_t i = 0; i < col.size(); ++i) {
      if (!mask[i] || std::isnan(col[i])) continue;
      if (std::find(targets.begin(), targets.end(), col[i]) != targets.end()) {
        col[i] = std::numeric_limits<double>::quiet_NaN();
        ++changed;
      }
    }
    if (changed == 0) continue;
    std::vector<std::string> cells;
    cells.reserve(col.size());
    for (double x : col) cells.push_back(numCell(x));
    if (auto e = setColumn(session, v, cells, false, false, "")) return *e;
    if (std::find(rewritten.begin(), rewritten.end(), v) == rewritten.end()) rewritten.push_back(v);
    text += rightAlign(abbrev12(v), 12) + ": " + std::to_string(changed) + " missing value" +
            (changed > 1 ? "s" : "") + " generated\n";
  }
  ojson out = ojson::array();
  if (!text.empty()) out.push_back(displayBlock(text));
  if (!rewritten.empty()) out.push_back(mutationResult(session, "gen", {}, {}, rewritten));
  return out;
}

}  // namespace piet::handlers
