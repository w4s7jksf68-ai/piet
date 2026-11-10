// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// numlabel -- add or remove numeric-code prefixes in value-label text (variable-metadata
// family; shared helpers in variable_metadata_support.h).
//
//   numlabel  every write is a `label define ..., modify` through runLabelDefine, so
//             both label stores (labelDefs and the per-variable valueLabels) and the
//             display-format widening move together, exactly as `label define` moves them.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers/variables.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "variable_metadata_support.h"

namespace {

// numlabel's syntax line, all read.
const piet::eval::OptVocab& kVocabNumlabel = piet::eval::registerOptVocab(
    {"numlabel"}, {{"add", "remove", "mask", "force", "detail"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

// ===========================================================================
// numlabel
// ===========================================================================
//
// Implements numlabel per its help file; identified against StataBE probes
// (w2ax_t1_official.log numlabel_*, mx1.log NL|*):
//   * exactly one of add/remove (both, or neither: r(198)); mask() must hold exactly one `#`
//     (r(198) "mask <m> should contain 1 substitution char #"), default "#. ";
//   * the label sets named, or every set (none named, or `_all`: review p6 nl_all); a name
//     that is not one r(198) (p4 nl_wild `* invalid name`), an unknown one r(111) "value
//     label <n> not found"; from there on r() is replaced (p9 nl_r); no set at all is a silent rc 0;
//   * per code, the prefix is the mask with `#` replaced by the code (-1 -> "(-1) ", .a ->
//     "(.a) "); a text "carries" the prefix when it begins with it;
//   * without `force`, a set in which some texts carry the prefix and some do not stops the
//     whole command with r(180) (the "partially numlabel-ed" note, the per-set listing under
//     `detail`); with `force` it is modified anyway (NL|partialforce);
//   * add prefixes every text that does not carry it (add twice changes nothing:
//     numlabel_add_again), remove strips it (a text that was only the prefix becomes empty,
//     i.e. the code loses its label: NL|empty); nothing to change -> "(no value label to be
//     modified)", rc 0 (numlabel_force);
//   * every set processed is rewritten with `label define ..., modify` -- here runLabelDefine,
//     so labelDefs, the attached variables' valueLabels and their display-format widening
//     (NL|fmt %22.0g) all follow the one writer `label define` uses.
ojson runNumlabel(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNumlabel.known,
                                            kVocabNumlabel.unimplemented))
    return ojson(*err);
  const bool add = hasFlag(parsed, "add"), remove = hasFlag(parsed, "remove");
  const bool force = hasFlag(parsed, "force"), detail = hasFlag(parsed, "detail");
  if (add && remove) return rcError("options add and remove must not be both specified", 198);
  if (!add && !remove) return rcError("option add or remove should be specified", 198);
  std::string mask = "#. ";
  if (auto m = optStr(parsed, "mask")) {
    // A quoted mask is taken as written; an unquoted one loses its outer blanks, as a
    // `str` option does (review p4 nl_mask_sp2: remove with mask(# - ) leaves " one").
    const std::string t = strutil::trim(*m);
    const std::vector<std::string> w = stataWords(t);
    const bool quoted = !t.empty() && (t[0] == '"' || (t.size() >= 2 && t[0] == '`' && t[1] == '"'));
    const std::string given = quoted && w.size() == 1 ? w.front() : t;
    if (!given.empty()) {
      if (std::count(given.begin(), given.end(), '#') != 1)
        return rcError("mask " + given + " should contain 1 substitution char #", 198);
      mask = given;
    }
  }

  // From here numlabel's own work replaces r() (review p9 nl_r: r(N) of an earlier
  // summarize is gone; nl_r_err: the option errors above leave it).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "numlabel", {});

  // Label names, `_all` among them meaning every set (review p6 nl_all, mx7.log NL|mixall);
  // a name that is not a name is r(198) at its first bad character (p4 nl_wild `* invalid
  // name`), an unknown one r(111).
  std::vector<std::string> sets;
  bool everySet = parsed.varlist.empty();
  for (const auto& nm : parsed.varlist) {
    if (nm == "_all") {
      everySet = true;
      continue;
    }
    if (!strutil::isValidStataIdentifier(nm)) {
      size_t bad = 0;
      while (bad < nm.size() && (std::isalnum(static_cast<unsigned char>(nm[bad])) || nm[bad] == '_')) ++bad;
      if (bad == 0 || bad >= nm.size()) bad = 0;
      return rcError(nm.substr(bad) + " invalid name", 198);
    }
    if (session.labelDefs.find(nm) == session.labelDefs.end())
      return rcError("value label " + nm + " not found", 111);
    if (std::find(sets.begin(), sets.end(), nm) == sets.end()) sets.push_back(nm);
  }
  if (everySet) {
    sets.clear();
    for (const auto& kv : session.labelDefs) sets.push_back(kv.first);
  }
  if (sets.empty()) return displayBlock("(dataset has no value labels)");  // mx1.log NL|nolabels

  struct Entry {
    std::string code, text, prefix;
    bool carries = false;
  };
  std::map<std::string, std::vector<Entry>> plan;
  for (const auto& s : sets) {
    auto& entries = plan[s];
    for (const auto& [code, text] : session.labelDefs.at(s)) {
      Entry e;
      e.code = code;
      e.text = text;
      e.prefix = mask;
      e.prefix.replace(e.prefix.find('#'), 1, code);
      e.carries = text.compare(0, e.prefix.size(), e.prefix) == 0;
      entries.push_back(std::move(e));
    }
  }

  if (!force) {
    std::vector<std::string> partial;
    for (const auto& s : sets) {
      bool anyYes = false, anyNo = false;
      for (const auto& e : plan[s]) (e.carries ? anyYes : anyNo) = true;
      if (anyYes && anyNo) partial.push_back(s);
    }
    if (!partial.empty()) {
      ojson out = ojson::array();
      if (detail) {
        std::string t = "\nThe following value labels are only partially numlabel-ed";
        for (const auto& s : partial) {
          t += "\n" + std::string(kLineSize, '-') + "\n-> lname = " + s + "\n\n  value         label";
          std::vector<std::pair<std::string, std::string>> rows;
          for (const auto& e : plan[s]) rows.emplace_back(e.code, e.text);
          std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            const bool am = !a.first.empty() && a.first[0] == '.';
            const bool bm = !b.first.empty() && b.first[0] == '.';
            if (am != bm) return bm;
            if (am) return a.first < b.first;
            return std::stoll(a.first) < std::stoll(b.first);
          });
          for (const auto& [code, text] : rows) t += "\n" + rightAlign(code, 11) + "     " + text;
          t += "\n";
        }
        out.push_back(displayBlock(t));
      } else {
        out.push_back(displayBlock(
            "there are value labels that are only partially numlabel-ed\n"
            "specify option detail for a list of these value labels"));
      }
      out.push_back(rcError("specify option force to modify value labels anyway", 180));
      return out;
    }
  }

  bool anyChange = false;
  for (auto& [s, entries] : plan) {
    for (auto& e : entries) {
      std::string next = e.text;
      if (add && !e.carries) next = e.prefix + e.text;
      if (remove && e.carries) next = e.text.substr(e.prefix.size());
      if (add && next.size() > 32000)
        return rcError("impossible to prefix a value to value labels > 32000 chars", 180);
      if (next != e.text) anyChange = true;
      e.text = next;
    }
  }
  if (!anyChange) return displayBlock("(no value label to be modified)");

  for (const auto& [s, entries] : plan) {
    std::string line = "label define " + s;
    for (const auto& e : entries) {
      // Compound quotes carry any text but one that itself closes or opens them.
      if (e.text.find("\"'") != std::string::npos || e.text.find("`\"") != std::string::npos)
        return notImplemented("numlabel: the text of code " + e.code + " in value label " + s +
                              " holds a compound-quote delimiter, which this version of PIET "
                              "cannot write back");
      line += " " + e.code + " `\"" + e.text + "\"'";
    }
    line += ", modify";
    ojson r = runLabelDefine(line, session);
    if (const ojson* e = firstError(r)) return *e;
  }
  return silent();
}

}  // namespace piet::handlers
