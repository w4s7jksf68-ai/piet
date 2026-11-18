// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table estpost.
#include "tables_internal.h"
#include "piet/handlers/descriptive.h"
#include "piet/handlers/estimates_tables.h"
namespace piet::handlers {
// descriptive/summarize.cpp: summarize's statistics under a weight (dispatch/handler_declarations.h).
ojson summarizeCoreStatsWeighted(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                 bool detail, const std::optional<std::string>& weightVar,
                                 const char* weightFlag);
}
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
//   summarize -> handlers::summarizeCoreStats (descriptive/summarize.cpp)
//   tabstat   -> the `cobra tabstat --tidy` argv, re-assembled here
// `runTabstatCore` (descriptive/descriptive_statistics.cpp) is in an anonymous namespace
// and is therefore NOT callable from this TU. Re-assembling its six-line argv
// is cheaper than exposing a symbol out of that hot file, and the SOURCE of the
// numbers -- the engine -- is identical either way, which is the property that
// matters. A native re-implementation of the statistics would not be.
// ===========================================================================



// A 1xk matrix in the `added.matrices` shape. estpost summarize builds its
// matrices from J(1, k, .), whose row is `r1` (estpost.ado; StataBE e(count)
// rowfullnames `r1`); tabstat names the row after the statistic or variable.
ojson rowMatrix(const std::vector<std::string>& colnames, const std::vector<double>& values,
                const std::string& rowname) {
  ojson m;
  m["rownames"] = ojson::array({rowname});
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


// The observation count Stata posts as e(N) for estpost summarize and tabstat: the rows left after
// the if / in restriction, a missing weight or a zero weight (marksample's own rules), a missing by()
// value unless `missing` was given, and rows missing on every variable of the list (listwise has
// already removed any row missing on one of them, so the rule is a no-op there). It is a count of
// observations, never of weights, and it is NOT any single variable's own count.
static bool estpostCellMissing(const cecon::Column& c, int64_t i) {
  if (c.dtype == cecon::DType::F64) return std::isnan(c.f64()[i]);
  return c.codes()[i] < 0 || (static_cast<size_t>(c.codes()[i]) < c.levels.size() && c.levels[static_cast<size_t>(c.codes()[i])].empty());
}

static double estpostSampleN(const Datastore& ds, const std::vector<std::string>& vars,
                             const std::optional<std::string>& weightVar,
                             const std::optional<std::string>& byVar, bool keepMissingBy) {
  const int64_t n = ds.nRows();
  std::vector<const cecon::Column*> vcols;
  for (const auto& v : vars) {
    if (ds.has(v)) vcols.push_back(&ds.col(v));
  }
  const cecon::Column* wcol = nullptr;
  if (weightVar.has_value() && !weightVar->empty() && ds.has(*weightVar)) wcol = &ds.col(*weightVar);
  const cecon::Column* bcol = nullptr;
  if (byVar.has_value() && !byVar->empty() && !keepMissingBy && ds.has(*byVar)) bcol = &ds.col(*byVar);
  int64_t count = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (wcol != nullptr) {
      if (wcol->dtype != cecon::DType::F64) continue;
      const double w = wcol->f64()[i];
      if (std::isnan(w) || w == 0.0) continue;
    }
    if (bcol != nullptr && estpostCellMissing(*bcol, i)) continue;
    bool anyValue = vcols.empty();
    for (const cecon::Column* c : vcols) {
      if (!estpostCellMissing(*c, i)) {
        anyValue = true;
        break;
      }
    }
    if (anyValue) ++count;
  }
  return static_cast<double>(count);
}

double jsonNum(const ojson& v) {
  if (v.is_number()) return v.get<double>();
  return std::numeric_limits<double>::quiet_NaN();
}

// estpost summarize's matrices in Stata's e() order: without detail `count
// sum_w mean Var sd min max sum` (w2v_estpost_ematrix.log:129); with detail
// `count sum_w mean Var sd skewness kurtosis sum min max p1 p5 p10 p25 p50 p75
// p90 p95 p99` (StataBE `estpost summarize price mpg, detail`). `detailOnly`
// entries are posted with detail only.
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

const std::vector<SummStat>& summStatsDetail() {
  static const std::vector<SummStat> k = {
      {"count", "N", false},       {"sum_w", "sum_w", false},       {"mean", "mean", false},
      {"Var", "var", false},       {"sd", "sd", false},             {"skewness", "skewness", true},
      {"kurtosis", "kurtosis", true}, {"sum", "sum", false},        {"min", "min", false},
      {"max", "max", false},       {"p1", "p1", true},              {"p5", "p5", true},
      {"p10", "p10", true},        {"p25", "p25", true},            {"p50", "p50", true},
      {"p75", "p75", true},        {"p90", "p90", true},            {"p95", "p95", true},
      {"p99", "p99", true},
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
                        const ojson& matrices, const std::vector<PseudoCoef>& coefs,
                        const std::vector<std::pair<std::string, std::string>>& locals) {
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
    std::vector<std::pair<std::string, std::string>> ls;
    for (const auto& [k, v] : locals) {
      if (!v.empty()) ls.emplace_back(k, v);
    }
    mirrorIntoEStores(session, scalars, ls, mm);
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
  // estpost summarize takes [aw fw iw]; pweights are r(101) in Stata. A weight on
  // the other implemented subcommand stays refused by name.
  const char* weightFlag = nullptr;
  std::optional<std::string> weightVar;
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    const char kind = static_cast<char>(std::tolower(static_cast<unsigned char>((*parsed.weightType)[0])));
    if (kind == 'p') return tblErrorRc("pweights not allowed", 101);
    // estpost tabstat's syntax line takes [aw fw]; iweights are not allowed there.
    if (isTabstat && kind == 'i') return tblErrorRc("iweights not allowed", 101);
    weightFlag = kind == 'a' ? "--aweight" : (kind == 'f' ? "--fweight" : "--iweight");
    weightVar = parsed.weightVar;
  }
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
  // summarize refuses iweights with detail, and estpost runs it without capture (rc 101).
  if (isSumm && hasFlag(parsed.opts, "detail") && weightFlag != nullptr &&
      std::string(weightFlag) == "--iweight") {
    return tblErrorRc("iweights not allowed", 101);
  }
  if (isSumm && (hasFlag(parsed.opts, "missing") || hasFlag(parsed.opts, "elabels"))) {
    // estpost summarize's syntax line has neither.
    return tblErrorRc(std::string("option ") + (hasFlag(parsed.opts, "missing") ? "missing" : "elabels") +
                          " not allowed",
                      198);
  }
  const bool quiet = hasFlag(parsed.opts, "quietly");
  std::optional<std::string> statsOpt = optStr(parsed.opts, "statistics");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stat");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stats");
  const std::optional<std::string> colsOpt = optStr(parsed.opts, "columns");
  bool byVariables = false;
  if (colsOpt.has_value()) {
    // estpost.ado: any leading part of `variables` or `statistics` (at least
    // one letter; `v`, `s` included), or `stats`; Stata compares as written.
    const std::string c = strutil::trim(dequote(*colsOpt));
    const auto leading = [&](const char* full) {
      return !c.empty() && std::string(full).compare(0, c.size(), c) == 0;
    };
    if (leading("variables")) {
      byVariables = true;
    } else if (!leading("statistics") && c != "stats") {
      return tblSyntax("estpost: columns(" + *colsOpt +
                       ") must be columns(statistics) or columns(variables)");
    }
  }

  // `listwise` is casewise deletion ACROSS the variable list, and it is folded
  // into the row filter rather than applied afterwards -- doing it afterwards
  // would summarize each variable over its own complete cases, which is the
  // question `listwise` exists to stop asking.
  std::string ifExprText = parsed.ifExpr.value_or(std::string());
  // `casewise` is the spelling of listwise estpost summarize and tabstat share (estpost.ado: if casewise, listwise).
  if (hasFlag(parsed.opts, "listwise") || hasFlag(parsed.opts, "casewise")) {
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
      // The SAME shape runSummarize uses (descriptive/summarize.cpp): the session's
      // own Datastore when nothing was filtered, and a temporary one wrapped in
      // a StatelessRead when it was. Opening a fresh Datastore on the head
      // unconditionally would evict the resident frame and force the very
      // re-materialisation the resident plane exists to avoid.
      if (dataPath == *session.currentHbin()) {
        summ = weightFlag != nullptr
                   ? summarizeCoreStatsWeighted(*session.datastore(), vars, /*detail=*/true, weightVar, weightFlag)
                   : summarizeCoreStats(*session.datastore(), vars, /*detail=*/true);
      } else {
        Datastore tmp(dataPath);
        SessionState::StatelessRead stateless(session);
        summ = weightFlag != nullptr
                   ? summarizeCoreStatsWeighted(tmp, vars, /*detail=*/true, weightVar, weightFlag)
                   : summarizeCoreStats(tmp, vars, /*detail=*/true);
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost summarize: ") + e.what());
    }
    if (isErrorBlock(summ)) return summ;
    const ojson& stats = summ.contains("stats") ? summ["stats"] : ojson::object();
    // A factor variable has no mean; summarizeCoreStats reports N with null
    // statistics for it (descriptive/summarize.cpp) and estpost carries that
    // through as MISSING, never as zero.
    const bool wantDetail = hasFlag(parsed.opts, "detail");
    for (const auto& st : wantDetail ? summStatsDetail() : summStats()) {
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
      matrices[st.eName] = rowMatrix(vars, vals, "r1");
      statNames.push_back(st.eName);
    }
    try {
      if (dataPath == *session.currentHbin()) {
        nObs = estpostSampleN(*session.datastore(), vars, weightVar, std::nullopt, false);
      } else {
        Datastore tmp(dataPath);
        nObs = estpostSampleN(tmp, vars, weightVar, std::nullopt, false);
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost summarize: ") + e.what());
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < vars.size(); ++j) {
        coefs.push_back(PseudoCoef{vars[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      // e(N) counts observations (`count if touse`), never weights; the weight
      // type and expression are posted as e(wtype) and e(wexp), empty without one.
      std::vector<std::pair<std::string, std::string>> wlocals;
      if (weightFlag != nullptr) {
        const std::string wf(weightFlag);
        wlocals.emplace_back("wtype", wf == "--aweight" ? "aweight" : (wf == "--fweight" ? "fweight" : "iweight"));
        wlocals.emplace_back("wexp", "= " + strutil::trim(parsed.weightExp.value_or(weightVar.value_or(""))));
      }
      postPseudoEstimate(session, "estpost_summarize", nObs, matrices, coefs, wlocals);
    }
  } else {
    // The `cobra tabstat --tidy` argv, re-assembled (see the block comment
    // above for why it is re-assembled and not called).
    std::vector<std::string> args = {"tabstat", "--vars", joinWith(vars, ",")};
    if (byOpt.has_value() && !byOpt->empty()) {
      args.push_back("--by");
      args.push_back(dequote(*byOpt));
      if (hasFlag(parsed.opts, "missing")) args.push_back("--missing");
    }
    // tabstat's own default is the mean alone (vs-stata bank 6542bb10: `estpost tabstat price, by(rep78)`
    // posts e(stats) `mean` and the single matrix e(mean)).
    std::string statCsv = "mean";
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
    if (weightFlag != nullptr) {
      args.push_back(weightFlag);
      args.push_back(weightVar.value_or(""));
    }
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
    try {
      const std::optional<std::string> byName =
          byOpt.has_value() ? std::optional<std::string>(dequote(*byOpt)) : std::nullopt;
      if (dataPath == *session.currentHbin()) {
        nObs = estpostSampleN(*session.datastore(), vars, weightVar, byName, hasFlag(parsed.opts, "missing"));
      } else {
        Datastore tmp(dataPath);
        nObs = estpostSampleN(tmp, vars, weightVar, byName, hasFlag(parsed.opts, "missing"));
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost tabstat: ") + e.what());
    }

    const bool grouped = byOpt.has_value() && raw.contains("groups") && raw["groups"].is_array();
    const bool nototal = hasFlag(parsed.opts, "nototal");
    // The groups as estpost names them (estpost.ado _estpost_namesandlabels for one row of
    // results, _estpost_eqnamesandlabels for several): each by-value's value label, unless
    // `elabels` was asked, a value holds a `.` (a missing group), or one of the labels holds `:`
    // or `.` or runs past 30 characters -- then every group is named by its value and the
    // dictionary of labels is saved in e(labels). `Total` last.
    std::vector<std::pair<std::string, const ojson*>> groups;
    std::string eLabels;
    if (grouped) {
      const std::string byVar = dequote(*byOpt);
      const auto vl = session.valueLabels.find(byVar);
      bool numBy = true;
      for (const auto& g : raw["groups"]) {
        if (g.contains("value") && g["value"].is_string()) numBy = false;
      }
      // One result row: the single variable (columns(statistics)) or the single statistic.
      const bool colsStat = colsOpt.has_value() ? !byVariables : vars.size() <= 1;
      const bool oneRow = colsStat ? vars.size() == 1 : statNames.size() == 1;
      bool hasLabels = hasFlag(parsed.opts, "elabels");
      std::vector<std::string> names0, names;
      std::string dict;
      size_t gi = 0;
      for (const auto& g : raw["groups"]) {
        std::string value, lbl;
        if (numBy) {
          value = g.contains("value") ? fmtCount(g["value"]) : std::string("g");
          if (vl != session.valueLabels.end() && g.contains("value") && g["value"].is_number()) {
            const double d = g["value"].get<double>();
            if (d == std::floor(d)) {
              auto it = vl->second.find(static_cast<int>(d));
              if (it != vl->second.end()) lbl = it->second;
            }
          }
        } else {
          value = std::to_string(gi + 1);
          lbl = g.value("value", std::string());
        }
        ++gi;
        if (value.find('.') != std::string::npos) {
          hasLabels = true;
          if (lbl.empty()) lbl = value;
          std::string v2;
          for (char c : value) v2 += c == '.' ? std::string("_missing_") : std::string(1, c);
          value = v2;
        }
        names0.push_back(value);
        if (oneRow) {
          if (!lbl.empty()) dict += (dict.empty() ? "" : " ") + value + " `\"" + lbl + "\"'";
        } else {
          if (lbl.empty()) lbl = value;
          dict += (dict.empty() ? "" : " ") + std::string("`\"") + lbl + "\"'";
        }
        if (hasLabels) continue;
        if (lbl.empty()) {
          names.push_back(value);
        } else if (lbl.find_first_of(":.\"") != std::string::npos || lbl.size() > 30) {
          hasLabels = true;
        } else {
          names.push_back(lbl);
        }
      }
      if (hasLabels) names = names0;
      gi = 0;
      for (const auto& g : raw["groups"]) {
        if (g.contains("stats")) groups.emplace_back(names[gi], &g["stats"]);
        ++gi;
      }
      if (!nototal && raw.contains("total")) groups.emplace_back("Total", &raw["total"]);
      if (hasLabels) {
        eLabels = dict;
        if (!oneRow && !nototal && !eLabels.empty()) eLabels += " Total";
      }
    } else if (raw.contains("total")) {
      groups.emplace_back("", &raw["total"]);
    }
    auto cellOf = [&](const ojson& block, const std::string& v, const std::string& st) {
      if (block.contains(v) && block[v].contains(st)) return jsonNum(block[v][st]);
      return std::numeric_limits<double>::quiet_NaN();
    };
    // the esttab-facing pseudo coefficients: one per (group, variable)
    std::vector<std::string> colnames;
    std::map<std::string, std::vector<double>> byStat;
    for (const auto& [gname, block] : groups) {
      for (const auto& v : vars) {
        colnames.push_back(gname.empty() ? v : (gname + ":" + v));
        for (const auto& st : statNames) byStat[st].push_back(cellOf(*block, v, st));
      }
    }
    // Stata's names for the statistics (estpost.ado: tabstat's `N` is posted as
    // `count`, `se(mean)` as `semean`).
    std::vector<std::string> stataStats;
    for (const auto& st : statNames) {
      const std::string l = strutil::lower(st);
      stataStats.push_back(l == "n" || l == "count" ? std::string("count")
                           : (l == "semean" || l == "se(mean)") ? std::string("semean")
                                                                : st);
    }
    // columns(): statistics for one variable, variables otherwise, unless given.
    bool colsStatistics = vars.size() <= 1;
    if (colsOpt.has_value()) colsStatistics = !byVariables;
    // estpost.ado: per group the inner names go to the column name when the
    // other dimension has one entry, else the group is the column equation.
    auto colName = [&](const std::string& gname, const std::string& inner, bool innerSingle) {
      if (gname.empty()) return inner;
      return innerSingle ? gname : gname + ":" + inner;
    };
    ojson posted = ojson::object();
    if (colsStatistics) {
      // columns(statistics): e(<stat>) 1 x (groups x variables), row `<stat>`
      // (StataBE `estpost tabstat price mpg weight, statistics(mean sd)
      // columns(statistics)`: e(mean) row `mean`, columns `price mpg weight`).
      for (size_t i = 0; i < statNames.size(); ++i) {
        std::vector<std::string> cn;
        std::vector<double> vals;
        for (const auto& [gname, block] : groups) {
          for (const auto& v : vars) {
            cn.push_back(colName(gname, v, vars.size() == 1));
            vals.push_back(cellOf(*block, v, statNames[i]));
          }
        }
        posted[stataStats[i]] = rowMatrix(cn, vals, stataStats[i]);
      }
    } else {
      // columns(variables): e(<var>) 1 x (groups x statistics), row `<var>`
      // (StataBE `estpost tabstat price mpg, by(foreign) statistics(mean sd)`:
      // e(price) columns `Domestic:mean Domestic:sd ... Total:sd`).
      for (const auto& v : vars) {
        std::vector<std::string> cn;
        std::vector<double> vals;
        for (const auto& [gname, block] : groups) {
          for (size_t i = 0; i < statNames.size(); ++i) {
            cn.push_back(colName(gname, stataStats[i], statNames.size() == 1));
            vals.push_back(cellOf(*block, v, statNames[i]));
          }
        }
        const std::string ename = v == "b" ? std::string("_b") : v == "V" ? std::string("_V") : v;
        posted[ename] = rowMatrix(cn, vals, v);
      }
    }
    for (size_t i = 0; i < statNames.size(); ++i) {
      matrices[statNames[i]] = rowMatrix(colnames, byStat[statNames[i]], stataStats[i]);
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < colnames.size(); ++j) {
        coefs.push_back(PseudoCoef{colnames[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      std::string statList;
      for (const auto& st : stataStats) statList += (statList.empty() ? "" : " ") + st;
      std::vector<std::pair<std::string, std::string>> tlocals = {
          {"stats", statList},
          {"vars", joinWith(vars, " ")},
          {"labels", eLabels},
          {"byvar", byOpt.has_value() ? dequote(*byOpt) : std::string()}};
      if (weightFlag != nullptr) {
        const std::string wf(weightFlag);
        tlocals.emplace_back("wtype", wf == "--aweight" ? "aweight" : "fweight");
        tlocals.emplace_back("wexp", "= " + strutil::trim(parsed.weightExp.value_or(weightVar.value_or(""))));
      }
      postPseudoEstimate(session, "estpost_tabstat", nObs, posted, coefs, tlocals);
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
