// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `bootstrap [exp_list] [, options] : command` -- Stata bsample replicates, e() posting,
// normal / percentile / bias-corrected CIs, the Wald model test and the table.
// Shared design note of the family: the head of jackknife_permute_prefix.cpp.
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

// A one-column CSV of int64 values, for `cobra convert` to turn into an hbin.
bool writeIndexCsv(const std::string& path, const std::string& colName,
                   const std::vector<int64_t>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f << colName << "\n";
  for (int64_t v : vals) f << v << "\n";
  return static_cast<bool>(f);
}

// Two-sided normal p-value, P(|Z| > |z|). erfc is the direct spelling; the
// engine has no normal p-value helper to share, so there is nothing to diverge
// from here.
double normalTwoSidedP(double z) { return std::erfc(std::fabs(z) * 0.7071067811865476); }

// bootstrap's exp_list: `name=exp` elements (exp one token or parenthesized) and
// UNNAMED ones, a parenthesized `(exp)` or a bare token such as
// `_b[treat:lower]`; an unnamed element at position i is named `_bs_i` and its
// e(exp#) loses the outer parentheses (StataBE scratch-r150/lee/bs/o5.log U1:
// `_b[treat:lower] (_b[treat:upper]*2)` -> _bs_1 _bs_2, e(exp2) `_b[treat:upper]*2`).
std::vector<ExpItem> parseBootstrapExpList(const std::string& s, std::string* err) {
  std::vector<ExpItem> out;
  size_t i = 0;
  auto ws = [&](size_t k) { return k < s.size() && std::isspace(static_cast<unsigned char>(s[k])); };
  // End of a bracket-balanced run starting at k that stops at depth-0 space.
  auto tokenEnd = [&](size_t k) {
    int depth = 0;
    while (k < s.size()) {
      const char c = s[k];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (depth <= 0 && std::isspace(static_cast<unsigned char>(c))) break;
      ++k;
    }
    return k;
  };
  while (true) {
    while (ws(i)) ++i;
    if (i >= s.size()) break;
    size_t m = i;
    while (m < s.size() && cecon::ident::isIdentByte(s[m])) ++m;
    size_t n = m;
    while (ws(n)) ++n;
    const bool named = m > i && n < s.size() && s[n] == '=' && (n + 1 >= s.size() || s[n + 1] != '=');
    if (named) {
      // name=exp: the expression is ONE token -- a spaced expression must be
      // parenthesized, as in Stata (`d = _b[x] - (_b[_cons])` is "expressions
      // must be bound in parentheses", o6.log I2; the bare `-` token below).
      size_t j = n + 1;
      while (ws(j)) ++j;
      const size_t e = tokenEnd(j);
      std::string expr = s.substr(j, e - j);
      if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
      if (expr.empty()) {
        *err = "empty expression for `" + s.substr(i, m - i) + "`";
        return {};
      }
      out.push_back(ExpItem{s.substr(i, m - i), expr});
      i = e;
      continue;
    }
    const size_t e = tokenEnd(i);
    std::string expr = s.substr(i, e - i);
    if (std::strchr("+-*/^&|!<>=,", expr[0]) != nullptr) {
      *err = "expressions must be bound in parentheses";
      return {};
    }
    if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
    if (expr.empty() || expr == "_b" || expr == "_se") {
      *err = "`" + s.substr(i, e - i) +
             "` mixed with other elements is not implemented in this version of PIET (the "
             "default _b alone is)";
      return {};
    }
    out.push_back(ExpItem{"_bs_" + std::to_string(out.size() + 1), expr});
    i = e;
  }
  if (out.empty()) *err = "expression list required";
  return out;
}

// `cmd ... [if exp] [in range] [weight] [, options]` with the if and in removed
// (outside quotes and brackets; the if-expression runs to ` in `, `[` or the
// option comma). Spacing of what remains is normalized to single blanks.
std::string stripIfIn(const std::string& cmd) {
  std::string head = cmd, tail;
  const long long comma = eval::findUnquotedComma(cmd);
  if (comma >= 0) {
    head = cmd.substr(0, static_cast<size_t>(comma));
    tail = cmd.substr(static_cast<size_t>(comma) + 1);
  }
  const std::vector<bool> quoted = eval::quotedSpanMask(head);
  // Depth-0, unquoted word starts.
  std::vector<std::pair<size_t, size_t>> words;  // [begin, end)
  {
    int depth = 0;
    size_t i = 0;
    while (i < head.size()) {
      while (i < head.size() && std::isspace(static_cast<unsigned char>(head[i])) && depth == 0) ++i;
      if (i >= head.size()) break;
      const size_t b = i;
      while (i < head.size()) {
        const char c = head[i];
        const bool q = i < quoted.size() && quoted[i];
        if (!q && (c == '(' || c == '[')) ++depth;
        else if (!q && (c == ')' || c == ']') && depth > 0) --depth;
        else if (!q && depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
        ++i;
      }
      words.emplace_back(b, i);
    }
  }
  std::string out;
  for (size_t w = 0; w < words.size(); ++w) {
    const std::string word = head.substr(words[w].first, words[w].second - words[w].first);
    if (w > 0 && word == "if") {
      // skip the expression: up to the next `in` word or a `[weight]` word
      size_t k = w + 1;
      while (k < words.size()) {
        const std::string nw = head.substr(words[k].first, words[k].second - words[k].first);
        if (nw == "in" || nw[0] == '[') break;
        ++k;
      }
      w = k - 1;
      continue;
    }
    if (w > 0 && word == "in" && w + 1 < words.size()) {
      ++w;  // the range
      continue;
    }
    if (!out.empty()) out += ' ';
    out += word;
  }
  while (!tail.empty() && std::isspace(static_cast<unsigned char>(tail.front()))) tail.erase(0, 1);
  return tail.empty() ? out : out + ", " + tail;
}

// An error block carrying Stata's rc (coarseRc reads the `r(NNN)` reason).
ojson bsRc(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

const ojson* firstErrorBlock(const ojson& blocks) {
  if (blocks.is_object()) return blocks.value("type", std::string()) == "error" ? &blocks : nullptr;
  if (!blocks.is_array()) return nullptr;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "error") return &b;
  return nullptr;
}

// Stata's `_pctile x, p(#)` (the default definition) on ascending `x`:
// P = n*p/100, i = floor(P); x_(i+1) when P has a fractional part, else the
// mean of x_(i) and x_(i+1) (clamped at the ends).
double pctileStata(const std::vector<double>& x, double p) {
  const size_t n = x.size();
  if (n == 0) return std::nan("");
  const double P = static_cast<double>(n) * p / 100.0;
  const double fi = std::floor(P);
  const long long i = static_cast<long long>(fi);
  auto at = [&](long long k) {  // 1-based, clamped
    if (k < 1) k = 1;
    if (k > static_cast<long long>(n)) k = static_cast<long long>(n);
    return x[static_cast<size_t>(k - 1)];
  };
  if (P - fi > 0) return at(i + 1);
  return (at(i) + at(i + 1)) / 2.0;
}

// chi2tail(df, x) for the bootstrap Wald model test. PIET's expression layer
// has no chi2tail(), so: df == 1 is 2*normal(-sqrt(x)) on Stata's normal()
// (cecon::stata_normal); otherwise the regularized upper incomplete gamma
// Q(df/2, x/2) by series / continued fraction -- the form qreg2_estimation.cpp
// chi2tailQ uses (file-local there, so kept in step here).
double chi2tailBs(double df, double x) {
  if (!(x > 0)) return 1.0;
  if (df == 1.0) return 2.0 * cecon::stata_normal(-std::sqrt(x));
  const double a = df / 2.0, z = x / 2.0;
  const double gln = std::lgamma(a);
  if (z < a + 1.0) {
    double ap = a, sum = 1.0 / a, del = sum;
    for (int n = 0; n < 1000; ++n) {
      ap += 1.0;
      del *= z / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
    }
    return 1.0 - sum * std::exp(-z + a * std::log(z) - gln);
  }
  double b = z + 1.0 - a, c = 1.0 / 1e-300, d = 1.0 / b, h = d;
  for (int i = 1; i < 1000; ++i) {
    const double an = -i * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-16) break;
  }
  return std::exp(-z + a * std::log(z) - gln) * h;
}

// Stata's option-abbreviation rule for one option: `key` names `full` when it
// is a prefix of it at least `minLen` long.
bool optIs(const std::string& key, const char* full, size_t minLen) {
  const std::string f(full);
  return key.size() >= minLen && key.size() <= f.size() && f.compare(0, key.size(), key) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// `bootstrap [exp_list] [, options] : command`
//
// Implements bootstrap's behaviour (with bsample), identified against StataBE probes (scratch-r150/lee/bs/o1.do,
// o1.log; T-numbers below are its sections):
//   * e() and r() are cleared, then the command runs once on the data (the
//     observed statistics). An e-class command's e(sample) is the resampling
//     base: rows outside it are dropped before the replicates (T6: 54 of 60
//     rows after a missing regressor; T10: `if x > 2`). A command that leaves
//     no e(sample) resamples the whole data.
//   * No exp_list (or `_b`): every column of the command's e(b), named as e(b)
//     names them, equation stripes included (T1, T4, T11 `y:x ... c:_cons`).
//     A command without e(b) is r(100) "expression list required".
//   * seed(#) is `set seed #` after the full-sample run (T9: `set seed 123` +
//     bootstrap equals seed(123); without seed() the draws continue the
//     session stream). Nothing else draws between replicates.
//   * cluster(var): the base is sorted by the cluster (stable; a string cluster
//     sorts by its bytes, T7) and each replicate is `bsample, cluster()`
//     (bsdraw::clustSrswr); a missing cluster value in the base is r(459)
//     (T8), one cluster r(460). Without cluster() each replicate is the
//     no-option `bsample` (bsdraw::srswrFast).
//   * Replicates are posted with `postfile` in FLOAT (bootstrap's default
//     storage), so e(V) is the (n-1)-divisor covariance of the float-rounded
//     replicate values over the replicates with no missing statistic; a
//     replicate whose command fails or whose e(b) changes shape posts missing.
//   * e(): e(b) the observed values, e(V) the replicate covariance, the
//     inner e() copied (default _b only, less F/chi2/df_r/df_m), e(cmd) the
//     inner e(cmd) for the default _b and "bootstrap" otherwise, plus
//     bootstrap's own: N, N_reps, N_misreps, N_clust, level, k_eq, k_exp,
//     k_eexp, k_extra, rank, bs_version, prefix, cmdname, command, exp#,
//     vce, vcetype, clustvar/cluster, title, rngstate, the Wald model test
//     (chi2/df_m/p/chi2type, default _b only) and the matrices b_bs, reps,
//     bias, z0, se, ci_normal, ci_percentile, ci_bc, V_modelbased.
//   * r(table): normal-based (z, df missing, crit invnormal(.975)).
// Refused by name (not implemented): strata(), size(), idcluster(), saving(),
// bca, mse, nodrop, reject(), and an r-class command with if/in.
ojson runBootstrap(const std::string& expListRaw, const std::string& optsRaw,
                          const std::string& innerCmd, SessionState& session) {
  const std::string method = "bootstrap";
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError("bootstrap: no command found after the colon");

  // -- the exp_list ----------------------------------------------------------
  std::string expTrim = expListRaw;
  while (!expTrim.empty() && std::isspace(static_cast<unsigned char>(expTrim.back()))) expTrim.pop_back();
  while (!expTrim.empty() && std::isspace(static_cast<unsigned char>(expTrim.front()))) expTrim.erase(0, 1);
  const bool bMode = expTrim.empty() || expTrim == "_b" || expTrim == "(_b)";
  std::vector<ExpItem> expList;
  if (!bMode) {
    std::string perr;
    expList = parseBootstrapExpList(expTrim, &perr);
    if (expList.empty()) {
      if (perr == "expressions must be bound in parentheses") return bsRc(perr, 198, "syntax_error");
      return rsError("bootstrap: " + perr);
    }
  }

  // -- options ---------------------------------------------------------------
  int reps = 50;
  std::string seedText;
  std::string clusterVar;
  double level = static_cast<double>(session.level);
  {
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() &&
             (std::isalnum(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == '_')) ++i;
      const std::string key = strutil::lower(optsRaw.substr(ns, i - ns));
      std::string val;
      bool hasVal = false;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        hasVal = true;
        int depth = 1;
        ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;
      }
      if (key.empty()) break;
      auto trimmed = [](std::string v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        return v;
      };
      if (optIs(key, "reps", 1)) {
        try {
          size_t pos = 0;
          const std::string v = trimmed(val);
          reps = std::stoi(v, &pos);
          if (pos != v.size()) throw std::invalid_argument(v);
        } catch (const std::exception&) {
          return bsRc("option reps() incorrectly specified", 198, "syntax_error");
        }
      } else if (key == "seed" || key == "rseed") {
        seedText = trimmed(val);
        if (seedText.empty()) return bsRc("option " + key + "() incorrectly specified", 198, "syntax_error");
      } else if (optIs(key, "cluster", 2)) {
        std::istringstream is(val);
        std::vector<std::string> names;
        for (std::string w; is >> w;) names.push_back(w);
        if (names.size() != 1)
          return rsError("bootstrap: cluster() with " + std::to_string(names.size()) +
                             " variables is not implemented in this version of PIET (one cluster "
                             "variable is).",
                         "unsupported_option");
        clusterVar = names[0];
      } else if (optIs(key, "level", 1)) {
        try {
          level = std::stod(trimmed(val));
        } catch (const std::exception&) {
          return bsRc("option level() incorrectly specified", 198, "syntax_error");
        }
        if (!(level >= 10.0 && level <= 99.99))
          return bsRc("level() must be between 10 and 99.99 inclusive", 198, "syntax_error");
      } else if (!hasVal && (key == "nodots" || key == "noisily" || key == "nowarn" ||
                             key == "notable" || key == "noheader" || key == "nolegend" ||
                             key == "verbose" || optIs(key, "noisily", 3) || optIs(key, "verbose", 1))) {
        // Presentation-only switches: nothing about the numbers depends on them.
      } else if (key == "dots") {
        // dots(#): progress output, which PIET does not print.
      } else {
        return rsError("bootstrap: option `" + key +
                           "` is not implemented in this version of PIET. Implemented: reps(), "
                           "seed(), cluster(), level(), and the presentation switches "
                           "(nodots/notable/...).",
                       "unsupported_option");
      }
    }
  }
  if (reps < 2) return bsRc("option reps() requires an integer greater than 1", 198, "syntax_error");

  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bootstrap: could not materialize the working dataset: ") + e.what());
  }
  const std::string startHbin = *session.currentHbin();
  std::istringstream cmdWords(innerCmd);
  std::string cmdname;
  cmdWords >> cmdname;
  {
    const size_t cut = cmdname.find(',');
    if (cut != std::string::npos) cmdname = cmdname.substr(0, cut);
  }
  // The command each replicate runs (and e(command)): the inner command without
  // its if/in -- the resampling base is already restricted to its sample, and an
  // `in` range or `_n` condition must not be re-applied to the resampled rows --
  // options and weights kept (bootstrap.ado's `command`; o1.log T10 e(command)
  // `regress y x` for `regress y x if x > 2`).
  const std::string repCmd = stripIfIn(innerCmd);

  // -- 1. the full-sample run ------------------------------------------------
  // `_prefix_clear, e r` first: an r-class command then leaves no e() behind,
  // which is how "no e(sample)" is told apart from the previous model's.
  (void)dispatch("ereturn clear", session);
  eval::replaceRClass(session.eval);
  {
    const ojson obs = dispatch(innerCmd, session);
    if (const ojson* err = firstErrorBlock(obs)) {
      session.load(startHbin);
      return *err;  // _prefix_run_error: the command's own error and rc
    }
  }
  std::vector<std::string> names;  // e(b)'s column names (eq:name when striped)
  std::vector<std::string> exprs;  // e(exp#)
  std::vector<double> observed;
  std::optional<eval::EvalMatrix> obsV;
  if (bMode) {
    int ebRc = 0;
    std::string ebWhy;
    const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b", &ebRc, &ebWhy);
    if (!eb.has_value() || eb->data.size() != 1 || eb->data[0].empty() ||
        eb->data[0].size() != eb->colnames.size()) {
      // A command that posts no e(b) is Stata's r(100); one whose e(b) this
      // build refuses to build keeps that named refusal.
      if (!eb.has_value() && ebRc == 198 && !ebWhy.empty())
        return bsRc("bootstrap: the default exp_list is _b, and " + ebWhy, 198, "unsupported_option");
      return bsRc("expression list required", 100);
    }
    names = eb->colnames;
    observed = eb->data[0];
    // e(exp#): `_b[x]`, or `[eq]_b[x]` on a striped e(b) (o2.log A3).
    for (const auto& n : names) {
      const size_t colon = n.find(':');
      exprs.push_back(colon == std::string::npos
                          ? "_b[" + n + "]"
                          : "[" + n.substr(0, colon) + "]_b[" + n.substr(colon + 1) + "]");
    }
    obsV = session.eval.resolveEMatrix("V");
  } else {
    for (const auto& e : expList) {
      std::optional<double> v = evalStat(e.expr, session);
      if (!v.has_value()) {
        return rsError("bootstrap: could not evaluate `" + e.expr + "` after running `" + innerCmd +
                       "` — it does not resolve to a number.");
      }
      names.push_back(e.name);
      exprs.push_back(e.expr);
      observed.push_back(*v);
    }
  }
  for (size_t k = 0; k < observed.size(); ++k) {
    if (std::isnan(observed[k])) {
      return bsRc("'" + exprs[k] + "' evaluated to missing in full sample", 322);
    }
  }
  const size_t K = names.size();

  // What the copied e() carries (default _b only): the inner fit's block, its
  // posted scalars/macros, its e(cmd)/e(depvar)/e(title).
  std::optional<ojson> innerBlock;
  std::vector<std::pair<std::string, double>> innerScalars;
  std::vector<std::pair<std::string, std::string>> innerStrings;
  const std::string innerECmd = session.eval.eStrings.get("cmd", std::string());
  const std::string innerDepvar = session.eval.eStrings.get("depvar", std::string());
  const std::string innerTitle = session.eval.eStrings.get("title", std::string());
  double innerDfM = std::nan("");
  if (bMode) {
    if (session.lastEstBlock.has_value()) innerBlock = *session.lastEstBlock;
    for (const auto& [k, v] : session.eval.eScalars.items()) innerScalars.emplace_back(k, v);
    for (const auto& [k, v] : session.eval.eStrings.items()) innerStrings.emplace_back(k, v);
    if (const std::optional<double> dfm = evalStat("e(df_m)", session)) innerDfM = *dfm;
  }

  // -- 2. the resampling base ------------------------------------------------
  // The working frame now (the full-sample run may have installed its
  // e(sample) column on it) is the frame the session returns to.
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bootstrap: could not materialize the working dataset: ") + e.what());
  }
  const std::string obsHbin = *session.currentHbin();
  int64_t nHead = 0;
  std::vector<char> inBase;  // per head row: in the resampling base (and e(sample) after)
  bool haveEsample = false;
  {
    Datastore d(obsHbin);
    nHead = d.nRows();
    inBase.assign(static_cast<size_t>(nHead), 1);
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kRefused) {
      return rsError("bootstrap: the estimation sample of `" + innerCmd +
                         "` cannot be determined in this version of PIET, so the rows to resample "
                         "are unknown: " + er.refusal,
                     "unsupported_option");
    }
    if (er.kind == SessionState::EsampleReading::Kind::kColumn && d.has(kEsampleColumn)) {
      const double* m = d.col(kEsampleColumn).f64();
      int64_t ones = 0;
      for (int64_t i = 0; i < nHead; ++i) {
        const bool in = m != nullptr && m[i] == static_cast<double>(er.serial);
        inBase[static_cast<size_t>(i)] = in ? 1 : 0;
        ones += in ? 1 : 0;
      }
      // _prefix_check4esample: all-0 e(sample) is "no e(sample)" (whole data).
      if (ones == 0) std::fill(inBase.begin(), inBase.end(), 1);
      else haveEsample = true;
    }
    if (!haveEsample) {
      // An r-class command: Stata marks its if/in and resamples those rows;
      // this build does not evaluate the inner qualifier itself.
      static const std::regex reQual(R"(\s(if|in)\s)");
      std::smatch qm;
      if (eval::searchUnquoted(" " + innerCmd + " ", reQual, qm)) {
        return rsError("bootstrap: `" + innerCmd +
                           "` sets no e(sample), and resampling the rows of its if/in qualifier "
                           "is not implemented in this version of PIET; restrict the data first "
                           "(preserve + keep if ...).",
                       "unsupported_option");
      }
    }
  }
  std::vector<int64_t> baseRows;  // 0-based head rows, in resampling order
  for (int64_t i = 0; i < nHead; ++i)
    if (inBase[static_cast<size_t>(i)]) baseRows.push_back(i);
  const int64_t nBase = static_cast<int64_t>(baseRows.size());
  if (nBase <= 1) {
    return bsRc("insufficient observations to compute bootstrap standard errors", 2000);
  }

  // cluster(): sort the base by the cluster, stable; number the clusters.
  std::vector<int64_t> clid;
  int64_t nClust = 0;
  if (!clusterVar.empty()) {
    Datastore d(obsHbin);
    if (!d.has(clusterVar) || isHiddenColumn(clusterVar)) {
      return bsRc("variable " + clusterVar + " not found", 111);
    }
    const cecon::Column& c = d.col(clusterVar);
    const bool isStr = c.dtype == cecon::DType::Factor;
    std::vector<std::string> skey(isStr ? static_cast<size_t>(nBase) : 0);
    std::vector<double> nkey(isStr ? 0 : static_cast<size_t>(nBase));
    for (int64_t k = 0; k < nBase; ++k) {
      const int64_t row = baseRows[static_cast<size_t>(k)];
      bool missing = false;
      if (isStr) {
        const int32_t code = c.codes()[row];
        missing = code < 0 || static_cast<size_t>(code) >= c.levels.size() ||
                  c.levels[static_cast<size_t>(code)].empty();
        if (!missing) skey[static_cast<size_t>(k)] = c.levels[static_cast<size_t>(code)];
      } else {
        const double v = c.f64()[row];
        missing = std::isnan(v);
        nkey[static_cast<size_t>(k)] = v;
      }
      if (missing) {
        return bsRc("missing values in cluster variable " + clusterVar + " not allowed", 459);
      }
    }
    std::vector<int64_t> ord(static_cast<size_t>(nBase));
    std::iota(ord.begin(), ord.end(), int64_t{0});
    std::stable_sort(ord.begin(), ord.end(), [&](int64_t a, int64_t b) {
      return isStr ? skey[static_cast<size_t>(a)] < skey[static_cast<size_t>(b)]
                   : nkey[static_cast<size_t>(a)] < nkey[static_cast<size_t>(b)];
    });
    std::vector<int64_t> sorted(static_cast<size_t>(nBase));
    clid.assign(static_cast<size_t>(nBase), 0);
    for (size_t q = 0; q < ord.size(); ++q) {
      const size_t k = static_cast<size_t>(ord[q]);
      sorted[q] = baseRows[k];
      const bool fresh =
          q == 0 || (isStr ? skey[k] != skey[static_cast<size_t>(ord[q - 1])]
                           : nkey[k] != nkey[static_cast<size_t>(ord[q - 1])]);
      if (fresh) ++nClust;
      clid[q] = nClust;
    }
    baseRows = std::move(sorted);
    if (nClust == 1) {
      return bsRc("singleton cluster detected", 460);
    }
  }

  // -- 3. the seed, then the replicates --------------------------------------
  if (!seedText.empty()) {
    const ojson s = dispatch("set seed " + seedText, session);
    if (const ojson* err = firstErrorBlock(s)) {
      return *err;
    }
  }
  const std::string rngstate = session.seed.stream().state_string();

  const std::string basePathIdx = nextPath(session.workdir(), "bs_baseidx");
  {
    CobraRunResult cr = runCobra({"eval", "--data", obsHbin, "--egen",
                                  std::string(kRowIdx) + " = seq()", "--out", basePathIdx});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return rsError(cobraErrorStr(ce), ce.code);
    }
  }
  SessionState::ScratchPin pin(session, {basePathIdx, startHbin, obsHbin});

  std::vector<std::vector<double>> repVals;  // one row per replicate, NaN = missing
  repVals.reserve(static_cast<size_t>(reps));
  std::string failure;
  for (int b = 0; b < reps && failure.empty(); ++b) {
    const std::vector<int64_t> pos =
        clusterVar.empty() ? bsdraw::srswrFast(session.seed.stream(), nBase)
                           : bsdraw::clustSrswr(session.seed.stream(), clid, nClust);
    std::vector<int64_t> idx;
    idx.reserve(pos.size());
    for (int64_t p : pos) idx.push_back(baseRows[static_cast<size_t>(p)] + 1);
    const std::string csv = session.workdir() + "/bs_idx.csv";
    registerScratchMint(csv);  // BUG-389
    if (!writeIndexCsv(csv, kRowIdx, idx)) { failure = "could not write the resample index"; break; }
    const std::string idxHbin = nextPath(session.workdir(), "bs_idx");
    CobraRunResult c1 = runCobra({"convert", "--data", csv, "--out", idxHbin, "--delim", ","});
    if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
    const std::string merged = nextPath(session.workdir(), "bs_merged");
    // master = the drawn rows (with repeats, in the resample's row order),
    // using = the base keyed by the same index; the using-only (undrawn) rows
    // are not part of the sample.
    CobraRunResult c2 = runCobra({"merge", "--data", idxHbin, "--using", basePathIdx, "--type",
                                  "m:1", "--key", kRowIdx, "--out", merged});
    if (!c2.ok) { failure = cobraErrorStr(errorFromRaw(c2)); break; }
    const std::string repPath = nextPath(session.workdir(), "bs_rep");
    CobraRunResult c3 = runCobra({"eval", "--data", merged, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", repPath});
    if (!c3.ok) { failure = cobraErrorStr(errorFromRaw(c3)); break; }
    // The two intermediates are never loaded; drop them now rather than at exit.
    for (const std::string& p : {idxHbin, merged}) {
      cecon::native_unlink(p);
      forgetScratchMint(p);
    }

    session.load(repPath);
    const ojson inner = dispatch(repCmd, session);
    std::vector<double> vals(K, std::nan(""));
    if (firstErrorBlock(inner) == nullptr) {
      if (bMode) {
        const std::optional<eval::EvalMatrix> rb = session.eval.resolveEMatrix("b");
        // A replicate whose e(b) is not the full sample's columns posts missing
        // (_loop_bs's _check_omit rule).
        if (rb.has_value() && rb->data.size() == 1 && rb->colnames == names &&
            rb->data[0].size() == K)
          vals = rb->data[0];
      } else {
        for (size_t k = 0; k < K; ++k) {
          std::optional<double> v = evalStat(expList[k].expr, session);
          if (v.has_value()) vals[k] = *v;
        }
      }
    }
    // postfile stores the replicates as float (bootstrap's default storage).
    for (double& v : vals) v = e3::toFloat(v);
    repVals.push_back(std::move(vals));
  }
  session.load(obsHbin);
  // A failed run leaves no e(): the live one would be the last replicate's fit
  // on resampled data, which no later `test`/`predict` may read.
  auto failNoE = [&](const ojson& err) {
    (void)dispatch("ereturn clear", session);
    eval::replaceRClass(session.eval);
    return err;
  };
  if (!failure.empty()) return failNoE(rsError("bootstrap: " + failure));

  // -- 4. _bs_sum ------------------------------------------------------------
  std::vector<const std::vector<double>*> good;  // listwise: every statistic present
  for (const auto& rv : repVals) {
    bool ok = true;
    for (double v : rv) ok = ok && !std::isnan(v);
    if (ok) good.push_back(&rv);
  }
  const int64_t nReps = static_cast<int64_t>(good.size());
  const int64_t nMis = static_cast<int64_t>(repVals.size()) - nReps;
  if (nReps <= 1)
    return failNoE(bsRc("insufficient observations to compute bootstrap standard errors; no "
                        "results will be saved",
                        2000));
  std::vector<double> mean(K, 0.0);
  for (size_t k = 0; k < K; ++k) {
    long double s = 0;
    for (const auto* rv : good) s += (*rv)[k];
    mean[k] = static_cast<double>(s / static_cast<long double>(nReps));
  }
  std::vector<std::vector<double>> V(K, std::vector<double>(K, 0.0));
  for (size_t a = 0; a < K; ++a) {
    for (size_t c = 0; c <= a; ++c) {
      long double s = 0;
      for (const auto* rv : good)
        s += (static_cast<long double>((*rv)[a]) - mean[a]) * (static_cast<long double>((*rv)[c]) - mean[c]);
      V[a][c] = V[c][a] = static_cast<double>(s / static_cast<long double>(nReps - 1));
    }
  }
  const double zalpha = cecon::stata_invnormal((100.0 + level) / 200.0);
  std::vector<double> se(K), bias(K), z0(K), pLo(K), pHi(K), bcLo(K), bcHi(K);
  for (size_t k = 0; k < K; ++k) {
    se[k] = V[k][k] >= 0 ? std::sqrt(V[k][k]) : std::nan("");
    bias[k] = mean[k] - observed[k];
    std::vector<double> xs;
    xs.reserve(good.size());
    for (const auto* rv : good) xs.push_back((*rv)[k]);
    std::sort(xs.begin(), xs.end());
    int64_t ct = 0;
    for (double x : xs) ct += x <= observed[k] ? 1 : 0;
    z0[k] = bcLo[k] = bcHi[k] = std::nan("");
    if (ct > 0 && ct < nReps) {
      z0[k] = cecon::stata_invnormal(static_cast<double>(ct) / static_cast<double>(nReps));
      bcLo[k] = pctileStata(xs, 100.0 * cecon::stata_normal(2 * z0[k] - zalpha));
      bcHi[k] = pctileStata(xs, 100.0 * cecon::stata_normal(2 * z0[k] + zalpha));
    }
    pLo[k] = pctileStata(xs, (100.0 - level) / 2.0);
    pHi[k] = pctileStata(xs, (100.0 + level) / 2.0);
  }

  // rank of e(V) and the Wald model test
  // (`test [#1]`, the first equation's coefficients but its _cons).
  const std::vector<std::vector<double>> Vinv = e3::invsym(V);
  int rank = 0;
  for (size_t k = 0; k < K; ++k) rank += Vinv[k][k] != 0.0 ? 1 : 0;
  double chi2 = std::nan(""), chi2p = std::nan(""), dfM = std::nan("");
  const bool modelTest = bMode && std::isfinite(innerDfM);
  if (modelTest) {
    const std::string eq1 = names[0].find(':') == std::string::npos
                                ? std::string()
                                : names[0].substr(0, names[0].find(':'));
    std::vector<size_t> sel;
    for (size_t k = 0; k < K; ++k) {
      const size_t colon = names[k].find(':');
      const std::string eq = colon == std::string::npos ? std::string() : names[k].substr(0, colon);
      const std::string nm = colon == std::string::npos ? names[k] : names[k].substr(colon + 1);
      if (eq != eq1) continue;
      if (nm == "_cons") continue;
      if (V[k][k] == 0.0 && observed[k] == 0.0) continue;  // a base / omitted column
      sel.push_back(k);
    }
    std::vector<std::vector<double>> Vs(sel.size(), std::vector<double>(sel.size()));
    for (size_t a = 0; a < sel.size(); ++a)
      for (size_t c = 0; c < sel.size(); ++c) Vs[a][c] = V[sel[a]][sel[c]];
    const std::vector<std::vector<double>> Vsi = e3::invsym(Vs);
    int df = 0;
    long double q = 0;
    for (size_t a = 0; a < sel.size(); ++a) {
      if (Vsi[a][a] != 0.0) ++df;
      for (size_t c = 0; c < sel.size(); ++c)
        q += static_cast<long double>(observed[sel[a]]) * Vsi[a][c] * observed[sel[c]];
    }
    dfM = df;
    if (df > 0) {
      chi2 = static_cast<double>(q);
      chi2p = chi2tailBs(static_cast<double>(df), chi2);
    }
  }

  // -- 5. post e() -----------------------------------------------------------
  const double crit = zalpha;  // invnormal((100+level)/200), as e(ci_normal)
  ojson rows = ojson::array();
  for (size_t k = 0; k < K; ++k) {
    ojson r;
    r["name"] = names[k];
    r["b"] = observed[k];
    if (std::isfinite(se[k]) && se[k] > 0) {
      const double z = observed[k] / se[k];
      r["se"] = se[k];
      r["z"] = z;
      r["p"] = normalTwoSidedP(z);
      r["ci_lo"] = observed[k] - crit * se[k];
      r["ci_hi"] = observed[k] + crit * se[k];
    } else {
      r["se"] = std::isfinite(se[k]) ? ojson(se[k]) : ojson(nullptr);
      r["z"] = nullptr;
      r["p"] = nullptr;
      r["ci_lo"] = nullptr;
      r["ci_hi"] = nullptr;
    }
    rows.push_back(r);
  }
  ojson block = innerBlock.has_value() && innerBlock->is_object() ? *innerBlock : ojson::object();
  // bootstrap's exclusions from the copied e() (F chi2 df_r df_m, the fit's
  // own V and its p), plus the fit's stripe/rank/cluster keys, which describe
  // the engine's coefficient table rather than this posting.
  // An eform fit's `coef_metric` would make r(table) exponentiate these
  // coefficient-metric rows.
  for (const char* key : {"df_r", "df_m", "F", "p_F", "chi2", "p", "e_V", "eb_stripe", "k_vars",
                          "n_clusters", "vce", "vcetype", "cluster", "clustvar", "coef_metric"})
    block.erase(key);
  block["type"] = "estimation";
  block["estimator"] = "bootstrap";
  const double nObs = static_cast<double>(std::count(inBase.begin(), inBase.end(), char{1}));
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = rows;
  if (bMode && !innerDepvar.empty()) block["depvar"] = innerDepvar;
  else block.erase("depvar");

  LastModel lm;
  lm.cobraCmd = "bootstrap";
  lm.stataCmd = bMode && !innerECmd.empty() ? innerECmd : "bootstrap";
  lm.depvar = bMode ? innerDepvar : std::string();
  lm.vcov = "bootstrap";
  lm.eVce = "bootstrap";
  lm.result = block;
  if (!e3::installEsampleMask(session, lm, inBase))
    return rsError("bootstrap: the results were computed, but e(sample) could not be installed");
  session.setLastModel(lm);
  session.lastEstBlock = block;
  {
    std::vector<double> bvec = observed;
    e3::postBV(session, names, bvec, V);
  }
  // The copied inner e() (default _b only).
  if (bMode) {
    static const std::set<std::string> kXScalars = {"F", "chi2", "df_r", "df_m", "N", "N_clust",
                                                    "rank", "p", "level"};
    static const std::set<std::string> kXStrings = {
        "cmd", "_estimates_name", "chi2type", "novariance", "clustvar", "vce", "vcetype",
        "cmdline", "title", "properties", "depvar", "cluster", "prefix"};
    for (const auto& [k, v] : innerScalars)
      if (!kXScalars.count(k)) e3::setEScalarOnFit(session, k, v);
    for (const auto& [k, v] : innerStrings)
      if (!kXStrings.count(k)) e3::setEStringOnFit(session, k, v);
    if (obsV.has_value() && obsV->data.size() == K) setEMatrix(session, "V_modelbased", *obsV);
  }
  e3::setEScalarOnFit(session, "N", nObs);
  e3::setEScalarOnFit(session, "level", level);
  e3::setEScalarOnFit(session, "N_reps", static_cast<double>(nReps));
  e3::setEScalarOnFit(session, "N_misreps", static_cast<double>(nMis));
  e3::setEScalarOnFit(session, "bs_version", 3);
  e3::setEScalarOnFit(session, "rank", rank);
  std::set<std::string> eqs;
  for (const auto& n : names) {
    const size_t colon = n.find(':');
    eqs.insert(colon == std::string::npos ? std::string() : n.substr(0, colon));
  }
  e3::setEScalarOnFit(session, "k_eq", static_cast<double>(eqs.size()));
  e3::setEScalarOnFit(session, "k_exp", bMode ? 0.0 : static_cast<double>(K));
  e3::setEScalarOnFit(session, "k_eexp", bMode ? static_cast<double>(K) : 0.0);
  e3::setEScalarOnFit(session, "k_extra", 0);
  if (!clusterVar.empty()) {
    e3::setEScalarOnFit(session, "N_clust", static_cast<double>(nClust));
    e3::setEStringOnFit(session, "clustvar", clusterVar);
    e3::setEStringOnFit(session, "cluster", clusterVar);
  }
  if (modelTest) {
    e3::setEScalarOnFit(session, "df_m", dfM);
    if (std::isfinite(chi2)) {
      e3::setEScalarOnFit(session, "chi2", chi2);
      e3::setEScalarOnFit(session, "p", chi2p);
    }
    e3::setEStringOnFit(session, "chi2type", "Wald");
  }
  {
    // Stata's `bootstrap `0'' (o1.log T1 `bootstrap , rep(50) seed(123): regress y x`).
    std::string cmdline = "bootstrap " + expTrim + (optsRaw.empty() ? "" : ", " + optsRaw) + ": " +
                          innerCmd;
    e3::setEStringOnFit(session, "cmdline", cmdline);
  }
  e3::setEStringOnFit(session, "cmdname", cmdname);
  e3::setEStringOnFit(session, "prefix", "bootstrap");
  e3::setEStringOnFit(session, "title", bootstrapTitle(innerTitle, cmdname));
  e3::setEStringOnFit(session, "rngstate", rngstate);
  for (size_t k = 0; k < K; ++k) e3::setEStringOnFit(session, "exp" + std::to_string(k + 1), exprs[k]);
  e3::setEStringOnFit(session, "command", repCmd);
  e3::setEStringOnFit(session, "vcetype", "Bootstrap");
  e3::setEStringOnFit(session, "vce", "bootstrap");
  e3::setEStringOnFit(session, "properties", "b V");
  {
    auto rowVec = [&](const std::vector<double>& v) {
      eval::EvalMatrix m;
      m.data.push_back(v);
      m.rownames = {"y1"};
      m.colnames = names;
      return m;
    };
    auto ci = [&](const std::vector<double>& lo, const std::vector<double>& hi) {
      eval::EvalMatrix m;
      m.data = {lo, hi};
      m.rownames = {"ll", "ul"};
      m.colnames = names;
      return m;
    };
    std::vector<double> repsV(K, static_cast<double>(nReps)), nlo(K), nhi(K);
    for (size_t k = 0; k < K; ++k) {
      nlo[k] = observed[k] - zalpha * se[k];
      nhi[k] = observed[k] + zalpha * se[k];
    }
    setEMatrix(session, "b_bs", rowVec(mean));
    setEMatrix(session, "reps", rowVec(repsV));
    setEMatrix(session, "bias", rowVec(bias));
    setEMatrix(session, "z0", rowVec(z0));
    setEMatrix(session, "se", rowVec(se));
    setEMatrix(session, "ci_normal", ci(nlo, nhi));
    setEMatrix(session, "ci_percentile", ci(pLo, pHi));
    setEMatrix(session, "ci_bc", ci(bcLo, bcHi));
  }

  // r(): the coefficient table's r(table) (normal-based) and r(N).
  eval::replaceRClass(session.eval);
  (void)postRTableFromBlock(session, block, level);
  session.eval.rValues.set("N", nObs);

  // -- 6. render -------------------------------------------------------------
  ojson lines = ojson::array();
  auto say = [&lines](const std::string& t) { lines.push_back(ojson{{"type", "display"}, {"text", t}}); };
  if (haveEsample) say("(running " + cmdname + " on estimation sample)");
  say("");
  say(padRight(bootstrapTitle(innerTitle, cmdname), 56) + "Number of obs = " +
      padLeft(commaGroup(static_cast<int64_t>(nObs)), 6));
  say(padRight("", 56) + "Replications  = " + padLeft(std::to_string(nReps), 6));
  if (modelTest && std::isfinite(chi2)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", chi2);
    say(padRight("", 56) + padRight("Wald chi2(" + std::to_string(static_cast<int>(dfM)) + ")", 14) +
        "= " + padLeft(buf, 6));
    say(padRight("", 56) + "Prob > chi2   = " + padLeft(fixed4(chi2p), 6));
  }
  if (!bMode) {
    say("");
    say("      Command: " + innerCmd);
    for (const auto& e : expList) say(padLeft(e.name, 13) + ": " + e.expr);
  }
  say("");
  if (!clusterVar.empty())
    say(padLeft("(Replications based on " + std::to_string(nClust) + " clusters in " + clusterVar + ")",
                78));
  const std::string rule(78, '-');
  say(rule);
  say("             |   Observed   Bootstrap                         Normal-based");
  {
    std::string corner = bMode && eqs.size() == 1 && eqs.count(std::string()) ? innerDepvar : "";
    say(padLeft(corner, 12) + " | coefficient  std. err.      z    P>|z|     [" +
        std::to_string(static_cast<int>(level)) + "% conf. interval]");
  }
  say("-------------+" + std::string(64, '-'));
  std::string lastEq = "\x01";
  for (size_t k = 0; k < K; ++k) {
    const size_t colon = names[k].find(':');
    const std::string eq = colon == std::string::npos ? std::string() : names[k].substr(0, colon);
    const std::string nm = colon == std::string::npos ? names[k] : names[k].substr(colon + 1);
    if (!eq.empty() && eq != lastEq) {
      if (lastEq != "\x01") say("-------------+" + std::string(64, '-'));
      say(padRight(eq, 12) + " |");
    }
    lastEq = eq;
    const ojson& r = rows[k];
    std::string line = padLeft(nm, 12) + " |" + padLeft(g9cell(observed[k]), 11);
    if (r["z"].is_number()) {
      line += padLeft(g9cell(se[k]), 11) + padLeft(fixed2(r["z"].get<double>()), 9) +
              padLeft(fixed3(r["p"].get<double>()), 8) + padLeft(g9cell(r["ci_lo"].get<double>()), 13) +
              padLeft(g9cell(r["ci_hi"].get<double>()), 12);
    } else {
      line += padLeft("(omitted)", 13);
    }
    say(line);
  }
  say(rule);
  if (nMis > 0)
    say("Note: One or more parameters could not be estimated in " + std::to_string(nMis) +
        " bootstrap replicate" + (nMis == 1 ? "" : "s") + ";\n      standard-error estimates "
        "include only complete replications.");

  ojson out;
  out["type"] = "resample";
  out["method"] = method;
  out["command"] = innerCmd;
  out["n_obs"] = static_cast<int64_t>(nObs);
  out["n_reps"] = nReps;
  out["n_reps_requested"] = reps;
  out["level"] = level;
  ojson srows = ojson::array();
  for (size_t k = 0; k < K; ++k) {
    ojson r = rows[k];
    r["bias"] = bias[k];
    srows.push_back(r);
  }
  out["rows"] = srows;
  out["est_sample"] = haveEsample;
  if (!seedText.empty()) out["seed"] = seedText;
  if (!clusterVar.empty()) {
    out["cluster"] = clusterVar;
    out["n_clust"] = nClust;
  }
  lines.push_back(out);
  return lines;
}

}  // namespace handlers
}  // namespace piet
