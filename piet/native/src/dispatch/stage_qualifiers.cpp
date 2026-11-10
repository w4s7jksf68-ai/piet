// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool foldCommandCondition(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask) {

  {
    static const std::unordered_set<std::string> kOwnRowCondition = {
        "generate", "gen", "g", "replace", "count", "keep", "drop",
        "by", "bys", "byso", "bysor", "bysort"};
    // Graph commands read their `if` per plot layer (graph_m4.cpp), not from here.
    const registry::CommandSpec* routeSpec = registry::findByName(cmd);
    const bool graphFamily = routeSpec != nullptr && routeSpec->family == registry::Family::Graph;
    // BUG-1327 / BUG-1093: assert's `if` reads its constant string cells
    // (substituteStringCells) before it is routed: the row evaluator has no `[`,
    // so `assert m == "a" if m[1] == "b" & _n > 1` failed in the mask below.
    if (cmd == "assert" && parsed.ifExpr.has_value())
      parsed.ifExpr = substituteStringCells(*parsed.ifExpr, session);
    if (parsed.ifExpr.has_value() && !kOwnRowCondition.count(cmd) && !graphFamily &&
        session.hasData() && strmutate::needsConditionMask(session, *parsed.ifExpr)) {
      strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *parsed.ifExpr);
      if (!mi.ok) {
        blocks.push_back(mi.error);
        return true;
      }
      conditionMask.columns.push_back(mi.column);
      parsed.ifExpr = mi.condition;
    }
    // A weight that is an expression (`[pw=1/wf]`, `[aw = 1/wf]`: w2aj_qualifiers
    // qf_rw_pw_expr(_sp), qf_sw_aw_expr(_sp)) is generated once into a hidden
    // column the command receives as its weight variable; e(wexp) keeps the text
    // (commandWexp above). Every weight reader takes a variable name, so this is
    // the one place an expression becomes one.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && session.hasData() &&
        !strutil::isValidStataIdentifier(*parsed.weightVar) &&
        !(cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor" || cmd == "bysort")) {
      strmutate::ConditionMaskInstall wi =
          strmutate::installWeightExpression(session, *parsed.weightVar);
      if (!wi.ok) {
        blocks.push_back(wi.error);
        return true;
      }
      conditionMask.columns.push_back(wi.column);
      parsed.weightVar = wi.column;
    } else if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && session.hasData() &&
               !(cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor" ||
                 cmd == "bysort")) {
      // A weight variable is read as Stata reads a name in an expression: the
      // variable, else its unique abbreviation, else r(111) "variable X not found"
      // (qf_sw_aw_nosuch, qf_rw_aw_nosuch). Each reader forwarded the text to the
      // engine, whose `no column: X` was rc 1.
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) known.push_back(vi.name);
      } else if (Datastore* wds = session.datastore()) {
        known = wds->names();
      }
      const std::string& wv = *parsed.weightVar;
      if (!known.empty() && std::find(known.begin(), known.end(), wv) == known.end()) {
        std::vector<std::string> hits;
        for (const auto& k : known)
          if (k.compare(0, wv.size(), wv) == 0) hits.push_back(k);
        if (hits.size() == 1) {
          parsed.weightVar = hits.front();
        } else {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              hits.empty() ? "variable " + wv + " not found" : wv + " ambiguous abbreviation",
              std::nullopt, std::nullopt, std::nullopt, "r(111)")));
          return true;
        }
      }
    }
  }
  return false;
}


bool noteConditionProfile(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks) {


  // INSPECT-2608: the per-FORM key, built at the same seam and for the same
  // reason — this is where the command is canonical AND fully parsed. A form is
  // the verb plus PRESENCE flags for the qualifiers that change how much work
  // the command does; values never enter the key, so `if x > .5` and `if x > .9`
  // are one row and the table stays bounded. Per-verb alone would average a
  // filtered `summarize` together with a whole-frame one, which is exactly the
  // distinction a per-command efficiency census exists to see.
  // No-op below PIET_EMIT_TIMING=4.
  if (piet::counters::timingLevel() >= 4) {
    std::string form = cmd;
    if (parsed.ifExpr.has_value()) form += "|if";
    if (parsed.inRange.has_value()) form += "|in";
    if (parsed.weightType.has_value()) form += "|w";
    piet::counters::noteResolvedForm(form);
  }
  return false;
}

}
