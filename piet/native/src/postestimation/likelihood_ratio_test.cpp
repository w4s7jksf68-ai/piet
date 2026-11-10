// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   lrtest    the stored e() of `estimates store` (SessionState::estimatesModels,
//             StoredModel::eScalars/eStrings) and the live e().
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers/postestimation.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "language/command_reply_support.h"

namespace {

// lrtest: Df() FORCE STats DIr ASIS, plus the pre-8 syntax's saving()/using()/
// model(). `stats` and `dir` print `estimates stats`/`estimates dir` tables
// after the test; the old syntax keeps models under LRTEST_ names. None is
// done here, so each is refused by name. `Df()` declares `d` as its minimum
// spelling (review l1 L15: `lrtest b3 b1, d(5)` is r(df) 5), which the prefix
// rule cannot reach -- `d` also begins `dir` -- so it is a declared alias.
const piet::eval::OptVocab& kVocabLrtest = piet::eval::registerOptVocab(
    {"lrtest"},
    {{"df", "force", "asis"}, {"stats", "dir", "saving", "using", "model"}, {{"d", "df"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

// ===========================================================================
// lrtest
// ===========================================================================
//
// Implements the likelihood-ratio test per lrtest's help file and its ado;
// identified against StataBE probes (w2ax_t1_official.log lrtest_regress,
// lrtest_reverse, lrtest_df, lrtest_logit, lrtest_current, lrtest_mixed,
// lrtest_mixed_force, lrtest_missing, lrtest_diffN; envp3 lr_*).
//
// THE RULE. Each model contributes e(N), e(ll), e(ll_0), e(cmd), e(depvar) and
// its parameter count e(rank). The first name is taken as unrestricted; when
// its rank is the smaller one the two swap (not under `asis`), so the order of
// the names does not matter (lrtest_reverse). df = rank_u - rank_r, or df(#);
// chi2 = 2*(ll_u - ll_r), p = upper chi2(df) tail. Under `force` neither the
// df checks nor the sample/estimator checks run: lrtest_mixed_force is regress
// (rank 4) against logit (rank 2) -> df 2, chi2 = 2*(ll_regress - ll_logit) =
// -222.62..., p 1. Without it: df < 0 or = 0, N different (lrtest_diffN
// `observations differ: 45 vs. 80`), e(cmd) different (lrtest_mixed `test
// involves different estimators: regress vs. logit`), e(depvar) different, or
// e(ll_0) different beyond 1e-6 are r(498); a robust/cluster/pweight/pseudo-
// likelihood fit is r(498) too. One name means "against the current results",
// which are named by their stored name when they are a stored model
// (lrtest_current `t1l1 nested within t1l2`; envp3 lr_self: naming that model
// as well is r(198) "specified more than once") and `.` otherwise.
//
// THE p-VALUE. No upper chi2 tail is linked into PIET (no chi2tail() in the
// expression layer; the engine's gammq lives in src/estimators/posthest.cpp,
// which piet_core does not compile, and qreg2's copy is file-local). For an
// integer df the tail has an exact finite form in std::exp/std::erfc --
// Q(m, z) = e^-z sum_{j<m} z^j/j! and Q(m+1/2, z) = erfc(sqrt z) +
// sum_{j<m} z^(j+1/2) e^-z / Gamma(j+3/2), z = chi2/2 -- and that is what is
// used here; a fractional df() (2.5) is refused by name (the general tail is
// the shared helper this file does not have), while an integer-valued one in
// any spelling (2.0, 1e300) is computed. df 0 has a missing p (envp3
// lr_eqdf_force), a negative chi2 p 1.
namespace {

// What `display <number>` prints: the default %9.0g, without its padding.
std::string shownNumber(double v) { return strutil::trim(fmtStataDefaultG(v)); }

// `df` is a positive integer-valued double (df(#) may be as large as 1e300,
// review l3 H1). The terms are those of a Poisson(z) pmf (shifted by 1/2 when
// df is odd): they rise to the mode at j ~ z and then fall geometrically, so
// the series stops once a term past the mode no longer moves the sum -- the
// remaining terms are below 1e-17 of it -- and a df far above chi2 costs
// O(z) terms, not O(df) (Stata answers df(1e300) at once: p = 1.0000).
double chi2UpperTailIntegerDf(double df, double x) {
  if (!(df > 0) || !std::isfinite(df)) return std::numeric_limits<double>::quiet_NaN();
  if (!(x > 0)) return 1.0;
  const double z = x / 2.0;
  const bool odd = std::fmod(df, 2.0) != 0.0;
  const double m = std::floor(df / 2.0);
  const double shift = odd ? 0.5 : 0.0;
  double sum = odd ? std::erfc(std::sqrt(z)) : 0.0;
  auto settled = [&](double j, double term) { return j > z && term <= sum * 1e-17; };
  if (z < 700.0) {
    // Forward recurrence in the term ratio z/(j+a): every term positive, no
    // cancellation.
    const double kPi = 3.14159265358979323846;
    double term = odd ? 2.0 * std::sqrt(z / kPi) * std::exp(-z) : std::exp(-z);
    for (double j = 0; j < m; j += 1) {
      if (j > 0) term *= z / (j + shift);
      sum += term;
      if (settled(j, term)) break;
    }
  } else {
    // Far tail: each term in log space so e^-z cannot underflow on its own.
    const double lz = std::log(z);
    for (double j = 0; j < m; j += 1) {
      const double a = j + shift;
      const double term = std::exp(a * lz - z - std::lgamma(a + 1.0));
      sum += term;
      if (settled(j, term)) break;
    }
  }
  return std::min(1.0, sum);
}

// Stata's abbrev(s, n) for a name: first n-2 characters, `~`, last character.
std::string abbrevName(const std::string& s, size_t n) {
  if (s.size() <= n || n < 3) return s;
  return s.substr(0, n - 2) + "~" + s.back();
}

struct LrModel {
  std::string name;  // as displayed: a stored name, or "."
  double N = std::numeric_limits<double>::quiet_NaN();
  double rank = 0;
  double ll = std::numeric_limits<double>::quiet_NaN();
  double ll0 = std::numeric_limits<double>::quiet_NaN();
  std::string cmd, depvar;
};

// e(NAME) as a number, through the macro layer's own e() resolver -- the one
// `display e(rank)` and `local k = e(ll)` read (it maps Stata's names onto the
// estimation result's keys: rank -> k_vars, N -> n_obs, ...; a posted e-scalar
// otherwise). NaN for an absent or missing value. Never a second key table.
double eScalarOf(const eval::EvalSession& ev, const std::string& name) {
  std::string t = strutil::trim(eval::resolveEstScalarRefs("e(" + name + ")", ev, nullptr));
  // A negative value comes back parenthesised, "(-153.53...)", so it stays one
  // operand inside the expression it was substituted into.
  if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = strutil::trim(t.substr(1, t.size() - 2));
  if (t.empty() || t[0] == '"' || t[0] == '.') {
    // "." / ".a" are missing; a leading-dot NUMBER (".5") is not what the
    // resolver writes (it substitutes pythonFloatRepr text, "0.5").
    if (t.size() > 1 && t[0] == '.' && std::isdigit(static_cast<unsigned char>(t[1]))) {
      if (auto v = parseNumber(t)) return *v;
    }
    return std::numeric_limits<double>::quiet_NaN();
  }
  return parseNumber(t).value_or(std::numeric_limits<double>::quiet_NaN());
}

// lrtest's GetInfo over one model's e(). `ev` is the live e() (the current
// results) or a resolver session holding a stored model's e() stores.
// Returns an error block, or nullopt with `m` filled; `note` collects the
// rank-0 note.
std::optional<ojson> lrModelInfo(const std::string& name, const eval::EvalSession& ev, bool force,
                                 LrModel& m, std::string& note) {
  m.name = name;
  const eval::OrderedDict& estr = ev.eStrings;
  const std::string cmd = estr.get("cmd");
  if (cmd == "vec" || cmd == "mswitch" || !estr.get("mecmd").empty())
    return ojson(eval::refusedCommand("lrtest after " + (cmd.empty() ? estr.get("mecmd") : cmd),
                                      "model_family_not_wired"));
  if (!force) {
    const std::string vcetype = estr.get("vcetype");
    const bool isRegFamily = cmd == "regress" || cmd == "areg" || cmd == "xtreg";
    if (vcetype == "Robust") return envError("LR test likely invalid for models with robust VCE", 498);
    if (!estr.get("clustvar").empty())
      return envError("LR test likely invalid with cluster(" + estr.get("clustvar") + ")", 498);
    if (vcetype == "HAC" && isRegFamily)
      return envError("LR test likely invalid for models with HAC VCEs", 498);
    if (vcetype == "DK robust" && isRegFamily)
      return envError("LR test likely invalid for models with Driscoll-Kraay VCEs", 498);
    if (estr.get("wtype") == "pweight")
      return envError("LR test invalid in case of pweighted estimators", 498);
    if (estr.get("crittype") == "log pseudolikelihood")
      return envError("LR test likely invalid with log pseudolikelihoods", 498);
  }
  const double rank = eScalarOf(ev, "rank");
  if (std::isfinite(rank)) {
    m.rank = rank;
  } else {
    const eval::EvalMatrix* V = nullptr;
    for (const auto& [k, mat] : ev.eMatrices)
      if (k == "V") V = &mat;
    if (V == nullptr) {
      note += "note: model " + name + " does not contain matrix e(V); rank = 0 assumed.\n";
      m.rank = 0;
    } else {
      // rowsof(V) - diag0cnt(syminv(V)): syminv zeroes the rows of a singular
      // (omitted) parameter, whose variance is the zero on V's diagonal.
      double k = 0;
      for (size_t i = 0; i < V->data.size(); ++i)
        if (i < V->data[i].size() && V->data[i][i] != 0 && std::isfinite(V->data[i][i])) k += 1;
      m.rank = k;
    }
  }
  const double ll = eScalarOf(ev, "ll");
  if (!std::isfinite(ll)) {
    const bool present = ev.eScalars.find("ll") != nullptr ||
                         (ev.lastModelResult.has_value() && ev.lastModelResult->is_object() &&
                          ev.lastModelResult->contains("ll"));
    return envError(present ? "model " + name + " has missing e(ll)"
                            : "model " + name + " does not contain scalar e(ll)",
                    498);
  }
  m.ll = ll;
  m.N = eScalarOf(ev, "N");
  m.ll0 = eScalarOf(ev, "ll_0");
  m.cmd = cmd;
  m.depvar = estr.get("depvar");
  return std::nullopt;
}

// One specification token of `lrtest`: a name, or a parenthesised name.
// Returns false (with `composite` set) for a parenthesised LIST, a wildcard or
// `_all` -- the namelist's composite forms, which this file does not take.
bool lrSpecTokens(const std::string& text, std::vector<std::string>& toks, bool& composite) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    if (text[i] == '(') {
      const size_t close = text.find(')', i);
      if (close == std::string::npos) return false;
      const std::vector<std::string> inner = splitWs(text.substr(i + 1, close - i - 1));
      if (inner.size() != 1) {
        composite = true;
        return false;
      }
      toks.push_back(inner[0]);
      i = close + 1;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    toks.push_back(text.substr(i, j - i));
    i = j;
  }
  for (const auto& t : toks)
    if (t == "_all" || t.find_first_of("*?~") != std::string::npos) composite = true;
  return !composite;
}

}  // namespace

nlohmann::ordered_json runLrtest(const eval::ParsedCommand& parsed, SessionState& session) {
  // lrtest is an rclass program: whatever it ends with, the previous r() is
  // gone -- a failed lrtest leaves r() empty (envp3: `return list` after the
  // r(198) of `lrtest r*` lists nothing). Cleared first, on every path.
  eval::replaceRClass(session.eval);
  // ...and it posts nothing until the test is computed, so an absent r() read
  // afterwards is Stata's `.`, never the r-class layer's named r(111) refusal
  // (review l4: `capture lrtest m2 nosuch` then `local pv = r(p)` is `.`).
  eval::declareRPosted(session.eval, "lrtest", {});
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLrtest.known,
                                            kVocabLrtest.unimplemented))
    return ojson(*err);
  const std::string curCmd = session.eval.eStrings.get("cmd");
  if (session.eval.eStrings.get("prefix") == "svy")
    return envError("lrtest is not appropriate with survey estimation results", 322);
  if (curCmd == "cfprobit") return envError("lrtest is not supported after cfprobit", 322);

  std::vector<std::string> toks;
  bool composite = false;
  if (!lrSpecTokens(parsed.argText, toks, composite)) {
    if (composite)
      return ojson(eval::refusedCommand("lrtest (composite model list)", "composite_models"));
    return envError("invalid syntax", 198);
  }
  if (toks.empty())
    // envp3 lr_none: the pre-8 syntax, whose unrestricted model is the one an
    // old `lrtest, saving(0)` stored -- which this build never stores.
    return envError("In the old syntax, the unrestricted model defaulted to a model saved under "
                    "the name 0. This model was not found.",
                    198);
  std::string name1 = toks[0];
  std::string name2;
  if (toks.size() >= 2) {
    name2 = toks[1];
  } else {
    if (curCmd.empty()) return envError("last estimates not found\nsecond specification expected", 301);
    name2 = ".";
  }
  if (toks.size() > 2) {
    std::vector<std::string> extra(toks.begin() + 2, toks.end());
    return envError("invalid input following specifications: " + joinWith(extra, " "), 198);
  }
  // `.` names the current results -- by their stored name when the
  // live e() IS a stored model (lrtest_current); a stored name must exist
  // (lrtest_missing: `estimation result t1nosuch not found`, r(111)).
  const std::string activeName = session.activeStoredName();
  auto expand = [&](std::string& n) -> std::optional<ojson> {
    if (n == ".") {
      if (curCmd.empty()) return envError("last estimates not found", 301);
      if (!activeName.empty()) n = activeName;
      return std::nullopt;
    }
    const auto it = session.estimatesModels.find(n);
    if (it == session.estimatesModels.end() || !it->second) {
      // A name the eststo snapshot list knows but with no stored e() behind it
      // is a store this build cannot read back, not a missing name.
      if (session.estimatesGet(n) != nullptr)
        return ojson(eval::refusedCommand("lrtest " + n, "stored_results_without_e"));
      return envError("estimation result " + n + " not found", 111);
    }
    return std::nullopt;
  };
  if (auto e = expand(name1)) return *e;
  if (auto e = expand(name2)) return *e;
  if (name1 == name2) return envError("models " + name1 + " specified more than once", 198);

  const bool force = hasOpt(parsed.opts, "force");
  const bool asis = hasOpt(parsed.opts, "asis");
  std::optional<double> dfOpt;
  std::string dfNote;
  std::string out;
  if (auto s = optStr(parsed.opts, "df")) {
    const std::string typed = strutil::trim(*s);
    const std::optional<double> v = parseNumber(typed);
    if (!v.has_value()) return envError("'" + typed + "' found where number expected", 7);
    if (!(*v > 0)) return envError("option df() should be positive", 198);  // envp3 lr_df0
    if (std::floor(*v) != *v)
      return ojson(eval::refusedCommand("lrtest, df(" + typed + ")", "noninteger_df"));
    // lrtest's `confirm integer number` is LEXICAL: only [+-]digits passes
    // (envp5: 2, +3, 05 and 2147483648 pass; 2.0, 1e3, 1e300 do not). A df()
    // spelled otherwise is computed with the note (review l3 H1 `df(1e300)`).
    size_t k = (typed[0] == '+' || typed[0] == '-') ? 1 : 0;
    bool lexInt = k < typed.size();
    for (; k < typed.size(); ++k)
      if (!std::isdigit(static_cast<unsigned char>(typed[k]))) lexInt = false;
    if (!lexInt) dfNote = "note: specified degrees of freedom (" + typed + ") is not an integer.\n";
    dfOpt = *v;
  }

  auto info = [&](const std::string& n, LrModel& m) -> std::optional<ojson> {
    if (n == ".") return lrModelInfo(n, session.eval, force, m, out);
    // A stored model's e(), read through a resolver session that holds only
    // that model's e() stores (estimation/ml_estimation.cpp's local EvalSession precedent):
    // nothing about the live session is touched, and no `estimates restore`
    // side effect (e(sample) install, the active name) happens.
    const auto& sm = *session.estimatesModels.at(n);
    eval::EvalSession probe;
    probe.lastModelResult = sm.lastModelResult;
    probe.eScalars = sm.eScalars;
    probe.eStrings = sm.eStrings;
    probe.eMatrices = sm.eMatrices;
    return lrModelInfo(n, probe, force, m, out);
  };
  LrModel u, r;
  if (auto e = info(name1, u)) return *e;
  if (auto e = info(name2, r)) return *e;
  if (u.rank < r.rank && !asis) std::swap(u, r);

  double df = u.rank - r.rank;
  if (dfOpt.has_value()) df = *dfOpt;
  if (!force) {
    if (df < 0)
      return envError("df(unrestricted) < df(restricted): " + shownNumber(u.rank) + " < " +
                          shownNumber(r.rank),
                      498);  // envp3 lr_asis_rev
    if (df == 0)
      return envError("df(unrestricted) = df(restricted) = " + shownNumber(u.rank), 498);  // lr_eqdf
    if (u.N != r.N)
      return envError("observations differ: " + shownNumber(u.N) + " vs. " + shownNumber(r.N), 498);
    if (u.cmd != r.cmd)
      return envError("test involves different estimators: " + u.cmd + " vs. " + r.cmd, 498);
    if (!u.depvar.empty() && !r.depvar.empty() && u.depvar != r.depvar)
      return envError("dependent variables differ: " + u.depvar + " vs. " + r.depvar, 498);
    if (std::isfinite(u.ll0) && std::isfinite(r.ll0)) {
      const double reldif = std::fabs(u.ll0 - r.ll0) / (std::fabs(r.ll0) + 1.0);
      // KNOWN, cosmetic: Stata's %9.0g here; fmtStataDefaultG is the %9.0g
      // renderer, but its spacing is not pinned against a captured message.
      if (reldif > 1e-6)
        return envError("log likelihood of null models differ:" + fmtStataDefaultG(u.ll0) + " vs. " +
                            fmtStataDefaultG(r.ll0),
                        498);
    }
  }

  const double chi2 = -2.0 * (r.ll - u.ll);
  // The series costs about chi2/2 + 40*sqrt(chi2/2) terms (or df/2, if less);
  // a statistic so large that this would not finish at once is refused by
  // name rather than left to spin.
  {
    const double z = chi2 / 2.0;
    const double work = std::min(df / 2.0, z > 0 ? z + 40.0 * std::sqrt(z) + 50.0 : 0.0);
    if (work > 1e8)
      return ojson(eval::refusedCommand("lrtest (chi2 = " + shownNumber(chi2) + ")",
                                        "chi2_tail_out_of_range"));
  }
  const double p = chi2UpperTailIntegerDf(df, chi2);
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("df", df);
  session.eval.rValues.set("chi2", chi2);
  session.eval.rValues.set("p", p);
  eval::declareRPosted(session.eval, "lrtest", {"df", "chi2", "p"});
  // lrtest.ado's "double save for backward compatibility": $S_3 = df, $S_6 =
  // chi2, $S_7 = p (review l1 L2 `|2|5.465765009417908|.0650315654465017|`),
  // each stored by the `global NAME = exp` path so the number is written as
  // Stata writes a macro's value.
  for (const auto& [g, v] : {std::pair<const char*, double>{"S_3", df}, {"S_6", chi2}, {"S_7", p}}) {
    const std::string num = std::isnan(v) ? std::string(".") : eval::pythonFloatRepr(v);
    eval::MacroFnError mfe;
    eval::tryCaptureDefinition(std::string("global ") + g + " = " + num, session.eval, &mfe);
  }

  // The display: lrtest's own layout (oracle lrtest_* log lines, envp3 lr_df25).
  out = dfNote + out;
  const std::string dichi2 = strutil::trim(fixedFmt(chi2, 10, 2, false));
  const std::string dip = std::isnan(p) ? "." : strutil::trim(fixedFmt(p, 9, 4, false));
  const std::string dilr = "LR chi2(" + shownNumber(df) + ")";
  const std::string diprob = "Prob > chi2";
  const size_t w1 = std::max(dilr.size(), diprob.size());
  const size_t w2 = std::max(dichi2.size(), dip.size());
  auto ralign = [](const std::string& s, size_t w) {
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
  };
  const std::string rshow = r.name == "." ? "." : abbrevName(r.name, 14);
  const std::string ushow = u.name == "." ? "." : abbrevName(u.name, 14);
  out += "\nLikelihood-ratio test\nAssumption: " + rshow + " nested within " + ushow + "\n\n" +
         ralign(dilr, w1) + " = " + ralign(dichi2, w2) + "\n" + ralign(diprob, w1) + " = " +
         ralign(dip, w2);
  return displayBlock(out);
}

}  // namespace piet::handlers
