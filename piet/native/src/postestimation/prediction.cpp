// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Prediction.
#include "piet/handlers/postestimation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

namespace postest_detail {

std::string nextPathEst(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-516: does every column the cached model READS still hold, in the LIVE
// frame, exactly what it held in the estimation-time frame, row for row?
//
// Only the absorbed-FE family asks. The engine's by-name prediction path
// (`--fit-data`) refuses `--fe` -- an absorbed effect cannot be evaluated on a
// row the fit never saw -- so once the head has moved, the one way left to
// predict on the live frame is to fit THERE. That is the fitted model if and
// only if the answer here is yes, and the answer is MEASURED, never assumed: a
// `replace x = ...`, a `drop if`, a `sort` between the fit and the predict each
// leave a frame that refits to different coefficients at rc 0. The ordinary
// history -- an earlier `predict`, a `generate` (w2_smalls.log section C is
// exactly that) -- only ADDS columns, and passes.
//
// The column set is "every identifier in y/x/fe that the estimation-time frame
// confirms is a column" -- column_fetch.h's predicate: never miss a real column.
// An operator prefix (`i`, `c`, `L2`) that happens to also be a column is
// compared too, which can only refuse more. A term that names NO known column
// (an abbreviation, a wildcard) cannot be verified, and refuses.
bool fitColumnsUnchangedM4(SessionState& session, const LastModel& lm, std::string& why,
                           const std::vector<std::string>& extraTerms = {}) {
  Datastore* live = nullptr;
  std::unique_ptr<Datastore> fit;
  try {
    live = session.datastore();
    fit = std::make_unique<Datastore>(lm.dataPath);
  } catch (const std::exception& e) {
    why = std::string("the estimation-time data could not be read back (") + e.what() + ")";
    return false;
  } catch (...) {
    why = "the estimation-time data could not be read back";
    return false;
  }
  if (live == nullptr) {
    why = "no dataset is in memory";
    return false;
  }
  if (live->nRows() != fit->nRows()) {
    why = "the number of observations changed (" + std::to_string(fit->nRows()) +
          " at the fit, " + std::to_string(live->nRows()) + " now)";
    return false;
  }
  std::set<std::string> cols;
  std::vector<std::string> terms = lm.x;
  terms.push_back(lm.y);
  terms.insert(terms.end(), lm.fe.begin(), lm.fe.end());
  terms.insert(terms.end(), extraTerms.begin(), extraTerms.end());
  for (const std::string& term : terms) {
    if (term.empty()) continue;
    bool any = false;
    for (size_t i = 0; i < term.size();) {
      const unsigned char c = static_cast<unsigned char>(term[i]);
      if (!std::isalpha(c) && c != '_') {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < term.size() &&
             (std::isalnum(static_cast<unsigned char>(term[j])) || term[j] == '_')) {
        ++j;
      }
      const std::string tok = term.substr(i, j - i);
      if (fit->has(tok)) {
        cols.insert(tok);
        any = true;
      }
      i = j;
    }
    if (!any) {
      why = "`" + term + "` names no variable of the estimation-time data";
      return false;
    }
  }
  // FU-768: a restricted fit refits under its `if` on the live frame, so every
  // column that expression reads is compared too (a name that is no column -- a
  // function, `_n` -- is not data and is skipped).
  for (size_t i = 0; i < lm.fitKeepif.size();) {
    const unsigned char c = static_cast<unsigned char>(lm.fitKeepif[i]);
    if (!std::isalpha(c) && c != '_') {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < lm.fitKeepif.size() &&
           (std::isalnum(static_cast<unsigned char>(lm.fitKeepif[j])) || lm.fitKeepif[j] == '_')) {
      ++j;
    }
    const std::string tok = lm.fitKeepif.substr(i, j - i);
    if (fit->has(tok)) cols.insert(tok);
    i = j;
  }
  const size_t n = static_cast<size_t>(fit->nRows());
  for (const std::string& name : cols) {
    if (!live->has(name)) {
      // A string condition's row mask (piet/datastore.h kQualifierColumnPrefix)
      // lives on the fit's frame only.
      why = isQualifierColumn(name)
                ? std::string("the fit's string `if` condition is not on the data in memory")
                : "variable " + name + " is no longer in the data";
      return false;
    }
    const cecon::Column& a = fit->col(name);
    const cecon::Column& b = live->col(name);
    bool same = a.dtype == b.dtype;
    if (same && a.dtype == cecon::DType::F64) {
      const double* pa = a.f64();
      const double* pb = b.f64();
      same = pa != nullptr && pb != nullptr;
      // Missing == missing: NaN never compares equal to itself, and a column that
      // was only ever carried forward must not read as "changed".
      for (size_t r = 0; same && r < n; ++r) {
        same = pa[r] == pb[r] || (std::isnan(pa[r]) && std::isnan(pb[r]));
      }
    } else if (same) {
      const int32_t* ca = a.codes();
      const int32_t* cb = b.codes();
      same = ca != nullptr && cb != nullptr && a.levels == b.levels;
      for (size_t r = 0; same && r < n; ++r) same = ca[r] == cb[r];
    }
    if (!same) {
      why = "variable " + name + " changed";
      return false;
    }
  }
  return true;
}

// ===========================================================================
// W2B predict floors (BUG-596). The statistics `predict` answers after the four
// least-squares commands the AER corpus leans on, per their own predict
// programs as StataBE answered them (tests/eval/golden/aer_w2/w2b_estim.log,
// the predict_* arms; aer_b4/predict-e-xbd.log for the reghdfe `resid` rule,
// the xtreg,fe "not allowed" pair and the estimation-sample masking):
//
//   regress   xb residuals score stdp stdf stdr rstandard rstudent leverage|hat
//             cooksd (`e`, `d`: r(198) "option e not allowed")
//   areg      xb stdp dresiduals, and on e(sample) only: xbd d residuals score
//   xtreg,fe  xb stdp ue residuals (= ue), and on e(sample) only: xbu u e
//             (`xbd`, `d`: r(198))
//   reghdfe   xb stdp, and on e(sample) only: xbd d residuals; xbd/d need the
//             fit to have kept its residuals (`resid`), else r(9) (`e`: r(198))
//
// Definitions ([R] regress postestimation and [R] areg postestimation, Methods
// and formulas; [XT] xtreg postestimation): xb = x b INCLUDING _cons. With the
// within residual e = y - xb - d, the absorbed component is d = y - xb - e over
// e(sample) and xbd = y - e. stdp = sqrt(x V x'). Under vce(ols) h = x V x'/s^2,
// stdr = s sqrt(1-h), stdf = s sqrt(1+h), rstandard = e/stdr and rstudent =
// e/(s_(j) sqrt(1-h)), s_(j)^2 = (s^2 df_r - e^2/(1-h))/(df_r - 1), and cooksd =
// e^2 h / (k s^2 (1-h)^2) with k the number of estimated coefficients (BUG-925:
// w2b_estim.log predict_reg_cooksd; w2aa_predict_type_restricted.log a_cooksd,
// a_if_cooksd -- after a restricted fit every row with y and x, as hat).
//
// Measured on main before this (frozen binary, w2b_estim.do lines 1-17 + 50-108):
// every statistic above except xb/residuals refused, `r` refused, and xb after
// areg / xtreg,fe / reghdfe wrote the WITHIN prediction x~b -- r(sum) 1.8e-15,
// r(sd) 6.196 where the oracle's column sums to 760.4 with sd 6.324 -- at rc 0.
// That last one is the silent wrong answer this block exists for: an absorbed-FE
// fit's `predict p` is now x b + _cons, evaluated here, never the engine's
// demeaned index.
//
// Nothing below is filled that was not computed for THIS model: the design is
// read off e(b)'s own names (a name this reader cannot evaluate refuses), the
// estimation sample is the one the engine's refit reports (its within residual
// is missing off the sample, and the count must equal e(N)), and each quantity
// is checked against an independent one the engine produced before it is
// written (see computePredictNativeM4).

enum class PredictFamilyM4 { Other, Regress, Areg, XtregFe, Reghdfe };

PredictFamilyM4 predictFamilyM4(const LastModel& lm) {
  if (lm.fe.empty()) return lm.stataCmd == "regress" ? PredictFamilyM4::Regress : PredictFamilyM4::Other;
  if (lm.stataCmd == "areg") return PredictFamilyM4::Areg;
  if (lm.stataCmd == "reghdfe") return PredictFamilyM4::Reghdfe;
  // `xtreg, fe` records stataCmd "xtreg" (its e(cmd)); the N_g it copies into
  // the cached reply (runXtreg's fixed-effects arm, BUG-538's e() half) is the
  // mark that tells it from xtreg, re -- nothing else in the tree writes that key.
  if ((lm.stataCmd.empty() || lm.stataCmd == "xtreg") && lm.cobraCmd == "feols" && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("N_g")) {
    return PredictFamilyM4::XtregFe;
  }
  return PredictFamilyM4::Other;
}

std::string predictFamilyCmdM4(PredictFamilyM4 f, const LastModel& lm) {
  return f == PredictFamilyM4::XtregFe ? std::string("xtreg, fe") : lm.stataCmd;
}

// A typed spelling -> the statistic it names. The dispatcher's option vocabulary
// (dispatcher.cpp kVocabPredict) already canonicalizes the unique prefixes
// (`rsta`, `rstu`, `lev`, `sc`, `dr`) and the `re`/`res`/`resi` aliases; `r` is a
// prefix of four names there and arrives as typed. Each row is an oracle spelling
// (w2b_estim.do lines 52, 85, 96, 107).
std::string predictStatisticM4(const std::string& k) {
  if (k == "r" || k == "re" || k == "res" || k == "resi" || k == "resid" || k == "residuals") {
    return "residuals";
  }
  if (k == "sc" || k == "score") return "score";
  if (k == "rsta" || k == "rstandard") return "rstandard";
  if (k == "rstu" || k == "rstudent") return "rstudent";
  if (k == "lev" || k == "leverage" || k == "hat") return "hat";
  if (k == "dr" || k == "dresid" || k == "dresiduals") return "dresiduals";
  return k;
}

// BUG-1257: a typed spelling -> the statistic predict reads after logit, probit
// or poisson, "" = Stata's "option <k> not allowed", r(198). Each spelling is
// StataBE 19.5's (2026-10-29): the minimum abbreviations of logit_p/probit_p/
// poisso_p, `i`..`index` the linear index there as everywhere (after poisson too:
// `i` is xb, not ir), `p` pr, `n` number after logit and n after poisson, `dx`
// dx2; `x`, `st`, `std`, `ru`, `as`, `e` and every other statistic refused. The
// dispatcher's prefix rule (kVocabPredict) may already have spelled `h` as `hat`,
// `de` as `deviance`, `sc` as `score`, `res` as `residuals`.
std::string mlPredictStatisticM4(const std::string& cmd, const std::string& k) {
  auto abbrev = [&k](const char* full, size_t minLen) {
    const std::string f(full);
    return k.size() >= minLen && k.size() <= f.size() && f.compare(0, k.size(), k) == 0;
  };
  if (k == "xb" || abbrev("index", 1)) return "xb";
  if (k == "stdp") return "stdp";
  if (abbrev("scores", 2)) return "score";
  if (abbrev("nooffset", 5)) return "nooffset";
  if (cmd == "poisson") {
    if (k == "n") return "n";
    if (k == "ir") return "ir";
    return "";
  }
  if (abbrev("pr", 1)) return "pr";
  if (abbrev("deviance", 2)) return "deviance";
  if (abbrev("rules", 4)) return "rules";
  if (k == "asif") return "asif";
  if (cmd != "logit") return "";
  if (abbrev("number", 1)) return "number";
  if (abbrev("residuals", 1)) return "residuals";
  if (abbrev("rstandard", 2)) return "rstandard";
  if (abbrev("dx2", 2)) return "dx2";
  if (abbrev("ddeviance", 2)) return "ddeviance";
  if (abbrev("dbeta", 2)) return "dbeta";
  if (abbrev("hat", 1)) return "hat";
  return "";
}

bool predictServesM4(PredictFamilyM4 f, const std::string& s) {
  static const std::set<std::string> kRegress = {"xb",   "residuals", "score",    "stdp",    "stdf",
                                                 "stdr", "rstandard", "rstudent", "hat",
                                                 "cooksd"};
  static const std::set<std::string> kAreg = {"xb", "stdp", "dresiduals", "xbd", "d", "residuals",
                                              "score"};
  static const std::set<std::string> kXtregFe = {"xb", "stdp", "ue", "xbu", "u", "e", "residuals"};
  // BUG-1259: reghdfe's dresiduals (y - xb) and score (the residual), as areg's
  // (StataBE 19.5 scratch-laneA/agents/N2/e12.do: dresiduals[8] -2.0888238492587301,
  // score[8] -.89046608668893679 after `reghdfe ... if t>=2, absorb(id) resid`).
  static const std::set<std::string> kReghdfe = {"xb",        "stdp",       "xbd",  "d",
                                                 "residuals", "dresiduals", "score"};
  switch (f) {
    case PredictFamilyM4::Regress: return kRegress.count(s) > 0;
    case PredictFamilyM4::Areg: return kAreg.count(s) > 0;
    case PredictFamilyM4::XtregFe: return kXtregFe.count(s) > 0;
    case PredictFamilyM4::Reghdfe: return kReghdfe.count(s) > 0;
    default: return false;
  }
}

// FU-768: after a RESTRICTED absorbed fit (`areg|xtreg|reghdfe ... if exp`, no
// time-series operator) the statistics w2aa_predict_type_restricted.log pins: xb
// over every row with x (b_if_xb, c_xt_if_xb, c_hdfe_if_xb), ue over every row
// with y and x (c_xt_if_ue), and on e(sample) only residuals/d/xbd (b_if_*,
// c_hdfe_if_*) and e/u/xbu (c_xt_if_*). BUG-1259 measured the rest the same way
// (StataBE 19.5 scratch-laneA/agents/N2/e12.do, `... if t>=2`): stdp, dresiduals
// and score after areg/reghdfe, residuals (= ue, every row with y and x: 60) and
// stdp after xtreg, fe -- each the unrestricted fit's statistic over the
// restricted fit's e(sample). Every statistic of the family is served.
bool predictServesRestrictedM4(PredictFamilyM4 f, const std::string& s) {
  return predictServesM4(f, s);
}

// Stata's own refusal, r(198) "option X not allowed": w2b_estim.log predict_reg_e /
// predict_reg_d; predict-e-xbd.log B_stat_xbd / B_stat_d (xtreg,fe) and P_res_e
// (reghdfe).
bool predictNotAllowedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress: return s == "e" || s == "d";
    case PredictFamilyM4::XtregFe: return s == "xbd" || s == "d";
    case PredictFamilyM4::Reghdfe: return s == "e";
    default: return false;
  }
}

// The statistics the engine's own predict already writes as Stata defines them,
// so they keep the historical argv byte for byte: `--xb` after regress, and the
// residual (`--resid`, the within residual under --fe, missing off e(sample)).
// "" = computed here.
std::string predictEngineServedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress:
      if (s == "xb") return "xb";
      if (s == "residuals" || s == "score") return "resid";
      return "";
    case PredictFamilyM4::Areg: return (s == "residuals" || s == "score") ? "resid" : "";
    case PredictFamilyM4::XtregFe: return s == "e" ? "resid" : "";
    case PredictFamilyM4::Reghdfe: return (s == "residuals" || s == "score") ? "resid" : "";
    default: return "";
  }
}

// The one statistic the options name, or the refusal block.
std::optional<ojson> resolvePredictStatM4(PredictFamilyM4 f, const LastModel& lm,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          std::string& stat) {
  stat.clear();
  for (const auto& [k, v] : opts) {
    if (k == "quietly" || k == "noisily") continue;
    const std::string s = predictStatisticM4(k);
    const bool bare = v.isBool && v.boolValue;
    if (bare && predictNotAllowedM4(f, s)) {
      return engineErrorBlockM4("option " + k + " not allowed", "syntax_error");
    }
    // BUG-1260: regress's e(a,b), E(y | a < y < b) (regre_p2.ado CalcE; StataBE
    // 19.5 `predict double OLS_ec, e(0,.)` rc 0), carried as "e(a,b)".
    if (f == PredictFamilyM4::Regress && k == "e" && !v.isBool) {
      if (!stat.empty()) return engineErrorBlockM4("predict: only one statistic may be specified", "syntax_error");
      stat = "e(" + v.strValue + ")";
      continue;
    }
    if (!bare || !predictServesM4(f, s)) {
      return engineErrorBlockM4("predict " + k + ": not supported after `" + predictFamilyCmdM4(f, lm) +
                                    "` in this version of PIET. Nothing was written to your data.",
                                "unsupported_option");
    }
    if (!stat.empty() && stat != s) {
      return engineErrorBlockM4("predict: only one statistic may be specified", "syntax_error");
    }
    stat = s;
  }
  if (stat.empty()) stat = "xb";  // Stata's default after all four
  return std::nullopt;
}

// One e(b) column of the design, as a product of live-frame columns. `level` < 0
// is the column's value (a plain or `c.` term; an `o.` omitted term, whose
// coefficient is 0); level >= 0 is the indicator `var == level` of a factor term
// (`2.t`, the base `1b.t`), missing where var is. `constant` is _cons.
struct DesignColM4 {
  bool constant = false;
  std::vector<std::pair<const double*, long long>> parts;
};

// Read e(b)'s names as design columns of the live frame. Accepts exactly the
// shapes above joined by `#`; anything else -- a time-series operator (`L.x`),
// a string column, a name that is not a column -- is not guessed at: `why` names
// it and the caller refuses. `tsCols` (BUG-1259): a time-series term's values,
// one per row of the live frame, as the engine generated them
// (materializeTsColumnsM4); such a segment reads them.
bool buildDesignM4(const Datastore& ds, const std::vector<std::string>& names,
                   std::vector<DesignColM4>& cols, std::string& why,
                   const std::map<std::string, std::vector<double>>* tsCols = nullptr) {
  cols.clear();
  for (const std::string& nm : names) {
    DesignColM4 c;
    if (nm == "_cons" || nm == "(intercept)") {
      c.constant = true;
      cols.push_back(std::move(c));
      continue;
    }
    size_t pos = 0;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg =
          nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      if (tsCols != nullptr) {
        const auto ts = tsCols->find(seg);
        if (ts != tsCols->end() && ts->second.size() == static_cast<size_t>(ds.nRows())) {
          c.parts.emplace_back(ts->second.data(), -1);
          if (hash == std::string::npos) break;
          pos = hash + 1;
          continue;
        }
      }
      std::string var = seg;
      long long level = -1;
      const size_t dot = seg.find('.');
      if (dot != std::string::npos) {
        const std::string op = seg.substr(0, dot);
        var = seg.substr(dot + 1);
        if (op != "c" && op != "o") {
          size_t d = 0;
          while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
          const std::string suffix = op.substr(d);
          if (d == 0 || d > 15 ||
              !(suffix.empty() || suffix == "b" || suffix == "o" || suffix == "bn" || suffix == "n")) {
            why = "the coefficient `" + nm + "` is not a term this version of PIET can evaluate";
            return false;
          }
          level = std::stoll(op.substr(0, d));
        }
      }
      if (var.empty() || var.find('.') != std::string::npos || !ds.has(var) ||
          ds.col(var).dtype != cecon::DType::F64 || ds.col(var).f64() == nullptr) {
        why = "the coefficient `" + nm + "` does not name a numeric variable of the data in memory";
        return false;
      }
      c.parts.emplace_back(ds.col(var).f64(), level);
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    cols.push_back(std::move(c));
  }
  return true;
}

double designValueM4(const DesignColM4& c, size_t r) {
  if (c.constant) return 1.0;
  double v = 1.0;
  for (const auto& [p, level] : c.parts) {
    const double x = p[r];
    if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    v *= (level < 0) ? x : (x == static_cast<double>(level) ? 1.0 : 0.0);
  }
  return v;
}

// BUG-1259: the time-series segments of e(b)'s names (`L.x1`, `D.x2`; a factor
// level, `c.` and `o.` are the design reader's own) over every row of the live
// frame, generated by the engine under the session's tsset/xtset -- the
// expression path `generate z = L.x1` takes (cobra eval --gen) -- so a lag reads
// across the fit's `if` exactly as the fit's design did. The frame comes back row
// for row, or nothing is returned.
bool materializeTsColumnsM4(SessionState& session, const std::vector<std::string>& names,
                            std::map<std::string, std::vector<double>>& out, std::string& why) {
  std::vector<std::string> segs;
  for (const std::string& nm : names) {
    size_t pos = 0;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.find('.');
      if (dot != std::string::npos) {
        const std::string op = seg.substr(0, dot);
        const bool level = !op.empty() && std::isdigit(static_cast<unsigned char>(op[0]));
        if (!level && op != "c" && op != "o" && std::find(segs.begin(), segs.end(), seg) == segs.end())
          segs.push_back(seg);
      }
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  }
  if (segs.empty()) return true;
  if (!session.currentHbin().has_value()) {
    why = "no dataset is in memory";
    return false;
  }
  const std::string head = *session.currentHbin();
  const std::string scratch = nextPathEst(session.workdir(), "predict_ts");
  std::vector<std::string> argv = {"eval", "--data", head};
  std::string exprs;
  for (size_t i = 0; i < segs.size(); ++i) {
    const std::string gen = "__piet_ts" + std::to_string(i) + "__=" + segs[i];
    argv.insert(argv.end(), {"--gen", gen});
    exprs += gen + " ";
  }
  argv.insert(argv.end(), {"--out", scratch});
  try {
    (void)runCobraJsonM4(translate::injectTsContextForExpr(argv, exprs, session));
    Datastore gen(scratch);
    Datastore* live = session.datastore();
    if (live == nullptr || gen.nRows() != live->nRows()) throw std::runtime_error("rows changed");
    for (size_t i = 0; i < segs.size(); ++i) {
      const std::string col = "__piet_ts" + std::to_string(i) + "__";
      const std::string var = segs[i].substr(segs[i].find('.') + 1);
      if (!gen.has(col) || gen.col(col).f64() == nullptr || !gen.has(var) || !live->has(var) ||
          gen.col(var).f64() == nullptr || live->col(var).f64() == nullptr)
        throw std::runtime_error("the generated columns are not readable");
      // The frame must come back in the live frame's row order.
      const double* a = gen.col(var).f64();
      const double* b = live->col(var).f64();
      for (int64_t r = 0; r < live->nRows(); ++r)
        if (!(a[r] == b[r] || (std::isnan(a[r]) && std::isnan(b[r]))))
          throw std::runtime_error("the generated frame is not the live frame row for row");
      const double* p = gen.col(col).f64();
      out[segs[i]].assign(p, p + gen.nRows());
    }
  } catch (const CobraError& ce) {
    cecon::native_unlink(scratch);
    why = "the time-series terms could not be evaluated (" + cobraErrorStrM4(ce) + ")";
    return false;
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    why = std::string("the time-series terms could not be evaluated (") + e.what() + ")";
    return false;
  }
  cecon::native_unlink(scratch);
  return true;
}

// e(V) over `names` (row-major), from the engine's own lincom: Var(b_j) and
// Var(b_j + b_l) in ONE call, V_jl = (Var(b_j + b_l) - V_jj - V_ll) / 2. It is the
// covariance every lincom/test answer already rests on (the refit forwards the
// fitted --vcov/--cluster, BS-098) -- and the only full one reachable: the fit's
// reply carries standard errors, not V, and `matrix V = e(V)` synthesizes a
// diagonal. The FULL-specification call (lastModelBaseArgv), deliberately not
// runPostEstimationM4: minting a §31 handle goes through setLastModel, which
// erases e(resid), and a later `predict, xbd` after `reghdfe ..., resid` would
// then refuse r(9) on a fit that kept its residuals. Its b and its standard errors
// must reproduce the fit's, or the V it carries belongs to some other fit (a refit
// on another sample, another VCE) and nothing is returned.
bool lincomVcovM4(const SessionState& session, const LastModel& lm, const std::vector<std::string>& names,
                  const std::vector<double>& b, const std::vector<double>& seFit,
                  std::vector<double>& V, std::string& why) {
  const size_t k = names.size();
  // k + k(k-1)/2 expressions ride on one argv; past this the list outgrows it.
  constexpr size_t kMaxTerms = 160;
  if (k == 0 || k > kMaxTerms) {
    why = "the model has " + std::to_string(k) + " coefficients (this version of PIET computes "
          "the prediction's variance for at most " + std::to_string(kMaxTerms) + ")";
    return false;
  }
  std::vector<std::string> spell(k);
  for (size_t j = 0; j < k; ++j) {
    spell[j] = names[j] == "(intercept)" ? std::string("_cons") : names[j];
    if (spell[j].find_first_of("+-* \t") != std::string::npos) {
      why = "the coefficient `" + names[j] + "` cannot be addressed by the engine's lincom";
      return false;
    }
  }
  std::vector<std::string> argv = lastModelBaseArgv("lincom", lm);
  // BUG-1257: after logit/probit/poisson the refit names its estimator, as
  // lincom's own does (BUG-203).
  if (postestGlmCmdsM4().count(lm.cobraCmd)) argv.insert(argv.end(), {"--model", lm.cobraCmd});
  for (size_t j = 0; j < k; ++j) argv.insert(argv.end(), {"--exp", spell[j]});
  for (size_t j = 0; j < k; ++j)
    for (size_t l = j + 1; l < k; ++l) argv.insert(argv.end(), {"--exp", spell[j] + " + " + spell[l]});
  argv.push_back("--tidy");
  // BUG-1259 (BUG-369's shape): an `L.x` regressor refits under the fit's tsset context.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    if (translate::hasTsToken(tsTokens)) argv = translate::injectTsContext(argv, session);
  }
  const nlohmann::json raw = runCobraJsonM4(argv);  // CobraError -> the caller's catch
  const size_t want = k + k * (k - 1) / 2;
  if (!raw.is_object() || !raw.contains("coefs") || !raw["coefs"].is_array() ||
      raw["coefs"].size() != want) {
    why = "the engine's lincom returned an unexpected reply";
    return false;
  }
  const auto& co = raw["coefs"];
  auto num = [&co](size_t i, const char* key) {
    return (co[i].is_object() && co[i].contains(key) && co[i][key].is_number())
               ? co[i][key].get<double>()
               : std::numeric_limits<double>::quiet_NaN();
  };
  V.assign(k * k, 0.0);
  for (size_t j = 0; j < k; ++j) {
    const double bj = num(j, "b");
    const double se = num(j, "se");
    if (!std::isfinite(bj) || !std::isfinite(se) || !std::isfinite(seFit[j]) ||
        std::fabs(bj - b[j]) > 1e-9 * std::max({1.0, std::fabs(b[j]), se}) ||
        std::fabs(se - seFit[j]) > 1e-9 * seFit[j]) {
      why = "the engine's post-estimation refit did not reproduce the fitted coefficient and "
            "standard error of `" + names[j] + "`";
      return false;
    }
    V[j * k + j] = se * se;
  }
  size_t idx = k;
  for (size_t j = 0; j < k; ++j) {
    for (size_t l = j + 1; l < k; ++l, ++idx) {
      const double se = num(idx, "se");
      if (!std::isfinite(se)) {
        why = "the engine's lincom returned no standard error for `" + spell[j] + " + " + spell[l] + "`";
        return false;
      }
      const double c = (se * se - V[j * k + j] - V[l * k + l]) / 2.0;
      V[j * k + l] = c;
      V[l * k + j] = c;
    }
  }
  return true;
}

double jsonNumM4(const ojson& o, const char* key) {
  return (o.is_object() && o.contains(key) && o[key].is_number())
             ? o[key].get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}

// Compute statistic `stat` (one the engine does not write itself) over every row
// of the live frame. `engineArgs` is runPredict's replay argv up to and including
// --fe (no --xb/--resid/--out): it is re-run here as a ONE-SHOT engine call into a
// scratch file whose two columns this function reads -- the engine's xb (after
// regress, the exact linear predictor over the rows the engine evaluates) and its
// residual (under --fe the within residual, missing off e(sample)). One-shot, so
// the output is a file on disk whatever plane the session runs on; every input
// path must therefore be on disk too, and is checked.
//
// Cross-checks before anything is returned (each failure refuses, naming why):
//   regress  the xb evaluated here equals the engine's on every row it evaluates;
//   FE       the engine's refit sample has e(N) rows, xb and y exist on all of
//            them, and sum over e(sample) of d is 0 (it is, exactly, when _cons =
//            ybar - xbar b, the engine's own convention -- GAP B in feols.cpp);
//   stdp     lincom's b reproduces e(b) (lincomVcovM4); under FE additionally
//            se(_cons)^2 == s^2/N + xbar V xbar', the engine's own extended-inverse
//            identity, which also proves the sample means used below.
std::optional<ojson> computePredictNativeM4(SessionState& session, const LastModel& lm,
                                            PredictFamilyM4 fam, const std::string& stat,
                                            std::vector<std::string> engineArgs,
                                            std::vector<double>& vals) {
  const std::string cmd = predictFamilyCmdM4(fam, lm);
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after `" + cmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Refusing rather than writing a number that was not computed "
                                  "for this model. Nothing was written to your data.",
                              "unsupported_option");
  };
  const bool fe = fam != PredictFamilyM4::Regress;
  const bool iid = lm.vcov == "iid" && lm.cluster.empty();
  // An absorbed-effects fit whose time-series regressors read rows outside its
  // `if` (the restriction rides as --fit-keepif, a row mask after the operators).
  const bool tsMaskFe =
      fe && std::find(engineArgs.begin(), engineArgs.end(), "--fit-keepif") != engineArgs.end() &&
      postestTsKeepifRefusalM4("predict", lm).has_value();

  // Stata: rstandard/rstudent/leverage/stdf/stdr are "not available if any vce()
  // other than vce(ols) was specified" ([R] regress postestimation). Under an
  // absorbed FE the prediction's variance needs _cons's covariances, which only
  // the iid algebra reaches from here (the engine's lincom cannot address the
  // reconstructed _cons row).
  if (!iid && (stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
               stat == "rstudent" || stat == "cooksd")) {
    return engineErrorBlockM4("predict, " + stat + " is not available after a `regress` with a "
                              "robust or clustered VCE; Stata computes it only under vce(ols)",
                              "unsupported_option");
  }
  if (fe && stat == "stdp" && !iid) {
    return refuse("the standard error of the prediction after absorbed fixed effects is computed "
                  "only under the default (iid) VCE");
  }
  // BUG-1260: regress's e(a,b) = E(xb + u | a < xb + u < b) with u ~ N(0, e(rmse)^2)
  // (regre_p2.ado CalcE); each bound a number or `.` (unbounded). A variable or
  // expression bound is not read here.
  const bool eStat = stat.rfind("e(", 0) == 0;
  double eLo = std::numeric_limits<double>::quiet_NaN(), eHi = eLo;
  if (eStat) {
    const std::string in = stat.substr(2, stat.size() - 3);
    const size_t comma = in.find(',');
    if (comma == std::string::npos || in.find(',', comma + 1) != std::string::npos)
      return engineErrorBlockM4("option e() incorrectly specified", "syntax_error");
    auto bound = [](std::string t, double& out) {
      t = trimM4(t);
      if (t == ".") return true;
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) return false;
      out = v;
      return true;
    };
    if (!bound(in.substr(0, comma), eLo) || !bound(in.substr(comma + 1), eHi))
      return refuse("a bound of e(a,b) that is not a number or `.`");
    if (!std::isnan(eLo) && !std::isnan(eHi) && eLo > eHi)
      return engineErrorBlockM4("option e(a,b) incorrectly specified; b must be >= a", "syntax_error");
  }

  // reghdfe answers xbd/d only when the fit kept its residuals (reghdfe_p: r(9)),
  // and reads them from that variable (predict-e-xbd.log R_drop_xbd: dropped ->
  // r(111)). w2b_estim.log predict_reghdfe_xbd_noresid is the r(9) arm.
  if (fam == PredictFamilyM4::Reghdfe && (stat == "xbd" || stat == "d")) {
    const std::string* rv = session.eval.eStrings.find("resid");
    if (rv == nullptr || rv->empty()) {
      return ojson(eval::errorBlock(
          "engine_error", "you must add the resid option to reghdfe before running this prediction",
          std::nullopt, std::nullopt, std::nullopt, "r(9)"));
    }
    Datastore* live0 = nullptr;
    try {
      live0 = session.datastore();
    } catch (const std::exception& e) {
      return refuse(e.what());
    }
    if (live0 == nullptr || !live0->has(*rv)) {
      return ojson(eval::engineError("variable " + *rv + " not found"));
    }
  }

  // Coefficients: e(b) as the engine reported it for this fit.
  std::vector<std::string> names;
  std::vector<double> b, se;
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array()) {
    return refuse("the fitted coefficients are not available");
  }
  for (const auto& row : (*lm.result)["coeftable"]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) {
      return refuse("the fitted coefficients are not available");
    }
    names.push_back(row["name"].get<std::string>());
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  size_t consIdx = names.size();
  for (size_t j = 0; j < names.size(); ++j)
    if (names[j] == "_cons" || names[j] == "(intercept)") consIdx = j;
  if (fe && consIdx == names.size()) {
    return refuse("the fit reports no _cons, and xb after absorbed effects is x b + _cons");
  }
  const double sigma2 = [&] {
    const double s2 = jsonNumM4(*lm.result, "sigma2");
    if (std::isfinite(s2)) return s2;
    const double rm = jsonNumM4(*lm.result, "rmse");
    return rm * rm;
  }();
  const double nObs = jsonNumM4(*lm.result, "n_obs");
  const double dfR = jsonNumM4(*lm.result, "df_r");
  // cooksd's k: the coefficients the fit estimated, e(N) - e(df_r) (regress only).
  const double kEst = nObs - dfR;
  if (stat == "cooksd" && !(std::isfinite(kEst) && kEst > 0.0)) {
    return refuse("the fit reports no residual degrees of freedom");
  }

  // The covariance, before any data is read (lincom never touches the session).
  const bool needV = stat == "stdp" || stat == "hat" || stat == "stdr" || stat == "stdf" ||
                     stat == "rstandard" || stat == "rstudent" || stat == "cooksd";
  std::vector<std::string> vNames;
  std::vector<double> vB, vSe, V;
  if (needV) {
    for (size_t j = 0; j < names.size(); ++j) {
      if (fe && j == consIdx) continue;  // the reconstructed _cons: identity below
      vNames.push_back(names[j]);
      vB.push_back(b[j]);
      vSe.push_back(se[j]);
    }
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, vNames, vB, vSe, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }
  if ((stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
       stat == "rstudent" || stat == "cooksd") && !(std::isfinite(sigma2) && sigma2 > 0.0)) {
    return refuse("the fit reports no residual variance");
  }

  // The engine's refit, one-shot into a scratch file on disk.
  Datastore* live = nullptr;
  try {
    live = session.datastore();  // materializes a virtual head before anything reads it
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  for (size_t i = 0; i + 1 < engineArgs.size(); ++i) {
    if ((engineArgs[i] == "--data" || engineArgs[i] == "--fit-data") &&
        !cecon::native_exists(engineArgs[i + 1])) {
      return refuse("the data the model was fitted on are not available on disk");
    }
  }
  const std::string kXb = "__piet_predict_xb__";
  const std::string kE = "__piet_predict_e__";
  const std::string scratch = nextPathEst(session.workdir(), "predict_refit");
  engineArgs.insert(engineArgs.end(), {"--xb", kXb, "--resid", kE, "--out", scratch});
  const CobraRunResult rr = runCobraOneShot(engineArgs);
  if (!rr.ok || !cecon::native_exists(scratch)) {
    cecon::native_unlink(scratch);
    const std::string err = rr.stderr_text.empty() ? rr.error : rr.stderr_text;
    return refuse("the engine's refit failed (" + strutil::trim(err) + ")");
  }
  std::vector<double> engXb, engE;
  try {
    Datastore eng(scratch);
    if (eng.nRows() != live->nRows() || !eng.has(kXb) || !eng.has(kE) ||
        eng.col(kXb).f64() == nullptr || eng.col(kE).f64() == nullptr) {
      throw std::runtime_error("the engine's refit returned an unexpected frame");
    }
    const double* px = eng.col(kXb).f64();
    const double* pe = eng.col(kE).f64();
    engXb.assign(px, px + eng.nRows());
    engE.assign(pe, pe + eng.nRows());
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    return refuse(e.what());
  }
  cecon::native_unlink(scratch);
  // BUG-1259: x V x' after time-series operators under `if` reads the operator
  // columns as the engine generates them over the live frame.
  std::map<std::string, std::vector<double>> tsCols;
  if (tsMaskFe && needV) {
    std::string why;
    if (!materializeTsColumnsM4(session, vNames, tsCols, why)) return refuse(why);
  }

  // The live frame again: nothing above may have kept the pointer current.
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  if (engXb.size() != n) return refuse("the engine's refit returned an unexpected frame");

  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).dtype == cecon::DType::F64) y = live->col(lm.y).f64();
  const bool needY = fe || stat == "residuals" || stat == "rstandard" || stat == "rstudent" ||
                     stat == "cooksd";
  if (needY && y == nullptr) return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> xb(n, kNaN);
  std::vector<DesignColM4> design, vDesign;
  if (tsMaskFe) {
    // FU-759 (w2_esample_tsrefit.log:251-263 T7): e(b) names a time-series term
    // this reader cannot evaluate, so the engine evaluates it over the full
    // series -- cmd_predict's --fe under the --fit-keepif row mask writes
    // x b + _cons, the refit's reconstructed constant. The checks below (the
    // refit's sample is e(N), the absorbed part sums to zero over it) hold that
    // prediction to the fitted model as they hold the design's. BUG-1259: every
    // statistic built from xb, y and the refit's within residual follows (StataBE
    // 19.5 scratch-laneA/agents/N2/e12.do: `xtreg y L.x1 x2 if t>=3, fe` u[9]
    // -2.8045004048291329 on 40 rows, xbu[9] 13.253036355985744; areg d/xbd the
    // same), and stdp reads the operator columns materialized above.
    xb = engXb;
    std::string why;
    if (needV && !buildDesignM4(*live, vNames, vDesign, why, &tsCols)) return refuse(why);
  } else {
    std::string why;
    if (!buildDesignM4(*live, names, design, why)) return refuse(why);
    if (needV && !buildDesignM4(*live, vNames, vDesign, why)) return refuse(why);
    for (size_t r = 0; r < n; ++r) {
      double s = 0.0;
      bool ok = true;
      for (size_t j = 0; j < design.size() && ok; ++j) {
        const double v = designValueM4(design[j], r);
        if (std::isnan(v)) ok = false;
        else s += v * b[j];
      }
      if (ok) xb[r] = s;
    }
  }

  // The within residual and the estimation sample (FE only).
  std::vector<double> d;
  std::vector<char> inSample;
  if (!fe) {
    // After regress the engine evaluates the fitted coefficients itself: the xb
    // built here from e(b)'s names must be its xb, row for row.
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engXb[r])) continue;
      if (std::isnan(xb[r]) || std::fabs(xb[r] - engXb[r]) > 1e-9 * (1.0 + std::fabs(engXb[r]))) {
        return refuse("the design read from e(b) does not reproduce the engine's linear prediction");
      }
    }
  } else {
    inSample.assign(n, 0);
    d.assign(n, kNaN);
    size_t count = 0;
    double sumD = 0.0, scale = 0.0;
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engE[r])) continue;
      if (y == nullptr || std::isnan(y[r]) || std::isnan(xb[r])) {
        return refuse("an estimation-sample row has no linear prediction");
      }
      inSample[r] = 1;
      ++count;
      d[r] = y[r] - xb[r] - engE[r];
      sumD += d[r];
      scale += std::fabs(y[r] - engE[r]) + std::fabs(xb[r]);
    }
    if (!std::isfinite(nObs) || static_cast<double>(count) != nObs) {
      return refuse("the engine's refit sample (" + std::to_string(count) +
                    " observations) is not the fitted one");
    }
    if (std::fabs(sumD) > 1e-9 * scale) {
      return refuse("the absorbed component does not sum to zero over the estimation sample, so "
                    "the design read from e(b) is not the fitted one");
    }
  }

  // x V x' (stdp; h = x V x'/s^2 under vce(ols)).
  std::vector<double> q;
  if (needV) {
    const size_t k = vDesign.size();
    std::vector<double> xbar(k, 0.0);
    if (fe) {
      // Frisch-Waugh: under iid the engine's extended inverse gives Cov(b, _cons)
      // = -V xbar' and Var(_cons) = s^2/N + xbar V xbar' (feols.cpp GAP B), so
      // x_c V_full x_c' = (x - xbar) V (x - xbar)' + s^2/N.
      size_t cnt = 0;
      for (size_t r = 0; r < n; ++r) {
        if (!inSample[r]) continue;
        ++cnt;
        for (size_t j = 0; j < k; ++j) xbar[j] += designValueM4(vDesign[j], r);
      }
      for (size_t j = 0; j < k; ++j) xbar[j] /= static_cast<double>(cnt);
      double quad = 0.0;
      for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l < k; ++l) quad += xbar[j] * V[j * k + l] * xbar[l];
      const double want = sigma2 / nObs + quad;
      const double got = se[consIdx] * se[consIdx];
      if (!(std::isfinite(want) && std::isfinite(got)) || std::fabs(want - got) > 1e-8 * std::fabs(got)) {
        return refuse("Var(_cons) does not satisfy the fit's own identity, so the estimation-sample "
                      "means are not the fitted ones");
      }
    }
    q.assign(n, kNaN);
    std::vector<double> x(k);
    for (size_t r = 0; r < n; ++r) {
      bool ok = true;
      for (size_t j = 0; j < k && ok; ++j) {
        x[j] = designValueM4(vDesign[j], r);
        if (std::isnan(x[j])) ok = false;
        else x[j] -= xbar[j];
      }
      if (!ok) continue;
      double s = 0.0;
      for (size_t j = 0; j < k; ++j) {
        double row = 0.0;
        for (size_t l = 0; l < k; ++l) row += V[j * k + l] * x[l];
        s += x[j] * row;
      }
      if (fe) s += sigma2 / nObs;
      q[r] = s;
    }
  }

  // e(a,b)'s scale is e(rmse) (regre_p2's "e(rmse)" argument).
  const double eSigma = [&] {
    const double rm = jsonNumM4(*lm.result, "rmse");
    return std::isfinite(rm) ? rm : std::sqrt(sigma2);
  }();
  if (eStat && !(eSigma > 0.0)) return refuse("the fit reports no root mean squared error");
  auto normalCdf = [](double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); };
  auto normalDen = [](double z) { return std::exp(-0.5 * z * z) / std::sqrt(2.0 * 3.14159265358979323846); };

  vals.assign(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    const double yr = y != nullptr ? y[r] : kNaN;
    const double res = yr - xb[r];  // NaN wherever either is
    if (stat == "xb") {
      vals[r] = xb[r];
    } else if (eStat) {
      if (std::isnan(xb[r])) continue;
      const double L = (eLo - xb[r]) / eSigma, U = (eHi - xb[r]) / eSigma;
      double v = xb[r];
      if (!std::isnan(eLo) && std::isnan(eHi)) v = xb[r] + eSigma * normalDen(L) / normalCdf(-L);
      else if (std::isnan(eLo) && !std::isnan(eHi)) v = xb[r] - eSigma * normalDen(U) / normalCdf(U);
      else if (!std::isnan(eLo))
        v = xb[r] - eSigma * (normalDen(U) - normalDen(L)) / (normalCdf(U) - normalCdf(L));
      vals[r] = std::isfinite(v) ? v : kNaN;
    } else if (stat == "stdp") {
      vals[r] = std::isnan(q[r]) ? kNaN : std::sqrt(std::max(0.0, q[r]));
    } else if (stat == "dresiduals" || stat == "ue" ||
               (fam == PredictFamilyM4::XtregFe && stat == "residuals")) {
      vals[r] = res;
    } else if (stat == "xbd" || stat == "xbu") {
      vals[r] = inSample[r] ? xb[r] + d[r] : kNaN;
    } else if (stat == "d" || stat == "u") {
      vals[r] = inSample[r] ? d[r] : kNaN;
    } else if (q.size() == n) {
      // regress, vce(ols): h, stdr, stdf, rstandard, rstudent.
      if (std::isnan(q[r])) continue;
      const double h = q[r] / sigma2;
      const double s = std::sqrt(sigma2);
      if (stat == "hat") {
        vals[r] = h;
      } else if (stat == "stdf") {
        vals[r] = s * std::sqrt(1.0 + h);
      } else if (stat == "stdr") {
        vals[r] = h < 1.0 ? s * std::sqrt(1.0 - h) : kNaN;
      } else if (stat == "rstandard") {
        vals[r] = (h < 1.0 && !std::isnan(res)) ? res / (s * std::sqrt(1.0 - h)) : kNaN;
      } else if (stat == "rstudent") {
        if (h < 1.0 && !std::isnan(res) && dfR > 1.0) {
          const double sj2 = (sigma2 * dfR - res * res / (1.0 - h)) / (dfR - 1.0);
          vals[r] = sj2 > 0.0 ? res / (std::sqrt(sj2) * std::sqrt(1.0 - h)) : kNaN;
        }
      } else if (stat == "cooksd") {
        if (h < 1.0 && !std::isnan(res) && kEst > 0.0) {
          vals[r] = res * res * h / (kEst * sigma2 * (1.0 - h) * (1.0 - h));
        }
      }
    }
  }
  return std::nullopt;
}

// BUG-1260: the statistics served after a weighted regress or areg -- the ones
// computePredictWeightedM4 builds from e(b), y and the estimation sample.
bool predictServesWeightedM4(PredictFamilyM4 f, const std::string& s) {
  if (f == PredictFamilyM4::Regress) return s == "xb" || s == "residuals" || s == "score" || s == "stdp";
  if (f == PredictFamilyM4::Areg)
    return s == "xb" || s == "xbd" || s == "d" || s == "residuals" || s == "score" || s == "dresiduals";
  return false;
}

// BUG-1260: predict after a weighted regress or areg ([aw]/[fw]/[pw]/[iw]). The
// engine's predict refits without the weight (BUG-519), so nothing here calls
// it: xb is e(b)'s design over every row with the covariates; after areg the
// absorbed effect of a level is the weighted mean of y - xb over its e(sample)
// rows -- the normal equation of the level's indicator in the weighted least
// squares the fit solved -- whose weighted sum over e(sample) is zero exactly
// when _cons is the weighted ybar - xbar b, checked before anything is returned
// (StataBE 19.5 scratch-laneA/agents/p-margins/s6.do, `areg y x [aw=wv],
// absorb(f2)`: xbd[5] 40.406576265917387 on the 237 e(sample) rows, the weighted
// mean of d 1.6e-15; the [fw] fit the same). stdp after regress is sqrt(x V x')
// over the weighted fit's own e(V) (lincom's refit carries the weight, BUG-571).
std::optional<ojson> computePredictWeightedM4(SessionState& session, const LastModel& lm,
                                              PredictFamilyM4 fam, const std::string& stat,
                                              std::vector<double>& vals) {
  const std::string cmd = predictFamilyCmdM4(fam, lm);
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after a weighted `" + cmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Nothing was written to your data.",
                              "unsupported_option");
  };
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array())
    return refuse("the fitted coefficients are not available");
  std::vector<std::string> names;
  std::vector<double> b, se;
  for (const auto& row : (*lm.result)["coeftable"]) {
    names.push_back(row.value("name", std::string()));
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  const bool areg = fam == PredictFamilyM4::Areg;
  if (areg && lm.fe.size() != 1) return refuse("areg absorbs one variable");
  std::vector<double> V;
  if (stat == "stdp") {
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, names, b, se, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }
  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  std::vector<DesignColM4> design;
  std::string why;
  if (!buildDesignM4(*live, names, design, why)) return refuse(why);
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> xb(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    double s = 0.0;
    bool ok = true;
    for (size_t j = 0; j < design.size() && ok; ++j) {
      const double v = designValueM4(design[j], r);
      if (std::isnan(v)) ok = false;
      else s += v * b[j];
    }
    if (ok) xb[r] = s;
  }
  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).f64() != nullptr) y = live->col(lm.y).f64();
  if (y == nullptr && stat != "xb" && stat != "stdp")
    return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");
  vals.assign(n, kNaN);
  if (stat == "xb") {
    vals = xb;
    return std::nullopt;
  }
  if (stat == "stdp") {
    const size_t k = names.size();
    for (size_t r = 0; r < n; ++r) {
      std::vector<double> x(k);
      bool ok = true;
      for (size_t j = 0; j < k && ok; ++j) {
        x[j] = designValueM4(design[j], r);
        ok = !std::isnan(x[j]);
      }
      if (!ok) continue;
      double q = 0.0;
      for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l < k; ++l) q += x[j] * V[j * k + l] * x[l];
      vals[r] = std::sqrt(std::max(0.0, q));
    }
    return std::nullopt;
  }
  if (!areg || stat == "dresiduals") {
    // regress residuals/score and areg dresiduals: y - xb wherever both exist.
    for (size_t r = 0; r < n; ++r) vals[r] = y[r] - xb[r];
    return std::nullopt;
  }
  // areg's absorbed effects over e(sample).
  const SessionState::EsampleReading er = session.esampleReading();
  if (er.kind != SessionState::EsampleReading::Kind::kColumn || !live->has(kEsampleColumn) ||
      live->col(kEsampleColumn).f64() == nullptr)
    return refuse("the estimation sample is not on the data in memory");
  const double* m = live->col(kEsampleColumn).f64();
  const std::string& g = lm.fe.front();
  if (!live->has(g) || live->col(g).f64() == nullptr || !live->has(lm.fitWeight) ||
      live->col(lm.fitWeight).f64() == nullptr)
    return refuse("the absorbed variable or the weight is not a numeric variable of the data");
  const double* gv = live->col(g).f64();
  const double* w = live->col(lm.fitWeight).f64();
  std::map<double, std::pair<double, double>> grp;  // level -> (sum w, sum w (y - xb))
  double count = 0.0, sumW = 0.0;
  for (size_t r = 0; r < n; ++r) {
    if (m[r] != static_cast<double>(er.serial)) continue;
    if (std::isnan(y[r]) || std::isnan(xb[r]) || std::isnan(gv[r]) || std::isnan(w[r]))
      return refuse("an estimation-sample row has no linear prediction");
    auto& a = grp[gv[r]];
    a.first += w[r];
    a.second += w[r] * (y[r] - xb[r]);
    count += 1.0;
    sumW += w[r];
  }
  const double eN = jsonNumM4(*lm.result, "n_obs");
  const bool fw = lm.fitWtype == "fw" || lm.fitWtype == "fweight";
  if (!(std::isfinite(eN) && (fw ? sumW : count) == eN))
    return refuse("the estimation sample on the data in memory is not the fitted one");
  double wsumD = 0.0, scale = 0.0;
  for (const auto& [lv, a] : grp) {
    if (!(a.first > 0.0)) return refuse("a level of the absorbed variable carries no weight");
    wsumD += a.second;
    scale += std::fabs(a.second);
  }
  for (size_t r = 0; r < n; ++r)
    if (m[r] == static_cast<double>(er.serial)) scale += std::fabs(w[r] * (y[r] - xb[r]));
  if (std::fabs(wsumD) > 1e-9 * std::max(1.0, scale))
    return refuse("the absorbed effects do not sum to zero over the estimation sample, so the "
                  "design read from e(b) is not the fitted one");
  for (size_t r = 0; r < n; ++r) {
    if (m[r] != static_cast<double>(er.serial)) continue;
    const auto& a = grp[gv[r]];
    const double d = a.second / a.first;
    if (stat == "d") vals[r] = d;
    else if (stat == "xbd") vals[r] = xb[r] + d;
    else vals[r] = y[r] - xb[r] - d;  // residuals, score
  }
  return std::nullopt;
}

// BUG-1257: stdp, score and ir after logit/probit/poisson, over every row whose
// covariates are there (out of the estimation sample too, as Stata fills them),
// from e(b) and the design read off the live frame. `engineArgs` is runPredict's
// --model replay argv (no --xb/--pred/--out); its xb must be the one built here
// on every row, which also refuses a fit whose linear index carries something
// e(b) does not (an offset). stdp is sqrt(x V x'), V from the engine's lincom
// (lincomVcovM4). The equation score, d lnL / d xb, is StataBE 19.5's: logit
// (y != 0) - p and probit phi/Phi or -phi/(1 - Phi), where a missing y counts as
// nonzero as `y != 0` does in Stata; cloglog's f/F or -exp(xb), missing with y;
// poisson y - exp(xb), missing with y. ir is exp(xb).
std::optional<ojson> computePredictMlM4(SessionState& session, const LastModel& lm,
                                        const std::string& stat,
                                        std::vector<std::string> engineArgs,
                                        std::vector<double>& vals) {
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after `" + lm.cobraCmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Nothing was written to your data.",
                              "unsupported_option");
  };
  std::vector<std::string> names;
  std::vector<double> b, se;
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array()) {
    return refuse("the fitted coefficients are not available");
  }
  for (const auto& row : (*lm.result)["coeftable"]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) {
      return refuse("the fitted coefficients are not available");
    }
    names.push_back(row["name"].get<std::string>());
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  std::vector<double> V;
  if (stat == "stdp") {
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, names, b, se, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }

  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  for (size_t i = 0; i + 1 < engineArgs.size(); ++i) {
    if ((engineArgs[i] == "--data" || engineArgs[i] == "--fit-data") &&
        !cecon::native_exists(engineArgs[i + 1])) {
      return refuse("the data the model was fitted on are not available on disk");
    }
  }
  const std::string kXb = "__piet_predict_xb__";
  const std::string scratch = nextPathEst(session.workdir(), "predict_refit");
  engineArgs.insert(engineArgs.end(), {"--xb", kXb, "--out", scratch});
  const CobraRunResult rr = runCobraOneShot(engineArgs);
  if (!rr.ok || !cecon::native_exists(scratch)) {
    cecon::native_unlink(scratch);
    const std::string err = rr.stderr_text.empty() ? rr.error : rr.stderr_text;
    return refuse("the engine's refit failed (" + strutil::trim(err) + ")");
  }
  std::vector<double> engXb;
  try {
    Datastore eng(scratch);
    if (eng.nRows() != live->nRows() || !eng.has(kXb) || eng.col(kXb).f64() == nullptr) {
      throw std::runtime_error("the engine's refit returned an unexpected frame");
    }
    const double* px = eng.col(kXb).f64();
    engXb.assign(px, px + eng.nRows());
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    return refuse(e.what());
  }
  cecon::native_unlink(scratch);

  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  if (engXb.size() != n) return refuse("the engine's refit returned an unexpected frame");
  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).dtype == cecon::DType::F64) y = live->col(lm.y).f64();
  if (stat == "score" && y == nullptr) {
    return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");
  }
  std::vector<DesignColM4> design;
  std::string why;
  if (!buildDesignM4(*live, names, design, why)) return refuse(why);

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const size_t k = design.size();
  std::vector<double> x(k);
  vals.assign(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    bool ok = true;
    double xb = 0.0;
    for (size_t j = 0; j < k && ok; ++j) {
      x[j] = designValueM4(design[j], r);
      if (std::isnan(x[j])) ok = false;
      else xb += x[j] * b[j];
    }
    if (!ok) {
      if (!std::isnan(engXb[r])) return refuse("the design read from e(b) does not cover a row the engine predicts");
      continue;
    }
    if (std::isnan(engXb[r]) || std::fabs(xb - engXb[r]) > 1e-9 * (1.0 + std::fabs(engXb[r]))) {
      return refuse("the design read from e(b) does not reproduce the engine's linear prediction");
    }
    if (stat == "stdp") {
      double s = 0.0;
      for (size_t j = 0; j < k; ++j) {
        double row = 0.0;
        for (size_t l = 0; l < k; ++l) row += V[j * k + l] * x[l];
        s += x[j] * row;
      }
      vals[r] = std::sqrt(std::max(0.0, s));
    } else if (stat == "ir") {
      vals[r] = std::exp(xb);
    } else if (lm.cobraCmd == "poisson") {
      vals[r] = y[r] - std::exp(xb);  // missing with y
    } else if (lm.cobraCmd == "logit") {
      vals[r] = (y[r] != 0.0 ? 1.0 : 0.0) - 1.0 / (1.0 + std::exp(-xb));
    } else if (lm.cobraCmd == "cloglog") {
      // F = 1 - exp(-exp(xb)): f/F = exp(xb) exp(-exp(xb)) / F when y != 0, -exp(xb)
      // else; missing with y, unlike logit's (StataBE 19.5, BUG-1257.do's data:
      // obs1 -.504196078796196, obs2 .713718029916928, obs30 with d missing `.`).
      const double ex = std::exp(xb);
      vals[r] = std::isnan(y[r]) ? kNaN : y[r] != 0.0 ? ex * std::exp(-ex) / -std::expm1(-ex) : -ex;
    } else {
      const double phi = std::exp(-0.5 * xb * xb) / std::sqrt(2.0 * 3.14159265358979323846);
      vals[r] = y[r] != 0.0 ? phi / (0.5 * std::erfc(-xb / std::sqrt(2.0)))
                            : -phi / (0.5 * std::erfc(xb / std::sqrt(2.0)));
    }
  }
  return std::nullopt;
}

// Write `vals` as the new f64 column `name` of the live frame into `outPath`
// (`cobra setcol --new`, the payload shape estimWriteColumn uses: one value per
// row, an EMPTY line for missing). `storeType` rides as --store-type: the engine
// stores each value as that Stata type holds it (BUG-924).
std::optional<ojson> writePredictColumnM4(SessionState& session, const std::string& liveHead,
                                          const std::string& name, const std::vector<double>& vals,
                                          const std::string& outPath, const std::string& storeType) {
  const std::string hb = mintScratchPath(session.workdir(), "predict_values");
  forgetScratchMint(hb);
  const std::string csv = hb.substr(0, hb.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double v : vals) {
      if (!std::isnan(v)) f << v;
      f << "\n";
    }
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
  }
  std::vector<std::string> argv = {"setcol", "--data", liveHead, "--var", name, "--values", csv,
                                   "--dtype", "f64", "--new", "--out", outPath};
  storage::appendStoreTypeArg(argv, storeType);
  const CobraRunResult r = runCobra(argv);
  cecon::native_unlink(csv);
  if (!r.ok) {
    return engineErrorBlockM4("predict: could not create " + name + " (" +
                              strutil::trim(r.stderr_text) + ")");
  }
  // The engine's writer creates --out before it can detect a write failure (the
  // estimWriteColumn note): a missing file is a failed write, not a head.
  if (!cecon::native_exists(outPath)) {
    return engineErrorBlockM4("predict: failed writing output dataset (disk full or I/O error)");
  }
  return std::nullopt;
}

// BUG-466: predict after `teffects nnmatch` (_teffects_match_p.ado): te, po and
// distance, read off the fit's own reply -- the engine returned them with the
// estimate (cmds_nnmatch.cpp: te, po0, po1 and each row's match distances,
// null off e(sample)) and LastModel holds that reply. StataBE 19.5
// (internal capture oracle.txt CASE 1, data_1.csv):
// `predict double te_, te`, `predict double po0_ po1_, po` and `predict double
// dd_*, distance` rc 0. The ado's own checks: generate() at estimation (else
// r(322)), the observation count unchanged (else r(459)), one statistic (else
// r(184)), te by default; one name for po is the control's outcome unless
// tlevel() names the treated level; distance makes one variable per index
// variable (a stub* or that many names); no if/in in its syntax.
ojson runNnmatchPredictM4(const std::vector<std::string>& varlist,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          SessionState& session, const LastModel& lm,
                          const std::optional<std::string>& ifExpr) {
  auto err = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  if (ifExpr.has_value() && !ifExpr->empty()) return err("if not allowed", 101);
  std::string which, tlevel;
  int nWhich = 0;
  for (const auto& [k, v] : opts) {
    if (k == "quietly" || k == "noisily") continue;
    if (k == "te" || k == "po" || k == "distance" || k == "dist" || k == "dista" || k == "distan" ||
        k == "distanc" || k == "biasadj" || k == "km" || k == "kn") {
      which = k.rfind("dist", 0) == 0 ? "distance" : k;
      ++nWhich;
    } else if (k == "tlevel" || k == "tl" || k == "tle" || k == "tlev" || k == "tleve") {
      tlevel = trimM4(v.strValue);
    } else if (k == "ps" || k == "lnsigma" || k == "lns" || k == "lnsi" || k == "lnsig" || k == "lnsigm") {
      return err("option " + std::string(k == "ps" ? "ps" : "lnsigma") + " not allowed", 198);
    } else {
      return err("option " + k + " not allowed", 198);
    }
  }
  if (nWhich > 1)
    return err("only one of te, po, km, kn, ps, lnsigma or distance may be specified", 184);
  if (which.empty()) which = "te";
  if (which == "biasadj") return err("option biasadj not allowed; bias adjustment was not used at estimation", 322);
  if (which == "km" || which == "kn")
    return engineErrorBlockM4("predict, " + which + " after `teffects nnmatch` is not supported in this "
                              "version of PIET. Nothing was written to your data.",
                              "unsupported_option");
  const std::string* indexvar = session.eval.eStrings.find("indexvar");
  if (indexvar == nullptr || indexvar->empty())
    return err("postestimation can only be performed when the generate(stub) option is used during "
               "estimation; see help generate(stub)", 322);
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("te") ||
      !(*lm.result)["te"].is_array())
    return engineErrorBlockM4("predict after `teffects nnmatch`: the fit's results are not available",
                              "unsupported_option");
  const ojson& res = *lm.result;
  const size_t nFit = res["te"].size();
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return engineErrorBlockM4(std::string("predict: ") + e.what());
  }
  if (live == nullptr) return eval::noDataLoaded();
  if (static_cast<size_t>(live->nRows()) != nFit)
    return err("there were " + std::to_string(nFit) + " observations at estimation time but there are " +
                   std::to_string(live->nRows()) + " now; nearest-neighbor indices are no longer valid",
               459);
  // Names: a leading storage type, then names or one stub*.
  std::vector<std::string> toks = varlist;
  std::string declaredType;
  if (toks.size() >= 2 && storage::isNumericType(lowerM4(toks[0]))) {
    declaredType = lowerM4(toks[0]);
    toks.erase(toks.begin());
  }
  const double control = res.value("control_level", 0.0), treated = res.value("treated_level", 1.0);
  const std::string tvar = res.value("tvar", std::string());
  auto levelText = [&](double lv) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(lv)));
    std::string t = buf;
    const auto vl = session.valueLabels.find(tvar);
    if (vl != session.valueLabels.end()) {
      const auto it = vl->second.find(static_cast<int>(std::llround(lv)));
      if (it != vl->second.end()) t = it->second;
    }
    return t;
  };
  int kvar = 0;
  for (size_t p = 0; p < indexvar->size();) {
    const size_t q = indexvar->find_first_not_of(' ', p);
    if (q == std::string::npos) break;
    const size_t e = indexvar->find(' ', q);
    ++kvar;
    p = e == std::string::npos ? indexvar->size() : e;
  }
  std::vector<std::string> names, labels;
  std::vector<std::vector<double>> cols;
  auto arr = [&](const ojson& a, size_t r) {
    return r < a.size() && a[r].is_number() ? a[r].get<double>() : std::numeric_limits<double>::quiet_NaN();
  };
  const size_t n = nFit;
  if (which == "te") {
    if (toks.size() != 1 || toks[0].find('*') != std::string::npos)
      return err(toks.size() > 1 ? "too many variables specified" : "varlist required", toks.size() > 1 ? 103 : 100);
    std::vector<double> c(n);
    for (size_t r = 0; r < n; ++r) c[r] = arr(res["te"], r);
    names.push_back(toks[0]);
    labels.push_back("treatment effects");
    cols.push_back(std::move(c));
  } else if (which == "po") {
    std::vector<std::string> pn = toks;
    if (pn.size() == 1 && pn[0].size() > 1 && pn[0].back() == '*' && tlevel.empty()) {
      const std::string stub = pn[0].substr(0, pn[0].size() - 1);
      pn = {stub + "0", stub + "1"};
    }
    if (pn.size() == 2 && tlevel.empty()) {
      std::vector<double> c0(n), c1(n);
      for (size_t r = 0; r < n; ++r) {
        c0[r] = arr(res["po0"], r);
        c1[r] = arr(res["po1"], r);
      }
      names = pn;
      labels = {"potential outcome, " + tvar + "=" + levelText(control),
                "potential outcome, " + tvar + "=" + levelText(treated)};
      cols.push_back(std::move(c0));
      cols.push_back(std::move(c1));
    } else if (pn.size() == 1 && pn[0].find('*') == std::string::npos) {
      double lv = std::min(control, treated);
      if (!tlevel.empty()) {
        char* end = nullptr;
        lv = std::strtod(tlevel.c_str(), &end);
        if (end == nullptr || *end != '\0' || (lv != control && lv != treated))
          return err("tlevel() must be valid treatment level", 198);
      }
      const bool isTreated = lv == treated;
      std::vector<double> c(n);
      for (size_t r = 0; r < n; ++r) c[r] = arr(res[isTreated ? "po1" : "po0"], r);
      names = pn;
      labels = {"potential outcome, " + tvar + "=" + levelText(isTreated ? treated : control)};
      cols.push_back(std::move(c));
    } else {
      return err("po requires two new variable names or one with tlevel()", 198);
    }
  } else {  // distance
    if (!tlevel.empty()) return err("tlevel() may only be specified with po or ps", 198);
    if (!res.contains("dist") || !res["dist"].is_array() || res["dist"].size() != n)
      return engineErrorBlockM4("predict, distance after `teffects nnmatch`: the fit's match distances "
                                "are not available",
                                "unsupported_option");
    std::vector<std::string> dn = toks;
    if (dn.size() == 1 && dn[0].size() > 1 && dn[0].back() == '*') {
      const std::string stub = dn[0].substr(0, dn[0].size() - 1);
      dn.clear();
      for (int k = 1; k <= kvar; ++k) dn.push_back(stub + std::to_string(k));
    }
    if (static_cast<int>(dn.size()) != kvar)
      return err(static_cast<int>(dn.size()) > kvar ? "too many variables specified"
                                                    : "too few variables specified",
                 static_cast<int>(dn.size()) > kvar ? 103 : 102);
    const std::string metric = res.value("metric", std::string("mahalanobis"));
    for (int k = 0; k < kvar; ++k) {
      std::vector<double> c(n);
      for (size_t r = 0; r < n; ++r) {
        const ojson& d = res["dist"][r];
        c[r] = d.is_array() && static_cast<int>(d.size()) > k && d[k].is_number()
                   ? d[k].get<double>()
                   : std::numeric_limits<double>::quiet_NaN();
      }
      names.push_back(dn[k]);
      labels.push_back("nearest-neighbor distance using " + metric);
      cols.push_back(std::move(c));
    }
  }
  for (const auto& nm : names) {
    if (!strutil::isValidStataIdentifier(nm)) return err(nm + " invalid name", 198);
    if (live->has(nm)) return ojson(eval::engineError("variable " + nm + " already defined"));
  }
  const std::string storeType = storage::resolveCreateType(session, declaredType);
  std::string head = *session.currentHbin();
  std::vector<std::string> written;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string out = nextPathEst(session.workdir(), "predict");
    if (auto rej = writePredictColumnM4(session, head, names[i], cols[i], out, storeType)) {
      for (const auto& w : written) cecon::native_unlink(w);
      return *rej;
    }
    written.push_back(out);
    head = out;
  }
  session.load(head);
  session.invalidateDatastore();
  for (size_t i = 0; i < names.size(); ++i) {
    storage::recordCreateType(session, names[i], declaredType, storeType);
    session.varMeta[names[i]].label = labels[i];
  }
  session.syncEvalVarLabels();
  Datastore* ds = session.datastore();
  session.seedNMissForPureAdd(ds, "gen", names, /*droppedVars=*/{}, /*rewrittenVars=*/{});
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "gen";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  out["new_vars"] = names;
  return out;
}

}  // namespace

ojson runPredict(const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session, const std::optional<std::string>& ifExpr) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) {
    // BUG-1248: with no estimates predict still reads its new variable and its `if`
    // before e(), with data in memory or none (StataBE 19.5): `predict` is r(100)
    // "varlist required", `predict 1p` r(198) "1p invalid name", `predict x1`
    // r(110) "variable x1 already defined", `predict p if nosuch > 1` r(111)
    // "nosuch not found"; `predict p`, `predict p if 1`, `predict p, bogus` and
    // `predict double p` go on to r(301).
    auto refuse = [](const std::string& message, int rc) {
      return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt,
                                    std::nullopt, "r(" + std::to_string(rc) + ")"));
    };
    if (varlist.empty()) return refuse("varlist required", 100);
    const size_t first =
        varlist.size() >= 2 && storage::isNumericType(lowerM4(varlist[0])) ? 1 : 0;
    for (size_t i = first; i < varlist.size(); ++i) {
      const std::string& nm = varlist[i];
      if (!strutil::isValidStataIdentifier(nm)) return refuse(nm + " invalid name", 198);
      if (resolveVarName(nm, session) == nm)
        return refuse("variable " + nm + " already defined", 110);
    }
    if (ifExpr.has_value()) {
      if (auto unbound = exprUnboundNameRefusal(*ifExpr, session))  // "r(111): NAME not found"
        return refuse(unbound->substr(unbound->find(": ") + 2), 111);
    }
    return noPreviousEstimationFor(session, true);
  }
  const LastModel& lm = *lmOpt;
  if (lm.cobraCmd == "nnmatch") return runNnmatchPredictM4(varlist, opts, session, lm, ifExpr);  // BUG-466
  // BUG-204 (BUG-203's sibling, and the worse half): `predict` never read the
  // estimator either. The argv built below carries --data/--y/--x and no model,
  // and cobra's cmd_predict calls feols_fit() unconditionally, so `logit y x1 x2`
  // -> `predict phat` wrote the LINEAR-PROBABILITY fitted values into the user's
  // dataset under a name they believe holds Pr(y=1). Measured against StataNow
  // 19.5: min -0.06666 / max 1.20595 where Stata's own predict gives min 0.04791
  // / max 0.97712 — values outside [0,1] for a probability, and they PERSIST as
  // data, surviving save/export, which is why this outranks the lincom shape.
  //
  // Why no gate saw it: OLS fitted values average to ybar and so do logit
  // predicted probabilities, so the MEAN agrees to 15 digits (0.5633333...) and
  // every mean-based sanity check passed identically. Any pin on this path must
  // assert the RANGE, not central tendency.
  //
  // BUG-297 closes BUG-204's open half for logit/probit/poisson: cobra's
  // cmd_predict now HAS a `--model` GLM prediction path (the postest_fit_spec
  // dispatcher BUG-203 built — the same refit lincom/nlcom/test forward to),
  // emitting Stata's default statistic for the model: `pr` after logit/probit,
  // `n` after poisson, `xb` on request. So the refusal NARROWS to exactly the
  // estimators the engine still cannot re-fit (nbreg, tobit, heckman, qreg,
  // xtreg-re, fepois/ppmlhdfe/xtpois/xtlogit — no absorbed-effects ML refit —
  // newey, prais, iv/liml/reg3, csdid, ...), same membership as lincom/nlcom/
  // test's postestGlmCmdsM4 gate. Unlike those verbs, predict is a POINT
  // prediction — it reads only beta, which vce() never moves — so a
  // logit/probit/poisson fitted with vce(robust)/vce(cluster) still predicts
  // (the argv below carries no --vcov/--cluster and the engine refit is the
  // plain fit with the identical beta). Absorbed FE would move beta, so that
  // still refuses.
  const bool glmPredict = postestGlmCmdsM4().count(lm.cobraCmd) > 0;
  if (!postestOlsCmdsM4().count(lm.cobraCmd) && !glmPredict) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` in this version of PIET — the engine can compute predictions only for the "
            "least-squares family and for logit/probit/poisson (via a maximum-likelihood "
            "re-fit); `" + lm.cobraCmd +
            "` has neither, so the column it could write would not be this model's "
            "prediction. Refusing rather than writing a number that is not the one asked "
            "for into your data.",
        "unsupported_option");
  }
  // FU-131: a no-constant fit predicts from the no-constant refit (--no-intercept
  // below; w2p_estim_ev.log:1387 G_nc_predict_xb, :1402 G_nc_predict_res), except
  // where the refit cannot drop the constant.
  if (postestNoConstantUnrefittableM4(lm)) return postestNoConstantRejectM4("predict", lm);
  if (glmPredict && !lm.fe.empty()) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` with absorbed fixed effects in this version of PIET — the engine's "
            "maximum-likelihood re-fit has no absorbed-effects path, so its coefficients "
            "(and therefore its predictions) would differ from the model you fitted. "
            "Refusing rather than predicting from a different model.",
        "unsupported_option");
  }
  // BUG-519: predict after a WEIGHTED fit wrote the UNWEIGHTED model's
  // predictions at rc 0. The fit's [aw/fw/pw/iw=] lives in lm.fitWeight (margins
  // forwards it), but the engine's predict has no weight input at all --
  // cmd_predict's refit is `build_model(*fit_ds, y, xs, fes, {})` and its ML path
  // reads only --model/--fe/--vcov/--cluster -- so every branch below (head
  // unmoved, `--fit-data`, the absorbed-FE refit, the logit/probit/poisson ML
  // refit) re-estimated without the weight. Measured on main 2026-09-22:
  // `regress y x2 t [aw=w]` / `predict p` -> p differs from _b[_cons] + _b[x2]*x2
  // + _b[t]*t by up to 0.38 on 12 rows, rc 0; the same under [fw=]. Stata serves
  // it (aer_b4/predict-e-xbd.log J_build_areg: `predict double A_xb, xb` after
  // `areg ... [aw=wv]`, rc 0) -- this build cannot, so it refuses by name and
  // writes nothing. BUG-1260: after a weighted regress or areg the statistics
  // computePredictWeightedM4 builds without the engine are served; every other
  // weighted fit and statistic keeps this refusal (the statistic's half below,
  // once the options are read).
  const bool weightedFit = !lm.fitWeight.empty();
  auto weightedRefusal = [&lm]() {
    return engineErrorBlockM4(
        "predict after a weighted fit ([" + (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) +
            "=" + lm.fitWeight + "]) is not supported in this version of PIET -- the "
            "engine's prediction re-fits the model without the weights, so the column it "
            "could write would hold a different model's predictions. Refusing rather than "
            "writing a number that is not the one asked for into your data. Nothing was "
            "written to your data.",
        "unsupported_option");
  };
  if (weightedFit && (glmPredict || (predictFamilyM4(lm) != PredictFamilyM4::Regress &&
                                     predictFamilyM4(lm) != PredictFamilyM4::Areg))) {
    return weightedRefusal();
  }

  static const std::set<std::string> kSupported = {"xb", "resid", "residuals", "fe"};
  static const std::set<std::string> kUnsupportedStats = {
      "cooksd", "leverage", "hat",     "stdp",     "stdf",   "stdr",  "rstandard",
      "rstudent", "dfbeta",  "pr",     "xbu",      "dresid", "anscombe",
      "deviance", "pearson", "response", "score",  "working",
  };
  bool wantXb = false, wantResid = false, wantMlDefault = false;
  std::string mlStat;  // stdp/score/ir after logit/probit/poisson (computePredictMlM4)
  const PredictFamilyM4 family = glmPredict ? PredictFamilyM4::Other : predictFamilyM4(lm);
  // FU-768: an absorbed-effects fit restricted by `if`/`in` without time-series
  // operators (those keep FU-759's own gate below). The engine refits it under
  // the restriction as a row mask over the live frame (cmd_predict --fit-keepif
  // with --fe), so its within residual lands on the fit's rows.
  const bool restrictedAbsorbed = !glmPredict && !lm.fe.empty() &&
                                  (!lm.fitKeepif.empty() || !lm.fitDataPath.empty()) &&
                                  !postestTsKeepifRefusalM4("predict", lm).has_value();
  std::string predictStat;  // the statistic, for the four families above
  bool nativeStat = false;  // computed here rather than by the engine's predict
  if (glmPredict) {
    // The ML option surface, per Stata: bare `predict newvar` = the model's
    // default statistic (pr after logit/probit, n after poisson); the explicit
    // spelling of that default (`pr` / `n`) is accepted; `xb` is the linear
    // index. EVERYTHING else refuses BY NAME — including `resid`/`residuals`,
    // which after an ML fit is a DIFFERENT statistic in Stata (a Pearson
    // residual, not y - xb), so serving the OLS shape under that name would be
    // BUG-204's silent-wrong all over again.
    //
    // BUG-1257: the refusal was rc 1. A statistic the estimator's own predict does
    // not take is Stata's r(198) "option ir not allowed"; one it takes that this
    // build does not compute is refused by name, r(198) as every option this build
    // will not take. stdp, score and ir are computed here (computePredictMlM4).
    const std::string defaultStat = (lm.cobraCmd == "poisson") ? "n" : "pr";
    for (const auto& [k, v] : opts) {
      if (k == "quietly" || k == "noisily") continue;
      // poisson's pr() takes its count(s): `pr(0)`, `pr(1,3)`; bare `pr` is refused.
      const std::string stat = (lm.cobraCmd == "poisson" && k == "pr" && !v.isBool)
                                   ? std::string("pr()")
                                   : mlPredictStatisticM4(lm.cobraCmd, k);
      if (stat.empty() || (stat != "pr()" && !(v.isBool && v.boolValue))) {
        return engineErrorBlockM4("option " + k + " not allowed", "syntax_error");
      }
      if (stat == "xb") { wantXb = true; continue; }
      if (stat == defaultStat) { wantMlDefault = true; continue; }
      if (stat == "stdp" || stat == "score" || stat == "ir") { mlStat = stat; continue; }
      return engineErrorBlockM4(
          "predict " + k + ": not supported after `" + lm.cobraCmd +
              "` in this version of PIET (supported there: the default `" + defaultStat +
              "` statistic, `xb`, `stdp`, `score`" +
              (lm.cobraCmd == "poisson" ? std::string(", `ir`") : std::string()) +
              "). Nothing was written to your data.",
          "unsupported_option");
    }
    if (!wantXb && mlStat.empty()) wantMlDefault = true;  // bare `predict newvar`
  } else if (family != PredictFamilyM4::Other) {
    // BUG-596: regress / areg / xtreg,fe / reghdfe answer their OWN statistic
    // vocabulary (the block above computePredictNativeM4). The engine keeps the
    // two it already writes as Stata defines them; everything else is computed
    // below from e(b), e(V) and the engine refit's estimation sample.
    if (auto rej = resolvePredictStatM4(family, lm, opts, predictStat)) return *rej;
    if (restrictedAbsorbed && !predictServesRestrictedM4(family, predictStat)) {
      return engineErrorBlockM4("predict, " + predictStat + " after a restricted `" +
                                    predictFamilyCmdM4(family, lm) + " ... if` fit is not supported "
                                    "in this version of PIET. Nothing was written to your data.",
                                "unsupported_option");
    }
    const std::string served = predictEngineServedM4(family, predictStat);
    wantXb = served == "xb";
    wantResid = served == "resid";
    nativeStat = served.empty();
    if (weightedFit) {
      // BUG-1260: no engine refit after a weighted fit (computePredictWeightedM4).
      if (!predictServesWeightedM4(family, predictStat)) return weightedRefusal();
      wantXb = wantResid = false;
      nativeStat = true;
    }
  } else {
  if (restrictedAbsorbed) {
    // FU-768: only the four families above have a captured restricted-fit answer.
    return engineErrorBlockM4("predict after a restricted fit with absorbed effects is not "
                              "supported after `" + lm.cobraCmd + "` in this version of PIET. "
                              "Nothing was written to your data.",
                              "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    if (kUnsupportedStats.count(k)) {
      // B4 P1: `unsupported_option` -> r(198). This refusal defaulted to
      // `engine_error`, i.e. rc 1 — a code no Stata command answers with, and
      // the one the standing rule reserves for nothing at all. An option this
      // build will not take is r(198), whatever the reason.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
    // Audit fix: this check used to also require `v.isBool && v.boolValue`,
    // so it only ever caught unknown BOOLEAN flags — an unknown VALUED
    // option (e.g. a typo'd `predict yhat, level(95)`, where `level` isn't
    // a real predict() option and v.isBool is false) matched neither this
    // branch nor kUnsupportedStats above, and fell through completely
    // silently (defaulting to plain `xb`) instead of naming the bad
    // option. Any key not in kSupported/kUnsupportedStats/quietly/noisily
    // is unrecognized regardless of whether it was written as a bare flag
    // or a valued option.
    if (!kSupported.count(k) && k != "quietly" && k != "noisily") {
      // B4 P1: same rc correction as the branch above — see its comment.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
  }

  for (const auto& [k, v] : opts) {
    if (k == "xb" && v.isBool && v.boolValue) wantXb = true;
    if ((k == "resid" || k == "residuals") && v.isBool && v.boolValue) wantResid = true;
  }
  if (!wantXb && !wantResid) wantXb = true;
  }

  // BUG-127: `predict <type> newvar` used to take the storage-type token AS
  // the new variable's name and silently discard the real name (`predict
  // double yh, xb` created a column literally named `double`; `yh` never
  // existed — gate-① silent-wrong). The leading token is stripped the way
  // gen/egen's stripStorageType does.
  //
  // BUG-924: and the new variable HAS that type, values included. Stata's predict
  // creates it as the declared type, else c(type) -- float by default -- and
  // stores the values that type holds (w2aa_predict_type_restricted.log a_p_default
  // `+1.dda8fe0000000X+003|float`, a_pf_float_xb, a_pi_int_xb `int` truncated,
  // a_p_settype_double `double`; w2e_float_default.log predict_default). PIET
  // stored the double under a float label, so `count if p == pd` was 60 where
  // Stata's is 0 (a_eq_p_pd). One resolver (storage::resolveCreateType, generate's)
  // decides the type and the engine rounds on store (--store-type, BUG-471 T1).
  std::vector<std::string> nameToks = varlist;
  std::string declaredType;  // the typed token, "" = none
  if (nameToks.size() >= 2 && storage::isNumericType(lowerM4(nameToks[0]))) {
    declaredType = lowerM4(nameToks[0]);
    nameToks.erase(nameToks.begin());
  }
  std::string storeType;
  if (family == PredictFamilyM4::Reghdfe || lm.stataCmd == "reghdfe") {
    // reghdfe's predict writes double whatever type it is given:
    // predict-e-xbd.log P_res_xb/xbd/d/residuals (`predict float Q_<stat>`) are
    // rc 0 and type_xbd reads `double`; w2aa c_hdfe_* untyped read `double`.
    // An integer type there is uncaptured.
    if (storage::isIntegerType(declaredType)) {
      return engineErrorBlockM4("predict: storage type " + declaredType +
                                    " is not supported after `reghdfe` in this version of PIET",
                                "unsupported_option");
    }
    storeType = "double";
  } else {
    storeType = storage::resolveCreateType(session, declaredType);
  }
  std::string newvar = nameToks.empty() ? std::string() : nameToks[0];
  if (newvar.empty()) {
    return engineErrorBlockM4("predict: a new variable name is required, e.g. `predict yhat`.",
                              "syntax_error");
  }
  // BUG-1120: `predict _cons' and `predict _b, resid' are "_cons invalid varname"
  // r(198), `predict _all, resid' "_all not allowed" r(101); `predict _se, stdp'
  // is legal (StataBE 19.5).
  if (strutil::isReservedVarName(newvar) || newvar == "_skip" || newvar == "_all") {
    const bool skipAll = !strutil::isReservedVarName(newvar);
    return ojson(eval::errorBlock("syntax_error",
                                  newvar + (skipAll ? " not allowed" : " invalid varname"),
                                  std::nullopt, std::nullopt, std::nullopt,
                                  skipAll ? "r(101)" : "r(198)"));
  }
  // BUG-597: `predict z` with a variable z already in the data REPLACED it at
  // rc 0 -- the engine's predict writes its column by name, and set_f64_column
  // overwrites (measured on main: `generate double z = 7` / `predict z, xb`
  // left z holding the fitted values, rc 0). Stata's predict creates a NEW
  // variable and answers r(110) "variable z already defined", writing nothing.
  // Card first, datastore as the fallback (residentReadCard's contract), so a
  // dirty virtual head is not snapshotted just to answer a name question.
  {
    bool exists = false;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const VarInfo& vi : *card) {
        if (vi.name == newvar) exists = true;
      }
    } else {
      Datastore* cur = nullptr;
      try {
        cur = session.datastore();
      } catch (const std::exception& e) {
        return engineErrorBlockM4(std::string("predict: ") + e.what());
      }
      exists = cur != nullptr && cur->has(newvar);
    }
    if (exists) return ojson(eval::engineError("variable " + newvar + " already defined"));
  }

  // BUG-516: predict computes on the LIVE frame and ADDS one column to it.
  //
  // `--data` used to be lm.dataPath -- the frame as it stood when the model was
  // FIT -- and the engine's output (that frame + newvar) was then adopted as the
  // session head. The first predict after a fit hid it (the head still WAS that
  // frame). Every later one silently REPLACED the user's data with the
  // estimation-time frame: `regress y x2 t` / `predict r1, residuals` /
  // `generate foo = 1` / `predict p2, xb` left a dataset with neither r1 nor
  // foo, at rc 0 (StataBE, w2_smalls.log section C: predict_twice rc 0, r1 still
  // summarizes over N 12). Rows dropped since the fit came BACK, a `replace` was
  // undone, a sort was reverted -- all the same adoption.
  //
  // So `--data` is the live head, always. What the fit needs rides separately:
  //  * head unmoved (the first predict): the historical argv, byte for byte.
  //  * head moved, no absorbed FE: the estimation-time sample goes in as
  //    `--fit-data`, the split BUG-163 built -- the engine fits THERE and
  //    evaluates the named coefficients over every live row, which is Stata's
  //    rule (current rows, current x values, the fitted coefficients).
  //  * head moved, absorbed FE: `--fit-data` refuses `--fe`, so the live frame
  //    is refit -- after fitColumnsUnchangedM4 has PROVED that is the same fit.
  //
  // BUG-163 (W:P5), unchanged: `--data` is the FULL dataset -- Stata's `predict`
  // writes a value for every row whose covariates are non-missing, not just the
  // rows the model was fitted on -- while the coefficients must come from the
  // FITTED sample (`--fit-data`/`--fit-keepif`).
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::string liveHead = *session.currentHbin();
  // e(sample) T2: a wired fit leaves the head on its mask-install frame
  // (LastModel::esampleInstallPath) -- the fit's own frame plus the hidden
  // column, same rows and values -- so that head has not moved either.
  const bool headMoved = liveHead != lm.dataPath &&
                         (lm.esampleInstallPath.empty() || liveHead != lm.esampleInstallPath);
  // FU-475: lm.fitKeepif rides as `--fit-keepif` (with `--fit-data` once the
  // head moved), which cmds_postest.cpp cmd_predict routes through plan_keepif:
  // the restriction as a row mask after the operators (w2_esample.log:751-775
  // C6 e(N) 40) -- served. The ML refit takes no row mask, the native statistics
  // (computePredictNativeM4) read the refit's sample as a filtered frame, and
  // the engine refuses a restricted fit with absorbed effects (an engine error,
  // not r(198)): those keep the named refusal.
  const bool tsKeepif = postestTsKeepifRefusalM4("predict", lm).has_value();
  // FU-759: xb after an absorbed-effects fit (xtreg,fe / areg / reghdfe) is
  // served on an unmoved head -- the engine's predict takes --fe under the
  // --fit-keepif row mask over that same frame (w2_esample_tsrefit.log:251-263
  // T7 rc 0, r(N) 40, the mean of T1's regress xb). BUG-1259: so is every other
  // statistic of the family -- the engine writes the within residual on the fit's
  // rows (e, areg/reghdfe residuals and score), and computePredictNativeM4 builds
  // the rest from that xb and residual (StataBE 19.5 N2/e12.do `xtreg y L.x1 x2 if
  // t>=3, fe` u/e/xbu/stdp, `areg ...` d/xbd). A moved head refits the live frame
  // once fitColumnsUnchangedM4 has proved it is the fit's (the absorbed-FE rule).
  const bool tsFeXb = tsKeepif && !lm.fe.empty() && !glmPredict && family != PredictFamilyM4::Other &&
                      lm.fitDataPath.empty() &&
                      !(namesHiddenColumn(lm.fitKeepif) && liveHead != lm.dataPath);
  if ((glmPredict || nativeStat || !lm.fe.empty()) && !tsFeXb) {
    if (auto refused = postestTsKeepifRefusalM4("predict", lm)) return *refused;
  }
  std::vector<std::string> args = {"predict", "--data", liveHead, "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  // FU-768: the restriction a restricted absorbed fit is refit under, as a row
  // mask over the live frame. xtreg's route kept its `if` (lm.fitKeepif); areg and
  // reghdfe fit a materialized copy (lm.fitDataPath, runRegress), whose rows are
  // the model's e(sample) -- the mask its install put on the frame, which follows
  // the rows. No mask on the data: refused by name.
  std::string restrictedKeepif;
  if (restrictedAbsorbed) {
    if (!lm.fitKeepif.empty() && lm.fitDataPath.empty()) {
      restrictedKeepif = lm.fitKeepif;
    } else {
      const SessionState::EsampleReading er = session.esampleReading();
      if (er.kind != SessionState::EsampleReading::Kind::kColumn) {
        return engineErrorBlockM4(
            "predict after a restricted `" + predictFamilyCmdM4(family, lm) +
                "` fit is not supported here in this version of PIET: the fit's estimation "
                "sample is not on the data in memory. Nothing was written to your data.",
            "unsupported_option");
      }
      restrictedKeepif = std::string("(") + kEsampleColumn + "==" + std::to_string(er.serial) + ")";
    }
  }
  if (!headMoved) {
    if (restrictedAbsorbed) {
      args.push_back("--fit-keepif");
      args.push_back(restrictedKeepif);
    } else if (!lm.fitDataPath.empty()) {
      args.push_back("--fit-data");
      args.push_back(lm.fitDataPath);
      // BUG-1166: a by-prefixed time-series fit read its group's whole slice
      // and applied its `if` (a mask column on that slice) after the operators;
      // the refit does the same (cmd_predict restricts --fit-data by
      // --fit-keepif). Without it the refit used every row of the group.
      if (!lm.fitKeepif.empty()) {
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      }
    } else if (!lm.fitKeepif.empty()) {
      // e(sample) T2: a --keepif that names the mask reads the cells the fit
      // read, under its predecessor's serial; the head is now this fit's install
      // frame, whose mask the install rewrote (0 rows: `[no_obs]` rc 1). The
      // fit's frame (lm.dataPath, kept by esampleInstallOnHead) goes along.
      if (namesHiddenColumn(lm.fitKeepif) && liveHead != lm.dataPath) {
        args.push_back("--fit-data");
        args.push_back(lm.dataPath);
      }
      args.push_back("--fit-keepif");
      args.push_back(lm.fitKeepif);
    }
  } else {
    std::string fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
    {
      // The refit reads the estimation-time sample AS A FILE. BUG-518: a
      // working frame that a resident mutation advanced before anything wrote
      // it to disk had none (`regress` / `generate` / `predict`: the fit-time
      // head only ever existed in the worker) -- SessionState::setLastModel now
      // pins that frame to disk at the fit, so this branch is the remainder:
      // a snapshot that failed (BUG-384's WARN), or an `estimates restore` of a
      // model whose scratch file reclaimSupersededScratch has since released
      // (exclusion (7) protects only the LIVE model's paths). Named here,
      // because the alternative -- fitting whatever the live frame now holds --
      // is a different model at rc 0.
      std::error_code fec;
      if (!std::filesystem::exists(cecon::to_native_path(fitFile), fec)) {
        return engineErrorBlockM4(
            "predict: the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
                "` was fitted on are no longer available to re-fit from in this version of "
                "PIET (the dataset changed before that working frame was written to disk). "
                "Re-run the estimation command, then predict. Nothing was written to your "
                "data.",
            "unsupported_option");
      }
    }
    const bool restrictedFit = !lm.fitDataPath.empty() || !lm.fitKeepif.empty();
    if (!lm.fe.empty() && (!restrictedFit || restrictedAbsorbed || tsFeXb)) {
      std::string why;
      // BUG-1259: after time-series operators the panel and time variables are
      // read by the lags too.
      std::vector<std::string> tsVars;
      if (tsFeXb) {
        if (session.panelvar.has_value()) tsVars.push_back(*session.panelvar);
        if (session.timevar.has_value()) tsVars.push_back(*session.timevar);
      }
      if (!fitColumnsUnchangedM4(session, lm, why, tsVars)) {
        return engineErrorBlockM4(
            "predict after a model with absorbed fixed effects is not supported once the "
                "model's variables or the observations have changed since the fit, in this "
                "version of PIET (" + why + "). The engine predicts an absorbed-effects "
                "model by re-fitting it, and re-fitting the changed data would be a "
                "different model. Re-run the estimation command, then predict. Nothing was "
                "written to your data.",
            "unsupported_option");
      }
      // Proven identical: fitting the live frame IS the fitted model. FU-768: a
      // restricted one refits under its restriction as a row mask over the live
      // frame (fitColumnsUnchangedM4 compared the columns an `if` reads too; the
      // e(sample) mask follows the rows).
      if (restrictedAbsorbed) {
        args.push_back("--fit-keepif");
        args.push_back(restrictedKeepif);
      } else if (tsFeXb) {
        // The fit's `if` as a row mask after the operators, over the live frame.
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      }
    } else {
      // (A restricted fit WITH absorbed FE that is not served above lands here too
      // and is refused below by name: the engine refuses `--fit-data` with `--fe`.)
      if (tsKeepif) {
        // The fit's frame and its `if` go to the engine together, which applies
        // the `if` after the operators (cmd_predict --fit-data + --fit-keepif).
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      } else if (!lm.fitKeepif.empty()) {
        // `--fit-keepif` filters `--data`, which is no longer the frame the fit
        // saw -- materialize the fitted sample instead (boottest's BUG-163 shape).
        try {
          auto ffr = piet::handlers::applyIfAndInFilter(session, fitFile, lm.fitKeepif,
                                                        std::nullopt);
          if (ffr.ok) fitFile = ffr.dataPath;
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
      args.push_back("--fit-data");
      args.push_back(fitFile);
    }
  }
  if (!lm.fe.empty() && std::find(args.begin(), args.end(), "--fit-data") != args.end()) {
    // FU-972: the engine cannot refit absorbed effects on a separate fit frame
    // (cmd_predict refuses --fit-data with --fe), and that refusal reached the
    // user as rc 1. Named here, r(198), before anything runs.
    return engineErrorBlockM4(
        "predict after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
            "` with absorbed effects fitted on a restricted sample (an `in` range, or an `if` "
            "on data that changed since the fit) is not supported in this version of PIET. "
            "Nothing was written to your data.",
        "unsupported_option");
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
    // e(sample) T2: the refit keeps the singleton groups the fit kept, or their
    // rows get no prediction (fitConventionFlagsM4).
    if (lm.keepSingletons) args.push_back("--keep-singletons");
  }
  // BUG-1067: the fit's collinear omission (fitConventionFlagsM4), so the refit's
  // coefficients are the posted e(b).
  for (const auto& f : fitConventionFlagsM4(lm))
    if (f == "--collinear-forward") args.push_back(f);
  // FU-131: part of the bare replay, so computePredictNativeM4's own refit (and
  // its cross-check against e(b)) is the no-constant model too.
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  if (glmPredict) {
    // BUG-297: forward the estimator; the engine re-fits through
    // postest_fit_spec and writes Stata's default statistic (--pred) or the
    // linear index (--xb). wantResid can never be true here (refused above).
    args.push_back("--model");
    args.push_back(lm.cobraCmd);
    pushFitAsisM4(args, lm);
    if (!mlStat.empty()) {
      // BUG-1257: the replay only; the statistic is computed below.
    } else if (wantMlDefault) {
      args.push_back("--pred");
      args.push_back(newvar);
    } else {
      args.push_back("--xb");
      args.push_back(newvar);
    }
  } else if (nativeStat) {
    // BUG-596: `args` stays the bare replay (through --fe); the statistic is
    // computed below and the engine's refit only supplies its checks.
  } else if (wantXb) {
    args.push_back("--xb");
    args.push_back(newvar);
  } else {
    args.push_back("--resid");
    args.push_back(newvar);
  }

  // BUG-369's shape for predict: an `L.x` regressor needs the tsset context the
  // fit had (bare `predict` after `regress y L.x1 x2` was ts_no_tsset, r(111),
  // on main 2026-10-13).
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);  // an operated depvar too (BUG-1277)
    if (translate::hasTsToken(tsTokens)) args = translate::injectTsContext(args, session);
  }
  std::string outPath = nextPathEst(session.workdir(), "predict");
  if (nativeStat || !mlStat.empty()) {
    std::vector<double> vals;
    if (weightedFit) {
      // BUG-1260 (nativeStat is set): no engine refit after a weighted fit.
      if (auto rej = computePredictWeightedM4(session, lm, family, predictStat, vals)) return *rej;
    } else if (nativeStat) {
      if (auto rej = computePredictNativeM4(session, lm, family, predictStat, args, vals)) return *rej;
    } else if (auto rej = computePredictMlM4(session, lm, mlStat, args, vals)) {
      return *rej;
    }
    if (auto rej = writePredictColumnM4(session, liveHead, newvar, vals, outPath, storeType)) return *rej;
  } else {
    storage::appendStoreTypeArg(args, storeType);
    args.push_back("--out");
    args.push_back(outPath);

    try {
      runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  // BUG-517: `predict newvar if exp` fills ONLY the rows that satisfy exp; the
  // rest are missing (StataBE, aer_b4/predict-e-xbd.log C_if_half: `predict
  // double P_e_half if half==1, e` -> "(135 missing values generated)", rc 0).
  // The qualifier used to be accepted and DROPPED -- runPredict never saw it --
  // so every row was filled at rc 0. Masked here, on the engine's output and
  // BEFORE it is adopted, so a bad expression leaves the user's data untouched
  // (Stata creates no variable on that error either). Same cond() fold `generate
  // ... if` uses (datamgmt/data_generate.cpp runGen), so the two agree on a missing condition.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    const std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + newvar + ",.)";
    const std::string maskedPath = nextPathEst(session.workdir(), "predict_if");
    std::vector<std::string> maskArgs = {"eval", "--data", outPath, "--gen", maskExpr,
                                         "--out", maskedPath};
    storage::appendStoreTypeArg(maskArgs, storeType);  // the column keeps its type
    try {
      runCobraJsonM4(translate::injectTsContextForExpr(maskArgs, maskExpr, session));
    } catch (const CobraError& ce) {
      cecon::native_unlink(outPath);
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    cecon::native_unlink(outPath);  // never a head: nothing else can name it
    outPath = maskedPath;
  }

  session.load(outPath);
  session.invalidateDatastore();
  // BUG-598: the new column's storage type, as `: type`/describe and the .dta
  // writer read it -- the type the values were stored as (BUG-924 above), recorded
  // by generate's rule (storage::recordCreateType): `predict double` -> double
  // (w2b_estim.log predict_double), reghdfe -> double (predict-e-xbd.log type_xbd),
  // `set type double` -> double (w2aa a_p_settype_double), untyped under the
  // factory default -> BUG-295's implicit float plane.
  storage::recordCreateType(session, newvar, declaredType, storeType);
  // reghdfe_p labels its default as its xb (BUG-1508 below).
  if (opts.empty() && postestOlsCmdsM4().count(lm.cobraCmd) && lm.fe.empty() &&
      lm.stataCmd != "reghdfe") {
    session.varMeta[newvar].label = "Fitted values";
    session.syncEvalVarLabels();
  } else {
    // BUG-1260/BUG-1508: the labels the predict programs give (StataBE 19.5
    // scratch-laneA/agents/p-margins/s6.do, file-new-1/i27.do and scratch-laneA2/
    // agents/post/pl.do): _predict's own for xb, stdp, stdf, stdr, hat (leverage),
    // cooksd, rstandard and rstudent; "Residuals" for residuals and score;
    // xtrefe_p.ado's u/e family; areg_p's "Xb" default and the `d[absvar]` family,
    // reghdfe_p's over e(absvars) (its dresiduals unlabelled); regre_p2.ado CalcE for
    // e(a,b); logit/probit/poisson's pr, n and xb ("Linear prediction (log odds)"
    // after logit and logistic).
    std::string label;
    const std::string& s = predictStat;
    const std::string dv = lm.depvar.empty() ? lm.y : lm.depvar;
    if (glmPredict) {
      // In the order the engine argv above reads the two flags.
      if (wantMlDefault)
        label = lm.cobraCmd == "poisson" ? "Predicted number of events" : "Pr(" + dv + ")";
      else if (wantXb)
        label = lm.cobraCmd == "logit" ? "Linear prediction (log odds)" : "Linear prediction";
      // BUG-1257's stdp/score/ir (StataBE 19.5, 2026-11-03, auto.dta after logit,
      // probit, poisson, logistic and cloglog): "S.E. of the prediction",
      // "equation-level score from logit|probit|poisson|logistic|cloglog",
      // "Predicted incidence rate".
      else if (mlStat == "stdp")
        label = "S.E. of the prediction";
      else if (mlStat == "score")
        label = "equation-level score from " +
                (lm.stataCmd == "logistic" ? lm.stataCmd : lm.cobraCmd);  // also cloglog
      else if (mlStat == "ir")
        label = "Predicted incidence rate";
    } else if (family == PredictFamilyM4::Other) {
      if (lm.stataCmd == "reghdfe") label = wantXb ? "Linear prediction" : "Residuals";  // noabsorb
    } else if (s.rfind("e(", 0) == 0) {
      const std::string in = s.substr(2, s.size() - 3);
      const size_t comma = in.find(',');
      const std::string lo = trimM4(in.substr(0, comma)), hi = trimM4(in.substr(comma + 1));
      if (lo == "." && hi == ".") label = "E(" + dv + ")";
      else if (lo == ".") label = "E(" + dv + "|" + dv + "<" + hi + ")";
      else if (hi == ".") label = "E(" + dv + "|" + dv + ">" + lo + ")";
      else label = "E(" + dv + "|" + lo + "<" + dv + "<" + hi + ")";
    } else if (family == PredictFamilyM4::Regress) {
      static const std::map<std::string, std::string> kRegressLabels = {
          {"xb", "Linear prediction"},
          {"residuals", "Residuals"},
          {"score", "Residuals"},
          {"stdp", "S.E. of the prediction"},
          {"stdf", "S.E. of the forecast"},
          {"stdr", "S.E. of the residual"},
          {"hat", "Leverage"},
          {"cooksd", "Cook's D"},
          {"rstandard", "Standardized residuals"},
          {"rstudent", "Studentized residuals"}};
      const auto it = kRegressLabels.find(s);
      if (it != kRegressLabels.end()) label = it->second;
    } else if (family == PredictFamilyM4::Areg && !lm.fe.empty()) {
      const std::string& a = lm.fe.front();
      if (opts.empty()) label = "Xb";
      else if (s == "xbd") label = "Xb + d[" + a + "]";
      else if (s == "d") label = "d[" + a + "]";
      else if (s == "dresiduals") label = "d[" + a + "] + residual";
      else if (s == "residuals" || s == "score") label = "Residuals";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
    } else if (family == PredictFamilyM4::XtregFe) {
      const std::string iv = lm.ivar.has_value() && !lm.ivar->empty()
                                 ? *lm.ivar
                                 : (lm.fe.empty() ? std::string() : lm.fe.front());
      if (s == "e") label = "e[" + iv + ",t]";
      else if (s == "u") label = "u[" + iv + "]";
      else if (s == "xbu") label = "Xb + u[" + iv + "]";
      else if (s == "ue") label = "u[" + iv + "] + e[" + iv + ",t]";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
      else if (s == "residuals") label = "Residuals";
    } else if (family == PredictFamilyM4::Reghdfe && !lm.fe.empty()) {
      std::string a;
      for (const auto& f : lm.fe) a += (a.empty() ? "" : " ") + f;
      if (s == "xbd") label = "Xb + d[" + a + "]";
      else if (s == "d") label = "d[" + a + "]";
      else if (s == "residuals" || s == "score") label = "Residuals";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
    }
    if (!label.empty()) {
      session.varMeta[newvar].label = label;
      session.syncEvalVarLabels();
    }
  }
  Datastore* ds = session.datastore();

  // C1-CARD-REBUILD-REDUCE A11 sibling: `predict` (engine `predict --xb/--resid`)
  // appends only `newvar` and byte-preserves every other column, so seed the
  // unchanged columns' missing-counts (action "gen" pure-ADD, newVars={newvar})
  // BEFORE the varlist rebuild below skips each column's O(nrows) nMissing scan.
  // Byte-identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(ds, "gen", {newvar}, /*droppedVars=*/{}, /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "gen";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  out["new_vars"] = ojson::array({newvar});
  return out;
}

// ===========================================================================
// estat (hettest/ovtest/vif/dwatson/bgodfrey/archlm/durbinalt diagnostic
// rewrite; endogenous/overid/firststage IV diagnostics (BS-082, BETA D5
// batch); simple/event/calendar/group/pretrend csdid aggregate readback)

// `estat event, post` — mirror dispatcher.py's WS-D: post e(b)/e(V) from
// cobra's `estat_event` block (Stata Pre_avg/Post_avg/Tm#/Tp# names, full E×E
// covariance) into session.eval.eMatrices so `matrix b = e(b)` / `matrix V =
// e(V)` read the posted event-study coefficients. Any later estimation
// overwrites them (matched by name).

}  // namespace piet::handlers
