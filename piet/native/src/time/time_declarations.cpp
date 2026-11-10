// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Time/panel declaration validation and execution, moved from dispatcher.cpp.
// Option registration and the common command lifecycle stay in the dispatcher.
#include "piet/time_declarations.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "dispatch/varlist_join.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/time_panel.h"
#include "piet/handlers/variables.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::commands {
using ojson = nlohmann::ordered_json;

namespace {
// B4-W3b: the PERIOD options. Each names a Stata date format and nothing else
// (`tsset t, quarterly` -> r(tsfmt) "%tq" and `format t %tq`), so the table
// below IS the implementation, not a lookup beside one. `generic` is Stata's
// own name for the %tg default; `clock`/`Clock` are the datetime pair.
const std::map<std::string, std::string>& kTssetPeriodFmt() {
  static const std::map<std::string, std::string> m = {
      {"yearly", "%ty"},   {"halfyearly", "%th"}, {"quarterly", "%tq"},
      {"monthly", "%tm"},  {"weekly", "%tw"},     {"daily", "%td"},
      {"generic", "%tg"},  {"clock", "%tc"},      {"clocktime", "%tc"},
      {"Clock", "%tC"}};
  return m;
}

// tsset.ado's and xtset.ado's `syntax [varlist(numeric max=2 default=none)]`
// reads the varlist before any option is looked at, by resolveByKeyNames' rule
// (a wildcard, range or unique abbreviation expands): a name that is no variable
// is r(111), a third variable r(103), a string one r(109). So `xtset zz, foo` is
// 111, `tset s, clear` 109 and `xtset id t s, clear` 103 (StataBE 19.5), never
// the option or `clear` refusal that follows them. Before the count, each word in
// turn: an operator is r(101) "factor-variable and time-series operators not
// allowed", an interaction r(101) "interactions not allowed", a word no name
// starts with r(198) "1 invalid name", a plain name that is no variable r(111) --
// `xtset L.id t` and `xtset id t L.abc` are 101, `xtset 1 L.t` 198, `xtset zz L.t`
// 111 (StataBE 19.5). `names` comes back resolved.
std::optional<ojson> tsVarlistPass(SessionState& session, std::vector<std::string>& names) {
  if (names.empty() || !session.hasData()) return std::nullopt;
  std::map<std::string, bool> isString;
  if (const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : *card) isString[v.name] = v.dtype != "f64";
  } else if (Datastore* ds = session.datastore()) {
    for (const auto& n : ds->names()) isString[n] = ds->dtype(n) != "f64";
  }
  auto refuse = [](const std::string& msg, const char* rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt, rc));
  };
  for (const auto& tok : names) {
    if (tok.find('.') != std::string::npos)
      return refuse("factor-variable and time-series operators not allowed", "r(101)");
    if (tok.find('#') != std::string::npos) return refuse("interactions not allowed", "r(101)");
    if (!tok.empty() && (std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '"'))
      return refuse(tok + " invalid name", "r(198)");
    if (!strutil::isValidStataIdentifier(tok) || isString.count(tok)) continue;
    int prefixHits = 0;
    for (const auto& [n, str] : isString) prefixHits += n.compare(0, tok.size(), tok) == 0;
    if (prefixHits == 0) return refuse("variable " + tok + " not found", "r(111)");
  }
  if (auto err = handlers::resolveByKeyNames(session, names)) return err;
  for (const auto& n : names)
    if (!isString.count(n)) return refuse("variable " + n + " not found", "r(111)");
  if (names.size() > 2) return refuse("too many variables specified", "r(103)");
  for (const auto& n : names)
    if (isString[n])
      return refuse("string variables not allowed in varlist;\n" + n + " is a string variable",
                    "r(109)");
  return std::nullopt;
}

// tsset.ado's display options, which xtset.ado's PanelSet takes too: mi, noblank,
// panelname(string) and displayindent(integer), each spelled in full (the option
// floors refuse `nob`, `displayi()`). They move out of `opts` into `decl`'s display
// fields (panelname() empty keeps "Panel", a negative indent is none). False when
// displayindent() holds no integer, which tsset answers "option displayindent()
// incorrectly specified" r(198) and PanelSet with its r(101) (StataBE 19.5).
bool takeTsDisplayOptions(std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          handlers::TsDeclare& decl) {
  std::vector<std::pair<std::string, eval::OptValue>> rest;
  for (auto& kv : opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "mi" || k == "noblank") {
      decl.display = true;
    } else if (k == "panelname") {
      decl.display = true;
      std::string label = v.isBool ? std::string() : strutil::trim(v.strValue);
      if (label.size() >= 2 && label.front() == '"' && label.back() == '"')
        label = label.substr(1, label.size() - 2);
      if (!label.empty()) decl.panelLabel = label;
    } else if (k == "displayindent") {
      const std::string t = v.isBool ? std::string() : strutil::trim(v.strValue);
      long long n = 0;
      size_t used = 0;
      try {
        n = std::stoll(t, &used);
      } catch (const std::exception&) {
        return false;
      }
      if (used != t.size()) return false;
      decl.indent = static_cast<int>(std::max(0LL, std::min(n, 80LL)));
      decl.display = true;
    } else {
      rest.push_back(kv);
    }
  }
  opts = std::move(rest);
  return true;
}

ojson tsDisplayIndentError() {
  return ojson(eval::errorBlock("syntax_error", "option displayindent() incorrectly specified",
                                std::nullopt, std::nullopt, std::nullopt, "r(198)"));
}

// The `syntax [, CLEAR]` both ados read when no variable is given: any other
// option is r(198) "option yearly not allowed" (`xtset, foo`, `xtset, delta(1)`,
// `tsset, clear yearly`; StataBE 19.5), in rejectUnknownOptions' wording.
std::optional<ojson> tsOnlyClearAllowed(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts)
    if (k != "clear")
      return ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option"));
  return std::nullopt;
}

}  // namespace

void runTimeDeclaration(const std::string& cmd, eval::ParsedCommand& parsed,
                        SessionState& session,
                        const std::unordered_set<std::string>& selectedKnownOptions,
                        nlohmann::ordered_json& blocks) {
  // BUG-896: tsset.ado / xtset.ado's `syntax` lines take no if/in, so either is
  // r(101) (Stata's "if not allowed" / "in range not allowed"), never a qualifier
  // silently dropped. BUG-1422: nor weights; `in` and weights reach this arm
  // (registry.h qualifiersRefusedByArm) and get Stata's texts (StataBE 19.5).
  // `tset` is `tsset` (BUG-1163).
  if ((cmd == "xtset" || cmd == "tsset" || cmd == "tset") &&
      (parsed.ifExpr.has_value() || parsed.inRange.has_value() || parsed.weightType.has_value())) {
    blocks.push_back(ojson(eval::errorBlock(
        "engine_error",
        parsed.ifExpr.has_value() ? "if not allowed"
        : parsed.inRange.has_value() ? "in range not allowed"
                                     : "weights not allowed",
        std::nullopt, std::nullopt, std::nullopt, "r(101)")));
    return;
  }
  if (cmd == "xtset") {
    if (auto err = tsVarlistPass(session, parsed.varlist)) {
      blocks.push_back(*err);
      return;
    }
    // xtset.ado's PanelSet: a panel variable alone takes only the display options
    // (`xtset id, noblank`, `, panelname(Grp)` are rc 0); any other option --
    // `xtset id, delta(1)`, `, yearly`, `, clear`, `, foo`, `, displayindent(x)` --
    // is r(101) (StataBE 19.5). They used to run or say "option not allowed" r(198).
    if (parsed.varlist.size() == 1 && !parsed.opts.empty()) {
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look) || !rest.empty()) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "options not allowed unless a time variable is specified",
            std::nullopt, std::nullopt, std::nullopt, "r(101)")));
        return;
      }
      blocks.push_back(
          handlers::runXtsetPanelOnly(parsed.varlist[0], session, /*declare=*/true, look));
      return;
    }
    // W2D-T06: `xtset, clear` (w2b_dataio.log:323 rc 0, w2d_uncovered.log:364 rc 0)
    // drops the declaration AND its _dta chars; anything combined with it is a
    // refusal, never a half-clear.
    bool xtClear = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "clear") xtClear = true;
    if (xtClear) {
      // The options are read first (`xtset id t, clear foo` is r(198) for foo);
      // then `clear` takes no varlist, r(101) "varlist not allowed" (StataBE 19.5).
      if (auto err = eval::rejectUnknownOptions(parsed.opts, selectedKnownOptions)) {
        blocks.push_back(ojson(*err));
        return;
      }
      if (!parsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return;
      }
      if (auto err = tsOnlyClearAllowed(parsed.opts)) {
        blocks.push_back(*err);
        return;
      }
      session.panelvar.reset();
      session.timevar.reset();
      session.delta.reset();
      session.writeTsChars();
      return;
    }
    if (parsed.varlist.empty() && parsed.opts.empty()) {
      // xtset: the bare query re-displays the declaration through the bare
      // `tsset` (w2w_tsset_tsfill.log:138 ts_xtread rc 0, w2b_dataio.log:318
      // xtset_query rc 0), PanelSet's when only a panel is declared, and
      // r(459) with nothing declared (w2d_uncovered.log:326-332).
      blocks.push_back(handlers::runTsRedisplay(session, "xtset", handlers::TsDeclare{}, false));
      return;
    }
    if (parsed.varlist.empty()) {
      // The display options re-display the declaration with them (`xtset,
      // panelname(Grp)` after `xtset id` is "Grp variable: id (balanced)"); any
      // other option is `syntax [, CLEAR]`'s r(198).
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look)) {
        blocks.push_back(tsDisplayIndentError());
        return;
      }
      if (!rest.empty()) {
        blocks.push_back(*tsOnlyClearAllowed(rest));
        return;
      }
      blocks.push_back(handlers::runTsRedisplay(session, "xtset", look, false));
      return;
    }
    if (parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::engineError(
          "xtset: unexpected extra tokens after ivar [tvar]: " + joinVarlist(std::vector<std::string>(
              parsed.varlist.begin() + 2, parsed.varlist.end())) +
              ". Put each command on its own line.",
          "syntax_error")));
      return;
    }
    std::optional<std::string> tvar;
    if (parsed.varlist.size() > 1) tvar = parsed.varlist[1];
    {
      // A period option (or format()) with `panelvar timevar`: tsset's own
      // declaration path, which formats the time variable and posts r(unit).
      // So does a display option, which shows tsset's Query lines with it.
      bool anyPeriod = false;
      for (const auto& [k, v] : parsed.opts)
        if ((kTssetPeriodFmt().count(k) || k == "format") && (!v.isBool || v.boolValue))
          anyPeriod = true;
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look)) {
        blocks.push_back(tsDisplayIndentError());
        return;
      }
      if ((anyPeriod || look.display) && tvar.has_value()) {
        if (auto err = eval::rejectUnknownOptions(parsed.opts, selectedKnownOptions)) {
          blocks.push_back(ojson(*err));
          return;
        }
        handlers::TsDeclare decl = look;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "delta" && !v.isBool) {
            auto d = eval::pythonFloat(v.strValue);
            if (!d.has_value() || *d <= 0.0) {
              blocks.push_back(ojson(eval::errorBlock(
                  "engine_error", "invalid argument in delta(): " + v.strValue, std::nullopt,
                  std::nullopt, std::nullopt, "r(198)")));
              return;
            }
            decl.delta = d;
          } else if (kTssetPeriodFmt().count(k) && (!v.isBool || v.boolValue)) {
            const std::string period = (k == "clock") ? std::string("clocktime") : k;
            if (!decl.period.empty() && decl.period != period) {
              blocks.push_back(ojson(eval::errorBlock(
                  "engine_error",
                  "may only specify one time-scale from daily, weekly, monthly, quarterly, "
                  "halfyearly, yearly, and generic",
                  std::nullopt, std::nullopt, std::nullopt, "r(198)")));
              return;
            }
            decl.period = period;
          } else if (k == "format" && !v.isBool) {
            decl.format = strutil::trim(v.strValue);
          }
        }
        decl.panelvar = parsed.varlist[0];
        decl.timevar = *tvar;
        blocks.push_back(handlers::runTsDeclare(decl, session, "xtset"));
        return;
      }
    }
    // BS-030 (BETA campaign C2): delta() forwards into the ts-context
    // (translate::injectTsContext -> cobra's --tsdelta); every other real
    // Stata xtset/tsset option (9 unitoptions: daily/weekly/monthly/
    // quarterly/halfyearly/yearly/generic/clocktime/business; noquery;
    // clear) has no engine-side representation in this build and is
    // honest-rejected instead of the prior silent no-op (SCOPE BS-030:
    // "all options silently dropped today").
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block in dispatcher.cpp; this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kXtsetKnownOpts = selectedKnownOptions;
    std::optional<double> xtsetDelta;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::engineError(
              "xtset: delta() requires a positive numeric value, got `" + v.strValue + "'",
              "syntax_error")));
          return;
        }
        xtsetDelta = d;
      }
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kXtsetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return;
    }
    // The handler declares the r() set it posted (tsset.ado's Query: an empty
    // local such as r(unit) after a %9.0g time variable is not posted).
    blocks.push_back(handlers::runXtset(parsed.varlist[0], tvar, session, xtsetDelta));
    return;
  }

  // BUG-1163: the obsolete `tset` is tset.ado's one line `tsset `0'`: every form,
  // option and refusal is tsset's (it used to read no option at all).
  if (cmd == "tsset" || cmd == "tset") {
    // BUG-896 (FU-581): tsset.ado's `syntax [varlist(numeric max=2)]` -- one
    // variable is a time series, two are `panelvar timevar` with the state xtset
    // leaves (xtset.ado hands its two-variable form to tsset), none re-displays
    // the declaration. Oracle tests/eval/golden/aer_w2/w2w_tsset_tsfill.log.
    //
    // `tsset, clear` (tsset.ado's Clear): drops the declaration and its chars, as
    // `xtset, clear` does (W2D-T06); nothing else may come with it.
    if (auto err = tsVarlistPass(session, parsed.varlist)) {
      blocks.push_back(*err);
      return;
    }
    bool tsClear = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "clear") tsClear = true;
    if (tsClear) {
      // As xtset's `clear` above: an unknown option first, then the varlist r(101).
      if (auto err = eval::rejectUnknownOptions(parsed.opts, selectedKnownOptions)) {
        blocks.push_back(ojson(*err));
        return;
      }
      if (!parsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return;
      }
      if (auto err = tsOnlyClearAllowed(parsed.opts)) {
        blocks.push_back(*err);
        return;
      }
      session.panelvar.reset();
      session.timevar.reset();
      session.delta.reset();
      session.writeTsChars();
      return;
    }
    if (parsed.varlist.size() > 2) {
      // w2w_tsset_tsfill.log:1460 rf_ts_three r(103).
      blocks.push_back(ojson(eval::errorBlock("engine_error", "too many variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(103)")));
      return;
    }
    // BUG-233: the one copy of this vocabulary lives in the file-scope
    // registration block in dispatcher.cpp.
    const std::unordered_set<std::string>& kTssetKnownOpts = selectedKnownOptions;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTssetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return;
    }
    handlers::TsDeclare decl;
    bool anyOption = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "invalid argument in delta(): " + v.strValue, std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return;
        }
        decl.delta = d;
        anyOption = true;
      }
    }
    // B4-W3b: the PERIOD option names a %t format; `format(%fmt)` states one.
    // tsset: at most one period (w2w_tsset_tsfill.log:1542 rf_ts_twoper
    // r(198)), and a period with format() is r(198) (runTsDeclare).
    for (const auto& [k, v] : parsed.opts) {
      if (kTssetPeriodFmt().count(k) && (!v.isBool || v.boolValue)) {
        const std::string period = (k == "clock") ? std::string("clocktime") : k;
        if (!decl.period.empty() && decl.period != period) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              "may only specify one time-scale from daily, weekly, monthly, quarterly, "
              "halfyearly, yearly, and generic",
              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return;
        }
        decl.period = period;
        anyOption = true;
      } else if (k == "format" && !v.isBool) {
        decl.format = strutil::trim(v.strValue);
        anyOption = true;
      }
    }
    // The display options ride the declaration (runTsDeclare then shows the
    // Query lines with them): `tsset t, displayindent(4)` indents each line.
    {
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, decl)) {
        blocks.push_back(tsDisplayIndentError());
        return;
      }
    }
    if (parsed.varlist.empty()) {
      // w2w_tsset_tsfill.log:121-125 ts_redisplay, :1574 rf_ts_bare_none r(111).
      blocks.push_back(handlers::runTsRedisplay(session, "tsset", decl, anyOption));
      return;
    }
    if (parsed.varlist.size() == 2) {
      decl.panelvar = parsed.varlist[0];
      decl.timevar = parsed.varlist[1];
    } else {
      decl.timevar = parsed.varlist[0];
    }
    blocks.push_back(handlers::runTsDeclare(decl, session, "tsset"));  // w2w_tsset_tsfill.log:96-99
    return;
  }

}

}  // namespace piet::commands
