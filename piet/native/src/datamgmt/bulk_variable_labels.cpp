// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// labvars -- assign variable labels in bulk (variable-metadata family; shared helpers in
// variable_metadata_support.h).
//
//   labvars   runLabelVariable.
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

// labvars.ado 1.0.5's syntax line, all read.
const piet::eval::OptVocab& kVocabLabvars = piet::eval::registerOptVocab(
    {"labvars"}, {{"remove", "carryon", "alternate", "names"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

// ===========================================================================
// labvars
// ===========================================================================
//
// Implements labvars per its help file (Daniel Klein, labvars 1.0.5); identified against
// StataBE probes (w2ax_t1_community.log labvars_*, mx3.log LV|*):
//   * `labvars [varlist] "l1" "l2" ... [, remove carryon alternate names]`; the varlist is the
//     text before a `\` or else before the first quote, or every variable when the first word
//     is quoted (LV|allq); the labels are Stata's words (labvars_dq counts 3);
//   * fewer labels than variables is r(198) "# variables but # labels specified" unless
//     remove/carryon (then the rest are cleared / take the last label: LV|remove1,
//     LV|carry); more labels than variables is always r(198) (labvars_more); NOTHING is
//     labelled on an error (labvars_fewer keeps v1's earlier label);
//   * no labels and no option r(100) "<k> variables but 0 labels specified"; `labvars,
//     remove` clears every variable; remove with carryon r(198);
//   * `alternate` reads var/label pairs (LV|alt), `names` labels each variable with its name;
//     either beside another option is r(198) "option <x> not allowed" before any label is
//     written (review p5 lv_altnam);
//   * the arguments are read as typed: an if/in qualifier is not one, its words count as
//     labels (p5 lv_if, 5 labels), and a `\` inside quotes is text (p7 lv_bs);
//   * each label keeps its first 80 characters, with Stata's note (LV|long).
namespace {

// `label variable v "text"`'s 80-character rule (the dispatcher's label-variable arm), with
// the note Stata prints for it.
ojson applyVarLabel(const std::string& v, std::string text, SessionState& session,
                    ojson& notes) {
  size_t chars = 0, cut = text.size();
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (chars == 80) {
      cut = i;
      break;
    }
    ++chars;
  }
  if (cut < text.size()) {
    text = text.substr(0, cut);
    notes.push_back(displayBlock("note: label truncated to 80 characters"));
  }
  return runLabelVariable(v, text, session);
}

std::string countText(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

}  // namespace

ojson runLabvars(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabvars.known,
                                            kVocabLabvars.unimplemented))
    return ojson(*err);
  const std::vector<std::string> allVars = allVarNames(session);
  if (allVars.empty()) return rcError("no variables defined", 111);
  const bool remove = hasFlag(parsed, "remove"), carryon = hasFlag(parsed, "carryon");
  const bool alternate = hasFlag(parsed, "alternate"), names = hasFlag(parsed, "names");
  // The arguments as typed, up to the options comma. labvars takes `anything`, so an
  // `if`/`in` qualifier is not one: its words are labels (review p5 lv_if: `labvars b
  // "lab b" if b > 0` is "1 variable but 5 labels specified").
  std::string text;
  {
    std::string raw = strutil::trim(parsed.raw);
    size_t sp = 0;  // the command word: `labvars, remove` has no blank before its comma
    while (sp < raw.size() &&
           (std::isalnum(static_cast<unsigned char>(raw[sp])) || raw[sp] == '_'))
      ++sp;
    raw = raw.substr(sp);
    const long long comma = eval::findUnquotedComma(raw);
    if (comma >= 0) raw = raw.substr(0, static_cast<size_t>(comma));
    text = strutil::trim(raw);
  }
  ojson notes = ojson::array();

  // The two subroutines take only their own option: anything else beside them is refused
  // before a label is written (review p5 lv_altnam `option names not allowed`).
  if (alternate || names) {
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const bool allowed = alternate ? k == "alternate" : k == "names";
      if (!allowed) return rcError("option " + k + " not allowed", 198);
    }
  }
  if (alternate || names) {
    if (alternate) {
      const std::vector<std::string> w = stataWords(text);
      if (w.empty()) return rcError("too few variables specified", 102);
      if (w.size() % 2 != 0)
        return rcError(countText((w.size() + 1) / 2, "variable") + " but " +
                           countText(w.size() / 2, "label") + " specified",
                       198);
      std::vector<std::string> vars;
      for (size_t i = 0; i < w.size(); i += 2) {
        std::vector<std::string> one;
        if (auto e = unabVarlist(w[i], session, &one)) return *e;
        if (one.size() != 1) return rcError(w[i] + " invalid name", 198);
        vars.push_back(one.front());
      }
      for (size_t i = 0; i < vars.size(); ++i)
        if (const ojson* e = firstError(applyVarLabel(vars[i], w[2 * i + 1], session, notes)))
          return *e;
    }
    if (names) {
      std::vector<std::string> vars = allVars;
      if (!text.empty())
        if (auto e = unabVarlist(text, session, &vars)) return *e;
      for (const auto& v : vars)
        if (const ojson* e = firstError(applyVarLabel(v, v, session, notes))) return *e;
    }
    return notes;
  }
  if (remove && carryon) return rcError("options remove and carryon not both allowed", 198);

  std::string vlText, labText;
  if (text.empty()) {
    if (!remove && !carryon)
      return rcError(countText(allVars.size(), "variable") + " but 0 labels specified", 100);
    vlText = "*";
  } else if (text[0] == '"' || (text.size() >= 2 && text[0] == '`' && text[1] == '"')) {
    vlText = "*";
    labText = text;
  } else {
    // The backslash separator counts outside quotes only (review p7 lv_bs: `labvars b
    // "a\b"` labels b with the backslash kept).
    size_t bs = std::string::npos;
    {
      const std::vector<bool> quoted = eval::quotedSpanMask(text);
      for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\\' && !quoted[i]) {
          bs = i;
          break;
        }
    }
    if (bs != std::string::npos) {
      vlText = strutil::trim(text.substr(0, bs));
      labText = text.substr(bs + 1);
      if (vlText.empty()) vlText = "*";
    } else {
      const size_t q = text.find('"');
      vlText = strutil::trim(q == std::string::npos ? text : text.substr(0, q));
      labText = q == std::string::npos ? std::string() : text.substr(q);
      if (q != std::string::npos && q > 0 && text[q - 1] == '`') {
        vlText = strutil::trim(text.substr(0, q - 1));
        labText = text.substr(q - 1);
      }
    }
  }
  std::vector<std::string> vars;
  if (auto e = unabVarlist(vlText, session, &vars)) return *e;
  const std::vector<std::string> labels = stataWords(labText);
  if (vars.size() != labels.size() && (vars.size() < labels.size() || (!remove && !carryon)))
    return rcError(countText(vars.size(), "variable") + " but " + countText(labels.size(), "label") +
                       " specified",
                   198);
  std::string lbl;
  for (size_t i = 0; i < vars.size(); ++i) {
    const std::string tok = i < labels.size() ? labels[i] : std::string();
    if (!tok.empty() || !carryon) lbl = tok;
    if (const ojson* e = firstError(applyVarLabel(vars[i], lbl, session, notes))) return *e;
  }
  return notes;
}

}  // namespace piet::handlers
