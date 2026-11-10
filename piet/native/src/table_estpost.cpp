// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table estpost.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// estpost -- the pseudo-estimate (plan design decision 3)
//
// Stata's estpost posts a summary table into e() so that `esttab, cells(mean)`
// can read it. The posted objects are 1xk MATRICES named after the statistic
// (e(mean), e(sd), ...), with the variables as column names -- which is exactly
// the shape `estadd matrix` already stores, so estpost reuses that bag rather
// than inventing a second one.
//
// The numbers come from the SAME cores the native commands use:
//   summarize -> handlers::summarizeCoreStats (handlers.cpp)
//   tabstat   -> the `cobra tabstat --tidy` argv, re-assembled here
// `runTabstatCore` (estimation_m4.cpp:967-1013) is in an anonymous namespace
// and is therefore NOT callable from this TU. Re-assembling its six-line argv
// is cheaper than exposing a symbol out of that hot file, and the SOURCE of the
// numbers -- the engine -- is identical either way, which is the property that
// matters. A native re-implementation of the statistics would not be.
// ===========================================================================



// A 1xk matrix in the `added.matrices` shape.
ojson rowMatrix(const std::vector<std::string>& colnames, const std::vector<double>& values) {
  ojson m;
  m["rownames"] = ojson::array({"y1"});
  m["colnames"] = colnames;
  ojson data = ojson::array();
  ojson row = ojson::array();
  for (double v : values) {
    if (std::isnan(v)) {
      row.push_back(nullptr);
    } else {
      row.push_back(v);
    }
  }
  data.push_back(row);
  m["data"] = data;
  return m;
}


double jsonNum(const ojson& v) {
  if (v.is_number()) return v.get<double>();
  return std::numeric_limits<double>::quiet_NaN();
}

const std::vector<SummStat>& summStats() {
  static const std::vector<SummStat> k = {
      {"count", "N", false}, {"sum_w", "sum_w", false}, {"mean", "mean", false},
      {"Var", "var", false}, {"sd", "sd", false}, {"min", "min", false},
      {"max", "max", false}, {"sum", "sum", false},
      {"p1", "p1", true},     {"p25", "p25", true},   {"p50", "p50", true},
      {"p75", "p75", true},   {"p99", "p99", true},
  };
  return k;
}


// Turn the posted matrices into the on-screen table. One row per variable, one
// column per statistic -- `columns(variables)` transposes it, which is what
// that option means and why it is not a silent no-op here.
Table estpostTable(const std::vector<std::string>& vars,
                   const std::vector<std::string>& statNames, const ojson& matrices,
                   bool byVariables, const std::string& title) {
  Table t;
  t.title = title;
  auto cell = [&](const std::string& stat, size_t varIdx) -> std::string {
    if (!matrices.contains(stat)) return "";
    const ojson& m = matrices.at(stat);
    if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) return "";
    const ojson& row = m["data"][0];
    if (varIdx >= row.size()) return "";
    if (row[varIdx].is_null()) return kMissingCell;
    return fmtNum(jsonNum(row[varIdx]), stat == "count" ? "%9.0f" : "%9.4f");
  };
  if (byVariables) {
    t.colHeads = vars;
    for (const auto& s : statNames) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = s;
      for (size_t v = 0; v < vars.size(); ++v) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  } else {
    t.colHeads = statNames;
    for (size_t v = 0; v < vars.size(); ++v) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = vars[v];
      for (const auto& s : statNames) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  }
  return t;
}


void postPseudoEstimate(SessionState& session, const std::string& estimator, double nObs,
                        const ojson& matrices, const std::vector<PseudoCoef>& coefs) {
  // eb-order T-P3 part 2: a NEW e() with no fitted model behind it. Reset
  // BEFORE anything is written: lastModel_reset also clears the block, the
  // estadd bag and the store name, which this function then writes again.
  session.lastModel_reset();
  ojson coeftable = ojson::array();
  for (const auto& c : coefs) {
    ojson row;
    row["name"] = c.name;
    row["b"] = std::isnan(c.b) ? ojson(nullptr) : ojson(c.b);
    row["se"] = std::isnan(c.se) ? ojson(nullptr) : ojson(c.se);
    // No t and no p: a summary statistic has no null hypothesis, and a zero
    // here would draw significance stars onto a column of means.
    row["t"] = nullptr;
    row["p"] = nullptr;
    coeftable.push_back(row);
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["depvar"] = "";
  // An INTEGER, because the shared fingerprint reads n_obs with get<long long>()
  // and because e(N) is a count.
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = coeftable;
  // eb-order T-P3: the coeftable above is esttab's table, not an e(b). Stata's
  // estpost posts no e(b)/e(V) (estpost), so `_b[x1]`, `matrix
  // list e(b)` and `: colnames e(b)` refuse and test/lincom/predict are r(321)
  // (w2v_estpost_ematrix.log ep13_local_bx1 111, ep13_list_eb 111, ep14_test
  // 321). SessionState::blockPostsEb reads the mark.
  block["posts_e_b"] = false;
  session.lastEstBlock = block;
  // W2 esttabnames: `ereturn post` drops the hidden e(_estimates_name), so
  // these results carry no store name (`esttab .` is the live posting).
  session.activeEstimatesName.clear();
  session.estAddedClear();
  session.estAdded["matrices"] = matrices;
  // BUG-408: estpost is a NEW e(), so the session stores are replaced, not
  // added to — then the posted matrices land in them and `matrix M = e(mean)`
  // resolves. Without the clear, a previous estimation's e(b)/e(V) would sit
  // beside estpost's summary matrices and `ereturn list` would show both.
  session.eval.eScalars.clear();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();
  // eb-order T-P3: e(cmd) `estpost` and e(subcmd) the wrapped command, which
  // estpost.ado posts for every subcommand (summarize, tabstat;
  // ~/Library/Application Support/Stata/ado/plus/e/estpost.ado).
  session.eval.setEString("cmd", "estpost");
  {
    const std::string pre = "estpost_";
    session.eval.setEString("subcmd", estimator.rfind(pre, 0) == 0 ? estimator.substr(pre.size())
                                                                    : estimator);
  }
  {
    std::vector<std::pair<std::string, ojson>> mm;
    for (const auto& [k, v] : matrices.items()) mm.emplace_back(k, v);
    std::vector<std::pair<std::string, double>> scalars = {{"N", nObs}};
    // estpost.ado posts k for summarize, while tabstat posts only N.
    if (estimator == "estpost_summarize")
      scalars.emplace_back("k", static_cast<double>(coefs.size()));
    mirrorIntoEStores(session, scalars, {}, mm);
  }
  // Stamped from the SHARED helper, after the block is complete -- the
  // coeftable length is part of the fingerprint.
  session.estAddedFp = SessionState::estAddFingerprint(block);
}


// The two statistics that become e(b) and e(se). `mean`/`sd` when they were
// computed, else the first two requested -- so `estpost tabstat x, stat(sum)`
// still posts something addressable rather than a blank column.
void pickPrimary(const std::vector<std::string>& statNames, std::string* bStat,
                 std::string* seStat) {
  const auto have = [&](const char* n) {
    return std::find(statNames.begin(), statNames.end(), n) != statNames.end();
  };
  *bStat = have("mean") ? "mean" : (statNames.empty() ? std::string() : statNames[0]);
  if (have("sd")) {
    *seStat = "sd";
  } else {
    for (const auto& s : statNames) {
      if (s != *bStat) { *seStat = s; break; }
    }
  }
}


// Read one cell out of a posted 1xk matrix by column position.
double matCell(const ojson& matrices, const std::string& stat, size_t j) {
  if (stat.empty() || !matrices.contains(stat)) return std::numeric_limits<double>::quiet_NaN();
  const ojson& m = matrices.at(stat);
  if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const ojson& row = m["data"][0];
  if (j >= row.size() || row[j].is_null()) return std::numeric_limits<double>::quiet_NaN();
  return jsonNum(row[j]);
}
}
namespace piet::handlers {
using namespace table_detail;


ojson runEstpost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstpost.known,
                                            kVocabEstpost.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("estpost: a subcommand is required (summarize or tabstat)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {
      "tabulate", "tab", "ttest", "correlate", "corr", "margins", "ci", "stci", "svy"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  const bool isSumm = (sub == "summarize" || sub == "sum" || sub == "summ");
  const bool isTabstat = (sub == "tabstat");
  if (!isSumm && !isTabstat) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }

  std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (vars.empty()) {
    if (isTabstat) return tblSyntax("estpost tabstat: at least one variable is required");
    vars = session.datastore()->names();
  }
  for (const auto& v : vars) {
    if (!session.datastore()->has(v)) {
      return tblError("variable " + v + " not found", "unknown_variable");
    }
  }

  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  const bool quiet = hasFlag(parsed.opts, "quietly");
  std::optional<std::string> statsOpt = optStr(parsed.opts, "statistics");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stat");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stats");
  const std::optional<std::string> colsOpt = optStr(parsed.opts, "columns");
  bool byVariables = false;
  if (colsOpt.has_value()) {
    const std::string c = strutil::lower(dequote(*colsOpt));
    if (c == "variables" || c == "var") {
      byVariables = true;
    } else if (c != "statistics" && c != "stat" && c != "stats") {
      return tblSyntax("estpost: columns(" + *colsOpt +
                       ") must be columns(statistics) or columns(variables)");
    }
  }

  // `listwise` is casewise deletion ACROSS the variable list, and it is folded
  // into the row filter rather than applied afterwards -- doing it afterwards
  // would summarize each variable over its own complete cases, which is the
  // question `listwise` exists to stop asking.
  std::string ifExprText = parsed.ifExpr.value_or(std::string());
  if (hasFlag(parsed.opts, "listwise")) {
    std::string cond;
    for (const auto& v : vars) {
      if (!cond.empty()) cond += " & ";
      cond += "!missing(" + v + ")";
    }
    ifExprText = ifExprText.empty() ? cond : ("(" + ifExprText + ") & (" + cond + ")");
  }
  std::optional<std::string> effIf;
  if (!ifExprText.empty()) effIf = ifExprText;

  std::string dataPath = *session.currentHbin();
  try {
    const auto filt = applyIfAndInFilter(session, dataPath, effIf, parsed.inRange);
    if (!filt.ok) return inRangeRejectError(filt, parsed.inRange.value_or(""), "estpost");
    dataPath = filt.dataPath;
  } catch (const CobraError& ce) {
    return tblError(cobraErrorMessage(ce), ce.code);
  }

  ojson matrices = ojson::object();
  std::vector<std::string> statNames;
  double nObs = 0;

  if (isSumm) {
    if (byOpt.has_value()) {
      // estpost summarize has no by() in Stata; saying so names the command
      // that does, instead of accepting the option and summarizing the pooled
      // sample under a grouped heading.
      return tblSyntax(
          "estpost summarize: by() is not part of estpost summarize; use `estpost tabstat "
          "..., by(g)'");
    }
    ojson summ;
    try {
      // The SAME shape runSummarize uses (handlers.cpp:1280-1310): the session's
      // own Datastore when nothing was filtered, and a temporary one wrapped in
      // a StatelessRead when it was. Opening a fresh Datastore on the head
      // unconditionally would evict the resident frame and force the very
      // re-materialisation the resident plane exists to avoid.
      if (dataPath == *session.currentHbin()) {
        summ = summarizeCoreStats(*session.datastore(), vars, /*detail=*/true);
      } else {
        Datastore tmp(dataPath);
        SessionState::StatelessRead stateless(session);
        summ = summarizeCoreStats(tmp, vars, /*detail=*/true);
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost summarize: ") + e.what());
    }
    if (isErrorBlock(summ)) return summ;
    const ojson& stats = summ.contains("stats") ? summ["stats"] : ojson::object();
    // A factor variable has no mean; summarizeCoreStats reports N with null
    // statistics for it (handlers.cpp:1193-1208) and estpost carries that
    // through as MISSING, never as zero.
    const bool wantDetail = hasFlag(parsed.opts, "detail");
    for (const auto& st : summStats()) {
      if (st.detailOnly && !wantDetail) continue;
      std::vector<double> vals;
      bool any = false;
      for (const auto& v : vars) {
        double d = std::numeric_limits<double>::quiet_NaN();
        if (stats.contains(v) && stats[v].contains(st.blockKey)) {
          d = jsonNum(stats[v][st.blockKey]);
          if (!std::isnan(d)) any = true;
        }
        vals.push_back(d);
      }
      if (!any && std::string(st.eName) != "count") continue;
      matrices[st.eName] = rowMatrix(vars, vals);
      statNames.push_back(st.eName);
    }
    for (const auto& v : vars) {
      if (stats.contains(v) && stats[v].contains("N")) {
        nObs = std::max(nObs, jsonNum(stats[v]["N"]));
      }
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < vars.size(); ++j) {
        coefs.push_back(PseudoCoef{vars[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_summarize", nObs, matrices, coefs);
    }
  } else {
    // The `cobra tabstat --tidy` argv, re-assembled (see the block comment
    // above for why it is re-assembled and not called).
    std::vector<std::string> args = {"tabstat", "--vars", joinWith(vars, ",")};
    if (byOpt.has_value() && !byOpt->empty()) {
      args.push_back("--by");
      args.push_back(dequote(*byOpt));
    }
    std::string statCsv = "mean,sd,min,max,n";
    if (statsOpt.has_value() && !statsOpt->empty()) {
      statCsv = joinWith(splitTopLevel(dequote(*statsOpt), ' '), ",");
      // `statistics(mean, sd)` -- a comma-separated list already; collapse the
      // double separator rather than sending an empty stat name to the engine.
      std::vector<std::string> cleaned;
      for (const auto& piece : splitTopLevel(statCsv, ',')) {
        const std::string p = strutil::trim(piece);
        if (!p.empty()) cleaned.push_back(p);
      }
      statCsv = joinWith(cleaned, ",");
    }
    args.push_back("--stats");
    args.push_back(statCsv);
    args.push_back("--data");
    args.push_back(dataPath);
    args.push_back("--tidy");

    ojson raw;
    try {
      raw = runCobraJson(args);
    } catch (const CobraError& ce) {
      return tblError(cobraErrorMessage(ce), ce.code);
    }
    if (raw.contains("stats") && raw["stats"].is_array()) {
      for (const auto& s : raw["stats"]) {
        if (s.is_string()) statNames.push_back(s.get<std::string>());
      }
    }
    if (raw.contains("n_obs")) nObs = jsonNum(raw["n_obs"]);

    const bool grouped = byOpt.has_value() && raw.contains("groups") && raw["groups"].is_array();
    const bool nototal = hasFlag(parsed.opts, "nototal");
    std::vector<std::string> colnames;
    std::map<std::string, std::vector<double>> byStat;
    auto addBlock = [&](const std::string& prefix, const ojson& block) {
      for (const auto& v : vars) {
        colnames.push_back(prefix.empty() ? v : (prefix + ":" + v));
        for (const auto& s : statNames) {
          double d = std::numeric_limits<double>::quiet_NaN();
          if (block.contains(v) && block[v].contains(s)) d = jsonNum(block[v][s]);
          byStat[s].push_back(d);
        }
      }
    };
    if (grouped) {
      for (const auto& g : raw["groups"]) {
        const std::string gname =
            g.contains("value") ? fmtCount(g["value"]) : std::string("g");
        if (g.contains("stats")) addBlock(gname, g["stats"]);
      }
      if (!nototal && raw.contains("total")) addBlock("Total", raw["total"]);
    } else if (raw.contains("total")) {
      addBlock("", raw["total"]);
    }
    for (const auto& s : statNames) {
      matrices[s] = rowMatrix(colnames, byStat[s]);
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < colnames.size(); ++j) {
        coefs.push_back(PseudoCoef{colnames[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_tabstat", nObs, matrices, coefs);
    }
    if (!colnames.empty()) vars = colnames;
  }

  if (quiet) return ojson::array();
  const Table t = estpostTable(vars, statNames, matrices,
                               byVariables, isSumm ? "Summary statistics"
                                                   : "Summary statistics (tabstat)");
  return echoBlock(renderFixed(t));
}
}
