// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// pca: principal components, e() posting and the eigenvalue/eigenvector tables.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"
#include "postestimation/rtable_internal.h"  // rtable_detail::postDefaultCmdline

namespace piet::handlers {

// Shared fit-sample filter (linear_estimation.cpp); keeps each row's position.
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
                                                   const std::optional<std::string>&,
                                                   const std::optional<std::string>&,
                                                   std::string*);

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

namespace fit_detail {
// estimation/estimation_results.cpp: Stata's ordered `: e(scalars)` / `: e(macros)` /
// `: e(matrices)` lists for the live fit (SessionState::ENameContract); nullopt
// leaves a kind unclaimed.
void installENameContractNL(SessionState& session, const std::string& cmd,
                            std::optional<std::vector<std::string>> scalars,
                            std::optional<std::vector<std::string>> macros,
                            std::optional<std::vector<std::string>> matrices,
                            std::vector<std::string> missingScalars);
}  // namespace fit_detail

namespace {
// pca.ado + pcamat.ado's options: CORrelation COVariance MEans BLanks() Level()
// noVCE VCE() and pcamat's COMponents() FActors() MINEigen(). Registered so the
// dispatcher canonicalizes an abbreviation (`com(2)`, `cov`, `mine(.7)`) to the
// name this handler reads: unregistered, the abbreviation was accepted and then
// missed, and pca ran with every component, on the correlation matrix.
const eval::OptVocab& kVocabPca = eval::registerOptVocab(
    {"pca"}, {{"components", "factors", "mineigen", "covariance", "correlation", "means", "blanks",
               "level", "vce"},
              {}});
}  // namespace

// `pca varlist [, components() mineigen() covariance correlation]`
ojson runPca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("pca")) return engineTooOld("pca", "pca");
  if (parsed.varlist.size() < 2) return aerErrorReason("pca: too few variables", 102);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  bool cov = false, corr = false, means = false;
  std::string comp, mineigen;
  (void)kVocabPca;  // registration only: the dispatcher canonicalizes the keys
  for (const auto& [k, v] : parsed.opts) {
    if (k == "covariance") cov = true;
    else if (k == "correlation") corr = true;
    else if (k == "components" || k == "comp" || k == "factors") comp = v.strValue;
    else if (k == "mineigen") mineigen = v.strValue;
    else if (k == "means" && v.isBool && v.boolValue) means = true;
    // vce(normal): the standard errors of the eigenvalues and vectors under
    // multivariate normality (e(V), e(b), the chi2 tests) -- not computed here, so
    // refused rather than accepted with none of them (vce(none) is the default).
    else if (k == "vce" && !(strutil::lower(strutil::trim(v.strValue)) == "none"))
      return aerErrorReason("pca: vce(" + strutil::trim(v.strValue) +
                                ") is not implemented in this version of PIET (only vce(none)).",
                            198);
  }
  if (cov && corr) return aerErrorReason("pca: options correlation and covariance are exclusive", 198);
  // [aweight=] / [fweight=] (registry: iw/pw are r(101)): pca.ado's weighted
  // `matrix accum` (vs-stata bank fff95685 aweight, c87e43c3 fweight e(N) 1018)
  std::string wtype;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && parsed.weightType.has_value()) {
    const std::string t = strutil::lower(*parsed.weightType);
    wtype = (t == "aw" || t == "aweight") ? "aweight" : (t == "fw" || t == "fweight") ? "fweight" : "";
    if (wtype.empty()) return aerErrorReason("weights not allowed", 101);
    // `means` prints estat summarize's weighted table: not drawn here
    if (means)
      return aerErrorReason("pca: option means with weights is not implemented in this version of PIET.",
                            198);
  }

  std::string vars;
  for (std::size_t i = 0; i < parsed.varlist.size(); ++i)
    vars += (i ? "," : "") + parsed.varlist[i];
  std::vector<std::string> args = {"pca", "--data", *session.currentHbin(), "--x", vars};
  if (cov) args.push_back("--covariance");
  if (!comp.empty()) { args.push_back("--comp"); args.push_back(comp); }
  if (!mineigen.empty()) { args.push_back("--mineigen"); args.push_back(mineigen); }
  if (!wtype.empty()) {
    args.push_back("--weight");
    args.push_back(*parsed.weightVar);
    args.push_back("--wtype");
    args.push_back(wtype == "aweight" ? "aw" : "fw");
  }
  // `if`: pca.ado's marksample. It was not forwarded, so `pca ... if foreign==0`
  // analysed all 74 cars.
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }
  const std::vector<std::string> fitArgv = args;  // captureFitFilterM4 reads the fit's own flags
  // `in`: the fit reads the if/in copy, which keeps each row's position in the
  // working frame, and the engine scatters e(sample) back through it (mean's and
  // regress's install).
  std::string esampleIndexVar;
  int64_t esampleHeadRows = 0;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    InRangeFilterResult filt;
    try {
      filt = applyIfAndInFilterKeepingIndex(session, *session.currentHbin(), parsed.ifExpr,
                                            parsed.inRange, &esampleIndexVar);
    } catch (const CobraError& ce) {
      return aerError("pca: " + cobraErrorMessage(ce), ce.code);
    }
    if (!filt.ok) return inRangeRejectError(filt, *parsed.inRange, "pca");
    args = {"pca", "--data", filt.dataPath, "--x", vars};
    for (std::size_t i = 5; i < fitArgv.size(); ++i) {
      if (fitArgv[i] == "--keepif") { ++i; continue; }
      args.push_back(fitArgv[i]);
    }
    if (!esampleIndexVar.empty()) esampleHeadRows = session.macroObsCount();
  }
  // e(sample): pca.ado's `ereturn repost, esample(touse)`; installed below.
  const std::string esampleValues = esampleFitArgs(session, args, esampleIndexVar, esampleHeadRows);

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    // a negative weight / a fractional fweight: Stata's r(402) / r(401)
    if (ce.detail == "r(402)") return aerErrorReason("negative weights encountered", 402);
    if (ce.detail == "r(401)") return aerErrorReason("may not use noninteger frequency weights", 401);
    return aerError("pca: " + cobraErrorMessage(ce), ce.code);
  }

  const int p = raw.value("p", 0);
  const int f = raw.value("f", 0);
  const std::vector<double> ev = jsonDoubles(raw["eigenvalues"]);
  const std::string ctype = raw.value("ctype", std::string("correlation"));

  // Install the fit before publishing its e(): setLastModel supersedes any
  // prior explicit posting, and must not erase the PCA matrices just written.
  LastModel lm;
  lm.cobraCmd = "pca";
  lm.vcov = "none";
  lm.eVce = std::string();  // pca posts no e(vce) (pcamat.ado: only under vce(normal))
  est_detail::captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  {
    ojson rawO = ojson::parse(raw.dump());
    (void)esampleInstallOnHead(session, esampleValues, rawO, lm);  // e(sample)
  }
  raw.erase("esample_n");  // the handshake key, not an e() result
  session.setLastModel(lm);
  session.eval.eScalars.clear();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();

  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("f", static_cast<double>(f));
  session.eval.setEScalar("rho", raw.value("rho", 0.0));
  if (raw.contains("trace") && raw["trace"].is_number())
    session.eval.setEScalar("trace", raw["trace"].get<double>());
  // A null lndet/cond is Stata's MISSING, which is what the collinear case
  // produces once the relative-1e-8 zeroing has run: pcamat.ado still posts it.
  if (raw.contains("lndet"))
    session.eval.setEScalar("lndet", raw["lndet"].is_number()
                                         ? raw["lndet"].get<double>()
                                         : std::numeric_limits<double>::quiet_NaN());
  if (raw.contains("cond"))
    session.eval.setEScalar("cond", raw["cond"].is_number()
                                        ? raw["cond"].get<double>()
                                        : std::numeric_limits<double>::quiet_NaN());

  session.eval.setEString("cmd", "pca");
  session.eval.setEString("Ctype", ctype);
  session.eval.setEString("title", "Principal components");
  session.eval.setEString("properties", "nob noV eigen");
  session.eval.setEString("predict", "pca_p");
  session.eval.setEString("rotate_cmd", "pca_rotate");
  session.eval.setEString("estat_cmd", "pca_estat");
  session.eval.setEString("marginsnotok", "_ALL");
  if (!wtype.empty()) {
    session.eval.setEString("wtype", wtype);
    session.eval.setEString("wexp", "= " + *parsed.weightVar);
  }
  // e(cmdline): pca.ado's `ereturn local cmdline "pca `0'"'.
  rtable_detail::postDefaultCmdline(session);

  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
    session.noteEOnFit(nm);
  };
  // The row names pca.ado / pcamat.ado give them: e(means) `mean`, e(sds) `sd`,
  // e(Psi) `Unexplained`; e(Ev) keeps the default r1 under the column equation
  // `Eigenvalues` (`matrix coleq Ev = Eigenvalues`).
  auto rowvec = [&](const std::vector<double>& v, std::vector<std::string> cn,
                    const char* rowname) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {rowname};
    m.colnames = std::move(cn);
    return m;
  };
  std::vector<std::string> compNames, evNames;
  for (int j = 0; j < p; ++j) {
    compNames.push_back("Comp" + std::to_string(j + 1));
    evNames.push_back("Eigenvalues:Comp" + std::to_string(j + 1));
  }
  postM("Ev", rowvec(ev, evNames, "r1"));

  if (f > 0 && raw.contains("loadings")) {
    const std::vector<double> L = jsonDoubles(raw["loadings"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < f; ++j) row.push_back(L[static_cast<std::size_t>(i * f + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    for (int j = 0; j < f; ++j) m.colnames.push_back("Comp" + std::to_string(j + 1));
    postM("L", std::move(m));
  }
  if (raw.contains("psi"))
    postM("Psi", rowvec(jsonDoubles(raw["psi"]), parsed.varlist, "Unexplained"));
  if (raw.contains("means"))
    postM("means", rowvec(jsonDoubles(raw["means"]), parsed.varlist, "mean"));
  // e(sds) exists ONLY for correlation PCA -- after a covariance run,
  // `confirm matrix e(sds)` is rc 111.
  if (raw.contains("sds"))
    postM("sds", rowvec(jsonDoubles(raw["sds"]), parsed.varlist, "sd"));
  if (raw.contains("c")) {
    const std::vector<double> C = jsonDoubles(raw["c"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < p; ++j) row.push_back(C[static_cast<std::size_t>(i * p + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    m.colnames = parsed.varlist;
    postM("C", std::move(m));
  }
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < 4; ++j) row.push_back(T4[static_cast<std::size_t>(i * 4 + j)]);
      m.data.push_back(row);
      m.rownames.push_back("Comp" + std::to_string(i + 1));
    }
    m.colnames = {"Eigenvalue", "Difference", "Proportion", "Cumulative"};
    postM("components", std::move(m));
  }

  // The complete actual matrix namespace is listed in Stata publication
  // order, not in engine-JSON key order (w2v_estpost_ematrix ep15c).
  const std::vector<std::string> matrixOrder{"sds","means","components","C","Psi","Ev","L"};
  std::stable_sort(session.eval.eMatrices.begin(),session.eval.eMatrices.end(),
      [&](const auto& a, const auto& b) {
        return std::find(matrixOrder.begin(),matrixOrder.end(),a.first) <
               std::find(matrixOrder.begin(),matrixOrder.end(),b.first);
      });
  for (const auto& v : session.eval.eScalars.items()) session.noteEOnFit(v.first);
  for (const auto& v : session.eval.eStrings.items()) session.noteEOnFit(v.first);
  // `: e(scalars)` / `: e(macros)`: pcamat.ado's postings, in Stata's store order
  // (macros newest first; pca.ado adds e(cmdline) last). Only for a fit that
  // retained components: the f == 0 posting is a different, shorter one.
  // `means`: pca_display's summary table is `estat summarize` over e(sample),
  // which returns r(stats) -- one row per analysed variable (e(C)'s names), the
  // columns mean sd min max. (Stata's r() is otherwise empty after pca.)
  if (means && raw.contains("means") && raw.contains("vars") && raw.contains("mins") &&
      raw.contains("maxs")) {
    const std::vector<double> mu = jsonDoubles(raw["means"]), var = jsonDoubles(raw["vars"]),
                              lo = jsonDoubles(raw["mins"]), hi = jsonDoubles(raw["maxs"]);
    std::vector<std::string> analysed;
    for (const auto& v : parsed.varlist) {
      bool dropped = false;
      if (raw.contains("dropped") && raw["dropped"].is_array())
        for (const auto& d : raw["dropped"])
          if (d.is_string() && d.get<std::string>() == v) dropped = true;
      if (!dropped) analysed.push_back(v);
    }
    if (analysed.size() == mu.size() && mu.size() == var.size() && mu.size() == lo.size() &&
        mu.size() == hi.size()) {
      eval::EvalMatrix st;
      st.colnames = {"mean", "sd", "min", "max"};
      for (std::size_t j = 0; j < mu.size(); ++j) {
        st.rownames.push_back(analysed[j]);
        st.data.push_back({mu[j], std::sqrt(var[j]), lo[j], hi[j]});
      }
      eval::replaceRClass(session.eval);
      session.eval.rMatrices.emplace_back("stats", std::move(st));
      eval::declareRPosted(session.eval, "pca", {"stats"});
    }
  }

  if (f > 0)
    fit_detail::installENameContractNL(
        session, "pca", std::vector<std::string>{"N", "f", "rho", "trace", "lndet", "cond"},
        wtype.empty()
            ? std::vector<std::string>{"cmdline", "cmd", "title", "marginsnotok", "estat_cmd",
                                       "rotate_cmd", "predict", "Ctype", "properties"}
            : std::vector<std::string>{"cmdline", "wexp", "wtype", "cmd", "title", "marginsnotok",
                                       "estat_cmd", "rotate_cmd", "predict", "Ctype", "properties"},
        std::nullopt, {});

  std::ostringstream out;
  char buf[256];
  out << "\nPrincipal components/" << ctype << "                 Number of obs    = "
      << std::setw(10) << raw.value("n_obs", 0) << "\n";
  std::snprintf(buf, sizeof(buf), "                                                 Number of comp.  = %10d\n", f);
  out << buf;
  if (raw.contains("trace") && raw["trace"].is_number()) {
    std::snprintf(buf, sizeof(buf), "                                                 Trace            = %10.10g\n",
                  raw["trace"].get<double>());
    out << buf;
  }
  std::snprintf(buf, sizeof(buf), "    Rotation: (unrotated = principal)            Rho              = %10.4f\n",
                raw.value("rho", 0.0));
  out << buf;
  out << "    --------------------------------------------------------------------------\n";
  out << "       Component |   Eigenvalue   Difference         Proportion   Cumulative\n";
  out << "    -------------+------------------------------------------------------------\n";
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    for (int i = 0; i < p; ++i) {
      const double diff = T4[static_cast<std::size_t>(i * 4 + 1)];
      std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12.6g %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                    T4[static_cast<std::size_t>(i * 4 + 0)], diff,
                    T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      if (std::isnan(diff))
        std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12s %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                      T4[static_cast<std::size_t>(i * 4 + 0)], ".",
                      T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      out << buf;
    }
  }
  out << "    --------------------------------------------------------------------------\n";
  if (raw.contains("dropped") && raw["dropped"].is_array())
    for (const auto& dv : raw["dropped"])
      out << "(" << dv.get<std::string>() << " dropped because of zero variance)\n";
  // `means`: pca_display's "Summary statistics of the variables", estat summarize's
  // table over the sample (the r(stats) posted above), numbers in %9.0g.
  if (const eval::EvalMatrix* st = means ? session.eval.findClassMatrix('r', "stats") : nullptr) {
    const auto g = [](double v) {
      if (std::isnan(v)) return std::string(".");
      char b[64];
      std::snprintf(b, sizeof b, "%.17g", v);
      std::string t = formatNumberAsDisplayed("%9.0g", b);
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      return t;
    };
    out << "\nSummary statistics of the variables\n\n";
    out << "  " << std::string(67, '-') << "\n";
    out << "      Variable |         Mean      Std. dev.         Min          Max\n";
    out << "  -------------+" << std::string(53, '-') << "\n";
    for (std::size_t i = 0; i < st->rownames.size() && i < st->data.size(); ++i) {
      const auto& r = st->data[i];
      if (r.size() < 4) continue;
      std::snprintf(buf, sizeof(buf), "  %12s | %12s %13s %12s %12s\n",
                    fitLeft(abbrevCp(st->rownames[i], 12), 12).c_str(), g(r[0]).c_str(),
                    g(r[1]).c_str(), g(r[2]).c_str(), g(r[3]).c_str());
      out << buf;
    }
    out << "  " << std::string(67, '-') << "\n";
  }
  return displayBlock(out.str());
}

}  // namespace piet::handlers
