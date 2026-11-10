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

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// `pca varlist [, components() mineigen() covariance correlation]`
ojson runPca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("pca")) return engineTooOld("pca", "pca");
  if (parsed.varlist.size() < 2) return aerErrorReason("pca: too few variables", 102);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  bool cov = false, corr = false;
  std::string comp, mineigen;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "covariance") cov = true;
    else if (k == "correlation") corr = true;
    else if (k == "components" || k == "comp" || k == "factors") comp = v.strValue;
    else if (k == "mineigen") mineigen = v.strValue;
  }
  if (cov && corr) return aerErrorReason("pca: options correlation and covariance are exclusive", 198);

  std::string vars;
  for (std::size_t i = 0; i < parsed.varlist.size(); ++i)
    vars += (i ? "," : "") + parsed.varlist[i];
  std::vector<std::string> args = {"pca", "--data", *session.currentHbin(), "--x", vars};
  if (cov) args.push_back("--covariance");
  if (!comp.empty()) { args.push_back("--comp"); args.push_back(comp); }
  if (!mineigen.empty()) { args.push_back("--mineigen"); args.push_back(mineigen); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
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
  // produces once the relative-1e-8 zeroing has run.
  if (raw.contains("lndet") && raw["lndet"].is_number())
    session.eval.setEScalar("lndet", raw["lndet"].get<double>());
  if (raw.contains("cond") && raw["cond"].is_number())
    session.eval.setEScalar("cond", raw["cond"].get<double>());

  session.eval.setEString("cmd", "pca");
  session.eval.setEString("Ctype", ctype);
  session.eval.setEString("title", "Principal components");
  session.eval.setEString("properties", "nob noV eigen");
  session.eval.setEString("predict", "pca_p");
  session.eval.setEString("marginsnotok", "_ALL");

  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
    session.noteEOnFit(nm);
  };
  auto rowvec = [&](const std::vector<double>& v, std::vector<std::string> cn) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {"r1"};
    m.colnames = std::move(cn);
    return m;
  };
  std::vector<std::string> compNames;
  for (int j = 0; j < p; ++j) compNames.push_back("Comp" + std::to_string(j + 1));
  postM("Ev", rowvec(ev, compNames));

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
    postM("Psi", rowvec(jsonDoubles(raw["psi"]), parsed.varlist));
  if (raw.contains("means"))
    postM("means", rowvec(jsonDoubles(raw["means"]), parsed.varlist));
  // e(sds) exists ONLY for correlation PCA -- after a covariance run,
  // `confirm matrix e(sds)` is rc 111.
  if (raw.contains("sds"))
    postM("sds", rowvec(jsonDoubles(raw["sds"]), parsed.varlist));
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
  return displayBlock(out.str());
}

}  // namespace piet::handlers
