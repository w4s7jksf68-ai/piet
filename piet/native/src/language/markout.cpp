// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   markout   a dispatched `quietly replace`, the shape runMarksample uses.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers/session_commands.h"
#include "piet/handlers/variables.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "command_reply_support.h"

namespace {

// markout: STRok SYSMISsok. `sysmissok` keeps a system-missing `.` and marks
// out only the extended .a-.z -- a distinction this build's frame does not
// carry (extended missings are stored as `.`; see runMvencode's note), so it
// is refused rather than answered as if every missing were `.`.
const piet::eval::OptVocab& kVocabMarkout =
    piet::eval::registerOptVocab({"markout"}, {{"strok"}, {"sysmissok"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

// ===========================================================================
// markout
// ===========================================================================
//
// Implements `markout markvar [varlist] [, strok]` per mark's help file;
// identified against StataBE probes (w2ax_t1_official.log markout_prog,
// markout_strok, markout_outside, markout_str_outside; envp2 mo_*). It works
// inside AND outside a program (markout_outside rc 0): unlike marksample it
// reads no `syntax` local, only the variables it is given. Rules 5-6 of the
// help: markvar is set to 0 where a numeric variable is missing; a string
// variable sets it to 0 in EVERY observation unless `strok`, under which ""
// counts as missing. The markvar must be an existing byte variable (envp2
// mo_strmark `s not byte`, mo_nonbyte `m2 not byte`: r(198), nothing changed).
// The write is a dispatched `quietly replace`, runMarksample's own shape.
nlohmann::ordered_json runMarkout(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarkout.known,
                                            kVocabMarkout.unimplemented))
    return ojson(*err);
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value() || parsed.weightType.has_value())
    return envError("markout takes no if, in or weight qualifier", 101);
  if (parsed.varlist.empty()) return envError("varlist required", 100);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return ojson(eval::engineError(std::string("markout: ") + e.what(), "data_io"));
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  // A unique prefix stands for its variable, the markvar and the varlist alike
  // (review k1 K1 `markout t xv` for xvar, K4 `markout tma xvar` for tmark:
  // rc 0) -- resolved by the by-key resolver every varlist-taking verb shares
  // (handlers::resolveByKeyNames: wildcards and ranges expand, a unique prefix
  // binds, several matches are r(111) "ambiguous abbreviation").
  std::vector<std::string> markv{parsed.varlist[0]};
  if (markv[0].find_first_of("*?-") != std::string::npos)
    return envError(markv[0] + " invalid name", 198);
  if (auto err = resolveByKeyNames(session, markv)) return *err;
  const std::string markvar = markv[0];
  if (!ds->has(markvar)) return envError("variable " + markvar + " not found", 111);
  {
    bool isByte = ds->dtype(markvar) == "f64" && session.implicitFloatVars.count(markvar) == 0;
    auto it = session.varMeta.find(markvar);
    if (isByte && it != session.varMeta.end() && it->second.stataType.has_value() &&
        !it->second.stataType->empty())
      isByte = *it->second.stataType == "byte";
    if (!isByte) return envError(markvar + " not byte", 198);
  }
  if (parsed.varlist.size() == 1) return ojson::array();  // envp2 mo_novarlist: rc 0

  std::vector<std::string> rest(parsed.varlist.begin() + 1, parsed.varlist.end());
  for (const auto& v : rest) {
    // A time-series operator needs the lag machinery `replace` would have to
    // evaluate per variable here; refused by name rather than read as a name.
    if (v.find('.') != std::string::npos)
      return ojson(eval::refusedCommand("markout " + v, "ts_operator_varlist"));
  }
  if (auto err = resolveByKeyNames(session, rest)) return *err;  // K5 `*v`, K12 `xvar-lv`
  const bool strok = hasOpt(parsed.opts, "strok");
  bool zeroAll = false;
  std::vector<std::string> terms;
  for (const auto& v : rest) {
    if (!ds->has(v)) return envError("variable " + v + " not found", 111);
    if (ds->dtype(v) == "f64") {
      terms.push_back("missing(" + v + ")");
    } else if (strok) {
      terms.push_back(v + " == \"\"");
    } else {
      zeroAll = true;  // rule 6 (markout_prog, markout_str_outside: count 0)
    }
  }
  std::string cmd = "quietly replace " + markvar + " = 0";
  if (!zeroAll) {
    if (terms.empty()) return ojson::array();
    cmd += " if " + joinWith(terms, " | ");
  }
  ojson blocks = dispatch(cmd, session);
  if (const ojson* e = firstError(blocks)) return *e;
  return ojson::array();
}

}  // namespace piet::handlers
