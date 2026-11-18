// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Instrumental variables.
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/estimation.h"
#include "piet/handlers/observations.h"  // runDrop (xtivreg2 savefirst)
#include "estimation_families_internal.h"  // fit_detail::fewClustersRankM4
#include "core/distributions.h"  // t_critical / z_critical (shared with cobra)
#include "piet/handlers/vce_types.h"
#include "matrix/lapack_dsyevr.h"  // ivreg2 condition numbers and canonical correlations
#include "estimator_command_support.h"  // e3::invsym
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist (the identification count)
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation_internal.h"
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

namespace extended_fit_detail {

// ── `first` / ivreg2 `partial()` helpers (task w2b-iv-options) ──────────────

// Stata's coefficient-table name column: 12 wide, right-aligned, long names
// abbreviated with `~` the way the regress family does.
std::string coefStubM4b(const std::string& name) {
  std::string n = (name == "(intercept)") ? std::string("_cons") : name;
  // Widths count CHARACTERS (StataBE, scratch-r150/uni-v/p4.do): a name past 12
  // keeps its first 10 characters, `~` and its last character, never splitting a
  // UTF-8 sequence.
  const size_t cps = cecon::ident::codePointCount(n);
  if (cps > 12) {
    size_t last = n.size();
    do {
      --last;
    } while (last > 0 && (static_cast<unsigned char>(n[last]) & 0xC0) == 0x80);
    n = cecon::ident::truncateCodePoints(n, 10) + "~" + n.substr(last);
    return n;  // exactly 12 characters
  }
  return std::string(12 - cps, ' ') + n;
}

std::string padLeftM4b(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

double numberAtM4b(const ojson& obj, const char* key, const std::string& name) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_object()) return std::nan("");
  const ojson& m = obj[key];
  if (!m.contains(name) || !m[name].is_number()) return std::nan("");
  return m[name].get<double>();
}

// One first stage's coefficient table, 78 columns, from the engine's own
// `--savefirst` estimate (apps/cmds/common.cpp add_first_stage: b, se, t, p and
// the CI off df_r): the cluster note, the rules and one row per name in `names`
// order. Shared by firstStageTextM4b and ivregress's report (ivregressFirstHeaderM4b).
std::string firstStageTableM4b(const ojson& fs, const std::vector<std::string>& names,
                               const std::string& vcov, const std::string& clustvar) {
  const std::string depvar = fs.value("depvar", std::string());
  std::string out;
  char buf[160];
  const bool robust = vcov != "iid" && !vcov.empty();
  if (vcov == "cluster" && fs.contains("n_clusters") && fs["n_clusters"].is_number()) {
    const std::string adj = "(Std. err. adjusted for " +
                            std::to_string(static_cast<long long>(fs["n_clusters"].get<double>())) +
                            " clusters in " + clustvar + ")";
    out += padLeftM4b(adj, 78) + "\n";
  }
  out += std::string(78, '-') + "\n";
  if (robust) out += "             |               Robust\n";
  out += coefStubM4b(depvar) + " | Coefficient  " + (robust ? "std. err." : "Std. err.") +
         "      t    P>|t|     [95% conf. interval]\n";
  out += std::string(13, '-') + "+" + std::string(64, '-') + "\n";
  // The cells are the coefficient table's %9.0g (w2b_estim.log:896 `_cons |
  // 2.505772   1.106975 ... .2890918    4.722452`), not display's default, which
  // gives 2.5057717 (BUG-1548).
  auto cell = [](double v) { return fmt::general(v, 9, 0); };
  for (const auto& name : names) {
    const double b = numberAtM4b(fs, "coef", name);
    const double se = numberAtM4b(fs, "se", name);
    const double t = numberAtM4b(fs, "tstat", name);
    const double p = numberAtM4b(fs, "pvalue", name);
    const double lo = numberAtM4b(fs, "ci_lo", name);
    const double hi = numberAtM4b(fs, "ci_hi", name);
    std::string row = coefStubM4b(name) + " |";
    if (!std::isfinite(b) || !std::isfinite(se)) {
      row += padLeftM4b("(omitted)", 11);
    } else {
      row += padLeftM4b(cell(b), 11) + padLeftM4b(cell(se), 11);
      std::snprintf(buf, sizeof(buf), "%9.2f%8.3f", t, p);
      row += buf;
      row += padLeftM4b(cell(lo), 13) + padLeftM4b(cell(hi), 12);
    }
    out += row + "\n";
  }
  out += std::string(78, '-');
  return out;
}

// One first stage as ivreghdfe/ivreg2's `first` prints it here: a title, the
// stage's Number of obs and Root MSE, then its table (firstStageTableM4b). Only
// what the engine's estimate carries is printed. `names` is the row order.
std::string firstStageTextM4b(const ojson& fs, const std::vector<std::string>& names,
                              const std::string& vcov, const std::string& clustvar) {
  const std::string depvar = fs.value("depvar", std::string());
  std::string out = "first-stage regression of " + depvar + " (option first)\n";
  char buf[160];
  if (fs.contains("n_obs") && fs["n_obs"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6lld\n", "", "Number of obs",
                  static_cast<long long>(fs["n_obs"].get<double>()));
    out += buf;
  }
  if (fs.contains("rmse") && fs["rmse"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6.4f\n", "", "Root MSE", fs["rmse"].get<double>());
    out += buf;
  }
  out += "\n";
  return out + firstStageTableM4b(fs, names, vcov, clustvar);
}

// The header ivregress prints above each first-stage table (ivregress's `first` report):
// Number of obs, F(df_m,
// df_r), Prob > F, R-squared, Adj R-squared, Root MSE (StataBE 19.5, BUG-1227;
// w2b_estim.log:883-890 `ivregress 2sls y x2 (x1 = z1), first`: F(2, 57) 4.11, Prob >
// F 0.0215, R-squared 0.1261, Adj R-squared 0.0954, Root MSE 3.0236). From the
// engine's stage: n_obs, df_r (N - K), rss, rmse and e_V; `tss` is the stage depvar's
// sum of squares over e(sample), centred when the stage has a constant and raw under
// noconstant, as regress takes it (NaN when not known: the two R-squared lines then
// read `.`; `ivregress ..., first noconstant` R-squared .7476, Adj R-squared .7389 =
// 1 - (1 - R2) N / df_r). F is the Wald test of every slope with the stage's own VCE
// (ivregress's first-stage `test`), which under the default VCE is regress's
// model F; Prob > F is Ftail(q, df_r, F), df_r staying N - K under cluster().
std::string ivregressFirstHeaderM4b(const ojson& fs, double tss, bool hasCons) {
  auto num = [&fs](const char* k) {
    return fs.contains(k) && fs[k].is_number() ? fs[k].get<double>() : std::nan("");
  };
  const double n = num("n_obs"), dfR = num("df_r"), rss = num("rss"), rmse = num("rmse");
  // Wald F over the slopes: b_s' V_ss^{-1} b_s / q, a slope with no positive
  // variance (omitted) left out of the test as `test` drops it.
  double F = std::nan(""), q = 0;
  if (fs.contains("e_V") && fs["e_V"].is_object() && fs["e_V"].contains("names") &&
      fs["e_V"].contains("matrix")) {
    const ojson& vn = fs["e_V"]["names"];
    const ojson& vm = fs["e_V"]["matrix"];
    std::vector<size_t> idx;
    for (size_t i = 0; i < vn.size(); ++i) {
      const std::string nm = vn[i].is_string() ? vn[i].get<std::string>() : std::string();
      if (nm.empty() || nm == "(intercept)" || nm == "_cons") continue;
      if (i < vm.size() && vm[i].is_array() && i < vm[i].size() && vm[i][i].is_number() &&
          vm[i][i].get<double>() > 0 && std::isfinite(numberAtM4b(fs, "coef", nm)))
        idx.push_back(i);
    }
    const size_t k = idx.size();
    std::vector<std::vector<double>> a(k, std::vector<double>(k + 1, 0.0));
    bool ok = k > 0;
    for (size_t r = 0; ok && r < k; ++r) {
      for (size_t c = 0; c < k; ++c) {
        const ojson& cell = vm[idx[r]][idx[c]];
        if (!cell.is_number()) ok = false;
        else a[r][c] = cell.get<double>();
      }
      a[r][k] = numberAtM4b(fs, "coef", vn[idx[r]].get<std::string>());
    }
    // Gaussian elimination with partial pivoting: x = V_ss^{-1} b_s.
    for (size_t c = 0; ok && c < k; ++c) {
      size_t piv = c;
      for (size_t r = c + 1; r < k; ++r)
        if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
      if (!(std::fabs(a[piv][c]) > 0)) {
        ok = false;
        break;
      }
      std::swap(a[c], a[piv]);
      for (size_t r = 0; r < k; ++r) {
        if (r == c) continue;
        const double f = a[r][c] / a[c][c];
        for (size_t j = c; j <= k; ++j) a[r][j] -= f * a[c][j];
      }
    }
    if (ok) {
      double w = 0;
      for (size_t r = 0; r < k; ++r)
        w += numberAtM4b(fs, "coef", vn[idx[r]].get<std::string>()) * (a[r][k] / a[r][r]);
      q = static_cast<double>(k);
      F = w / q;
    }
  }
  double p = std::nan("");
  if (std::isfinite(F) && q > 0 && std::isfinite(dfR) && dfR > 0) {
    char expr[160];
    std::snprintf(expr, sizeof(expr), "Ftail(%.17g,%.17g,%.17g)", q, dfR, F);
    if (const auto v = eval::evalArith(expr, nullptr)) {
      try {
        p = std::stod(*v);
      } catch (...) {
      }
    }
  }
  const double r2 = (std::isfinite(tss) && tss > 0 && std::isfinite(rss)) ? 1.0 - rss / tss
                                                                         : std::nan("");
  const double r2a = std::isfinite(r2) && dfR > 0
                         ? 1.0 - (1.0 - r2) * (hasCons ? n - 1.0 : n) / dfR
                         : std::nan("");
  char buf[160];
  auto line = [&buf](const std::string& label, const char* fmt, double v) {
    char val[64];
    if (std::isfinite(v)) std::snprintf(val, sizeof(val), fmt, v);
    else std::snprintf(val, sizeof(val), "%6s", ".");
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %s\n", "", label.c_str(), val);
    return std::string(buf);
  };
  std::string out;
  if (std::isfinite(n)) {
    // _coef_table_header's %#.0fc: thousands separated by commas.
    std::string digits = std::to_string(static_cast<long long>(n));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(i, ",");
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6s\n", "", "Number of obs", digits.c_str());
    out += buf;
  }
  char fl[64];
  std::snprintf(fl, sizeof(fl), "F(%.0f, %.0f)", q, dfR);
  out += line(fl, "%6.2f", F);
  out += line("Prob > F", "%6.4f", p);
  out += line("R-squared", "%6.4f", r2);
  out += line("Adj R-squared", "%6.4f", r2a);
  out += line("Root MSE", "%6.4f", rmse);
  return out;
}

// ivreg2's `ffirst` output: the summary block of the first-stage statistics, one row
// per endogenous regressor, from the engine's e(first) column (`ffirst`, rows in
// cecon::ivreg2_first_stat_rows() order: F df df_r pvalue at 3-6, SW chi2/p at 11-12,
// SW F at 7). The full first-stage tables are `first`'s, not printed here.
std::string ffirstSummaryTextM4b(const ojson& stages, const std::vector<std::string>& endog) {
  std::string out = "Summary results for first-stage regressions\n"
                    "-------------------------------------------\n\n";
  char buf[200];
  for (const auto& e : endog) {
    if (!stages.contains(e) || !stages[e].contains("ffirst") || !stages[e]["ffirst"].is_array() ||
        stages[e]["ffirst"].size() != 21)
      continue;
    const ojson& f = stages[e]["ffirst"];
    auto at = [&](size_t i) { return f[i].is_number() ? f[i].get<double>() : std::nan(""); };
    // The name column is 12 CHARACTERS wide, left-aligned (not bytes).
    std::string eStub = cecon::ident::truncateCodePoints(e, 12);
    eStub += std::string(12 - cecon::ident::codePointCount(eStub), ' ');
    std::snprintf(buf, sizeof(buf),
                  "%s | F(%3.0f,%6.0f) = %9.2f  P-val = %6.4f | SW Chi-sq(%3.0f) = %9.2f  P-val = "
                  "%6.4f | SW F(%3.0f,%6.0f) = %9.2f\n",
                  eStub.c_str(), at(4), at(5), at(3), at(6), at(8), at(11), at(12), at(8),
                  at(9), at(7));
    out += buf;
  }
  return out;
}

// ivreg2 `savefirst`: one first stage stored as `estimates store NAME, title("First-
// stage regression: ENDOG")` (ivreg2), from the engine's stage (b, se,
// e_V, N, rmse, df_r, df_m, k_eq, n_clusters; apps/cmds/common.cpp add_first_stage).
// The stage is made the active e() for the store and then superseded by the parent
// fit, as in ivreg2, which stores the stages before it posts its own results: the
// parent's `lm` is copied for the data, sample, weights and VCE (the first stage is
// `regress ENDOG EXEXOG INEXOG` under the parent's vce, so predict after `estimates
// restore` replays that regression), with a fresh fit serial. Every e() name it posts
// beyond setLastModel's own is noted on the fit, so the parent's setLastModel drops
// them again. e() as the oracle's probe lists it: cmd ivreg2, model first, depvar,
// N, rmse, df_r, df_m, k_eq, N_clust, vce `robust cluster`, vcetype Robust, clustvar.
// xtivreg2 (`xt` non-null) re-stores each stage with e(cmd) xtivreg2, the panel
// scalars and an EMPTY title (xtivreg2: the stage is restored, re-posted
// and stored with title(`e(_estimates_title)'), which a restore does not set;
// w2d_uncovered_xtivreg2.log:2714 `st1x1 | xtivreg2  x1  3` with no title).
// Returns the error block of a failed store, or nullopt.
struct XtivPanelM4b {
  double N_g = 0, g_min = 0, g_avg = 0, g_max = 0;
  bool clustered = false;
  std::string ivar, tvar;
};
std::optional<ojson> storeIvreg2FirstStageM4b(const ojson& fs, const LastModel& parent,
                                              SessionState& session,
                                              const XtivPanelM4b* xt = nullptr) {
  const std::string endog = fs.value("depvar", std::string());
  const std::string name = fs.value("eqname", std::string());
  if (endog.empty() || name.empty() || !fs.contains("names") || !fs["names"].is_array())
    return engineErrorBlockM4("savefirst: the engine's first stage is incomplete", "engine_error");
  LastModel f = parent;
  f.fitSerial = LastModel().fitSerial;
  f.cobraCmd = "feols";
  f.stataCmd = xt != nullptr ? "xtivreg2" : "ivreg2";
  f.y = endog;
  f.depvar = endog;
  // The regressors as typed (factor/operator tokens intact, as every replay takes
  // them): excluded instruments, then the included exogenous ones.
  f.x = parent.ivInstruments;
  for (const auto& x : parent.ivExog) f.x.push_back(x);
  f.ivEndog.clear();
  f.ivExog.clear();
  f.ivInstruments.clear();
  f.fe.clear();
  f.eVce.reset();
  ojson ct = ojson::array();
  for (const auto& n : fs["names"]) {
    if (!n.is_string()) continue;
    const std::string nm = n.get<std::string>();
    auto num = [&](const char* key) -> ojson {
      return fs.contains(key) && fs[key].is_object() && fs[key].contains(nm) ? fs[key][nm]
                                                                             : ojson(nullptr);
    };
    ct.push_back(ojson{{"name", nm}, {"b", num("coef")}, {"se", num("se")}, {"t", num("tstat")},
                       {"p", num("pvalue")}, {"ci_lo", num("ci_lo")}, {"ci_hi", num("ci_hi")}});
  }
  ojson raw = fs;
  raw["coeftable"] = ct;
  raw["estimator"] = "ivreg2";
  raw.erase("ffirst");
  raw.erase("rss");  // ivreg2's stored first stage posts no e(rss)
  f.result = raw;
  ojson block = cobraToEstimationM4(raw, "feols", &session);
  session.setLastModel(f);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);
  auto put = [&](const std::string& nm, const char* key) {
    if (fs.contains(key) && fs[key].is_number()) {
      session.eval.setEScalar(nm, fs[key].get<double>());
      session.noteEOnFit(nm);
    }
  };
  put("N", "n_obs");
  put("rmse", "rmse");
  put("df_r", "df_r");
  put("df_m", "df_m");
  put("k_eq", "k_eq");
  auto putS = [&](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  putS("model", "first");
  if (xt != nullptr) {
    session.eval.setEString("cmd", "xtivreg2");
    auto putD = [&](const std::string& nm, double v) {
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    const double dfb = fs.contains("df_m") && fs["df_m"].is_number() ? fs["df_m"].get<double>()
                                                                     : std::nan("");
    putD("N_g", xt->N_g);
    putD("df_a", xt->N_g);
    putD("df_b", dfb);
    // :519-524: FEs with cluster use up no degrees of freedom.
    putD("df_m", xt->clustered ? dfb : xt->N_g + dfb);
    if (fs.contains("rmse") && fs["rmse"].is_number()) putD("sigma_e", fs["rmse"].get<double>());
    putD("g_min", xt->g_min);
    putD("g_avg", xt->g_avg);
    putD("g_max", xt->g_max);
    putS("xtmodel", "fe");
    putS("predict", "xtivreg2_p");
    putS("ivar", xt->ivar);
    putS("tvar", xt->tvar);
  }
  if (parent.vcov == "cluster" && !parent.cluster.empty()) {
    put("N_clust", "n_clusters");
    putS("vce", "robust cluster");
    putS("vcetype", "Robust");
    putS("clustvar", parent.cluster.front());
  } else if (parent.vcov == "hc1") {
    putS("vce", "robust");
    putS("vcetype", "Robust");
  }
  const ojson stored = runEststo(
      name, xt != nullptr ? std::string() : std::string("First-stage regression: ") + endog, session,
      "", false);
  if (stored.is_object() && stored.value("type", std::string()) == "error") return stored;
  // xtivreg2 fits (and stores the stages) on its within-transformed copy of the
  // data, inside `preserve`: the store's `_est_NAME` sample variable goes with that
  // copy, so none is left on the user's data (bank f34c2bf0 `xtivreg2 ..., fe
  // savefirst`: c(k) 37, the 37 the data had before; ivreg2's stages, stored on the
  // data itself, keep theirs).
  if (xt != nullptr) {
    const auto it = session.estimatesModels.find(name);
    if (it != session.estimatesModels.end() && it->second && it->second->esampleVar) {
      auto sm = std::make_shared<SessionState::StoredModel>(*it->second);
      sm->esampleVar = false;
      session.estimatesModelSet(name, std::move(sm));
      (void)runDrop({"_est_" + name}, session);
    }
  }
  return std::nullopt;
}

// Drops `drop` from every per-coefficient map of an iv reply and from its
// coeftable. Used by ivreg2's partial(): by Frisch-Waugh-Lovell the remaining
// coefficients and their standard errors are the full fit's (w2b_estim.log:1013
// ivreg2_partial .1611032742759026 against :1060 ivreg2_ctl .1611032742759016),
// and ivreg2 counts the partialled-out variables in K for every small-sample
// adjustment (its own footnote, :1004-1006), so the full fit's dof are already
// the right ones.
void dropCoefsM4b(ojson& r, const std::set<std::string>& drop) {
  for (const char* key : {"coef", "se", "tstat", "pvalue", "ci_lo", "ci_hi"}) {
    if (!r.contains(key) || !r[key].is_object()) continue;
    for (const auto& name : drop) r[key].erase(name);
  }
  if (r.contains("coeftable") && r["coeftable"].is_array()) {
    ojson kept = ojson::array();
    for (const auto& row : r["coeftable"]) {
      if (row.is_object() && drop.count(row.value("name", std::string()))) continue;
      kept.push_back(row);
    }
    r["coeftable"] = kept;
  }
}

}  // namespace

// Stata's ordered e() name lists after ivregress 2sls, multiway (estimation_results.cpp).
void postENameListsIvregressM4(SessionState& session, const ojson& raw, size_t kEndog);
void installIvregENamesM4(SessionState& session, std::vector<std::string> scalars,
                          std::vector<std::string> macros, std::vector<std::string> missing);

namespace {
// Stata's `ivreg` e() (ivreg.ado: `_regress y endog exog (exog excluded)`, then the
// ado's own macros). The small-sample statistics are regress's: e(df_r) N-K (G-1
// under cluster()), e(rmse) sqrt(RSS/(N-K)), e(F) the Wald F of the slopes with
// the small-sample V, e(mss) = TSS - RSS (negative when the IV fit is worse than
// the mean). Measured (bank af8d994e base, fbf39cf3 robust, eb7250c9 cluster(rep78),
// 4b1e00b3 noconstant): with a constant and e(mss) < 0, e(r2) and e(r2_a) are
// missing and e(F) is posted; under noconstant e(F), e(r2) and e(r2_a) are all
// missing, e(mss) the uncentered model sum of squares. With a constant and e(mss) >= 0
// they are regress's (bank 45a574f9 `... if foreign==0`: e(r2) .012393748433458418 =
// mss/TSS, e(r2_a) 1 - (1-r2)(N-1)/(N-K) = -.0279167), under cluster() too: regress's
// r2_a keeps N-K there (bank b8ed9d5d `regress ..., vce(cluster foreign)`: r2_a
// .27348459, the unclustered value). With fewer clusters than coefficients (G-1 < K)
// _regress's rule holds: e(df_m) is the clustered V's rank less the constant and
// e(F) is missing (fit_detail::fewClustersRankM4; bank 5aa7ac2b, b8ed9d5d).
void postIvregEResultsM4b(SessionState& session, const ojson& raw, const LastModel& lm,
                          const IvParenResult& iv, bool noConstant,
                          const eval::ParsedCommand& parsed) {
  if (!raw.is_object() || parsed.weightType.has_value() ||
      lm.esample != LastModel::EsampleState::kColumn)
    return;
  const auto num = [&raw](const char* k) -> std::optional<double> {
    if (!raw.contains(k) || !raw[k].is_number()) return std::nullopt;
    return raw[k].get<double>();
  };
  const auto N = num("n_obs"), rmseA = num("rmse"), dofR = num("dof_resid"), dfR = num("df_r"),
             chi2 = num("chi2"), chi2df = num("chi2_df");
  if (!N || !rmseA || !dofR || !dfR || !(*dofR > 0)) return;
  // TSS over e(sample): centred with the constant, uncentred without.
  std::optional<double> tss;
  try {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(lm.depvar) && ds->dtype(lm.depvar) == "f64" &&
        ds->has(kEsampleColumn)) {
      const double* yv = ds->col(lm.depvar).f64();
      const double* mk = ds->col(kEsampleColumn).f64();
      const double serial = static_cast<double>(lm.esampleSerial);
      double n = 0, sum = 0;
      for (int64_t i = 0; i < ds->nRows(); ++i)
        if (mk[i] == serial && !std::isnan(yv[i])) {
          ++n;
          sum += yv[i];
        }
      if (n == *N && n > 0) {
        const double ybar = noConstant ? 0.0 : sum / n;
        double t = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(yv[i])) t += (yv[i] - ybar) * (yv[i] - ybar);
        tss = t;
      }
    }
  } catch (const std::exception&) {
    return;
  }
  if (!tss) return;
  const double rss = (*rmseA) * (*rmseA) * (*N);
  const double mss = *tss - rss;
  const bool fitR2 = !noConstant && mss >= 0 && *tss > 0;
  const double kRegs = static_cast<double>(iv.endog.size() + iv.exog.size());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double r2 = fitR2 ? mss / *tss : nan;
  const double r2a = fitR2 ? 1.0 - (1.0 - r2) * (*N - 1.0) / *dofR : nan;
  double F = (!noConstant && chi2 && chi2df && *chi2df > 0) ? *chi2 / *chi2df : nan;
  double dfm = kRegs;
  bool fewClusters = false;
  if (lm.vcov == "cluster") {
    const auto g = num("n_clusters");
    if (!g) return;
    if (const auto rk = fit_detail::fewClustersRankM4(raw, *g, kRegs + (noConstant ? 0.0 : 1.0))) {
      fewClusters = true;
      dfm = std::max(0.0, *rk - (noConstant ? 0.0 : 1.0));
      F = nan;
    }
  }
  if (!noConstant && !fewClusters && std::isnan(F)) return;
  auto put = [&session](const std::string& nm, double v) {
    session.eval.setEScalar(nm, v);
    session.noteEOnFit(nm);
  };
  for (const char* gone : {"tss", "ll", "ll_0", "rank"}) session.eval.eScalars.erase(gone);
  const bool clustered = lm.vcov == "cluster";
  std::vector<std::string> scalars;
  if (clustered) scalars.push_back("N_clust");
  for (const char* nm : {"N", "df_m", "df_r", "F", "r2", "rmse", "mss", "rss", "r2_a"})
    scalars.push_back(nm);
  if (clustered) {
    const auto g = num("n_clusters");
    if (!g) return;
    put("N_clust", *g);
  }
  put("N", *N);
  put("df_m", dfm);
  put("df_r", *dfR);
  put("F", F);
  put("r2", r2);
  put("rmse", std::sqrt(rss / *dofR));
  put("mss", mss);
  put("rss", rss);
  put("r2_a", r2a);
  // The ado's macros (ivreg.ado), e(vce)/e(vcetype) only under robust/cluster.
  std::string insts;
  for (const auto& x : iv.exog) insts += (insts.empty() ? "" : " ") + x;
  for (const auto& z : iv.instruments) insts += (insts.empty() ? "" : " ") + z;
  std::string instd;
  for (const auto& e : iv.endog) instd += (instd.empty() ? "" : " ") + e;
  auto putS = [&session](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  putS("insts", insts);
  putS("footnote", "ivreg_footnote");
  putS("title", "Instrumental-variables (2SLS) regression");
  putS("marginsprop", "nolinearize");
  putS("instd", instd);
  putS("properties", "b V");
  putS("model", "iv");
  putS("predict", "regriv_p");
  std::vector<std::string> macros = {"cmd",   "insts",  "footnote", "title",      "marginsprop",
                                     "instd", "depvar", "properties", "model",    "predict"};
  if (lm.vcov == "hc1" || clustered) {
    putS("vcetype", "Robust");
    putS("vce", clustered ? "cluster" : "robust");
    macros.push_back("vcetype");
    macros.push_back("vce");
    if (clustered) {
      if (lm.cluster.empty()) return;
      putS("clustvar", lm.cluster.front());
      macros.push_back("clustvar");
    }
  } else {
    // Stata's default ivreg posts no e(vce): an empty entry (setEString would drop
    // the key, and the reader would then fall back to the engine's `iid`).
    session.eval.eStrings.set("vce", "");
    session.eval.eStrings.set("vcetype", "");
  }
  // ivreg.ado (version 6 code) posts no e(cmdline); the empty entry also keeps the
  // dispatcher's default one (postDefaultCmdline) from being written after this.
  session.eval.eStrings.set("cmdline", "");
  // Any other macro the session holds for this fit is listed after Stata's, so the
  // list stays complete.
  for (const auto& item : session.eval.eStrings.items())
    if (!item.second.empty() && !eval::isReservedStoreKey(item.first) &&
        std::find(macros.begin(), macros.end(), item.first) == macros.end())
      macros.push_back(item.first);
  installIvregENamesM4(session, scalars, macros, {});
}

// ivregress's own e() beside the fit's numbers (ivregress.ado's posting; bank e711c736
// `ivregress 2sls price weight (mpg = length turn)` and every other ivregress cell):
// e(estimator) the method word, e(instd) = e(endog) the instrumented regressors,
// e(exogr) the included exogenous regressors as e(b) names them (`0b.foreign
// 1.foreign` for i.foreign: d21497fc), e(insts) = e(exog) those followed by the
// excluded instruments, e(idtype) `justidentified` / `overidentified` (excluded
// instruments vs instrumented), e(footnote), e(robust_prolog), e(robust_epilog),
// e(marginsprop) `nolinearize`, e(marginsnotok) (2sls `Residuals SCores XBD D`, liml
// and gmm `Residuals SCores`), and the scalars e(k_endog), e(iterations) (0 for 2sls
// and liml, 1 for two-step gmm: bank a9d50251) and e(kappa) (1 for 2sls, liml's
// estimated kappa: bank 447495d2 1.006787; gmm posts none). The instrument macros
// (e(b)'s names are final only after the fit is installed) are posted with the rest of
// the fit's contract (fit_default_e_macros.cpp postStataEContract).
void postIvregressMacrosM4b(SessionState& session, const ojson& raw, const std::string& method,
                            const IvParenResult& iv, const eval::ParsedCommand& parsed,
                            bool noConstant) {
  auto putS = [&session](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  auto putN = [&session](const std::string& nm, double v) {
    session.eval.setEScalar(nm, v);
    session.noteEOnFit(nm);
  };
  std::string m = strutil::lower(method);
  if (m != "2sls" && m != "liml" && m != "gmm") return;
  bool gmmIterated = false;
  for (const auto& [k0, v] : parsed.opts) {
    const std::string k = strutil::lower(k0);
    // onestep / igmm (any abbreviation): not two-step, its count is not posted
    if (k.rfind("one", 0) == 0 || (!k.empty() && std::string("igmm").rfind(k, 0) == 0))
      gmmIterated = true;
  }
  putS("estimator", m);
  putS("footnote", "ivreg_footnote");
  putS("robust_prolog", "ivregress_prolog");
  putS("robust_epilog", "ivregress_epilog");
  putS("marginsprop", "nolinearize");
  putS("marginsnotok", m == "2sls" ? "Residuals SCores XBD D" : "Residuals SCores");
  putN("k_endog", static_cast<double>(iv.endog.size()));
  if (m == "2sls") {
    putN("iterations", 0.0);
    putN("kappa", 1.0);
  } else if (m == "liml") {
    putN("iterations", 0.0);
    if (raw.is_object() && raw.contains("kappa") && raw["kappa"].is_number())
      putN("kappa", raw["kappa"].get<double>());
  } else if (!gmmIterated) {
    putN("iterations", 1.0);
  }
  // e(V_modelbased) after a one-way robust/cluster 2SLS fit: (Xhat'Xhat)^-1, the same
  // under `small` (bank 4ea6b15d vce(robust): V_modelbased[1,1] .0395392 = the default
  // fit's e(V)[1,1] 1229000.4 / (rss/N); 5e3f92c1 small equal to f4e7cb42), as the
  // engine's `e_Vmb`, resolved on e(V)'s stripe when read (macro_matrices.cpp).
  if (m == "2sls" && raw.is_object() && raw.contains("e_Vmb") && raw["e_Vmb"].is_object() &&
      session.lastEstBlock.has_value() && session.lastEstBlock->is_object()) {
    (*session.lastEstBlock)["e_Vmb"] = raw["e_Vmb"];
    // e(rank_mb): its rank, the full column count of this unpenalised fit (bank
    // 4ea6b15d 3; abbf51ac noconstant 2)
    if (raw["e_Vmb"].contains("names") && raw["e_Vmb"]["names"].is_array())
      putN("rank_mb", static_cast<double>(raw["e_Vmb"]["names"].size()));
  }
  // The model test. `small`: the F form, the Wald statistic of the slopes on the
  // small-sample V over its df, and no e(chi2) (bank b093f0fc `..., small`: e(F)
  // 3.6360595, e(df_r) 71, no e(chi2)); otherwise the Wald chi2. 2sls runs that test
  // whatever the constant (bank abbf51ac `2sls ..., noconstant small vce(robust)`: e(F)
  // 252.77; 5b3a8e84 `noconstant`: e(chi2) 489.76), while liml and gmm hand
  // `noconstant` on to it and then post neither statistic. gmm's e(F) is not posted:
  // this build's gmm V is not the small-sample one, so its F would not be Stata's.
  const bool small = raw.is_object() && raw.contains("small") && raw["small"].is_boolean() &&
                     raw["small"].get<bool>();
  if ((m == "liml" || m == "gmm") && noConstant) {
    session.eval.eScalars.erase("chi2");
  } else if (small) {
    session.eval.eScalars.erase("chi2");
    if (m != "gmm" && raw.contains("chi2") && raw["chi2"].is_number() && raw.contains("chi2_df") &&
        raw["chi2_df"].is_number() && raw["chi2_df"].get<double>() > 0)
      putN("F", raw["chi2"].get<double>() / raw["chi2_df"].get<double>());
  }
}
}  // namespace

double chi2UpperTailIntegerDf(double df, double x);  // postestimation/likelihood_ratio_test.cpp

namespace {
// ivreg2's e() contract for a 2SLS fit (ivreg2, method identified from its
// documentation and the bank's stored results; bank a7117e73 iid, eacbe232 robust,
// eacd5824 cluster(rep78), 135149f2 small, f2098361 noconstant). Computed here from
// the fitted e(b) over e(sample):
//  * fit: rss, yy = y'y, yyc = centred y'y, mss = yyc - rss (yy - rss without a
//    constant), r2c = 1 - rss/yyc, r2u = 1 - rss/yy, r2 = r2c (r2u without a constant),
//    r2_a = 1 - (1-r2)(N-1)/(N-K) (N/(N-K) without a constant), ll =
//    -N/2 (1 + ln 2pi + ln(rss/N)); df_m = Fdf1 = K - cons; df_r = N - K (clusters - 1);
//  * F from the Wald chi2 of the slopes on e(V): chi2/Fdf1 (N-K)/N, cluster
//    chi2/Fdf1 (M-1)/M (N-K)/(N-1), small chi2/Fdf1; Fp = Ftail(Fdf1, df_r, F);
//  * S, the covariance of the moments Z'u/N: sigma2 Z'Z/N (sigma2 = rss/N), robust
//    sum u_i^2 z_i z_i'/N, cluster sum_g (Z_g'u_g)(Z_g'u_g)'/N; W = inv(sigma2 Z'Z/N),
//    the 2SLS weight; j = the Sargan statistic u'Pz u/sigma2 (iid; also e(sargan)) or
//    Hansen's J from the two-step efficient GMM fit on S (robust/cluster);
//  * the canonical correlations of the endogenous regressors and the excluded
//    instruments with the included ones (and the constant) partialled out: e(ccev)
//    their squares in descending order, e(cdev) = ccev/(1-ccev), e(cd) the last;
//    under iid e(idstat) = N min(ccev), e(iddf) = L - K + 1;
//  * condxx/condzz the 2-norm condition numbers of X'X and Z'Z; ranks K and L.
// Posted only when every variable is a plain numeric column, no weights, no
// partial(), one or no cluster variable. ivreg2 leaves its last `_ms_parse_parts`
// (the last excluded instrument) in r().
struct Ivreg2ContractIn {
  std::string depvar, clusterVar;
  std::vector<std::string> endog, exog, exexog;
  bool cons = true, small = false;
  bool firstStats = false;  // first, ffirst or savefirst: the AR and SW statistics
  bool first = false;       // `first`
  std::string vce;  // iid | hc1 | cluster
  // xtivreg2, fe: the data are within-transformed over `ivar` and ivreg2 runs with
  // dofminus(N_g) (a large-sample adjustment: sigma2 = rss/(N-dofminus), df_r and r2_a
  // lose dofminus, the identification statistics use N-dofminus).
  std::string ivar;
  double dofminus = 0;
  std::string version = "04.1.12";
  std::string predict = "ivreg2_p";
  // the engine's Kleibergen-Paap rk LM and Wald F of the within fit (xtivreg2 robust /
  // cluster, one endogenous regressor), rescaled to ivreg2's dofminus conventions
  double kpLm = std::nan(""), kpWaldF = std::nan("");
  // ivreghdfe: its own tags, and with absorb() the absorbed factors partialled out
  // (sdofminus = their degrees of freedom, or 1 for the constant when every one is
  // nested in a cluster variable), plus reghdfe's dof bookkeeping (dofTable rows K, M,
  // K-M, inexact, nested)
  std::string ivreg2cmd = "ivreg2";
  std::vector<std::string> fe;
  double sdofminus = 0;
  std::vector<std::vector<double>> dofTable;
  bool hdfeTags = false;  // ivreghdfe's r(mcmethod) instead of ivreg2's r(name)/r(type)
};

double symCond(const std::vector<std::vector<double>>& A) {
  const int n = static_cast<int>(A.size());
  std::vector<double> a(static_cast<size_t>(n) * n), w, z;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) a[static_cast<size_t>(j) * n + i] = A[i][j];
  if (piet::lapack::dsyevrVAL(n, std::move(a), 0.0, w, z) != 0 || w.empty() || !(w.front() > 0))
    return std::nan("");
  return w.back() / w.front();
}

bool postIvreg2ContractM4b(SessionState& session, const LastModel& lm, Ivreg2ContractIn in,
                           ojson* block) {
  using Mat = std::vector<std::vector<double>>;
  const auto rows = est_detail::esampleRowsM4(session, lm);
  if (!rows.has_value() || rows->empty()) return false;
  const auto ebA = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  const auto eVA = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  if (!ebA.matrix.has_value() || !eVA.matrix.has_value() || ebA.matrix->data.size() != 1)
    return false;
  const eval::EvalMatrix* eb = &*ebA.matrix;
  const eval::EvalMatrix* eV = &*eVA.matrix;
  const std::vector<std::string>& bn = eb->colnames;
  const size_t K = bn.size();
  // the constant: the fit's own e(b) says whether it has one
  in.cons = !bn.empty() && bn.back() == "_cons";
  // e(b): endog, then exog, then _cons
  std::vector<std::string> xNames = in.endog;
  for (const auto& x : in.exog) xNames.push_back(x);
  if (in.cons) xNames.push_back("_cons");
  if (xNames != bn || eV->data.size() != K) return false;
  std::vector<std::string> zNames = in.exexog;
  for (const auto& x : in.exog) zNames.push_back(x);
  const size_t L = zNames.size() + (in.cons ? 1 : 0);
  const size_t N = rows->size();
  if (L < K || N <= L) return false;
  auto col = [&session](const std::string& v) { return est_detail::f64ColumnM4(session, v); };
  const double* yc = col(in.depvar);
  if (yc == nullptr) return false;
  std::vector<const double*> xc, zc;
  for (size_t j = 0; j + (in.cons ? 1 : 0) < K; ++j) {
    xc.push_back(col(bn[j]));
    if (xc.back() == nullptr) return false;
  }
  for (const auto& z : zNames) {
    zc.push_back(col(z));
    if (zc.back() == nullptr) return false;
  }
  const double* cc = nullptr;
  if (in.vce == "cluster") {
    cc = col(in.clusterVar);
    if (cc == nullptr) return false;
  }
  // the data over e(sample)
  Mat X(N, std::vector<double>(K, 1.0)), Z(N, std::vector<double>(L, 1.0));
  std::vector<double> y(N);
  for (size_t i = 0; i < N; ++i) {
    const int64_t r = (*rows)[i];
    y[i] = yc[r];
    for (size_t j = 0; j < xc.size(); ++j) X[i][j] = xc[j][r];
    for (size_t j = 0; j < zc.size(); ++j) Z[i][j] = zc[j][r];
  }
  std::vector<std::string> groupVars = in.fe;
  if (!in.ivar.empty()) groupVars = {in.ivar};
  if (!groupVars.empty()) {
    // the within transform: each variable less its projection on the absorbed factors
    // over e(sample). One factor is its group means, exactly. Two or more solve the
    // normal equations D'D a = D'v of the stacked indicator design D by conjugate
    // gradients preconditioned with the group counts (alternating projections alone
    // crawl on weakly connected factors); the residual D'(v - Da) over the counts is
    // the group means of the transformed column, iterated to below 1e-14 of its scale.
    if (in.cons) return false;
    std::vector<std::vector<int32_t>> codes;  // per factor, the row's level
    std::vector<std::vector<double>> counts;  // per factor, the level's row count
    for (const auto& g : groupVars) {
      const double* ic = col(g);
      if (ic == nullptr) return false;
      std::map<double, int32_t> level;
      for (size_t i = 0; i < N; ++i) level.emplace(ic[(*rows)[i]], 0);
      int32_t nl = 0;
      for (auto& kv : level) kv.second = nl++;
      codes.emplace_back(N);
      counts.emplace_back(static_cast<size_t>(nl), 0.0);
      for (size_t i = 0; i < N; ++i) {
        const int32_t c = level.find(ic[(*rows)[i]])->second;
        codes.back()[i] = c;
        counts.back()[static_cast<size_t>(c)] += 1.0;
      }
    }
    const size_t F = codes.size();
    std::vector<double> v(N);
    // group sums of a row vector, per factor
    auto groupSums = [&](const std::vector<double>& r, std::vector<std::vector<double>>& s) {
      for (size_t f = 0; f < F; ++f) {
        s[f].assign(counts[f].size(), 0.0);
        for (size_t i = 0; i < N; ++i) s[f][static_cast<size_t>(codes[f][i])] += r[i];
      }
    };
    // the row vector D a
    auto expand = [&](const std::vector<std::vector<double>>& a, std::vector<double>& r) {
      r.assign(N, 0.0);
      for (size_t f = 0; f < F; ++f)
        for (size_t i = 0; i < N; ++i) r[i] += a[f][static_cast<size_t>(codes[f][i])];
    };
    bool converged = true;
    auto demean = [&]() {
      double scale = 0;
      for (double x : v) scale = std::max(scale, std::fabs(x));
      if (!(scale > 0)) return;
      if (F == 1) {
        std::vector<std::vector<double>> s(1);
        groupSums(v, s);
        for (size_t i = 0; i < N; ++i)
          v[i] -= s[0][static_cast<size_t>(codes[0][i])] / counts[0][static_cast<size_t>(codes[0][i])];
        return;
      }
      const double tol = 1e-14 * scale;
      std::vector<std::vector<double>> a(F), r(F), z(F), p(F), q(F);
      for (size_t f = 0; f < F; ++f) a[f].assign(counts[f].size(), 0.0);
      groupSums(v, r);  // residual of the normal equations at a = 0
      auto meanMax = [&](const std::vector<std::vector<double>>& rr) {
        double m = 0;
        for (size_t f = 0; f < F; ++f)
          for (size_t l = 0; l < rr[f].size(); ++l) m = std::max(m, std::fabs(rr[f][l] / counts[f][l]));
        return m;
      };
      double rz = 0;
      for (size_t f = 0; f < F; ++f) {
        z[f].resize(r[f].size());
        for (size_t l = 0; l < r[f].size(); ++l) {
          z[f][l] = r[f][l] / counts[f][l];
          rz += r[f][l] * z[f][l];
        }
        p[f] = z[f];
      }
      std::vector<double> dp;
      const int maxit = 100000;
      int it = 0;
      for (; it < maxit && meanMax(r) > tol && rz > 0; ++it) {
        expand(p, dp);
        groupSums(dp, q);
        double pq = 0;
        for (size_t f = 0; f < F; ++f)
          for (size_t l = 0; l < p[f].size(); ++l) pq += p[f][l] * q[f][l];
        if (!(pq > 0)) break;
        const double alpha = rz / pq;
        double rzNew = 0;
        for (size_t f = 0; f < F; ++f)
          for (size_t l = 0; l < p[f].size(); ++l) {
            a[f][l] += alpha * p[f][l];
            r[f][l] -= alpha * q[f][l];
            z[f][l] = r[f][l] / counts[f][l];
            rzNew += r[f][l] * z[f][l];
          }
        const double beta = rzNew / rz;
        rz = rzNew;
        for (size_t f = 0; f < F; ++f)
          for (size_t l = 0; l < p[f].size(); ++l) p[f][l] = z[f][l] + beta * p[f][l];
      }
      std::vector<double> da;
      expand(a, da);
      for (size_t i = 0; i < N; ++i) v[i] -= da[i];
      // the recursive residual drifts from the true one: confirm on the transformed column
      std::vector<std::vector<double>> chk(F);
      groupSums(v, chk);
      if (meanMax(chk) > 1e-10 * scale) converged = false;
    };
    for (size_t i = 0; i < N; ++i) v[i] = y[i];
    demean();
    for (size_t i = 0; i < N; ++i) y[i] = v[i];
    for (size_t j = 0; j < K; ++j) {
      for (size_t i = 0; i < N; ++i) v[i] = X[i][j];
      demean();
      for (size_t i = 0; i < N; ++i) X[i][j] = v[i];
    }
    for (size_t j = 0; j < L; ++j) {
      for (size_t i = 0; i < N; ++i) v[i] = Z[i][j];
      demean();
      for (size_t i = 0; i < N; ++i) Z[i][j] = v[i];
    }
    if (!converged) return false;
  }
  const double sdm = in.sdofminus;
  const double dm = in.dofminus;
  const std::vector<double>& b = eb->data[0];
  std::vector<double> u(N);
  double rss = 0, yy = 0, ysum = 0;
  for (size_t i = 0; i < N; ++i) {
    double xb = 0;
    for (size_t j = 0; j < K; ++j) xb += X[i][j] * b[j];
    u[i] = y[i] - xb;
    rss += u[i] * u[i];
    yy += y[i] * y[i];
    ysum += y[i];
  }
  const double Nd = static_cast<double>(N), Kd = static_cast<double>(K), Ld = static_cast<double>(L);
  double yyc = 0;
  for (size_t i = 0; i < N; ++i) yyc += (y[i] - ysum / Nd) * (y[i] - ysum / Nd);
  auto cross = [N](const Mat& A, const Mat& B) {
    Mat C(A[0].size(), std::vector<double>(B[0].size(), 0.0));
    for (size_t i = 0; i < N; ++i)
      for (size_t a = 0; a < A[0].size(); ++a)
        for (size_t c = 0; c < B[0].size(); ++c) C[a][c] += A[i][a] * B[i][c];
    return C;
  };
  auto mul = [](const Mat& A, const Mat& B) {
    Mat C(A.size(), std::vector<double>(B[0].size(), 0.0));
    for (size_t i = 0; i < A.size(); ++i)
      for (size_t k = 0; k < B.size(); ++k)
        for (size_t j = 0; j < B[0].size(); ++j) C[i][j] += A[i][k] * B[k][j];
    return C;
  };
  auto tr = [](const Mat& A) {
    Mat T(A[0].size(), std::vector<double>(A.size()));
    for (size_t i = 0; i < A.size(); ++i)
      for (size_t j = 0; j < A[0].size(); ++j) T[j][i] = A[i][j];
    return T;
  };
  const Mat XX = cross(X, X), ZZ = cross(Z, Z), ZX = cross(Z, X);
  Mat Zu(L, std::vector<double>(1, 0.0)), Zy(L, std::vector<double>(1, 0.0));
  for (size_t i = 0; i < N; ++i)
    for (size_t a = 0; a < L; ++a) {
      Zu[a][0] += Z[i][a] * u[i];
      Zy[a][0] += Z[i][a] * y[i];
    }
  const Mat ZZi = e3::invsym(ZZ);
  const double sigma2 = rss / (Nd - dm);
  Mat S(L, std::vector<double>(L, 0.0)), Wm(L, std::vector<double>(L, 0.0));
  for (size_t a = 0; a < L; ++a)
    for (size_t c = 0; c < L; ++c) Wm[a][c] = ZZi[a][c] * Nd / sigma2;  // inv(sigma2 Z'Z/N)
  if (in.vce == "iid") {
    for (size_t a = 0; a < L; ++a)
      for (size_t c = 0; c < L; ++c) S[a][c] = sigma2 * ZZ[a][c] / Nd;
  } else if (in.vce == "hc1") {
    for (size_t i = 0; i < N; ++i)
      for (size_t a = 0; a < L; ++a)
        for (size_t c = 0; c < L; ++c) S[a][c] += u[i] * u[i] * Z[i][a] * Z[i][c];
    for (auto& r : S)
      for (auto& v : r) v /= Nd - dm;
  } else {
    std::map<double, std::vector<double>> g;
    for (size_t i = 0; i < N; ++i) {
      auto& s = g[cc[(*rows)[i]]];
      s.resize(L, 0.0);
      for (size_t a = 0; a < L; ++a) s[a] += Z[i][a] * u[i];
    }
    for (const auto& [k, s] : g)
      for (size_t a = 0; a < L; ++a)
        for (size_t c = 0; c < L; ++c) S[a][c] += s[a] * s[c];
    for (auto& r : S)
      for (auto& v : r) v /= Nd;
  }
  // j: Sargan (iid) or Hansen's J at the two-step efficient GMM fit
  double j = std::nan("");
  if (in.vce == "iid") {
    const Mat q = mul(tr(Zu), mul(ZZi, Zu));
    j = q[0][0] / sigma2;
  } else {
    const Mat Si = e3::invsym(S);
    const Mat XZSi = mul(tr(ZX), Si);
    const Mat A = e3::invsym(mul(XZSi, ZX));
    const Mat b2 = mul(A, mul(XZSi, Zy));
    Mat g(L, std::vector<double>(1, 0.0));
    for (size_t i = 0; i < N; ++i) {
      double xb = 0;
      for (size_t c = 0; c < K; ++c) xb += X[i][c] * b2[c][0];
      for (size_t a = 0; a < L; ++a) g[a][0] += Z[i][a] * (y[i] - xb);
    }
    j = mul(tr(g), mul(Si, g))[0][0] / Nd;
  }
  // canonical correlations: endog and excluded instruments, the included ones partialled out
  const size_t K1 = in.endog.size(), L1 = in.exexog.size(), P = in.exog.size() + (in.cons ? 1 : 0);
  std::vector<double> ccev;
  if (K1 > 0 && L1 >= K1) {
    Mat X1(N, std::vector<double>(K1)), Z1(N, std::vector<double>(L1)), X2(N, std::vector<double>(P, 1.0));
    for (size_t i = 0; i < N; ++i) {
      for (size_t a = 0; a < K1; ++a) X1[i][a] = X[i][a];
      for (size_t a = 0; a < L1; ++a) Z1[i][a] = Z[i][a];
      for (size_t a = 0; a < in.exog.size(); ++a) X2[i][a] = Z[i][L1 + a];
    }
    if (P > 0) {
      const Mat X2i = e3::invsym(cross(X2, X2));
      auto partial = [&](Mat& A) {
        const Mat coef = mul(X2i, cross(X2, A));
        for (size_t i = 0; i < N; ++i)
          for (size_t a = 0; a < A[0].size(); ++a) {
            double s = 0;
            for (size_t k = 0; k < P; ++k) s += X2[i][k] * coef[k][a];
            A[i][a] -= s;
          }
      };
      partial(X1);
      partial(Z1);
    }
    const Mat Sxx = cross(X1, X1), Szz = cross(Z1, Z1), Sxz = cross(X1, Z1);
    // Sxx = R'R (Cholesky); M = R^-T Sxz Szz^-1 Szx R^-1 is symmetric with the ccev
    Mat R(K1, std::vector<double>(K1, 0.0));
    bool ok = true;
    for (size_t c = 0; c < K1 && ok; ++c) {
      double d = Sxx[c][c];
      for (size_t k = 0; k < c; ++k) d -= R[k][c] * R[k][c];
      if (!(d > 0)) ok = false;
      else R[c][c] = std::sqrt(d);
      for (size_t r2 = c + 1; r2 < K1 && ok; ++r2) {
        double s = Sxx[c][r2];
        for (size_t k = 0; k < c; ++k) s -= R[k][c] * R[k][r2];
        R[c][r2] = s / R[c][c];
      }
    }
    if (ok) {
      Mat Ri(K1, std::vector<double>(K1, 0.0));  // inverse of upper-triangular R
      for (size_t c = 0; c < K1; ++c) {
        Ri[c][c] = 1.0 / R[c][c];
        for (size_t r2 = c; r2-- > 0;) {
          double s = 0;
          for (size_t k = r2 + 1; k <= c; ++k) s += R[r2][k] * Ri[k][c];
          Ri[r2][c] = -s / R[r2][r2];
        }
      }
      const Mat M = mul(tr(Ri), mul(Sxz, mul(e3::invsym(Szz), mul(tr(Sxz), Ri))));
      const int n = static_cast<int>(K1);
      std::vector<double> a(K1 * K1), w, zv;
      for (size_t r2 = 0; r2 < K1; ++r2)
        for (size_t c = 0; c < K1; ++c) a[c * K1 + r2] = 0.5 * (M[r2][c] + M[c][r2]);
      if (piet::lapack::dsyevrVAL(n, std::move(a), 0.0, w, zv) == 0 && w.size() == K1)
        ccev.assign(w.rbegin(), w.rend());
    }
  }
  // the Wald chi2 of the slopes on e(V)
  const size_t nSlopes = K - (in.cons ? 1 : 0);
  double chi2 = std::nan("");
  if (nSlopes > 0) {
    Mat Vs(nSlopes, std::vector<double>(nSlopes));
    Mat bs(nSlopes, std::vector<double>(1));
    for (size_t a = 0; a < nSlopes; ++a) {
      bs[a][0] = b[a];
      for (size_t c = 0; c < nSlopes; ++c) Vs[a][c] = eV->data[a][c];
    }
    chi2 = mul(tr(bs), mul(e3::invsym(Vs), bs))[0][0];
  }
  double nClust = std::nan("");
  if (in.vce == "cluster") {
    std::set<double> gs;
    for (const int64_t r : *rows) gs.insert(cc[r]);
    nClust = static_cast<double>(gs.size());
  }
  const double cons = in.cons ? 1.0 : 0.0;
  const double Fdf1 = Kd - cons;
  const double dfR = in.vce == "cluster" ? nClust - 1.0 : Nd - Kd - dm - sdm;
  double F = std::nan("");
  if (Fdf1 > 0 && std::isfinite(chi2)) {
    if (in.small) F = chi2 / Fdf1;
    else if (in.vce == "cluster") F = chi2 / Fdf1 * (nClust - 1.0) / nClust * (Nd - Kd - sdm) / (Nd - 1.0);
    else F = chi2 / Fdf1 * dfR / (Nd - dm);
  }
  auto num = [](double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto tail = [&session, &num](const std::string& fn, std::vector<double> args) {
    std::string e = fn + "(";
    for (size_t i = 0; i < args.size(); ++i) e += (i ? "," : "") + num(args[i]);
    const auto v = evalScalarExpr(e + ")", session);
    return v.has_value() ? *v : std::nan("");
  };
  auto put = [&session](const std::string& nm, double v) {
    session.eval.setEScalar(nm, v);
    session.noteEOnFit(nm);
  };
  auto putS = [&session](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  auto putM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = std::move(m);
        session.noteEOnFit(nm);
        return;
      }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
    session.noteEOnFit(nm);
  };
  const double r2c = 1.0 - rss / yyc, r2u = 1.0 - rss / yy;
  const double r2 = in.cons ? r2c : r2u;
  put("inexog_ct", static_cast<double>(in.exog.size()));
  put("exexog_ct", static_cast<double>(L1));
  put("endog_ct", static_cast<double>(K1));
  put("partial_ct", sdm);
  if (in.ivar.empty()) put("df_m", Fdf1);  // xtivreg2 posts its own df_m
  put("sdofminus", sdm);
  put("dofminus", dm);
  put("center", 0.0);
  put("r2", r2);
  put("rss", rss);
  put("mss", (in.cons ? yyc : yy) - rss);
  put("r2_a", 1.0 - (1.0 - r2) * (in.cons ? Nd - 1.0 : Nd) / (Nd - Kd - dm - sdm));
  put("F", F);
  put("Fp", std::isfinite(F) ? tail("Ftail", {Fdf1, dfR, F}) : std::nan(""));
  put("Fdf1", Fdf1);
  put("Fdf2", dfR);
  put("yy", yy);
  put("yyc", yyc);
  put("cons", cons);
  put("partialcons", sdm);
  put("nocollin", 0.0);
  if (in.small) put("df_r", dfR);
  const double jdf = Ld - Kd;
  put("j", jdf == 0 ? 0.0 : j);
  put("jdf", jdf);
  if (jdf > 0) put("jp", chi2UpperTailIntegerDf(jdf, j));
  if (in.vce == "iid") {
    put("sargan", jdf == 0 ? 0.0 : j);
    put("sargandf", jdf);
    if (jdf > 0) put("sarganp", chi2UpperTailIntegerDf(jdf, j));
  }
  put("ll", -0.5 * Nd * (1.0 + std::log(2.0 * M_PI) + std::log(rss / Nd)));
  put("rankV", Kd);
  put("rankS", Ld);
  put("rankxx", Kd);
  put("rankzz", Ld);
  put("condxx", symCond(XX));
  put("condzz", symCond(ZZ));
  put("r2c", r2c);
  put("r2u", r2u);
  if (!ccev.empty()) {
    eval::EvalMatrix ce, cd;
    ce.rownames = cd.rownames = {"r1"};
    std::vector<double> cev, cdv;
    for (size_t a = 0; a < ccev.size(); ++a) {
      ce.colnames.push_back("c" + std::to_string(a + 1));
      cev.push_back(ccev[a]);
      cdv.push_back(ccev[a] / (1.0 - ccev[a]));
    }
    cd.colnames = ce.colnames;
    ce.data.push_back(cev);
    cd.data.push_back(cdv);
    put("cd", cdv.back());
    putM("ccev", std::move(ce));
    putM("cdev", std::move(cd));
    // xtivreg2: ivreg2 posts the Cragg-Donald F on N - L - dofminus (the engine's
    // first-stage F does not carry dofminus)
    if (!in.ivar.empty() || in.hdfeTags) {
      const double cdf = cdv.back() * (Nd - sdm - Ld - dm) / static_cast<double>(L1);
      put("cdf", cdf);
      if (in.vce == "iid") put("widstat", cdf);
    }
    // xtivreg2 robust/cluster: ivreg2's idstat = chi2/N (N-dofminus) (robust) or chi2
    // (cluster); rkf from the engine's Wald F, whose cluster form divides by N-L-1 where
    // ivreg2's has N-L (bank e6d8d96f robust: idstat 3.7000425 = 4.3167162 x 288/336,
    // rkf 1.8990335; 49fc43aa cluster: idstat 1.8205421, rkf .85977685 = .85719494 x
    // 333/332)
    // ivreghdfe: the engine's KP statistics are ivreghdfe's as they stand (bank 9e7223da
    // robust: idstat 2.3373244, rkf 1.5037092; 23841851 cluster: 1.3951206, 10.521661)
    if ((!in.ivar.empty() || in.hdfeTags) && in.vce != "iid" && K1 == 1 &&
        std::isfinite(in.kpLm) && std::isfinite(in.kpWaldF)) {
      const bool xt = !in.ivar.empty();
      const double idstat = xt && in.vce == "hc1" ? in.kpLm * (Nd - dm) / Nd : in.kpLm;
      const double rkf =
          !xt || in.vce == "hc1" ? in.kpWaldF : in.kpWaldF * (Nd - Ld) / (Nd - Ld - 1.0);
      const double iddf = Ld - Kd + 1.0;
      put("idstat", idstat);
      put("iddf", iddf);
      put("idp", chi2UpperTailIntegerDf(iddf, idstat));
      put("rkf", rkf);
      put("widstat", rkf);
    }
    if (in.vce == "iid") {
      const double idstat = (Nd - dm) * ccev.back();
      const double iddf = Ld - Kd + 1.0;
      put("idstat", idstat);
      put("iddf", iddf);
      put("idp", chi2UpperTailIntegerDf(iddf, idstat));
    }
  }
  std::vector<std::string> zRow = zNames;
  if (in.cons) zRow.push_back("_cons");
  eval::EvalMatrix sm, wm;
  sm.rownames = sm.colnames = wm.rownames = wm.colnames = zRow;
  sm.data = S;
  wm.data = Wm;
  putM("S", std::move(sm));
  putM("W", std::move(wm));
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
  };
  const std::string insts = join(zNames);
  for (const std::string sfx : {"", "0", "1"}) {
    if (!sfx.empty()) putS("depvar" + sfx, in.depvar);
    putS("instd" + sfx, join(in.endog));
    putS("insts" + sfx, insts);
    putS("exexog" + sfx, join(in.exexog));
    putS("inexog" + sfx, join(in.exog));
  }
  putS("ivreg2cmd", in.ivreg2cmd);
  if (!in.fe.empty()) {
    // reghdfe's absorption bookkeeping (bank 906be622 `absorb(rep78)`, ab2d02d8
    // `absorb(rep78 foreign)`, cb6c948f `absorb(rep78) cl(rep78)`)
    double initial = 0, redundant = 0, nested = 0;
    eval::EvalMatrix dt;
    for (size_t g = 0; g < in.fe.size(); ++g) {
      const auto& r = in.dofTable[g];
      initial += r[0];
      redundant += r[1];
      if (r[4] != 0) nested += r[0];
      dt.rownames.push_back("1." + in.fe[g]);  // reghdfe's row name (bank 906be622 `1.rep78`)
      dt.data.push_back(r);
    }
    dt.colnames = {"Categories", "Redundant", "Num Coefs", "Inexact?", "Nested?"};
    put("df_a", initial - redundant);
    put("df_a_initial", initial);
    put("df_a_redundant", redundant);
    put("df_a_nested", nested);
    put("N_hdfe", static_cast<double>(in.fe.size()));
    put("N_hdfe_extended", static_cast<double>(in.fe.size()));
    putM("dof_table", std::move(dt));
    const std::string fes = join(in.fe);
    putS("absvars", fes);
    putS("extended_absvars", fes);
    putS("title2", "Absorbing " + std::to_string(in.fe.size()) + " HDFE group" +
                       (in.fe.size() == 1 ? "" : "s"));
    putS("dofmethod", "pairwise clusters continuous");
  }
  putS("version", in.version);
  putS("predict", in.predict);

  putS("title", "IV (2SLS) estimation");
  putS("ranktestcmd", "ranktest");
  putS("partialsmall", "small");
  putS("marginsok", "XB default");
  putS("marginsnotok", "Residuals SCores");
  putS("properties", "b V");
  putS("hacsubtitleB", "Estimates efficient for homoskedasticity only");
  putS("hacsubtitleV",
       in.vce == "iid" ? "Statistics consistent for homoskedasticity only"
                       : (in.vce == "hc1" ? "Statistics robust to heteroskedasticity"
                                          : "Statistics robust to heteroskedasticity and clustering on " +
                                                in.clusterVar));
  // first/ffirst/savefirst: the Anderson-Rubin and Stock-Wright tests of the excluded
  // instruments in the reduced form of y (iid only; bank f130998d `ivreg2 ..., savefirst`:
  // arf 6.4911704, archi2 13.724189, sstat 11.57708): with the included instruments
  // partialled out, q = y~'P(Z1~) y~; arf = (q/L1) / (rss_rf/(N-L)), archi2 = q/(rss_rf/N),
  // sstat = N q / y~'y~; dfs L1 and N-L (N less dofminus under xtivreg2: bank f34c2bf0).
  if (in.firstStats && in.vce == "iid" && L1 > 0) {
    Mat Z1(N, std::vector<double>(L1)), X2(N, std::vector<double>(P, 1.0)), yt(N, std::vector<double>(1));
    for (size_t i = 0; i < N; ++i) {
      for (size_t a = 0; a < L1; ++a) Z1[i][a] = Z[i][a];
      for (size_t a = 0; a < in.exog.size(); ++a) X2[i][a] = Z[i][L1 + a];
      yt[i][0] = y[i];
    }
    if (P > 0) {
      const Mat X2i = e3::invsym(cross(X2, X2));
      for (Mat* A : {&Z1, &yt}) {
        const Mat coef = mul(X2i, cross(X2, *A));
        for (size_t i = 0; i < N; ++i)
          for (size_t a = 0; a < (*A)[0].size(); ++a) {
            double s2 = 0;
            for (size_t k = 0; k < P; ++k) s2 += X2[i][k] * coef[k][a];
            (*A)[i][a] -= s2;
          }
      }
    }
    const Mat zy = cross(Z1, yt);
    const double q = mul(tr(zy), mul(e3::invsym(cross(Z1, Z1)), zy))[0][0];
    const double yty = cross(yt, yt)[0][0];
    const double rssRf = yty - q;
    const double ardf = static_cast<double>(L1), ardfR = Nd - Ld - dm - sdm;
    const double arf = (q / ardf) / (rssRf / ardfR), archi2 = q / (rssRf / (Nd - dm)),
                 sstat = (Nd - dm) * q / yty;
    put("arf", arf);
    put("arfp", tail("Ftail", {ardf, ardfR, arf}));
    put("archi2", archi2);
    put("archi2p", chi2UpperTailIntegerDf(ardf, archi2));
    put("ardf", ardf);
    put("ardf_r", ardfR);
    put("sstat", sstat);
    put("sstatdf", ardf);
    put("sstatp", chi2UpperTailIntegerDf(ardf, sstat));
  }
  if (in.vce == "cluster") putS("vce", "robust cluster");  // ivreg2's spelling (bank eacd5824)
  if (in.small) {
    putS("small", "small");
    putS("_r_z__CL", "t");
  }
  // ivreghdfe with absorb() is a small-sample fit referenced on t(df_r): the block carries
  // the df its table uses, so r(table) is posted on it (bank 906be622: r(table) df 62)
  if (in.small && !in.fe.empty() && block != nullptr && block->is_object()) {
    (*block)["df_r"] = dfR;
    if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
      (*session.lastEstBlock)["df_r"] = dfR;
  }
  if (!in.ivar.empty() || in.hdfeTags) putS("model", K1 > 0 ? "iv" : "ols");
  // r(): ivreg2's last `_ms_parse_parts`, the last excluded instrument (posted on top
  // of the fit's r() by the r-class lifecycle, block key r_after_fit)
  if (!in.exexog.empty() && block != nullptr && block->is_object()) {
    ojson ra;
    ra["scalars"] = {{"omit", 0.0}, {"k_fac", 0.0}, {"k_var", 1.0}, {"k_Lvar", 0.0}, {"k_func", 0.0}};
    ra["macros"] = {{"name", in.exexog.back()}, {"type", "variable"}};
    // ivreghdfe ends on its coefficient table's settings (bank 1cc14749: r(mcmethod))
    if (in.hdfeTags) {
      ra["scalars"] = ojson::object();
      ra["macros"] = {{"mcmethod", "noadjust"}};
    }
    // `first` stores the first stages as _ivreg2_<endog> and its estimates listing
    // leaves their names (bank 674356a1: r(names) _ivreg2_mpg)
    if (in.first) {
      std::string names;
      const std::string pre = in.ivar.empty() ? "_ivreg2_" : "_xtivreg2_";  // bank 8a466213
      for (const auto& e : in.endog) names += (names.empty() ? "" : " ") + (pre + e);
      ra["macros"]["names"] = names;
    }
    (*block)["r_after_fit"] = ra;
    if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
      (*session.lastEstBlock)["r_after_fit"] = ra;
  }
  return true;
}
}  // namespace

// `reportOut` (BUG-1227): ivregress's `first` report -- "First-stage regressions" and
// each stage's header and table (ivregressFirstHeaderM4b) -- as a display block the
// caller pushes ahead of the estimation block, where Stata prints it. The dispatcher's
// ivregress arm passes it (declared there: piet/handlers.h keeps the seven-argument
// form, which keeps every first stage in the block's warnings, as before).
ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                    const std::string& stataCmd, SessionState& session, bool smallDefault,
                    bool forceSmall, ojson* reportOut) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/true);
  if (iv.errored) {
    return engineErrorBlockM4("syntax_error: `" + stataCmd + "` " + iv.errorMsg, "syntax_error");
  }
  // FU-475: the engine applies --keepif after its time-series operators only
  // for a restriction it can evaluate on the full frame.
  if (auto refused = tsFitRestrictionRefusal(parsed, stataCmd)) return *refused;

  bool isLiml = (method == "liml");
  std::string cobraCmd = isLiml ? "liml" : "iv";

  // `first` (w2b_estim.log:879 ivregress_first, :1064 ivreghdfe_first):
  // each first stage is displayed, the fit itself is unchanged (both arms'
  // _b/_se are the option-free fits' -- :919 against ivreg2_ctl :1060, :1185
  // against ivreghdfe_ctl :1301). The stages come from the
  // engine's `--savefirst` estimate: `regress ENDOG EXCLUDED_IVS EXOG` under the
  // parent's vce, small-sample statistics always (cmd_iv's usage text; the
  // FE-absorbed stage reproduces :1078-1079, z1 .2657743 / .1072108). gmm and
  // liml are refused: their first-stage tables are not measured, and liml's
  // engine command has no --savefirst at all.
  bool wantFirst = false;
  // ivreg2's partial(varlist) (w2b_estim.log:969-1013 ivreg2_partial):
  // Frisch-Waugh-Lovell -- see dropCoefsM4b. ivreg2/xtivreg2 only: ivregress and
  // ivreghdfe keep refusing the key through the net below.
  const bool partialAllowed = (stataCmd == "ivreg2" || stataCmd == "xtivreg2");
  bool wantPartial = false;
  std::vector<std::string> partialVars;
  // ivregress's `small` (t/F small-sample statistics; w2s_multiway_class.log
  // :875/:880 mw_iv_c2s `vce(cluster id id2) small`: e(small) `small`, df_r 6).
  bool wantSmallOpt = false;
  std::optional<double> ivLevel;  // ivregress level(#)
  for (const auto& [k, v] : parsed.opts) {
    if (k == "small" && stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress") {
      if (!v.isBool) return stataRefusalM4b("option small() not allowed", 198);
      wantSmallOpt = true;
    }
  }
  if (wantSmallOpt) forceSmall = true;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "first") {
      if (!v.isBool) return stataRefusalM4b("option first() not allowed", 198);
      wantFirst = true;
    } else if (k == "partial" && partialAllowed) {
      if (v.isBool || splitTokensM4(v.strValue).empty()) {
        return stataRefusalM4b("option partial() requires a varlist", 198);
      }
      wantPartial = true;
      for (const auto& t : splitTokensM4(v.strValue)) partialVars.push_back(t);
    }
  }
  // ivreg2's ffirst / savefirst / savefprefix(P) (ivreg2 runs the first-stage
  // block for any of first/ffirst/savefirst; savefprefix implies savefirst).
  // ffirst prints the summary statistics only; savefirst stores each first stage as
  // `estimates store <prefix><endog>` (default prefix _ivreg2_); both post
  // e(first) (:2071-2073). w2d_uncovered.log:1506 ivreg2_ffirst_savefirst_068,
  // :1623 ivreg2_cluster_ffirst_057. xtivreg2 passes first/ffirst/savefprefix() on to
  // ivreg2 (xtivreg2): its default prefix is `_xtivreg2_`, and a
  // bare `first` saves the stages only to drop them again, so it
  // posts e(first) without stored estimates or e(firsteqs).
  const bool ivreg2Typed = stataCmd == "ivreg2" && lowerM4(parsed.cmd) == "ivreg2";
  const bool xtivreg2Typed = stataCmd == "xtivreg2" && lowerM4(parsed.cmd) == "xtivreg2";
  const bool ivreg2Family = ivreg2Typed || xtivreg2Typed;
  // ivreg2's and ivreghdfe's legacy `gmm` option (the full word; `gmm2` is gmm2s):
  // refused r(198) with the translation table they print (bank 7ef1c059 `ivreg2 ...,
  // gmm`, 418e7967 `ivreghdfe ..., absorb(rep78) gmm`). xtivreg2 runs it (bank 9927b253).
  if (ivreg2Typed || (stataCmd == "ivreghdfe" && lowerM4(parsed.cmd) == "ivreghdfe")) {
    for (const auto& kv : parsed.opts)
      if (lowerM4(kv.first) == "gmm")
        return stataRefusalM4b(
            "-gmm- is no longer a supported option; use -gmm2s- with the appropriate option\n"
            "      gmm             =  gmm2s robust\n"
            "      gmm robust      =  gmm2s robust\n"
            "      gmm bw()        =  gmm2s bw()\n"
            "      gmm robust bw() =  gmm2s robust bw()\n"
            "      gmm cluster()   =  gmm2s cluster()",
            198);
  }
  bool wantFfirst = false, wantSavefirst = false;
  std::string savefprefix;
  if (ivreg2Family) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "ffirst" || k == "savefirst") {
        if (!v.isBool) return stataRefusalM4b("option " + k + "() not allowed", 198);
        (k == "ffirst" ? wantFfirst : wantSavefirst) = true;
      } else if (k == "savefprefix") {
        const std::string pre = v.isBool ? std::string() : trimM4(v.strValue);
        if (pre.empty() || pre.find_first_of(" \t") != std::string::npos)
          return stataRefusalM4b("option savefprefix() incorrectly specified", 198);
        savefprefix = pre;
        wantSavefirst = true;
      }
    }
    if (xtivreg2Typed && wantSavefirst && savefprefix.empty()) savefprefix = "_xtivreg2_";
  }
  if (wantFirst && method != "2sls") {
    return engineErrorBlockM4(
        "Option `first` is a valid Stata option that this version of PIET implements for the "
        "2SLS estimator only (`" + stataCmd + "` with " + method + " has no measured "
        "first-stage table).",
        "unsupported_option");
  }
  if (wantPartial) {
    // The partialled-out variables must be included exogenous regressors typed
    // as plain names (`_cons` alone partials the constant only). A factor or
    // operator token has no single coefficient to drop, so it is refused rather
    // than guessed at.
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (pv.find_first_of(".#()") != std::string::npos) {
        return engineErrorBlockM4(
            "partial(" + pv + "): this version of PIET partials out plain exogenous "
            "variables only (no factor-variable or time-series operators).",
            "unsupported_option");
      }
      if (std::find(iv.exog.begin(), iv.exog.end(), pv) == iv.exog.end()) {
        return stataRefusalM4b("partial(): " + pv + " is not an included exogenous regressor",
                               198);
      }
    }
    if (method != "2sls") {
      // Efficient two-step GMM re-weights with the partialled-out regressors in
      // the moment conditions; FWL invariance is not measured there.
      return engineErrorBlockM4(
          "Option `partial()` is a valid Stata option that this version of PIET implements for "
          "the 2SLS estimator only.",
          "unsupported_option");
    }
  }

  std::vector<std::string> argv = {cobraCmd,        "--y",      iv.y,
                                    "--endog",       joinCommaM4(iv.endog),
                                    "--instruments", joinCommaM4(iv.instruments)};
  // cobra iv/liml both REJECT an explicit `--x "(intercept)"` placeholder
  // (live-verified: "column not found: (intercept)") — unlike feols/xtreg,
  // which REQUIRE that literal when there are no exogenous controls. Only
  // push --x when there is a genuine exogenous-controls list.
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  // ivreghdfe's cluster() with three or more variables (StataBE 19.5, BUG-1189;
  // scratch-laneA/agents/p-est-a/k3.do): ivreghdfe clusters on the first two only
  // (ivreghdfe `tokenize `cluster''), drops the rows missing any listed
  // variable (markout) and posts e(clustvar) with every one. cluster(id id2
  // id3) is the cluster(id id2) fit -- N 180, N_clust 7 (30 and 7), _se[x1]
  // .15468183635897548, e(vce) "robust two-way cluster", e(clustvar) "id id2 id3" --
  // and with id3 missing in 12 rows it is that fit `if !missing(id3)` (N 168). `pv` is
  // the parse the VCE reader sees: the list cut to its first two names.
  eval::ParsedCommand pv = parsed;
  // ivregress 2sls/liml: vce(unadjusted) (abbreviated to `un`..) is the default
  // conventional VCE (bank 01ad996841 `ivregress 2sls ..., vce(unadjusted)`: the default
  // fit's e(V), e(vce) unadjusted).
  if (stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress" &&
      (method == "2sls" || method == "liml")) {
    pv.opts.erase(std::remove_if(pv.opts.begin(), pv.opts.end(),
                                 [](const auto& kv) {
                                   if (kv.first != "vce" || kv.second.isBool) return false;
                                   const std::string t = lowerM4(trimM4(kv.second.strValue));
                                   return t.size() >= 2 && std::string("unadjusted").rfind(t, 0) == 0;
                                 }),
                  pv.opts.end());
  }
  std::vector<std::string> extraClusterVars;
  std::string allClusterVars;
  if (stataCmd == "ivreghdfe" && lowerM4(parsed.cmd) == "ivreghdfe") {
    for (auto& [k, v] : pv.opts) {
      if (v.isBool) continue;
      std::vector<std::string> toks;
      std::string head;
      if (k == "cluster") {
        toks = splitTokensM4(v.strValue);
      } else if (k == "vce" || k == "vcov") {
        std::string rest;
        if (canonicalVceType(v.strValue, &rest) == "cluster") {
          toks = splitTokensM4(rest);
          head = "cluster ";
        }
      }
      if (toks.size() < 3) continue;
      allClusterVars = joinSpaceM4(toks);
      extraClusterVars.assign(toks.begin() + 2, toks.end());
      v.strValue = head + toks[0] + " " + toks[1];
    }
  }
  std::optional<std::string> keepif = parsed.ifExpr;
  if (!extraClusterVars.empty()) {
    std::string present;
    for (const auto& c : extraClusterVars)
      present += (present.empty() ? "" : " & ") + std::string("!missing(") + c + ")";
    keepif = keepif.has_value() ? "(" + *keepif + ") & " + present : present;
  }
  if (keepif.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*keepif);
  }

  // --fe via absorb() — ivreghdfe / the reghdfe-paren form ONLY. Real
  // Stata's `ivregress` has no absorb() option of its own at all.
  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  // ivreghdfe's small-sample statistics come with absorb() (the absorbed effects are
  // partialled out, e(partialsmall) small); without absorb() it is ivreg2's
  // large-sample fit (bank 1cc14749 `ivreghdfe price weight (mpg = length turn)`:
  // e(V)[1,1] 1229064.56 = the small-sample 1280996.87 x 71/74, e(rmse) sqrt(RSS/N);
  // 114b5857 `..., nocons` alike).
  if (stataCmd == "ivreghdfe" && !sawAbsorb) smallDefault = false;
  if (sawAbsorb) {
    if (!allowAbsorb) {
      return engineErrorBlockM4("option `absorb` not allowed", "unsupported_option");
    }
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // vce()/robust/legacy cluster(). cobra `iv` supports iid/hc1/cluster/
  // twoway (BS-067's splitClusterVarsM4 reused unchanged). cobra `liml` has
  // NO working vcov surface at all — live-verified: `--vcov hc1` is
  // silently ACCEPTED (exit 0, no error) but produces the IDENTICAL
  // classical/asymptotic SE either way, i.e. a true engine no-op rather
  // than a rejection. Forwarding a user's vce()/robust/cluster() to liml
  // would silently claim a robust/clustered SE the engine never actually
  // computed — an honest reject before ever building the argv, matching
  // this batch's "never forward a flag the engine demonstrably ignores"
  // rule (see also noconstant/level below, and the report's cobra_runner.cpp
  // masking-bug finding for the OTHER half of that rule: options the engine
  // hard-errors on but whose stdout gets parsed as a false success anyway).
  // Observation weights: [aw=]/[pw=] reach here for ivreg2, xtivreg2, ivregress and
  // ivreghdfe (their registry rows admit them). ivreg2 treats pweight as
  // aweight plus robust, so a bare pweight fits robust, as regress does (BUG-536's twin).
  std::string ivWtype;
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "aw" || wt == "aweight") ivWtype = "aw";
    else if (wt == "pw" || wt == "pweight") ivWtype = "pw";
    // xtivreg2's fe and ivreghdfe are the absorbed fits that admit them: the within
    // transform uses the weighted group means (xtivreg2), then the
    // weighted 2SLS (BUG-1283; ivregress's own absorb() is not measured weighted).
    if (ivWtype.empty() || isLiml || (sawAbsorb && !xtivreg2Typed && stataCmd != "ivreghdfe")) {
      return engineErrorBlockM4(
          "[" + *parsed.weightType + "=] is a valid Stata weight that this version of PIET does not "
          "implement for `" + stataCmd + "`" + (sawAbsorb ? " together with absorbed fixed effects" : "") +
              " (aweight and pweight are implemented for the unabsorbed 2SLS/GMM fit).",
          "unsupported_option");
    }
  }
  // The engine's liml now takes --vcov hc1 and --vcov cluster --cluster C, the
  // robust/cluster sandwich of ivregress liml (score rows (P_Z X)_i u_i,
  // `small` rescale); w2t_boottest_family.log:1060-1075 D2 `ivregress liml ...,
  // vce(cluster id)` x1 se .0647215. Multiway clustering stays refused for liml
  // (Stata's multiway-cluster liml path is not implemented).
  {
    // BUG-1278: ivregress's own parser refuses a cluster variable that is not in
    // the data (r(198) under vce(), r(111) under cluster()), not the engine's
    // "[missing_column]", and vce() with cluster(). Not ivreg's: Stata's ivreg
    // refuses vce() outright and words cluster() as regress does (r(198));
    // ivreghdfe's is not measured. vce(cluster a b) is multiway here.
    if (stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress") {
      if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kIvregress,
                                          /*multiwayVceRefused=*/false))
        return *bad;
    }
    VcovResult vr = vcovFromOpts(pv);
    if (vr.errored) {
      return vcovErrorBlockM4(vr);
    }
    if (vr.argv.empty() && ivWtype == "pw") vr.argv = {"--vcov", "hc1"};
    // ivreg2's gmm2s without robust/cluster is efficient GMM under homoskedasticity
    // (2SLS's estimate and variance); the engine's --gmm defaults to the robust weight.
    if (vr.argv.empty() && method == "gmm" && stataCmd == "ivreg2") vr.argv = {"--vcov", "iid"};
    if (isLiml) {
      for (size_t i = 0; i + 1 < vr.argv.size(); ++i) {
        if (vr.argv[i] == "--vcov" && vr.argv[i + 1] != "iid" && vr.argv[i + 1] != "hc1" &&
            vr.argv[i + 1] != "cluster") {
          return engineErrorBlockM4(
              "vce(" + vr.argv[i + 1] + ") is not supported for `ivregress liml` in this version "
              "of PIET -- the engine's liml computes the classical, robust and one-way cluster "
              "VCE only.",
              "unsupported_option");
        }
      }
    }
    for (const auto& a : vr.argv) argv.push_back(a);
  }
  if (!ivWtype.empty()) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(ivWtype);
  }

  // ivreghdfe (including the `reghdfe ... (endog=instr), absorb()` alias)
  // reports the absorbed/cluster small-sample correction by default. cobra
  // `iv` exposes that convention explicitly as --small; plain ivregress must
  // retain its ASYMPTOTIC default. Until 2026-09-09 that distinction rode on
  // `allowAbsorb`, which made "accepts absorb()" and "reports small-sample SEs"
  // the same bit — so BUG-474's one-line flip moved ivregress's standard errors
  // (D1a/D1f selftest RED). `smallDefault` now carries the spelling's own
  // convention; `forceSmall` is `ivreg2 ..., small` arriving from runIvreg2,
  // which used to parse that flag and throw it away. Either reason is
  // sufficient and the flag is idempotent, so this is an OR.
  if (smallDefault || forceSmall) argv.push_back("--small");
  // ivregress ..., small under a multiway cluster: the built-in convention's
  // per-combination factors (engine FU-387;
  // w2s_multiway_class.log:880 mw_iv_c2s_se .0585934519606149).
  if (wantSmallOpt && method != "gmm" && !sawAbsorb) {
    for (size_t i = 0; i + 1 < argv.size(); ++i)
      if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) {
        argv.push_back("--multiway-subset-factors");
        break;
      }
  }
  bool ivMultiway = false;
  for (size_t i = 0; i + 1 < argv.size(); ++i)
    if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) ivMultiway = true;
  // Two-step GMM under a multiway cluster(): the engine builds its weight matrix from
  // the heteroskedasticity-robust meat whatever the cluster dimensions, a different
  // estimator (bank 7b19fa52 `ivreg2 ... gmm2s cluster(industry occupation)`:
  // e(rmse) 8.92 against the fit's 15.15).
  if (method == "gmm" && ivMultiway)
    return engineErrorBlockM4(
        "Two-step GMM with a multiway cluster() is a valid Stata call that this version of PIET "
        "does not implement (one-way cluster() and robust are implemented).",
        "unsupported_option");
  if ((wantFfirst || wantSavefirst) && ivMultiway) {
    return engineErrorBlockM4(
        "Options ffirst and savefirst are valid ivreg2 options that this version of PIET does not "
        "implement together with a multiway cluster() (ivreg2's two-way first-stage statistics are "
        "not computed).",
        "unsupported_option");
  }
  if (wantFirst || wantFfirst || wantSavefirst) argv.push_back("--savefirst");
  if (wantSavefirst && !savefprefix.empty()) {
    argv.push_back("--savefprefix");
    argv.push_back(savefprefix);
  }
  // ivreg2's two-way cluster variance is S1 + S2 - S12 left as it is (no repair of a
  // negative eigenvalue), so a coefficient whose variance comes out negative has a
  // missing standard error (w2d_uncovered.log:1866 ivreg2_cluster2_aw_030 `|.|`).
  if (ivreg2Typed && ivMultiway) argv.push_back("--multiway-no-psd-fix");
  // xtivreg2's ivreg2 call: dofminus(N_g) on the within-transformed data
  // (xtivreg2) -- a large-sample adjustment (rss/(N-N_g), robust meat
  // N/(N-N_g), cluster untouched), never a small-sample one (cobra iv --dofminus-fe).
  // Two-way clustering is not implemented there and is refused here.
  if (xtivreg2Typed && sawAbsorb) {
    if (ivMultiway || method == "gmm")
      return engineErrorBlockM4(
          "xtivreg2 with " +
              std::string(ivMultiway ? "a two-way cluster()"
                                     : "gmm2s (or its legacy spelling gmm) and robust or cluster()") +
              " is a valid Stata call that this version of PIET does not implement.",
          "unsupported_option");
    argv.push_back("--dofminus-fe");
  }

  // gmm sub-estimator (Stata `ivregress gmm`): efficient two-step GMM,
  // robust weight matrix + robust SEs by default; combines with --vcov
  // cluster for the cluster-robust GMM weight matrix (cobra iv's own doc).
  if (method == "gmm") argv.push_back("--gmm");
  // ivreg2's gmm2s reports the efficient-GMM variance (X'Z W Z'X)^-1, not
  // ivregress gmm's sandwich on the final residuals (bank 4c6ea412, c81bd316,
  // c2d573b9).
  if (method == "gmm" && stataCmd == "ivreg2") argv.push_back("--gmm-efficient-vce");
  // ivregress drops an excluded instrument collinear with [endog exog constant]
  // (ivregress_check2collin); the engine checks it and refuses (r(481) when that leaves
  // the equation under-identified).
  if (stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress" && !sawAbsorb && !isLiml)
    argv.push_back("--inst-collin-check");

  // keepsingletons (BS-068) — reghdfe's own option name. cobra `iv` DOES
  // expose --keep-singletons (live-verified, despite it not being printed
  // in `cobra iv`'s own usage banner); cobra `liml` has no FE machinery at
  // all (absorb() is already rejected above for it via !allowAbsorb, or
  // simply structurally unreachable for plain ivregress), so
  // keepsingletons is meaningless there — honest reject instead of a
  // silent no-op.
  bool sawKeepSingletons = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "keepsingletons" && v.isBool && v.boolValue) sawKeepSingletons = true;
  }
  if (sawKeepSingletons) {
    if (isLiml) {
      return engineErrorBlockM4("option `keepsingletons` not allowed", "unsupported_option");
    }
    argv.push_back("--keep-singletons");
  }

  // 2SLS/GMM now supports --no-intercept. LIML and absorb() combinations
  // stay refused below, as does level(): those engine surfaces are absent.
  bool ivNoConstant = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      // The engine's 2SLS/GMM fit takes --no-intercept (w2d_uncovered.log
      // ivregress_nocons_ctl b[x1] 2.8397 / ivregress_nocons_vce_cluster_106);
      // LIML has no such surface and absorb() already removes the constant.
      if (isLiml || sawAbsorb) {
        return engineErrorBlockM4(
            "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
            "implement for `" + stataCmd + "` " +
                (isLiml ? std::string("with liml.") : std::string("together with absorb().")),
            "unsupported_option");
      }
      ivNoConstant = true;
      continue;
    }
    if (k == "level" && !v.isBool && stataCmd == "ivregress") {
      // ivregress / ivreg: the interval at level(#) is b -/+ crit*se on the fit's own
      // reference (z, or t(e(df_r)) under small / ivreg), set on the table below;
      // nothing else in the fit depends on it (bank 918bc860 `level(90)`, 019b7896
      // `level(99)`). Stata's cilevel range is 10..99.99 (r(198) outside).
      char* end = nullptr;
      const std::string t = strutil::trim(v.strValue);
      const double lv = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99))
        return stataRefusalM4b("level() must be between 10 and 99.99 inclusive", 198);
      ivLevel = lv;
      continue;
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          // BUG-286: this used to say the engine "ignores --level", which is a
          // description of a WORSE behaviour than the real one and would send a
          // reader looking for a silent drop. Re-measured 2026-08-08:
          // `cobra iv ... --level 90` answers `unrecognized option '--level' for
          // 'iv'` (bad_input) with an empty result — it has no --level surface at
          // all. The reject PIET gives here is therefore the honest mirror of a
          // missing engine flag, not a guard against a silent drop.
          "implement for `" + stataCmd + "` (the engine's IV/LIML estimator has no --level "
          "surface — it answers `unrecognized option '--level' for 'iv'`).",
          "unsupported_option");
    }
  }
  if (ivNoConstant) argv.push_back("--no-intercept");

  // kVocabIvFirst is the same registry slot as kVocabIvRegress (see their
  // registrations), so `first` is in this set; partial() joins it for ivreg2.
  std::unordered_set<std::string> known = kVocabIvFirst.known;
  if (partialAllowed) known.insert("partial");
  if (wantSmallOpt) known.insert("small");
  if (ivreg2Family) {
    known.insert("ffirst");
    known.insert("savefirst");
    known.insert("savefprefix");
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // BUG-1189: reghdfe counts an absorbed effect redundant when it is nested in ANY
  // listed cluster variable (k3.do: absorb(id) cluster(id2 id3 id) gives _se[x1]
  // .17410222432235206 because id nests in the third), which the engine, fitting on
  // the first two, cannot see. Such a fit is refused: an effect nested in a listed
  // variable past the second (checked on every row where both are present, a
  // superset of the sample), or an absorb term that is not a plain numeric variable.
  if (!extraClusterVars.empty()) {
    auto refuseNested = [&](const std::string& why) {
      return engineErrorBlockM4(
          "ivreghdfe with three or more cluster variables (" + allClusterVars + ") is served "
          "when no absorbed effect is nested in a cluster variable past the second; " + why +
              ", which this version of PIET does not implement.",
          "unsupported_option");
    };
    Datastore* ds = session.datastore();
    if (ds == nullptr) return refuseNested("the data could not be read");
    for (const auto& f : splitTokensM4(feRaw)) {
      if (!ds->has(f) || ds->dtype(f) != "f64")
        return refuseNested("absorb term `" + f + "` is not a numeric variable");
      for (const auto& c : extraClusterVars) {
        if (!ds->has(c) || ds->dtype(c) != "f64")
          return refuseNested("cluster variable `" + c + "` is not numeric");
        const double* fv = ds->col(f).f64();
        const double* cv = ds->col(c).f64();
        std::map<double, double> seen;
        bool nested = true;
        for (int64_t i = 0; i < ds->nRows() && nested; ++i) {
          if (std::isnan(fv[i]) || std::isnan(cv[i])) continue;
          const auto [it, fresh] = seen.emplace(fv[i], cv[i]);
          if (!fresh && it->second != cv[i]) nested = false;
        }
        if (nested && !seen.empty())
          return refuseNested("`" + f + "` is nested in `" + c + "`");
      }
    }
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: `iv` writes the fit's admitted rows over the --data rows (its
  // own --keepif, FE singleton drops and listwise deletion included); installed
  // below, before setLastModel. `liml` writes its listwise sample the same way
  // (bank 447495d2 `ivregress liml ...`: e(sample) 74 rows).
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  const std::string esampleValues = esampleFitArgs(session, argv, std::string(), 0);
  // eb-order T-P1: the fit's full e(V) (`iv` and, since BUG-1196, `liml` carry
  // --emit-v). Not recorded in fitArgv.
  argv.push_back("--emit-v");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    // xtivreg2 (exit 198).
    if (ce.code == "cluster_spans_panel")
      return stataRefusalM4b("cluster option not supported if a panel spans more than one cluster",
                             198);
    // BUG-1269: ivreghdfe with absorb(), like reghdfe (linear_estimation.cpp runRegress),
    // calls an empty sample insufficient, r(2001), and so a one-row one, whose
    // single row the singleton drop removes (StataBE 19.5: `ivreghdfe y (x = w) if
    // x > 20, absorb(g)` and `if x == 1`); without absorb() it is r(2000).
    if (stataCmd == "ivreghdfe" && sawAbsorb && ce.code == "no_obs" &&
        std::string(ce.what()) == "no observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    // ivregress drops an excluded instrument collinear with the exogenous regressors and
    // the constant (_rmcoll2list) and then counts: with as many instruments as
    // instrumented variables, the drop leaves the equation under-identified, r(481)
    // (bank c42fd466 `ivregress 2sls price weight (mpg=w2)` with w2 = 2*weight,
    // d46cb2ad `(mpg=one)` with one = 1). With more instruments Stata fits without the
    // dropped one, which the engine does not: that stays the engine's refusal.
    if (ce.code == "iv_not_identified")
      return stataRefusalM4b("equation not identified; must have at least as many instruments "
                             "not in the regression as there are instrumented variables",
                             481);
    if (ce.code == "iv_instrument_collinear")
      return engineErrorBlockM4(
          "ivregress with an excluded instrument collinear with the regressors or another "
          "instrument is a valid Stata call that this version of PIET does not implement "
          "(Stata omits the instrument and fits on the others).",
          "unsupported_option");
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  // partial(): drop the partialled-out regressors and the constant from the
  // reported fit (dropCoefsM4b). The model Wald statistic then covers the
  // remaining slopes: with one left it is that coefficient's own z^2 (ivreg2
  // reports F(1,57) 137.81 = 1.94039^2/.1611033^2 * 57/60, :978); with more the
  // joint test needs the covariance the reply does not carry, so it is not
  // reported rather than left describing the full model.
  std::set<std::string> partialled;
  if (wantPartial) {
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (!raw.contains("coef") || !raw["coef"].is_object() || !raw["coef"].contains(pv)) {
        return engineErrorBlockM4("partial(): the fit has no coefficient for `" + pv + "`",
                                  "engine_error");
      }
      partialled.insert(pv);
    }
    partialled.insert("(intercept)");
    dropCoefsM4b(raw, partialled);
    raw.erase("chi2");
    raw.erase("chi2_df");
    if (raw.contains("coeftable") && raw["coeftable"].is_array() &&
        raw["coeftable"].size() == 1 && raw["coeftable"][0].contains("t") &&
        raw["coeftable"][0]["t"].is_number()) {
      const double t = raw["coeftable"][0]["t"].get<double>();
      raw["chi2"] = t * t;
      raw["chi2_df"] = 1;
    }
  }

  // first: one display text per endogenous regressor, built from the engine's
  // stage, then the stage is taken OUT of the cached reply -- `--savefirst`'s
  // first_stage/firsteqs are ivreg2's stored-estimate surface, which `first`
  // does not create (e(firsteqs) stays unset, as after a plain fit).
  ojson firstStages = ojson::array();
  std::vector<std::string> firstTexts;
  // The engine's stages as sent, before `first` below takes them out of the reply:
  // ffirst/savefirst/first all read e(first) from them, savefirst the stored estimates.
  const bool wantFirstMatrix = ivreg2Family && (wantFirst || wantFfirst || wantSavefirst);
  ojson savedFirst;
  if (wantFirstMatrix) {
    if (!raw.contains("first_stage") || !raw["first_stage"].is_object()) {
      return engineErrorBlockM4("ffirst/savefirst: the engine returned no first-stage estimates",
                                "engine_error");
    }
    savedFirst = raw["first_stage"];
    if (wantSavefirst && wantPartial) {
      return engineErrorBlockM4(
          "Option savefirst is a valid ivreg2 option that this version of PIET does not implement "
          "together with partial() (the stored first stages would have to be the partialled ones).",
          "unsupported_option");
    }
  }
  // BUG-1227: ivregress's own report takes its first stages; it is built below, once
  // e(sample) is installed (the R-squareds need the stage depvar's TSS over it).
  const bool firstReport = wantFirst && stataCmd == "ivregress" && reportOut != nullptr;
  std::string firstVcov = "iid", firstClustvar;
  if (wantFirst) {
    if (!raw.contains("first_stage") || !raw["first_stage"].is_object()) {
      return engineErrorBlockM4("first: the engine returned no first-stage estimates",
                                "engine_error");
    }
    std::string clustvar;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--cluster") clustvar = argv[i + 1];
    }
    std::string vcovArg = "iid";
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov") vcovArg = argv[i + 1];
    }
    firstVcov = vcovArg;
    firstClustvar = clustvar;
    const std::set<std::string> instrSet(iv.instruments.begin(), iv.instruments.end());
    for (const auto& endogName : iv.endog) {
      if (!raw["first_stage"].contains(endogName)) continue;
      ojson fs = raw["first_stage"][endogName];
      if (!partialled.empty()) dropCoefsM4b(fs, partialled);
      std::vector<std::string> names;
      for (const auto& n : fs.value("names", ojson::array())) {
        if (n.is_string() && !partialled.count(n.get<std::string>())) {
          names.push_back(n.get<std::string>());
        }
      }
      if (stataCmd == "ivregress") {
        // ivregress lists the stage as exog, excluded instruments, _cons
        // (:894-896 x2 z1 _cons; w2d_uncovered.log:2070-2074 w1 w2 z1 z2 _cons);
        // the engine's order is ivreg2's (excluded instruments first), which is
        // what ivreghdfe prints (:1078-1079 z1 x2).
        std::vector<std::string> ordered;
        for (const auto& n : names)
          if (!instrSet.count(n) && n != "(intercept)") ordered.push_back(n);
        for (const auto& n : names)
          if (instrSet.count(n)) ordered.push_back(n);
        for (const auto& n : names)
          if (n == "(intercept)") ordered.push_back(n);
        names = ordered;
      }
      fs["names"] = names;
      if (!firstReport) firstTexts.push_back(firstStageTextM4b(fs, names, vcovArg, clustvar));
      firstStages.push_back(fs);
    }
    raw.erase("first_stage");
    raw.erase("firsteqs");
  }
  if (wantFirstMatrix) {
    raw.erase("first_stage");
    raw.erase("firsteqs");
    if (wantFfirst && !wantFirst) firstTexts.push_back(ffirstSummaryTextM4b(savedFirst, iv.endog));
  }

  // Small-sample statistics (`ivregress ..., small`, and Stata's `ivreg`, which is
  // `_regress` with instruments) are t/F referenced: e(df_r) is N-K, or G-1 under a
  // one-way cluster (bank af8d994e `ivreg`: df 71, crit invttail(71,.025); eb7250c9
  // cluster(rep78): df 4). The engine sends the residual df as dof_resid only, so the
  // table's reference distribution and e(df_r) had nothing to read. Absorbed and
  // multiway fits post their own df_r below.
  if ((smallDefault || forceSmall) && stataCmd == "ivregress" && !sawAbsorb &&
      raw.is_object() && !raw.contains("df_r")) {
    const std::string vt = raw.value("vcov_type", std::string());
    const bool oneWayCluster = vt.rfind("cluster", 0) == 0;
    const bool multiway = vt.rfind("twoway", 0) == 0 || vt.rfind("multiway", 0) == 0;
    if (oneWayCluster && raw.contains("n_clusters") && raw["n_clusters"].is_number() &&
        raw["n_clusters"].get<double>() > 1)
      raw["df_r"] = raw["n_clusters"].get<double>() - 1.0;
    else if (!oneWayCluster && !multiway && raw.contains("dof_resid") &&
             raw["dof_resid"].is_number())
      raw["df_r"] = raw["dof_resid"];
  }
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // level(#) (ivregress / ivreg): the table's interval at the declared level on the
  // block's reference distribution (t(df_r) when the block carries a df_r, z otherwise).
  if (ivLevel.has_value() && block.contains("coeftable") && block["coeftable"].is_array()) {
    const double alpha = 1.0 - *ivLevel / 100.0;
    const bool tRef = block.contains("df_r") && block["df_r"].is_number() &&
                      block["df_r"].get<double>() > 0 && !isLiml;
    const bool tRefLiml = isLiml && (smallDefault || forceSmall) && block.contains("df_r") &&
                          block["df_r"].is_number() && block["df_r"].get<double>() > 0;
    const double crit = (tRef || tRefLiml) ? cecon::t_critical(alpha, block["df_r"].get<double>())
                                           : cecon::z_critical(alpha);
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
          !row["se"].is_number() || !(row["se"].get<double>() > 0))
        continue;
      const double b = row["b"].get<double>(), se = row["se"].get<double>();
      row["ci_lo"] = b - crit * se;
      row["ci_hi"] = b + crit * se;
    }
  }
  // The LIML reply carries its residual dof whatever `small` says; without `small` the
  // fit is large-sample (z, no e(df_r): erased from the cached reply below), so the
  // block's table is too (bank 293a8a77 `ivregress liml price weight (mpg=length)`:
  // r(table) df row missing, crit 1.959964).
  if (isLiml && stataCmd == "ivregress" && !(smallDefault || forceSmall)) block.erase("df_r");
  // The first stages print ABOVE the IV table, which is where the estimation
  // block's `warnings` render (console.js renderEstimation); the structured
  // stages ride along under `first_stage` for any consumer that reads numbers.
  if (wantFirst || wantPartial || !firstTexts.empty()) {
    ojson notes = block.contains("warnings") && block["warnings"].is_array() ? block["warnings"]
                                                                             : ojson::array();
    for (const auto& t : firstTexts) notes.push_back(t);
    if (wantPartial) {
      std::string list;
      for (const auto& pv : partialVars)
        if (pv != "_cons") list += pv + " ";
      notes.push_back("Partialled-out: " + list + "_cons (total SS, model F/chi2 and R2s are after "
                      "partialling-out; small-sample adjustments count the partialled-out "
                      "variables in K)");
    }
    if (!notes.empty()) block["warnings"] = notes;
    if (wantFirst) block["first_stage"] = firstStages;
  }
  // Estimator label (task requirement): "ivregress 2sls"/"ivregress gmm"/
  // "ivregress liml"/"ivreghdfe" — independent of cobra's own raw
  // "estimator" field ("iv2sls"/"liml") and independent of whether the
  // ivreghdfe form was typed as `ivreghdfe` or as the `reghdfe (...)` paren
  // form (both compute the identical cobra iv --fe call).
  block["estimator"] = (stataCmd == "ivregress") ? ("ivregress " + method) : std::string("ivreghdfe");

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "iv" or "liml" — margins' kLinearCmds already
                           // anticipates "iv" (pre-existing) and now "liml"
                           // (this batch's addition, see runMargins above).
  // FU-748: e(cmd) is the command typed -- `ivreghdfe` after ivreghdfe
  // (w2t_boottest_family.log:1197 D3, :1282 D4), `ivregress` after ivregress
  // (:1047 D1, :1411 D6) -- where it read the engine's `iv`. Set only when the
  // typed word is the Stata command this path answers for (ivregress, ivreghdfe,
  // ivreg2, xtivreg2): `ivreg` and reghdfe's IV paren form reach here under
  // another name and keep the engine's spelling, as before.
  if (lowerM4(parsed.cmd) == stataCmd) lm.stataCmd = stataCmd;
  // Stata's pre-11 `ivreg` posts e(cmd) `ivreg` (ivreg.ado `est local cmd ivreg`).
  else if (lowerM4(parsed.cmd) == "ivreg" && stataCmd == "ivregress") lm.stataCmd = "ivreg";
  lm.y = iv.y;
  // Stata's own ivregress coefficient table lists ENDOGENOUS regressors
  // first, then exogenous (oracle-verified: D1b's table shows `beertax`
  // before `emppop` even though `emppop` was typed BEFORE the paren block)
  // — matches cobra iv's own coef-dict insertion order too. test/lincom/
  // margins/predict after an IV fit are a KNOWN coarse approximation
  // (cobra's own test/lincom/predict CLIs always re-fit as plain OLS from
  // --y/--x — they have no concept of endog/instruments/gmm at all; this is
  // a pre-existing, disclosed limitation shared by every non-feols
  // estimator already wired into LastModel, not something introduced here).
  lm.x = iv.endog;
  for (const auto& e : iv.exog) lm.x.push_back(e);
  // BETA Phase G (estat endogenous/overid wiring): preserve the role split
  // `lm.x` above discards — see LastModel's own comment (session_state.h).
  lm.ivEndog = iv.endog;
  lm.ivExog = iv.exog;
  lm.ivInstruments = iv.instruments;
  if (sawAbsorb && allowAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  // liml included: its --vcov/--cluster are served now (cmd_liml, ivregress liml).
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) {
      for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
    }
  }
  lm.depvar = block.value("depvar", "");
  // ivreghdfe's e(vce) is ivreg2's spelling (ivreghdfe.ado posts ivreg2's
  // `vce`): `robust cluster` for one cluster variable, `robust two-way cluster`
  // for two, `robust` under vce(robust), and none for the default
  // (w2l_vce_shape_det.log ivreghdfe_ctl_cluster `|robust cluster|Robust|30|`,
  // :3340 ivreghdfe_vce_c2 `|robust two-way cluster|Robust|7|`, :3508
  // ivreghdfe_empty_cluster `0|ivreghdfe|||.|180`).
  if (lm.stataCmd == "ivreghdfe") {
    if (lm.vcov == "cluster") lm.eVce = "robust cluster";
    else if (lm.vcov == "twoway") lm.eVce = "robust two-way cluster";
    else if (lm.vcov == "iid") lm.eVce = "";
  }
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  // ivregress: e(small) is the macro `small` under the option and empty
  // otherwise (never the engine's 0/1), and a multiway cluster posts no
  // e(N_clust) (w2s_multiway_class.log:811 mw_iv_c2_names `|.|.|3|180||`, :875
  // mw_iv_c2s_names `|.|6|3|180|small|`).
  const bool ivregressTyped = lm.stataCmd == "ivregress";
  // xtivreg2: e(small) is ivreg2's macro (posted below), and e(df_r) exists only
  // under `small`: N-K-N_g, or N_clust-1 under cluster (ivreg2;
  // probe fe_iid_small 28, fe_cl_small 9).
  if (xtivreg2Typed && sawAbsorb && lm.result->is_object()) {
    lm.result->erase("small");
    if (smallDefault || forceSmall) {
      ojson& rr = *lm.result;
      if (lm.vcov == "cluster" && rr.contains("n_clusters") && rr["n_clusters"].is_number())
        rr["df_r"] = rr["n_clusters"].get<double>() - 1.0;
      else if (rr.contains("dof_resid") && rr["dof_resid"].is_number())
        rr["df_r"] = rr["dof_resid"];
    }
  }
  if (ivregressTyped && lm.result->is_object()) {
    lm.result->erase("small");
    // Without `small` ivregress reports large-sample z and chi2 statistics and posts
    // no e(df_r); the engine's LIML reply carries its residual dof regardless (bank
    // 447495d2 `ivregress liml price weight (mpg=length turn)`: e(df_r) ., where it
    // read 71; ed138da9 vce(cluster industry) and 9b0d511c vce(robust) alike).
    if (!(smallDefault || forceSmall)) lm.result->erase("df_r");
    // The model test (postIvregressMacrosM4b): no e(chi2) under `small` (e(F) instead),
    // nor after liml / gmm with noconstant; e(chi2) reads fall back on this reply.
    if ((smallDefault || forceSmall) || (ivNoConstant && (method == "liml" || method == "gmm")))
      lm.result->erase("chi2");
    if (lm.vcov == "twoway" || lm.vcov == "multiway") {
      // `small`: e(df_r) is the smallest cluster count less one (mw_iv_c2s
      // df_r 6 with G = 30 and 7).
      if (wantSmallOpt && !lm.result->contains("df_r") && lm.result->contains("n_clusters") &&
          (*lm.result)["n_clusters"].is_number())
        (*lm.result)["df_r"] = (*lm.result)["n_clusters"].get<double>() - 1.0;
      lm.result->erase("n_clusters");
    }
  }
  // ivregress under a one-way cluster VCE posts e(rank) = the clustered V's rank,
  // rowsof(V) - diag0cnt(invsym(V)), beside the model-based e(rank_mb); e(df_m) and
  // e(chi2) keep the column count (bank 886292d4 `ivregress 2sls price weight
  // (mpg=length turn), vce(cluster c3)`, 3 clusters: rank 2, rank_mb 3, df_m 2, chi2
  // 110.34; bbd11114 `... i.sid (beertax = ...), vce(cluster sid)`: the state
  // dummies' and _cons's cluster scores vanish, rank 2 of 50). Cached reply only.
  if (ivregressTyped && lm.vcov == "cluster" && lm.result->is_object() &&
      lm.result->contains("e_V") && (*lm.result)["e_V"].is_object() &&
      (*lm.result)["e_V"].contains("matrix") && (*lm.result)["e_V"]["matrix"].is_array()) {
    const ojson& m = (*lm.result)["e_V"]["matrix"];
    const size_t k = m.size();
    bool ok = k > 0;
    std::vector<double> V(k * k, 0.0);
    for (size_t r = 0; ok && r < k; ++r) {
      ok = m[r].is_array() && m[r].size() == k;
      for (size_t c = 0; ok && c < k; ++c) {
        ok = m[r][c].is_number();
        if (ok) V[r * k + c] = m[r][c].get<double>();
      }
    }
    if (ok) {
      const double rankMb = (*lm.result).contains("k_vars") && (*lm.result)["k_vars"].is_number()
                                ? (*lm.result)["k_vars"].get<double>()
                                : static_cast<double>(k);
      (*lm.result)["rank_mb"] = rankMb;
      (*lm.result)["k_vars"] = std::min(rankMb, static_cast<double>(glmVceRank(V, k)));
    }
  }
  // ivreghdfe's e(df_a) and e(df_r) (StataBE 19.5, BUG-1224: absorb(id) -> df_a 30, df_r
  // 148 = 180 - 2 - 30; absorb(id) cluster(id) -> df_a 0, df_r 29). df_a is reghdfe's
  // count of the absorbed coefficients (ivreghdfe HDFE.df_a), absorbedDofA's rule.
  // absorb() implies small (ivreghdfe), so e(df_r) is N - K - df_a, the
  // engine's dof_resid, or N_clust - 1 under cluster, N_clust being the smaller count of a
  // two-way cluster (ivreghdfe; the reply's n_clusters is that smallest).
  // Cached reply only (key == name fall-through), as after reghdfe.
  if (lm.stataCmd == "ivreghdfe" && sawAbsorb && lm.result->is_object()) {
    ojson& rr = *lm.result;
    if (const auto dfA = absorbedDofA(rr, "reghdfe")) rr["df_a"] = *dfA;
    const bool clustered = lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway";
    if (clustered && rr.contains("n_clusters") && rr["n_clusters"].is_number() &&
        rr["n_clusters"].get<double>() > 0)
      rr["df_r"] = rr["n_clusters"].get<double>() - 1.0;
    else if (!clustered && rr.contains("dof_resid") && rr["dof_resid"].is_number())
      rr["df_r"] = rr["dof_resid"];
  }
  // The small-sample e(rmse): sqrt(RSS / (N - K - sdofminus)), where the engine's
  // reply carries the asymptotic sqrt(RSS / N) whatever `small` says. K is the
  // posted coefficient count (_cons included when it is in e(b)); under ivreghdfe's
  // absorb() the absorbed coefficients are partialled out, sdofminus = e(df_a), and
  // at least the one constant when every absorbed category is nested in a cluster
  // variable (e(partial_ct) 1). Bank: b093f0fc `ivregress 2sls price weight
  // (mpg=length turn), small` 5691.836 = 5575.267 x sqrt(74/71); abbf51ac
  // noconstant: 74/72; 135149f2 ivreg2 `small` alike; 906be622 `ivreghdfe ...,
  // absorb(rep78)` 69/(69-2-5); 23841851 `... cluster(rep78)` (df_a 0) 69/(69-2-1);
  // ad940e9a absorb(rep78 foreign) 69/(69-2-6). Not under partial() or xtivreg2,
  // whose own dof conventions post their rmse. Cached reply only.
  if ((smallDefault || forceSmall) && !wantPartial && !xtivreg2Typed && lm.stataCmd != "ivreg" &&
      lm.result->is_object()) {
    ojson& rr = *lm.result;
    const auto numR = [&rr](const char* k) -> std::optional<double> {
      if (!rr.contains(k) || !rr[k].is_number()) return std::nullopt;
      return rr[k].get<double>();
    };
    // K is the model's rank (the residual dof N - rank of the fit), never the clustered
    // V's: under a one-way cluster VCE the block above has put that V-rank in k_vars
    // and kept the model rank as rank_mb (`..., small vce(cluster foreign)` with 2
    // clusters divides by N - 3 as b093f0fc does, not by N - 1).
    const auto n = numR("n_obs"), rmse = numR("rmse");
    const auto kv = numR("rank_mb") ? numR("rank_mb") : numR("k_vars");
    std::optional<double> sdof = 0.0;
    if (sawAbsorb) {
      auto dfA = numR("df_a");
      // every absorbed variable nested in a cluster variable: e(df_a) 0 (bank 0221736b
      // absorb(sid year) cluster(sid year): 336/(336-2-1)), a case absorbedDofA leaves.
      if (!dfA && rr.contains("absorbed") && rr["absorbed"].is_object() && !rr["absorbed"].empty() &&
          rr.contains("nested_fe") && rr["nested_fe"].is_array()) {
        size_t hit = 0;
        for (const auto& nmf : rr["nested_fe"])
          if (nmf.is_string() && rr["absorbed"].contains(nmf.get<std::string>())) ++hit;
        if (hit == rr["absorbed"].size()) dfA = 0.0;
      }
      sdof = (lm.stataCmd == "ivreghdfe" && dfA) ? std::optional<double>(std::max(*dfA, 1.0))
                                                 : std::nullopt;
    }
    if (n && rmse && kv && sdof && *n - *kv - *sdof > 0)
      rr["rmse"] = *rmse * std::sqrt(*n / (*n - *kv - *sdof));
  }
  // ivregress's absorb() (Stata 19): e(k_absorb) the absorbed categories, e(df_a) one
  // fewer per absorbed variable, with no redundancy between them (StataBE 19.5, BUG-1501,
  // 180 rows: absorb(id) 30 / 29, absorb(id id2) 37 / 35, absorb(id id2 id3) 42 / 39, and
  // absorb(id id4) 33 / 31 with id4 nested in id; the same under vce(robust),
  // vce(cluster id) and [aw=]). e(absvar) is posted with the model below. Cached reply
  // only, like ivreghdfe's e(df_a) above.
  const bool ivregressAbsorb = ivregressTyped && sawAbsorb && !lm.fe.empty() &&
                               lm.result->is_object() && lm.result->contains("absorbed") &&
                               (*lm.result)["absorbed"].is_object() &&
                               (*lm.result)["absorbed"].size() == lm.fe.size();
  if (ivregressAbsorb) {
    ojson& rr = *lm.result;
    double kAbsorb = 0.0;
    bool ok = true;
    for (const auto& f : lm.fe) {
      ok = ok && rr["absorbed"].contains(f) && rr["absorbed"][f].is_number();
      if (ok) kAbsorb += rr["absorbed"][f].get<double>();
    }
    if (ok) {
      rr["k_absorb"] = kAbsorb;
      rr["df_a"] = kAbsorb - static_cast<double>(lm.fe.size());
    }
  }
  // savefirst: the first stages are stored BEFORE the parent posts (see
  // storeIvreg2FirstStageM4b); e(firsteqs) lists the names that were stored.
  std::string firstEqs;
  // xtivreg2's panel scalars (xtivreg2): N_g the groups in the
  // estimation sample, g_min/g_avg/g_max their row counts (unweighted for aw/pw).
  std::optional<XtivPanelM4b> xtPanel;
  if (xtivreg2Typed && sawAbsorb && session.panelvar.has_value() &&
      lm.esample == LastModel::EsampleState::kColumn) {
    try {
      Datastore* ds = session.datastore();
      const std::string& pv = *session.panelvar;
      if (ds != nullptr && ds->has(pv) && ds->dtype(pv) == "f64" && ds->has(kEsampleColumn)) {
        const double* idv = ds->col(pv).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double serial = static_cast<double>(lm.esampleSerial);
        std::map<double, int64_t> sizes;
        int64_t nIn = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial) {
            ++sizes[idv[i]];
            ++nIn;
          }
        if (!sizes.empty()) {
          XtivPanelM4b xp;
          xp.N_g = static_cast<double>(sizes.size());
          int64_t mn = sizes.begin()->second, mx = mn;
          for (const auto& [k, c] : sizes) {
            (void)k;
            mn = std::min(mn, c);
            mx = std::max(mx, c);
          }
          xp.g_min = static_cast<double>(mn);
          xp.g_max = static_cast<double>(mx);
          xp.g_avg = static_cast<double>(nIn) / xp.N_g;
          xp.clustered = lm.vcov == "cluster";
          xp.ivar = pv;
          xp.tvar = session.timevar.value_or(std::string());
          xtPanel = xp;
        }
      }
    } catch (const std::exception&) {
      // an unreadable frame leaves the panel scalars unposted (missing)
    }
  }
  if (wantSavefirst) {
    for (const auto& endogName : iv.endog) {
      if (!savedFirst.contains(endogName)) continue;
      const ojson& fs = savedFirst[endogName];
      // ivreg2 `capture est store`: a name Stata cannot store is skipped.
      if (!fs.value("stored", false)) continue;
      // ... and so is one whose store fails here (the parent posts regardless).
      if (storeIvreg2FirstStageM4b(fs, lm, session, xtPanel ? &*xtPanel : nullptr).has_value())
        continue;
      firstEqs += (firstEqs.empty() ? "" : " ") + fs.value("eqname", std::string());
    }
  }
  session.setLastModel(lm);
  if (ivregressTyped && wantSmallOpt) {
    session.eval.setEString("small", "small");
    session.noteEOnFit("small");
  }
  // BUG-1501: e(absvar), the absorbed variables in absorb() order (StataBE 19.5 "id id2",
  // "id4 id"; e(absvars) stays empty).
  if (ivregressAbsorb) {
    std::string absvar;
    for (const auto& f : lm.fe) absvar += (absvar.empty() ? "" : " ") + f;
    session.eval.setEString("absvar", absvar);
    session.noteEOnFit("absvar");
  }
  // BUG-1189: e(clustvar) names every listed cluster variable (k3.do `id id2 id3`);
  // the fit itself, and so lm.cluster, has the first two.
  if (!allClusterVars.empty()) {
    session.eval.setEString("clustvar", allClusterVars);
    session.noteEOnFit("clustvar");
  }
  // FU-28 (BUG-759): e(vce) after a default-VCE `ivregress 2sls` is
  // `unadjusted` (w2d_uncovered.log:2187 ivregress_first_2endog_ctl, :2323
  // ivregress_factors_ctl, :2403 ivregress_nocons_ctl, :2536
  // ivregress_exog_before_ctl); setLastModel() published lm.vcov's engine
  // spelling `iid`, which the replay machinery keys on, so only the macro store
  // is corrected (regress `ols` and xtreg ,fe `conventional` do the same).
  // robust -> `robust` and cluster -> `cluster` are already Stata's, with
  // e(vcetype) `Robust`; `unadjusted` keeps the empty e(vcetype) setLastModel
  // gave iid. gmm and liml defaults have no captured arm and are left alone.
  if (stataCmd == "ivregress" && (method == "2sls" || method == "liml") && lm.vcov == "iid") {
    session.eval.setEString("vce", "unadjusted");
  }
  // `ivregress gmm` with neither vce() nor wmatrix(): the weight matrix and the VCE are
  // both the heteroskedasticity-robust ones (bank a9d50251: e(vce) robust, e(vcetype)
  // Robust, e(wmatrix) robust), which the engine's --gmm computes under its default.
  if (stataCmd == "ivregress" && method == "gmm" && lm.vcov == "iid") {
    bool given = false;
    for (const auto& [k0, v] : parsed.opts) {
      const std::string k = lowerM4(k0);
      given = given || k.rfind("vce", 0) == 0 || k.rfind("wm", 0) == 0 || k.rfind("rob", 0) == 0 ||
              k == "r" || k.rfind("cl", 0) == 0;
    }
    if (!given) {
      for (const auto& [nm, v] : {std::pair<const char*, const char*>{"vce", "robust"},
                                  {"vcetype", "Robust"}, {"wmatrix", "robust"}}) {
        session.eval.setEString(nm, v);
        session.noteEOnFit(nm);
      }
      // e(J) is not posted: Stata's Hansen J (bank a9d50251 .70484148) is not the
      // engine's hansen_j (.68377521, another weight matrix).
    }
  }
  session.lastEstBlock = block;
  // eb-order T-P1: e(V) is the fit's own covariance. Not after partial(): the
  // reported fit is not the one the engine's e_V describes, so e(V) stays the
  // named refusal there rather than a sub-block of another model's matrix.
  if (!wantPartial) session.attachFitCovariance(raw);
  // ivregress-escalars (w2j_e3_acreg.log:2148 I05_ivregress_ctl rss/tss/r2):
  // ivregress 2sls posts e(mss), the hidden e(tss), e(rss), e(r2),
  // e(r2_a) and e(df_m). The engine sends rmse = sqrt(RSS/N) (iv2sls.cpp, the
  // asymptotic scale whatever `small` says), so RSS = N*rmse^2; TSS is the
  // centred sum of squares of the depvar over e(sample). Published without absorb().
  // noconstant: e(tss) is the uncentred sum of squares, e(mss) = tss - rss, e(df_m) the
  // K regressors, and no e(r2)/e(r2_a) (bank c5b7a188 `..., noc`: tss 3447834321, rss
  // 453411227.98, df_m 2). [aw=]/[pw=]: the weights rescaled to sum to N, tss about the
  // weighted mean (bank 3abb5795 `[aweight=w]`: tss 623316809.8, rss = N rmse^2).
  const bool tssWeighted = ivWtype == "aw" || ivWtype == "pw";
  if (stataCmd == "ivregress" && !sawAbsorb && (!parsed.weightType.has_value() || tssWeighted) &&
      lm.esample == LastModel::EsampleState::kColumn && raw.contains("rmse") &&
      raw["rmse"].is_number() && raw.contains("n_obs") && raw["n_obs"].is_number()) {
    try {
      const double N = raw["n_obs"].get<double>();
      // BUG-1088: the engine sends TSS. Summing it here from the working frame
      // wrote the whole frame to disk to read two columns; that stays only for
      // an engine that does not send it.
      std::optional<double> tssOpt;
      if (!ivNoConstant && !tssWeighted && raw.contains("tss_centred") &&
          raw["tss_centred"].is_number()) {
        if (N > 0) tssOpt = raw["tss_centred"].get<double>();
      } else if (Datastore* ds = session.datastore();
                 ds != nullptr && ds->has(lm.depvar) && ds->dtype(lm.depvar) == "f64" &&
                 ds->has(kEsampleColumn) &&
                 (!tssWeighted || (ds->has(*parsed.weightVar) &&
                                   ds->dtype(*parsed.weightVar) == "f64"))) {
        const double* yv = ds->col(lm.depvar).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double* wv = tssWeighted ? ds->col(*parsed.weightVar).f64() : nullptr;
        const double serial = static_cast<double>(lm.esampleSerial);
        double n = 0, sw = 0, sum = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(yv[i])) {
            const double w = wv != nullptr ? wv[i] : 1.0;
            ++n;
            sw += w;
            sum += w * yv[i];
          }
        if (n == N && n > 0 && sw > 0) {
          const double ybar = ivNoConstant ? 0.0 : sum / sw;
          const double scale = n / sw;  // weights rescaled to sum to N
          double tss = 0;
          for (int64_t i = 0; i < ds->nRows(); ++i)
            if (mk[i] == serial && !std::isnan(yv[i]))
              tss += (wv != nullptr ? wv[i] * scale : 1.0) * (yv[i] - ybar) * (yv[i] - ybar);
          tssOpt = tss;
        }
      }
      if (tssOpt.has_value()) {
        const double tss = *tssOpt;
        const double rmse = raw["rmse"].get<double>();
        const double rss = rmse * rmse * N;
        const double K = block.contains("coeftable") && block["coeftable"].is_array()
                             ? static_cast<double>(block["coeftable"].size())
                             : 0.0;
        // e(r2) = mss/tss and e(r2_a) = 1 - (rss/tss)(N-1)/(N-K), each missing when it
        // falls outside its range: 2sls posts neither when r2 < 0 and a negative r2_a
        // as missing (bank f8add582 `... if foreign==0`: r2 .0123937, r2_a .); gmm and
        // liml set r2 missing outside [0, 1] and r2_a from it (bank cf66faf8 `ivregress
        // gmm wage ...`: mss < 0, r2 and r2_a .).
        const double nanv = std::numeric_limits<double>::quiet_NaN();
        double r2 = 1 - rss / tss;
        double r2a = (K > 0 && N > K) ? 1 - (1 - r2) * (N - 1) / (N - K) : nanv;
        if (method == "2sls") {
          if (r2 < 0) r2 = nanv;
          if (std::isnan(r2) || r2a < 0) r2a = nanv;
        } else {
          if (r2 < 0 || r2 > 1) r2 = nanv;
          if (std::isnan(r2)) r2a = nanv;
        }
        auto put = [&session](const std::string& nm, double v) {
          session.eval.setEScalar(nm, v);
          session.noteEOnFit(nm);
        };
        put("rss", rss);
        put("tss", tss);
        put("mss", tss - rss);
        if (ivNoConstant) {
          if (K > 0) put("df_m", K);
          session.eval.setEString("constant", "noconstant");
          session.noteEOnFit("constant");
        } else {
          put("r2", r2);
          if (K > 0 && N > K) put("r2_a", r2a);
          if (K > 0) put("df_m", K - 1);
        }
      }
    } catch (const std::exception&) {
      // a frame that cannot be read leaves these e() scalars unposted (missing)
    }
  }
  // BUG-1227: ivregress's `first` report (the "First-stage
  // regressions" title and its rule, then per endogenous regressor a blank line, the
  // stage's header, a blank line and its table). Each stage's TSS is its depvar's
  // sum of squares over e(sample) (centred unless the stage has no constant), read as
  // e(tss) is above, and only when the mask holds the stage's own row count.
  if (firstReport) {
    auto sumSq = [&](const std::string& var, double nExpect, bool centred) -> double {
      try {
        Datastore* ds = session.datastore();
        if (ds == nullptr || lm.esample != LastModel::EsampleState::kColumn ||
            parsed.weightType.has_value() || !ds->has(var) || ds->dtype(var) != "f64" ||
            !ds->has(kEsampleColumn))
          return std::nan("");
        const double* v = ds->col(var).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double serial = static_cast<double>(lm.esampleSerial);
        double n = 0, sum = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(v[i])) {
            ++n;
            sum += v[i];
          }
        if (n == 0 || n != nExpect) return std::nan("");
        const double mean = centred ? sum / n : 0.0;
        double tss = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(v[i])) tss += (v[i] - mean) * (v[i] - mean);
        return tss;
      } catch (const std::exception&) {
        return std::nan("");
      }
    };
    std::string text = "\nFirst-stage regressions\n-----------------------\n";
    for (const auto& fs : firstStages) {
      std::vector<std::string> names;
      for (const auto& n : fs.value("names", ojson::array()))
        if (n.is_string()) names.push_back(n.get<std::string>());
      const double nObs =
          fs.contains("n_obs") && fs["n_obs"].is_number() ? fs["n_obs"].get<double>() : -1.0;
      const bool hasCons = std::find(names.begin(), names.end(), "(intercept)") != names.end();
      const double tss = sumSq(fs.value("depvar", std::string()), nObs, hasCons);
      text += "\n" + ivregressFirstHeaderM4b(fs, tss, hasCons) + "\n" +
              firstStageTableM4b(fs, names, firstVcov, firstClustvar) + "\n";
    }
    ojson rep;
    rep["type"] = "display";
    rep["text"] = text;
    *reportOut = rep;
  }
  // ivreg2's identification statistics (ivreg2):
  // e(cdf) the Cragg-Donald F (one endogenous regressor: the first-stage F of the
  // excluded instruments; several: the minimum-eigenvalue statistic), e(rkf) the
  // Kleibergen-Paap rk Wald F under robust/cluster, and e(widstat) = rkf under
  // robust/cluster/kernel, else cdf (w2d_uncovered.log:1687 ivreg2_ffirst_ctl
  // `|128.5633117654573|` under cluster(g)). A statistic the engine did not compute
  // (a multiway cluster's rkf, say) stays unposted, i.e. missing.
  if (ivreg2Family && raw.is_object()) {
    auto put = [&session](const std::string& nm, double v) {
      if (!std::isfinite(v)) return;
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    auto num = [&raw](const char* key) {
      return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
    };
    // xtivreg2's own e() over ivreg2's (xtivreg2): N_g, df_a = N_g,
    // df_b = ivreg2's df_m (K), df_m = N_g + K, or K under cluster ("FEs with cluster
    // don't use up degrees of freedom"), sigma_e = rmse, singleton, xtmodel, ivar,
    // tvar; ivreg2 posts dofminus(N_g) and sdofminus 0. StataBE probe
    // (impl_F2/stata_probe/probe.log fe_iid df_m 12, fe_cl_ffirst df_m 2).
    if (xtivreg2Typed && xtPanel) {
      // partial(varlist): the partialled regressors stay in ivreg2's K and are its
      // sdofminus (e(partial_ct)), though the table no longer shows them (bank
      // 18be4322 `xtivreg2 frate unemp (beertax = ...), fe partial(unemp)`: df_b 2,
      // df_m 50, sdofminus 1).
      double nPartial = 0.0;
      if (wantPartial)
        for (const auto& pv : partialVars)
          if (pv != "_cons") nPartial += 1.0;
      const double K = (block.contains("coeftable") && block["coeftable"].is_array()
                            ? static_cast<double>(block["coeftable"].size())
                            : std::nan("")) +
                       nPartial;
      put("N_g", xtPanel->N_g);
      put("df_a", xtPanel->N_g);
      put("df_b", K);
      put("df_m", xtPanel->clustered ? K : xtPanel->N_g + K);
      put("g_min", xtPanel->g_min);
      put("g_avg", xtPanel->g_avg);
      put("g_max", xtPanel->g_max);
      put("sigma_e", num("rmse"));
      put("rmse", num("rmse"));
      put("dofminus", xtPanel->N_g);
      put("sdofminus", nPartial);
      put("singleton", num("n_dropped_singletons"));
      // ivreg2's e(small) is `small` or empty and its e(vce) `robust cluster` /
      // `robust` / empty (probe R2 rows), not the engine's 0/1 and iid/hc1/cluster.
      const std::string vceMacro =
          lm.vcov == "cluster" ? "robust cluster" : (lm.vcov == "hc1" ? "robust" : "");
      const bool smallOn = raw.contains("small") && raw["small"].is_boolean() && raw["small"].get<bool>();
      for (const auto& [nm, v] :
           {std::pair<std::string, std::string>{"xtmodel", "fe"}, {"ivar", xtPanel->ivar},
            {"tvar", xtPanel->tvar}, {"predict", "xtivreg2_p"}, {"vce", vceMacro},
            {"small", smallOn ? "small" : ""}}) {
        session.eval.setEString(nm, v);
        session.noteEOnFit(nm);
      }
    }
  }
  if ((ivreg2Family || stataCmd == "ivreghdfe") && raw.is_object()) {
    auto put = [&session](const std::string& nm, double v) {
      if (!std::isfinite(v)) return;
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    auto num = [&raw](const char* key) {
      return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
    };
    double cdf = std::nan("");
    if (iv.endog.size() == 1 && raw.contains("first_stage_F") && raw["first_stage_F"].is_object() &&
        raw["first_stage_F"].contains(iv.endog[0]) && raw["first_stage_F"][iv.endog[0]].is_number())
      cdf = raw["first_stage_F"][iv.endog[0]].get<double>();
    else if (iv.endog.size() > 1)
      cdf = num("cragg_donald_F");
    const bool robustVce = lm.vcov == "hc1" || lm.vcov == "cluster";
    const double rkf = robustVce ? num("kp_wald_F") : std::nan("");
    // xtivreg2: the identification statistics carry dofminus/sdofminus conventions
    // the engine's kp/cdf numbers are not verified against, so they stay unposted.
    if (ivreg2Typed) {
    put("cdf", cdf);
    put("rkf", rkf);
    put("widstat", robustVce ? rkf : (lm.vcov == "iid" ? cdf : std::nan("")));
    }
    if (ivreg2Typed && robustVce && iv.endog.size() == 1) {
      put("idstat", num("kp_lm"));
      put("idp", num("kp_lm_p"));
      put("iddf", num("kp_lm_df"));
    }
    // ivreg2's e() contract of a 2SLS fit (postIvreg2ContractM4b)
    const bool xtFe = xtivreg2Typed && xtPanel.has_value() && sawAbsorb;
    const bool hdfeTyped = stataCmd == "ivreghdfe" && lowerM4(parsed.cmd) == "ivreghdfe";
    // ivreghdfe's absorption bookkeeping (reghdfe's rule, as linear_estimation.cpp posts
    // it for reghdfe, for one or two absorbed variables): each has K categories, M
    // redundant -- all of them when nested in a cluster variable, else one for every
    // variable after the first (or for the first too when a nested one exists), the
    // second taking the connected components it forms with the first
    std::vector<std::vector<double>> hdfeDof;
    bool hdfeOk = hdfeTyped && (!sawAbsorb || (lm.fe.size() >= 1 && lm.fe.size() <= 2 &&
                                               raw.contains("absorbed") && raw["absorbed"].is_object()));
    if (hdfeTyped && sawAbsorb && hdfeOk) {
      std::set<std::string> nestedSet;
      if (raw.contains("nested_fe") && raw["nested_fe"].is_array())
        for (const auto& n : raw["nested_fe"])
          if (n.is_string()) nestedSet.insert(n.get<std::string>());
      bool anyNested = false;
      for (const auto& f : lm.fe) {
        if (!raw["absorbed"].contains(f) || !raw["absorbed"][f].is_number()) hdfeOk = false;
        anyNested = anyNested || nestedSet.count(f) > 0;
      }
      size_t rest = 0;
      for (size_t g = 0; hdfeOk && g < lm.fe.size(); ++g) {
        const double Kg = raw["absorbed"][lm.fe[g]].get<double>();
        const bool nst = nestedSet.count(lm.fe[g]) > 0;
        double M = nst ? Kg : ((rest > 0 || anyNested) ? 1.0 : 0.0);
        if (!nst) ++rest;
        hdfeDof.push_back({Kg, M, Kg - M, 0.0, nst ? 1.0 : 0.0});
      }
      if (hdfeOk && lm.fe.size() == 2 && !anyNested) {
        if (!raw.contains("k_absorbed") || !raw["k_absorbed"].is_number()) hdfeOk = false;
        else {
          const double comps = hdfeDof[0][0] + hdfeDof[1][0] - raw["k_absorbed"].get<double>();
          hdfeDof[1][1] = comps;
          hdfeDof[1][2] = hdfeDof[1][0] - comps;
        }
      }
    }
    // an instrument or regressor the engine omitted (collinear) changes ivreg2's lists
    // and counts, which the contract does not model: nothing is posted then
    const bool engineOmitted =
        (raw.contains("omitted") && raw["omitted"].is_array() && !raw["omitted"].empty()) ||
        (raw.contains("omitted_exog") && raw["omitted_exog"].is_array() &&
         !raw["omitted_exog"].empty());
    if ((ivreg2Typed || xtFe || (hdfeTyped && hdfeOk)) && method == "2sls" && !wantPartial &&
        !engineOmitted &&
        !parsed.weightType.has_value() &&
        !ivMultiway && (!sawAbsorb || xtFe || hdfeTyped) && !lm.depvar.empty() &&
        (lm.vcov == "iid" || lm.vcov == "hc1" || (lm.vcov == "cluster" && lm.cluster.size() == 1))) {
      // e(b) of a noconstant fit is read through the block's noconstant mark (set again
      // below for every IV fit)
      if (ivNoConstant && session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
        (*session.lastEstBlock)["noconstant"] = true;
      Ivreg2ContractIn in;
      in.depvar = lm.depvar;
      in.endog = iv.endog;
      in.exog = iv.exog;
      in.exexog = iv.instruments;
      in.cons = !ivNoConstant;
      in.small = raw.contains("small") && raw["small"].is_boolean() && raw["small"].get<bool>();
      in.vce = lm.vcov;
      if (lm.vcov == "cluster") in.clusterVar = lm.cluster.front();
      in.first = wantFirst;
      if (xtFe) {
        in.ivar = xtPanel->ivar;
        in.dofminus = xtPanel->N_g;
        in.version = "01.0.17";
        in.predict = "xtivreg2_p";
        in.kpLm = num("kp_lm");
        in.kpWaldF = num("kp_wald_F");
      }
      if (hdfeTyped) {
        in.kpLm = num("kp_lm");
        in.kpWaldF = num("kp_wald_F");
        in.ivreg2cmd = "ivreghdfe";
        in.version = "04.1.11";
        in.predict = sawAbsorb ? "reghdfe_p" : "reghdfe";
        in.hdfeTags = true;
        if (sawAbsorb) {
          in.fe = lm.fe;
          in.dofTable = hdfeDof;
          double dfa = 0;
          for (const auto& r : hdfeDof) dfa += r[2];
          in.sdofminus = dfa > 0 ? dfa : 1.0;
          const double singles = num("n_dropped_singletons");
          put("N_full", num("n_obs") + (std::isfinite(singles) ? singles : 0.0));
          put("num_singletons", std::isfinite(singles) ? singles : 0.0);
          put("drop_singletons", sawKeepSingletons ? 0.0 : 1.0);
        }
      }
      in.firstStats = wantFirst || wantFfirst || wantSavefirst;
      (void)postIvreg2ContractM4b(session, lm, in, &block);
    }
    // Two-way cluster: e(N_clust) is the smaller count, and each dimension's own
    // (ivreg2; w2d_uncovered.log:1866 `|5|5|6|`).
    if (ivreg2Family && ivMultiway && raw.contains("n_clusters_by") && raw["n_clusters_by"].is_array() &&
        raw["n_clusters_by"].size() == 2) {
      for (size_t d = 0; d < 2; ++d)
        if (raw["n_clusters_by"][d].is_number())
          put("N_clust" + std::to_string(d + 1), raw["n_clusters_by"][d].get<double>());
    }
    // e(first): rows rmse ... APr2, one column per endogenous regressor
    // (ivreg2; posted with first, ffirst or savefirst).
    if (ivreg2Family && wantFirstMatrix) {
      static const std::vector<std::string> kRows = {
          "rmse", "sheapr2", "pr2", "F", "df", "df_r", "pvalue",
          "SWF", "SWFdf1", "SWFdf2", "SWFp", "SWchi2", "SWchi2p", "SWr2",
          "APF", "APFdf1", "APFdf2", "APFp", "APchi2", "APchi2p", "APr2"};
      eval::EvalMatrix m;
      m.rownames = kRows;
      m.data.assign(kRows.size(), {});
      bool complete = !iv.endog.empty();
      for (const auto& e : iv.endog) {
        if (!savedFirst.contains(e) || !savedFirst[e].contains("ffirst") ||
            !savedFirst[e]["ffirst"].is_array() || savedFirst[e]["ffirst"].size() != kRows.size()) {
          complete = false;
          break;
        }
        m.colnames.push_back(e);
        for (size_t r = 0; r < kRows.size(); ++r) {
          const ojson& v = savedFirst[e]["ffirst"][r];
          m.data[r].push_back(v.is_number() ? v.get<double>() : std::nan(""));
        }
      }
      if (complete) {
        session.eval.eMatrices.emplace_back("first", std::move(m));
        session.noteEOnFit("first");
      }
    }
    if (ivreg2Family && wantSavefirst) {
      session.eval.setEString("firsteqs", firstEqs);
      session.noteEOnFit("firsteqs");
    }
  }
  // A noconstant fit's e(b) has no `_cons` (resolveEMatrixFor's kNeedCons
  // reads this mark, as after `regress ..., noconstant`).
  if (ivNoConstant && session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
  // Stata's ordered `: e(scalars)' after the built-in multiway 2SLS fit
  // (w2s_multiway_class mw_iv_c2_scalars); every other IV fit keeps the refusal.
  session.eNameContract.reset();
  if (stataCmd == "ivregress" && method == "2sls" && !ivNoConstant && !sawAbsorb &&
      !wantSmallOpt && !wantPartial && !parsed.weightType.has_value() &&
      (lm.vcov == "twoway" || lm.vcov == "multiway"))
    postENameListsIvregressM4(session, raw, lm.ivEndog.size());
  if (stataCmd == "ivregress")
    postIvregressMacrosM4b(session, raw, method, iv, parsed, ivNoConstant);
  if (lm.stataCmd == "ivreg") {
    postIvregEResultsM4b(session, raw, lm, iv, ivNoConstant, parsed);
    // The header's F is e(F): missing when the fit has fewer clusters than it needs.
    if (block.is_object() && block.contains("F")) {
      const double* eF = session.eval.eScalars.find("F");
      if (eF != nullptr && std::isnan(*eF)) {
        block["F"] = nullptr;
        if (block.contains("p_F")) block["p_F"] = nullptr;
      }
    }
  }
  return block;
}

ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                    const std::string& stataCmd, SessionState& session, bool smallDefault,
                    bool forceSmall) {
  return runIvRegress(method, parsed, allowAbsorb, stataCmd, session, smallDefault, forceSmall,
                      nullptr);
}

// ===========================================================================
// P7 — IV maximum likelihood: ivprobit / ivtobit
//
// `ivprobit ybin x1 (endog=z), vce(robust)` — the same `(endog = instruments)`
// varlist shape ivregress already parses, so parseIvParenM4 is reused rather
// than given a second, subtly different implementation.

// Install the engine's actual named full parameter vector/covariance. The
// displayed structural table stays compact; every stored coefficient reader
// sees the full equation stripe on the session block.
static bool postNamedMleFitM4b(SessionState& session, const ojson& raw) {
  if (!raw.contains("e_b") || !raw.contains("e_V") || !session.lastEstBlock.has_value()) return false;
  const auto& eb = raw["e_b"];
  const auto& ev = raw["e_V"];
  if (!eb.contains("names") || !eb.contains("values") || !ev.contains("names") ||
      !ev.contains("matrix") || eb["names"] != ev["names"]) return false;
  const size_t k = eb["names"].size();
  if (eb["values"].size() != k || ev["matrix"].size() != k) return false;
  eval::EvalMatrix b, V;
  b.rownames = {"y1"};
  b.data.emplace_back();
  ojson ct = ojson::array();
  for (size_t i = 0; i < k; ++i) {
    if (!eb["names"][i].is_string() || !eb["values"][i].is_number() ||
        !ev["matrix"][i].is_array() || ev["matrix"][i].size() != k) return false;
    const std::string name = eb["names"][i].get<std::string>();
    const double value = eb["values"][i].get<double>();
    const double se = std::sqrt(std::max(0.0, ev["matrix"][i][i].get<double>()));
    b.colnames.push_back(name);
    b.data[0].push_back(value);
    V.colnames.push_back(name);
    V.rownames.push_back(name);
    V.data.push_back(ev["matrix"][i].get<std::vector<double>>());
    ct.push_back(ojson{{"name", name}, {"b", value}, {"se", se}});
  }
  (*session.lastEstBlock)["coeftable"] = std::move(ct);
  (*session.lastEstBlock)["e_V"] = ev;
  session.eval.eMatrices.emplace_back("b", std::move(b));
  session.eval.eMatrices.emplace_back("V", std::move(V));
  return true;
}

// BUG-1500: the two-step fit of runIvMl, `ivprobit ..., twostep`. The estimator
// has its own covariance and no ML option (StataBE 19.5, scratch-laneA2 feat/i1.do):
// vce(robust) / vce(cluster id) / vce(oim) are r(198) "vcetype robust not allowed",
// the `robust` and `cluster(id)` options r(198) "option robust not allowed" /
// "option cluster(id) not allowed", and vce(twostep) is the default. vce(bootstrap)
// and vce(jackknife) are served by Stata and refused here by name. After the fit
// Stata posts e(method) e(vce) "twostep", e(chi2type) "Wald", e(predict)
// "ivprobit_p", e(depvar) "ybe xe", e(exog) "x1 z2" (exogenous, then the
// instruments), e(endog) "xe" and e(properties) "b V"; the e() scalars come from
// the engine reply (chi2 df_m p_chi2 chi2_exog df_exog p_exog k_vars N_cds N_cdf).
static ojson runIvProbitTwostepM4b(const eval::ParsedCommand& parsed, SessionState& session,
                                   std::vector<std::string> argv, const IvParenResult& iv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "robust") return stataRefusalM4b("option robust not allowed", 198);
    if (k == "cluster")
      return stataRefusalM4b(v.isBool ? std::string("option cluster not allowed")
                                      : "option cluster(" + trimM4(v.strValue) + ") not allowed",
                             198);
    if ((k == "vce" || k == "vcov") && !v.isBool) {
      const std::string raw = trimM4(v.strValue);
      const std::string word = lowerM4(raw.substr(0, raw.find_first_of(" \t,")));
      if (word == "twostep" && raw.find_first_of(" \t,") == std::string::npos) continue;
      if (word == "bootstrap" || word == "jackknife")
        return engineErrorBlockM4("Option `vce(" + word + ")` is a valid Stata option that this "
                                  "version of PIET does not implement for `ivprobit, twostep`.",
                                  "unsupported_option");
      const std::string canon = canonicalVceType(raw, nullptr);
      return stataRefusalM4b("vcetype " + (canon.empty() ? word : canon) + " not allowed", 198);
    }
  }
  argv.push_back("--twostep");
  pushLevelIfGivenM4(parsed, argv);
  argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7Mle("ivprobit", argv, session, &raw);
  if (block.value("type", std::string()) == "error") return block;
  if (!postNamedMleFitM4b(session, raw))
    return engineErrorBlockM4("the two-step fit returned an incomplete parameter covariance");
  auto putE = [&](const char* name, const std::string& value) {
    session.eval.setEString(name, value);
    session.noteEOnFit(name);
  };
  // e(vce) is republished by every model install, as logit's `oim` (FU-30).
  session.eval.setEString("vce", "twostep");
  putE("method", "twostep");
  putE("chi2type", "Wald");
  putE("predict", "ivprobit_p");
  putE("properties", "b V");
  std::vector<std::string> exog(iv.exog);
  exog.insert(exog.end(), iv.instruments.begin(), iv.instruments.end());
  putE("depvar", iv.y + " " + joinSpaceM4(iv.endog));
  putE("exog", joinSpaceM4(exog));
  putE("endog", joinSpaceM4(iv.endog));
  // e(p), the model test's p-value; e(p) resolves from the reply's `p_F`, which a
  // chi2 test does not carry.
  if (raw.contains("p_chi2") && raw["p_chi2"].is_number()) {
    session.eval.setEScalar("p", raw["p_chi2"].get<double>());
    session.noteEOnFit("p");
  }
  return block;
}

ojson runIvMl(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
              SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-1500: `ivprobit ..., twostep`, Newey's two-step estimator (the engine's
  // --twostep; cmds_ivml.cpp run_ivprobit_twostep). ivtobit's two-step form is
  // not implemented.
  bool twostep = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "twostep" && v.isBool && v.boolValue) twostep = true;
  if (twostep && cobraCmd == "ivtobit")
    return engineErrorBlockM4("Option `twostep` is a valid Stata option that this version of PIET "
                              "does not implement for `ivtobit`.", "unsupported_option");
  // ivprobit/ivtobit keep refusing exogenous regressors after the block: no
  // captured arm measures that spelling for them.
  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/false);
  // ivprobit.ado / ivtobit.ado (_iv_parse): `(x = )` is r(481) "no instrument
  // variables specified" (vs-stata bank d2722911 `ivprobit foreign mpg (weight = )`).
  if (iv.errored && iv.noInstruments)
    return stataRefusalM4b("no instrument variables specified", 481);
  if (iv.errored) return engineErrorBlockM4("syntax_error: " + cobraCmd + " " + iv.errorMsg,
                                            "syntax_error");
  // Fewer instruments than endogenous regressors: r(481) "equation not
  // identified; must have at least as many instruments not in the regression as
  // there are instrumented variables" (bank 0a8962ca `ivprobit foreign (weight
  // mpg = length)`). The counts are of the EXPANDED lists, as Stata counts them
  // (`length-displacement` is three instruments, `t*` every t-variable). A list
  // whose expanded length this check cannot know exactly (factor or time-series
  // operators, other wildcard forms) is not judged here: the fit's own path
  // answers for it.
  const auto expandedCount = [&session](const std::vector<std::string>& toks)
      -> std::optional<size_t> {
    for (const auto& t : toks) {
      if (t.find_first_of(".#()?~") != std::string::npos) return std::nullopt;
      const size_t star = t.find('*');
      if (star != std::string::npos && star + 1 != t.size()) return std::nullopt;
    }
    if (toks.empty()) return size_t{0};
    const loops::VarlistExpandResult ex = loops::expandVarlist(joinSpaceM4(toks), session);
    if (ex.isError) return std::nullopt;
    return ex.tokens.size();
  };
  const std::optional<size_t> nInst = expandedCount(iv.instruments);
  const std::optional<size_t> nEndog = expandedCount(iv.endog);
  if (nInst.has_value() && nEndog.has_value() && *nInst < *nEndog)
    return stataRefusalM4b(
        "equation not identified; must have at least as many instruments not in the regression "
        "as there are instrumented variables",
        481);
  std::vector<std::string> argv = {cobraCmd, "--y", iv.y};
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  argv.push_back("--endog");
  argv.push_back(joinCommaM4(iv.endog));
  argv.push_back("--instruments");
  argv.push_back(joinCommaM4(iv.instruments));
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "ivtobit") {
    bool sawLimit = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "ll" || k == "ul") && !v.isBool) {
        // Stata's bare `ll`/`ul` (censor at the observed extreme) is a different
        // request from `ll(0)`, and the engine has no bare form — rejecting is
        // parity; guessing a limit would answer a different question.
        argv.push_back(k == "ll" ? "--ll" : "--ul");
        argv.push_back(trimM4(v.strValue));
        sawLimit = true;
      } else if ((k == "ll" || k == "ul") && v.isBool) {
        return engineErrorBlockM4(
            "Option `" + k + "` without a value is a valid Stata option that this version of PIET "
            "does not implement for `ivtobit` — give an explicit limit, e.g. ll(0).",
            "unsupported_option");
      }
    }
    if (!sawLimit)
      return engineErrorBlockM4("syntax_error: ivtobit requires at least one of ll()/ul()",
                                "syntax_error");
    // ll(a) ul(b) with a >= b censors every observation on one side: r(2000) "no
    // uncensored observations" (vs-stata bank 8c4e434f `ivtobit mpg trunk (weight
    // = length displacement), ll(30) ul(17)`).
    {
      std::optional<double> lo, hi;
      for (const auto& [k, v] : parsed.opts) {
        if ((k != "ll" && k != "ul") || v.isBool) continue;
        const std::string a = trimM4(v.strValue);
        char* end = nullptr;
        const double x = std::strtod(a.c_str(), &end);
        if (a.empty() || end == nullptr || *end != '\0') continue;
        (k == "ll" ? lo : hi) = x;
      }
      if (lo.has_value() && hi.has_value() && *lo >= *hi)
        return stataRefusalM4b("no uncensored observations", 2000);
    }
  }
  if (twostep) return runIvProbitTwostepM4b(parsed, session, std::move(argv), iv);
  // vce
  // FU-23 (BUG-756): ivprobit/ivtobit take the cluster VCE ONLY as
  // vce(cluster var). w2l_vce_shape.log refuses every other spelling r(198):
  // the cluster() option in any shape -- `cluster(id)` (ivprobit_ctl_cluster
  // "option cluster(id) not allowed"), bare `cluster`, `cluster()`,
  // `cluster(id id2)` -- a valued `robust(x1)`, `vce(cluster id id2)`
  // ("multiway cluster VCE not allowed"), `vce(cluster)` ("option vce(cluster
  // clustvar) incorrectly specified") and `vce(robust x1)` ("option vce()
  // incorrectly specified"); ivtobit alike. This loop used to fit a clustered
  // model for cluster(id), skip a bare `cluster` and `robust(x1)` (iid at rc 0),
  // drop the `x1` of vce(robust x1), and hand `id id2` to the engine as ONE
  // column name (rc 1).
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster != OptShapeM4b::kAbsent) {
    return stataRefusalM4b(vceShape.cluster == OptShapeM4b::kBare
                               ? std::string("option cluster not allowed")
                               : "option cluster(" + vceShape.clusterRaw + ") not allowed",
                           198);
  }
  if (vceShape.robust == OptShapeM4b::kEmpty || vceShape.robust == OptShapeM4b::kOne ||
      vceShape.robust == OptShapeM4b::kMany) {
    return stataRefusalM4b("option robust(" + vceShape.robustRaw + ") not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.size() > 1) {
    return stataRefusalM4b("multiway cluster VCE not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce(cluster clustvar) incorrectly specified", 198);
  }
  if ((vceShape.vceType == "robust" || vceShape.vceType == "hc1" || vceShape.vceType == "iid") &&
      !vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce() incorrectly specified", 198);
  }
  std::optional<std::string> vceVal;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
  }
  std::string want, cvar;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: one canonicaliser (piet/handlers.h), measured abbreviation floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (lv == "cluster") {
      want = "cluster";
      cvar = vceRest;
    } else {
      want = lv.empty() ? lowerM4(*vceVal) : lv;
    }
  } else if (robustFlag) {
    want = "robust";
  }
  if (!want.empty()) {
    if (want != "iid" && want != "robust" && want != "cluster")
      return engineErrorBlockM4(
          "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
          "implement for `" + cobraCmd + "`.", "unsupported_option");
    argv.push_back("--vce");
    argv.push_back(want);
    if (want == "cluster") {
      argv.push_back("--cluster");
      argv.push_back(cvar);
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7Mle(cobraCmd, argv, session, &raw);
  if (block.value("type", std::string()) == "error") return block;
  if (!postNamedMleFitM4b(session, raw))
    return engineErrorBlockM4("the IV-ML fit returned an incomplete parameter covariance");
  return block;
}

// ===========================================================================
// reg3 (BS-065): `reg3 (y1 x1 x2) (y2 x3 y1) [(y3 ...) ...] [, sure]`


}  // namespace piet::handlers
