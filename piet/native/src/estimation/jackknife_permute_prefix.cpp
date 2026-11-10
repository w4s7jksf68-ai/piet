// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// resampling prefixes — `bootstrap:`, `jackknife:`, `permute:` (P6 wave).
//
// DESIGN — the loop lives HERE, on the PIET side, and the inner command runs
// through the ORDINARY dispatcher. That is what makes these prefixes general:
// whatever PIET can run, they can wrap, and whatever the expression layer can
// evaluate against r()/e()/_b[] is a legal exp_list element. There is no
// engine-side "resample" verb and no list of blessed inner commands.
//
// The shape, once, for all three methods:
//
//     run the inner command ONCE           -> the observed statistics
//     for each replicate:
//         transform the data               (the ONLY per-method difference)
//         run the inner command again      (normal dispatch, output discarded)
//         evaluate the exp_list            (r()/e()/_b[] as of that run)
//     restore the session's dataset; aggregate; render Stata's table
//
// WHY THE EXP_LIST MUST NOT BE MACRO-EXPANDED AT PARSE TIME. dispatch() expands
// macro and r() references on the raw line BEFORE routing (step 0b). For an
// ordinary command that is right; for `bootstrap mean=r(mean), reps(30):
// summarize y` it is fatal — `r(mean)` would be substituted with whatever the
// PREVIOUS command left (or error as unset) instead of being evaluated after
// each replicate. So the resampling prefixes are intercepted BEFORE that step
// and hand the exp_list here verbatim; only the options and the inner command
// are expanded, because those may legitimately carry locals
// (`reps(`R')`, `: regress `depvar' x`).
//
// RNG. `bootstrap` draws Stata's own replicates (User ruling 2026-10-27: the
// session RNG is Stata-exact MT19937-64 and resampling draws follow the Stata
// stream). runBootstrap below implements bootstrap's behaviour, identified
// against StataBE probes (scratch-r150/lee/bs/o1.do/.log): `seed()` is a
// `set seed` issued after the full-sample run, each replicate is one `bsample`
// call (bsample_draws.h, shared with the `bsample` command) on the session
// stream, so with the same seed PIET reproduces Stata's bootstrap standard
// errors, and without one the draws continue the session stream as Stata's do.
// `permute` still shuffles on Philox4x32-10 (the engine's counter-based rng.h,
// reproducible for a fixed seed() but NOT Stata's permutation stream): it is
// checked on its own counting identities, not on Stata's p-values.
//
// MATERIALIZATION — PIET has no hbin writer, so a computed column reaches the
// data the way every other PIET-side computation does: write a CSV, run `cobra
// convert`, join it back (the datamgmt/string_generate_replace.cpp precedent). Each replicate is a
// handful of engine calls; on the resident `serve` worker those are in-process,
// not process spawns. This is not free, and the wave records the cost rather
// than hiding it — the engine-side fast path is a named P2-PERF candidate, not
// something to smuggle in here.
#include "piet/handlers/resampling.h"
#include "piet/handlers/results.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "bsample_draws.h"       // bsample's Stata draws (shared with the bsample command)
#include "core/ident.h"          // the shared Unicode name rule
#include "core/distributions.h"  // t_critical / t_twosided_pvalue / z_critical (shared with cobra)
#include "core/rng.h"            // Philox4x32-10, the engine's own counter-based RNG (permute)
#include "estimator_command_support.h"  // postBV / installEsampleMask / invsym / setE*OnFit
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "resampling_prefix_internal.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

using namespace resampling_detail;

namespace {

// One replicate's evaluated statistics, parallel to the exp_list.
using RepValues = std::vector<double>;

// P2-PERF ARC-C C2-②: the HEADERLESS single-column payload `cobra setcol`
// reads (`Frame::read_csv_column` — one raw value per row, row count == n_rows).
// It replaces the old two-column (index, value) CSV, whose whole purpose was to
// give a 1:1 merge something to join on. permute does not need a join: the
// permuted values are already in the base frame's own row order, so the column
// can simply be written over the one it replaces.
bool writePermuteValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// Stata prints a probability in (0,1) without the leading zero (".0000").
std::string prob4(double v) {
  std::string s = fixed4(v);
  if (s.size() > 1 && s[0] == '0' && s[1] == '.') s.erase(0, 1);
  return s;
}

// Clopper-Pearson exact binomial interval — what Stata's permute reports as
// [95% CI(p)]. Verified against the oracle's own row: c=30/n=30 gives a lower
// limit of .8843 (= (alpha/2)^(1/n)) and c=0/n=30 an upper limit of .1157,
// which is the TWO-sided form, not the one-sided one. Inverted by bisection on
// the regularized incomplete beta (cecon::betai) rather than a second beta
// implementation of our own.
double betaInv(double target, double a, double b) {
  if (a <= 0) return 0.0;
  if (b <= 0) return 1.0;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (cecon::betai(a, b, mid) < target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}
std::pair<double, double> clopperPearson(int64_t c, int64_t n, double alpha) {
  const double lo = (c <= 0) ? 0.0
                             : betaInv(alpha / 2.0, static_cast<double>(c),
                                       static_cast<double>(n - c + 1));
  const double hi = (c >= n) ? 1.0
                             : betaInv(1.0 - alpha / 2.0, static_cast<double>(c + 1),
                                       static_cast<double>(n - c));
  return {lo, hi};
}

}  // namespace

// ---------------------------------------------------------------------------
// The driver.
//
// `method` is bootstrap | jackknife | permute. `expListRaw` is verbatim (never
// macro-expanded — see the file header). `optsRaw` and `innerCmd` HAVE been
// expanded by the caller, so their locals are already resolved.
ojson runResample(const std::string& method, const std::string& permuteVar,
                  const std::string& expListRaw, const std::string& optsRaw,
                  const std::string& innerCmd, SessionState& session) {
  if (method == "bootstrap") return runBootstrap(expListRaw, optsRaw, innerCmd, session);
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError(method + ": no command found after the colon");

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError(method + ": " + perr);

  // -- options ---------------------------------------------------------------
  int reps = 50;                 // Stata's own default for bootstrap/permute
  uint64_t seed = 0;
  bool haveSeed = false;
  {
    // The option list is a flat `name(value)` / `name` sequence; parse it here
    // rather than through the tokenizer because this line was deliberately kept
    // out of the normal parse path.
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1;
        ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;  // ')'
      }
      if (key.empty()) break;
      if (key == "reps" || key == "rep") {
        try { reps = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": reps() needs a positive integer (got `" + val + "`)");
        }
      } else if (key == "seed") {
        try { seed = static_cast<uint64_t>(std::stoll(val)); haveSeed = true; }
        catch (const std::exception&) {
          return rsError(method + ": seed() needs an integer (got `" + val + "`)");
        }
      } else if (key == "nodots" || key == "dots" || key == "noisily" || key == "nowarn" ||
                 key == "notable" || key == "noheader" || key == "nolegend" || key == "verbose") {
        // Presentation-only switches: accepted and inert. Nothing about the
        // numbers depends on them, so silently honouring them is not a
        // narrowing — dots are progress output PIET does not emit at all.
      } else if (key == "level") {
        // handled below via session.level
        try { session.level = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": level() needs an integer");
        }
      } else {
        return rsError(method + ": option `" + key +
                       "` is not implemented in this build. Implemented: reps(), seed(), level(), "
                       "and the presentation switches (nodots/notable/...).");
      }
    }
  }
  if (reps <= 0) return rsError(method + ": reps() must be positive");

  // The head may be a DIRTY VIRTUAL resident frame whose path names a file that
  // was never written (`keep if` deliberately leaves it that way — see
  // inrange_filter.cpp). This driver both re-opens the base as a file and
  // returns the session to it at the end, so anchor it on disk first.
  // datastore() is the established materialization point.
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(method + ": could not materialize the working dataset: " + e.what());
  }
  const std::string startHbin = *session.currentHbin();

  // -- 1. the observed run ---------------------------------------------------
  // Also the run that tells us what the estimation sample IS: an e-class
  // command leaves a LastModel behind, an r-class one does not.
  {
    ojson obs = dispatch(innerCmd, session);
    for (const auto& b : obs) {
      if (b.is_object() && b.value("type", "") == "error") {
        session.load(startHbin);
        return rsError(method + ": the command `" + innerCmd + "` failed, so there is nothing to " +
                       (method == "permute" ? "permute" : "resample") + ": " +
                       b.value("message", std::string("(no message)")));
      }
    }
  }
  RepValues observed;
  for (const auto& e : expList) {
    std::optional<double> v = evalStat(e.expr, session);
    if (!v.has_value()) {
      session.load(startHbin);
      return rsError(method + ": could not evaluate `" + e.expr + "` after running `" + innerCmd +
                     "` — it does not resolve to a number.");
    }
    observed.push_back(*v);
  }
  // jackknife posts its own e() over the observed run's estimation sample
  // (BUG-1456): its mask and e(title) are read here, while that fit is live.
  std::vector<char> jkSample;
  const std::string innerTitle = session.eval.eStrings.get("title", std::string());
  if (method == "jackknife") {
    const SessionState::EsampleReading er = session.esampleReading();
    Datastore* d = session.datastore();
    if (d != nullptr) {
      const int64_t nHead = d->nRows();
      if (er.kind == SessionState::EsampleReading::Kind::kColumn && d->has(kEsampleColumn)) {
        const double* m = d->col(kEsampleColumn).f64();
        jkSample.assign(static_cast<size_t>(nHead), 0);
        for (int64_t i = 0; i < nHead; ++i)
          jkSample[static_cast<size_t>(i)] = m != nullptr && m[i] == static_cast<double>(er.serial);
      } else if (!session.lastModel().has_value()) {
        // An r-class command: the whole data unless it carries an if/in, whose
        // rows this build does not mark (e(sample) then stays unavailable).
        static const std::regex reQual(R"(\s(if|in)\s)");
        std::smatch qm;
        if (!eval::searchUnquoted(" " + innerCmd + " ", reQual, qm))
          jkSample.assign(static_cast<size_t>(nHead), 1);
      }
    }
  }

  // -- 2. the resampling base ------------------------------------------------
  // Stata draws from the inner command's ESTIMATION SAMPLE when it sets one,
  // and from the whole dataset (with a warning) when it does not. Our carriage
  // for "the sample the fit actually ran on" is LastModel (the P5 BUG-163
  // mechanism): its fitDataPath/fitKeepif carry an if/in restriction, and the
  // model's own y/x lists let us drop the rows a missing value excluded.
  std::string basePath = startHbin;
  bool haveEstSample = false;
  std::vector<std::string> missingFilterVars;
  if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    haveEstSample = true;
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    Datastore probe(basePath);
    auto consider = [&](const std::string& v) {
      // Only plain existing columns: a factor-expanded term like `i.region`
      // is not a column and carries no missingness of its own.
      if (!v.empty() && probe.has(v)) missingFilterVars.push_back(v);
    };
    consider(lm.y);
    for (const auto& x : lm.x) consider(x);
    std::string keepif = lm.fitKeepif;
    for (const auto& v : missingFilterVars) {
      const std::string term = "!missing(" + v + ")";
      keepif = keepif.empty() ? term : ("(" + keepif + ") & " + term);
    }
    if (!keepif.empty()) {
      const std::string filtered = nextPath(session.workdir(), method + "_base");
      CobraRunResult cr =
          runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
      if (!cr.ok) {
        session.load(startHbin);
        CobraError ce = errorFromRaw(cr);
        return rsError(cobraErrorStr(ce), ce.code);
      }
      basePath = filtered;
    }
  }

  // Every transform joins on a row index, so materialize it once.
  const std::string basePathIdx = nextPath(session.workdir(), method + "_baseidx");
  {
    CobraRunResult cr = runCobra(
        {"eval", "--data", basePath, "--egen", std::string(kRowIdx) + " = seq()", "--out", basePathIdx});
    if (!cr.ok) {
      session.load(startHbin);
      CobraError ce = errorFromRaw(cr);
      return rsError(cobraErrorStr(ce), ce.code);
    }
  }
  // The loop below writes several scratch files per replicate, which pushes the
  // reclaim budget over and makes these two — the oldest at that moment — the
  // first candidates for eviction, while they are still the thing being drawn
  // from. Pin them for the length of this command (RAII: released on every
  // exit path, including the throw one, since the loop dispatches arbitrary
  // user commands).
  SessionState::ScratchPin pin(session, {basePath, basePathIdx, startHbin});

  int64_t nBase = 0;
  {
    Datastore d(basePathIdx);
    nBase = d.nRows();
  }
  if (nBase <= 1) {
    session.load(startHbin);
    return rsError(method + ": need more than one observation to resample (have " +
                   std::to_string(nBase) + ")");
  }

  // permute's variable must be numeric and present.
  std::vector<double> permOriginal;
  if (method == "permute") {
    if (permuteVar.empty()) {
      session.load(startHbin);
      return rsError("permute: the permutation variable is required (permute VAR name=exp: cmd)");
    }
    Datastore d(basePathIdx);
    if (!d.has(permuteVar)) {
      session.load(startHbin);
      return rsError("variable " + permuteVar + " not found");
    }
    const cecon::Column& c = d.col(permuteVar);
    if (c.dtype == cecon::DType::Factor) {
      session.load(startHbin);
      return rsError("permute: `" + permuteVar +
                     "` is a string/factor variable; this build permutes numeric variables only.");
    }
    const double* p = c.f64();
    permOriginal.assign(p, p + static_cast<size_t>(nBase));
  }

  // jackknife's replicate count is not a user choice — it is n.
  const int64_t nReps = (method == "jackknife") ? nBase : static_cast<int64_t>(reps);

  // -- 3. the replicate loop -------------------------------------------------
  std::vector<RepValues> reps_values;
  reps_values.reserve(static_cast<size_t>(nReps));
  std::string failure;
  for (int64_t b = 0; b < nReps && failure.empty(); ++b) {
    std::string repPath;
    if (method == "jackknife") {
      // Leave observation b+1 out.
      repPath = nextPath(session.workdir(), "jk_rep");
      CobraRunResult cr = runCobra({"eval", "--data", basePathIdx, "--keepif",
                                    std::string(kRowIdx) + "!=" + std::to_string(b + 1), "--out",
                                    repPath});
      if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }
    } else {  // permute
      std::vector<double> shuffled = permOriginal;
      // Fisher-Yates over the Philox stream.
      for (int64_t j = nBase - 1; j > 0; --j) {
        const uint32_t d = cecon::Philox::draw32(seed, static_cast<uint64_t>(b),
                                                 static_cast<uint64_t>(j));
        const int64_t k = static_cast<int64_t>(d % static_cast<uint32_t>(j + 1));
        std::swap(shuffled[static_cast<size_t>(j)], shuffled[static_cast<size_t>(k)]);
      }
      // P2-PERF ARC-C C2-②: ONE call, not three. The old shape wrote a
      // two-column CSV, converted it to an hbin, dropped the original column
      // and 1:1-joined the new one back on the row index — a whole join
      // apparatus to move n doubles into a column that was already there, in
      // the order it was already in. `cobra setcol` patches exactly one column
      // from a headerless payload and preserves every other byte, which is the
      // operation this always was. It also stops MOVING the column: the join
      // re-appended it at the end of the varlist, where an in-place replace
      // leaves it where the user put it — closer to Stata, not further.
      const std::string csv = session.workdir() + "/pm_val.csv";
      registerScratchMint(csv);  // BUG-389
      if (!writePermuteValuesCsv(csv, shuffled)) {
        failure = "could not write the permuted values";
        break;
      }
      repPath = nextPath(session.workdir(), "pm_rep");
      CobraRunResult c1 = runCobra({"setcol", "--data", basePathIdx, "--var", permuteVar, "--values",
                                    csv, "--dtype", "f64", "--out", repPath});
      if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
    }

    session.load(repPath);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") {
        innerFailed = true;
        break;
      }
    }
    if (innerFailed) {
      // Stata drops a replicate whose command fails and reports the loss rather
      // than averaging over a hole it never mentions. Same here: a failed
      // replicate contributes nothing and is counted.
      continue;
    }
    RepValues vals;
    bool ok = true;
    for (const auto& e : expList) {
      std::optional<double> v = evalStat(e.expr, session);
      if (!v.has_value()) { ok = false; break; }
      vals.push_back(*v);
    }
    if (ok) reps_values.push_back(std::move(vals));
  }

  // The session's own dataset is never left on a replicate.
  session.load(startHbin);
  if (!failure.empty()) return rsError(method + ": " + failure);

  const int64_t nCompleted = static_cast<int64_t>(reps_values.size());
  if (nCompleted < 2) {
    return rsError(method + ": only " + std::to_string(nCompleted) +
                   " replicate(s) completed successfully — too few to report a standard error.");
  }
  // jackknife keeps its replicates as float (postfile's default storage; the
  // `double` option is not implemented here) and computes e(V) from those
  // (StataBE 19.5, BUG-1456: _se[b1] .15455305643343692, where the double
  // replicates give .15455301963687726).
  if (method == "jackknife")
    for (auto& rv : reps_values)
      for (double& v : rv) v = static_cast<double>(static_cast<float>(v));

  // -- 4. aggregate ----------------------------------------------------------
  const double alpha = 1.0 - static_cast<double>(session.level) / 100.0;
  ojson rows = ojson::array();
  for (size_t k = 0; k < expList.size(); ++k) {
    double sum = 0.0;
    for (const auto& rv : reps_values) sum += rv[k];
    const double mean = sum / static_cast<double>(nCompleted);
    double ss = 0.0;
    for (const auto& rv : reps_values) {
      const double d = rv[k] - mean;
      ss += d * d;
    }

    ojson row;
    row["name"] = expList[k].name;
    row["b"] = observed[k];

    if (method == "permute") {
      // Counting, not a standard error: how often a permuted statistic reaches
      // the observed one. Stata reports both one-sided counts and the two-sided
      // p; SE(p) is the binomial standard error of the Monte-Carlo proportion.
      int64_t cLower = 0, cUpper = 0;
      for (const auto& rv : reps_values) {
        if (rv[k] <= observed[k]) ++cLower;
        if (rv[k] >= observed[k]) ++cUpper;
      }
      const double n = static_cast<double>(nCompleted);
      const double pLower = static_cast<double>(cLower) / n;
      const double pUpper = static_cast<double>(cUpper) / n;
      const double pTwo = std::min(1.0, 2.0 * std::min(pLower, pUpper));
      row["c_lower"] = cLower;
      row["c_upper"] = cUpper;
      row["n"] = nCompleted;
      row["p_lower"] = pLower;
      row["p_upper"] = pUpper;
      row["p_twosided"] = pTwo;
      row["se_lower"] = std::sqrt(pLower * (1.0 - pLower) / n);
      row["se_upper"] = std::sqrt(pUpper * (1.0 - pUpper) / n);
    } else if (method == "jackknife") {
      // Deterministic, and the one place the oracle judges our arithmetic digit
      // for digit. Stata's jackknife variance is the leave-one-out scaling
      // (n-1)/n * sum((theta_i - theta_bar)^2), with t(n-1) inference — fitted
      // against the P6 oracle row (SE .1256869 on `jackknife bx1=_b[x1]:
      // regress y x1 x2`), not assumed from the first plausible formula.
      const double n = static_cast<double>(nCompleted);
      const double se = std::sqrt((n - 1.0) / n * ss);
      const double dof = n - 1.0;
      const double t = se > 0 ? observed[k] / se : std::nan("");
      row["se"] = se;
      row["t"] = t;
      row["p"] = cecon::t_twosided_pvalue(t, dof);
      const double crit = cecon::t_critical(alpha, dof);
      row["ci_lo"] = observed[k] - crit * se;
      row["ci_hi"] = observed[k] + crit * se;
      row["df"] = dof;
    }
    rows.push_back(row);
  }

  // -- 4b. e() ----------------------------------------------------------------
  // Neither prefix leaves the last replicate's fit live (BUG-1456; StataBE 19.5):
  // permute empties e(); jackknife posts its own -- e(b) the observed statistics
  // under the exp_list names, e(V) the jackknife covariance (n-1)/n * sum of the
  // replicates' cross-deviations, e(b_jk) the mean of the pseudovalues
  // n*b - (n-1)*mean(replicates) (2.0658634 against b 2.0621382), e(sample) the
  // observed run's sample, and the scalars and macros `ereturn list` shows there.
  clearEResults(session);
  if (method == "jackknife") {
    const size_t K = expList.size();
    const double n = static_cast<double>(nCompleted);
    std::vector<std::string> names;
    std::vector<double> means(K, 0.0);
    for (size_t k = 0; k < K; ++k) {
      names.push_back(expList[k].name);
      for (const auto& rv : reps_values) means[k] += rv[k];
      means[k] /= n;
    }
    std::vector<std::vector<double>> V(K, std::vector<double>(K, 0.0));
    for (size_t a = 0; a < K; ++a)
      for (size_t c = 0; c < K; ++c) {
        double s = 0.0;
        for (const auto& rv : reps_values) s += (rv[a] - means[a]) * (rv[c] - means[c]);
        V[a][c] = (n - 1.0) / n * s;
      }
    ojson block = ojson::object();
    block["type"] = "estimation";
    block["estimator"] = "jackknife";
    block["n_obs"] = nBase;
    block["coeftable"] = rows;
    LastModel lm;
    lm.cobraCmd = "jackknife";
    lm.stataCmd = "jackknife";
    lm.vcov = "jackknife";
    lm.eVce = "jackknife";
    lm.result = block;
    if (!jkSample.empty() && !e3::installEsampleMask(session, lm, jkSample))
      return rsError("jackknife: the results were computed, but e(sample) could not be installed");
    session.setLastModel(lm);
    session.lastEstBlock = block;
    e3::postBV(session, names, observed, V);
    {
      eval::EvalMatrix m;
      std::vector<double> bjk(K);
      for (size_t k = 0; k < K; ++k) bjk[k] = n * observed[k] - (n - 1.0) * means[k];
      m.data.push_back(bjk);
      m.rownames = {"y1"};
      m.colnames = names;
      setEMatrix(session, "b_jk", std::move(m));
    }
    const std::vector<std::vector<double>> Vinv = e3::invsym(V);
    int rank = 0;
    for (size_t k = 0; k < K; ++k) rank += Vinv[k][k] != 0.0 ? 1 : 0;
    e3::setEScalarOnFit(session, "df_r", n - 1.0);
    e3::setEScalarOnFit(session, "N_reps", static_cast<double>(nReps));
    e3::setEScalarOnFit(session, "N_misreps", static_cast<double>(nReps - nCompleted));
    e3::setEScalarOnFit(session, "rank", rank);
    e3::setEScalarOnFit(session, "k_eq", 1);
    e3::setEScalarOnFit(session, "k_exp", static_cast<double>(K));
    e3::setEScalarOnFit(session, "k_eexp", 0);
    e3::setEScalarOnFit(session, "k_extra", 0);
    e3::setEScalarOnFit(session, "singleton", std::nan(""));
    e3::setEScalarOnFit(session, "N", static_cast<double>(nBase));
    std::string cmdname;
    {
      std::istringstream words(innerCmd);
      words >> cmdname;
      const size_t cut = cmdname.find(',');
      if (cut != std::string::npos) cmdname = cmdname.substr(0, cut);
    }
    std::string title = bootstrapTitle(innerTitle, cmdname);
    if (title == "Bootstrap results") title = "Jackknife results";
    e3::setEStringOnFit(session, "cmdline", "jackknife " + strutil::trim(expListRaw) +
                                                (optsRaw.empty() ? "" : ", " + optsRaw) + ": " +
                                                innerCmd);
    e3::setEStringOnFit(session, "cmd", "jackknife");
    e3::setEStringOnFit(session, "prefix", "jackknife");
    e3::setEStringOnFit(session, "cmdname", cmdname);
    if (haveEstSample) e3::setEStringOnFit(session, "nfunction", "e(N)");
    e3::setEStringOnFit(session, "command", innerCmd);
    for (size_t k = K; k-- > 0;)
      e3::setEStringOnFit(session, "exp" + std::to_string(k + 1), expList[k].expr);
    e3::setEStringOnFit(session, "title", title);
    e3::setEStringOnFit(session, "vcetype", "Jackknife");
    e3::setEStringOnFit(session, "vce", "jackknife");
    e3::setEStringOnFit(session, "properties", "b V");
  }

  // -- 5. render, in Stata's own layout -------------------------------------
  // Emitted as `display` text blocks: the numbers reach the console, the log
  // tee and the webview through a path that already exists, so no ResultBlock
  // variant is added and contract B does not move. The layout is copied from
  // the oracle log, which is the spec for it.
  ojson lines = ojson::array();
  auto say = [&lines](const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  };
  {
    const std::string obsStr = commaGroup(nBase);
    if (method == "permute") {
      say(padRight("Monte Carlo permutation results", 51) +
          "Number of observations = " + padLeft(obsStr, 3));
      say(padRight("Permutation variable: " + permuteVar, 51) +
          "Number of permutations = " + padLeft(std::to_string(nCompleted), 3));
    } else {
      const std::string title = "Linear regression";  // jackknife
      say(padRight(title, 59) + "Number of obs = " + padLeft(obsStr, 3));
      say(padRight("", 59) + "Replications  = " + padLeft(std::to_string(nCompleted), 3));
    }
    say("");
    say("      Command: " + innerCmd);
    for (const auto& e : expList) {
      say(padLeft(e.name, 13) + ": " + e.expr);
    }
    say("");
  }
  if (method == "permute") {
    const std::string rule(79, '-');
    say(rule);
    say("             |                                               Monte Carlo error");
    say("             |                                              -------------------");
    say("           T |    T(obs)       Test       c       n      p  SE(p)   [95% CI(p)]");
    say("-------------+" + std::string(65, '-'));
    for (const auto& row : rows) {
      const int64_t cl = row["c_lower"].get<int64_t>();
      const int64_t cu = row["c_upper"].get<int64_t>();
      const int64_t n = row["n"].get<int64_t>();
      const double pl = row["p_lower"].get<double>();
      const double pu = row["p_upper"].get<double>();
      const double p2 = row["p_twosided"].get<double>();
      const auto ciL = clopperPearson(cl, n, alpha);
      const auto ciU = clopperPearson(cu, n, alpha);
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 10) + padLeft("lower", 11) +
          padLeft(std::to_string(cl), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pl), 7) + padLeft(prob4(row["se_lower"].get<double>()), 7) +
          padLeft(prob4(ciL.first), 7) + padLeft(prob4(ciL.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("upper", 11) +
          padLeft(std::to_string(cu), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pu), 7) + padLeft(prob4(row["se_upper"].get<double>()), 7) +
          padLeft(prob4(ciU.first), 7) + padLeft(prob4(ciU.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("two-sided", 15) +
          std::string(12, ' ') + padLeft(prob4(p2), 7) + padLeft(prob4(0.0), 7) +
          padLeft(".", 7) + padLeft(".", 7));
    }
    say(rule);
    say("Notes: For lower one-sided test, c = #{T <= T(obs)} and p = p_lower = c/n.");
    say("       For upper one-sided test, c = #{T >= T(obs)} and p = p_upper = c/n.");
    say("       For two-sided test, p = 2*min(p_lower, p_upper); SE and CI approximate.");
  } else {
    const std::string rule(78, '-');
    const std::string stat = "t";  // jackknife (bootstrap renders in runBootstrap)
    say(rule);
    say("             |              Jackknife");
    say("             | Coefficient  std. err.      t    P>|t|     [95% conf. interval]");
    say("-------------+" + std::string(64, '-'));
    for (const auto& row : rows) {
      const double statv = row[stat].get<double>();
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 11) + padLeft(g9cell(row["se"].get<double>()), 11) +
          padLeft(fixed2(statv), 9) + padLeft(fixed3(row["p"].get<double>()), 8) +
          padLeft(g9cell(row["ci_lo"].get<double>()), 13) + padLeft(g9cell(row["ci_hi"].get<double>()), 12));
    }
    say(rule);
  }

  ojson out;
  out["type"] = "resample";
  out["method"] = method;
  out["command"] = innerCmd;
  out["n_obs"] = nBase;
  out["n_reps"] = nCompleted;
  out["n_reps_requested"] = nReps;
  out["level"] = session.level;
  out["rows"] = rows;
  out["est_sample"] = haveEstSample;
  if (haveSeed) out["seed"] = static_cast<long long>(seed);
  if (method == "permute") out["permute_var"] = permuteVar;
  // Stata warns when the inner command sets no estimation sample, because the
  // resampling then cannot exclude the rows the command itself would have
  // dropped. Carry the same warning rather than resampling silently.
  if (!haveEstSample) {
    out["warning"] = std::string(innerCmd.substr(0, innerCmd.find(' '))) +
                     " does not set e(sample), so no observations will be excluded from the "
                     "resampling because of missing values or other reasons.";
  }
  // The structured block rides LAST so a consumer that wants the numbers has
  // them, while the human-facing text is what the console and log show. Its
  // type is new but it is never the sole carrier of a result, and the
  // ResultBlock union the frontend switches on is untouched.
  lines.push_back(out);
  return lines;
}



}  // namespace handlers
}  // namespace piet
