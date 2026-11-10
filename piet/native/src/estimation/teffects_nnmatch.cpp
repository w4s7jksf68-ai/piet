// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// teffects nnmatch: nearest-neighbour matching and its gen() ids.

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

// ===========================================================================
// AER scope-2 B2-4 — `teffects nnmatch` (StataCorp [CAUSAL])
// ===========================================================================
//
// Paper 076 (`code/clean/clean_15_matching.do:366`) is the reason this exists:
//
//   sort PNR
//   teffects nnmatch (outcome i.earnlevel_0 i.ageg ... i.mother_ageg) (treatment), gen(match_id)
//
// `outcome` is a constant 0 there, so the ESTIMATE is not what the paper
// consumes — `match_id1` of the TREATED rows is (lines 382-413 join it to
// control row numbers). That makes two things load-bearing that a coefficient
// gate would never see: the ids are 1-based `_n` of the CURRENT data (MEASURED
// under `if`: an odd-rows-only subsample comes back with ODD ids, so the row
// numbers are the FULL frame's — INFO|nn_edge|ifodd_* in
// tests/eval/golden/aer_estim2_oracle.log), and their ORDER inside a tie is
// Stata's, not ours. Gate ① still wants e(b)/e(V) exact, and it gets them.
//
// Method identity: internal note teffects-nnmatch-20260908/REPORT.md as
// CORRECTED by REFUTE-0.md. The kernel comment in src/estimators/nnmatch.cpp
// carries the H2 rule and its named divergence; this handler owns only the
// surface: parsing, option honesty, e() posting, and gen().
//
// NAMED DIVERGENCES, all of them deliberate:
//   * `e(mscale)`/`e(mstats)` are HIDDEN matrices in Stata and are not posted
//     here (the engine computes them; nothing in PIET reads them back).
//   * `predict te|po|distance` after this command is not wired in this landing;
//     the engine already returns te/po0/po1/dist in the LastModel's result, so
//     it is a wiring step, not a method question. Refused by name until then.
//   * a treatment variable that is not 0/1 is ACCEPTED with control = the lower
//     level (Stata's `ATE:r2vs1.t12` colname is reproduced), but only the
//     colname of that case was measured.
namespace {
const eval::OptVocab& kVocabTeffects = eval::registerOptVocab(
    {"teffects"},
    {{"generate", "gen", "nneighbor", "nn", "ate", "atet", "metric", "vce", "level", "dtolerance"},
     // Known to Stata, absent here: each is a DIFFERENT estimator or a rule
     // this investigation never measured. Named, never silently ignored.
     // dtolerance (BUG-468) is served: the engine's --dtolerance.
     {"biasadj", "ematch", "caliper", "osample", "control", "tlevel",
      "dmvariables", "pstolerance", "iterate", "aequations", "tolerance", "nolog",
      "coeflegend"},
     // _teffects_nnmatch.ado's syntax line: GENerate(), NNeighbor(), ATET,
     // METric() -- stems the 2-character prefix rule cannot reach on its own.
     {{"gen", "generate"}, {"nn", "nneighbor"}}});

// The two parenthesised groups of `teffects <sub> (…) (…)`. Scanned off the RAW
// text because the tokenizer has no notion of them; only the first two
// top-level groups are read, and everything before the first `(` must be the
// command and sub-command words.
bool teffectsGroups(const std::string& raw, std::vector<std::string>* g1,
                    std::vector<std::string>* g2, std::string* why) {
  std::vector<std::string> groups;
  std::size_t i = 0;
  while (groups.size() < 2 && i < raw.size()) {
    if (raw[i] != '(') { ++i; continue; }
    int depth = 0;
    std::size_t j = i;
    for (; j < raw.size(); ++j) {
      if (raw[j] == '(') ++depth;
      else if (raw[j] == ')') { if (--depth == 0) break; }
    }
    if (j >= raw.size()) { *why = "unbalanced parentheses"; return false; }
    groups.push_back(raw.substr(i + 1, j - i - 1));
    i = j + 1;
  }
  if (groups.size() < 2) { *why = "two parenthesised groups are required"; return false; }
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        if (c == ',') out.push_back(",");
      } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
  };
  *g1 = split(groups[0]);
  *g2 = split(groups[1]);
  return true;
}

// `vce(robust, nn(3))` / `vce(robust)` / `vce(iid)`. Returns false with a
// message when the spelling is one this build does not answer for.
bool parseTeffectsVce(const std::string& v, int* nn, std::string* why) {
  std::string s = strutil::trim(v);
  std::string head = s, tail;
  const auto comma = s.find(',');
  if (comma != std::string::npos) { head = strutil::trim(s.substr(0, comma));
                                    tail = strutil::trim(s.substr(comma + 1)); }
  if (strutil::lower(head) != "robust") { *why = strutil::lower(head); return false; }
  if (tail.empty()) return true;
  static const std::regex reNn(R"(^nn\s*\(\s*([0-9]+)\s*\)$)");
  std::smatch m;
  if (!std::regex_match(tail, m, reNn)) { *why = tail; return false; }
  *nn = std::stoi(m.str(1));
  return true;
}
}  // namespace

ojson runTeffects(const std::string& sub, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  // Every other teffects estimator is a DIFFERENT method with its own
  // inference; each keeps its own named refusal rather than borrowing this one.
  if (sub != "nnmatch")
    return ojson(eval::refusedCommand("teffects", "teffects_" + sub + "_unimplemented"));
  if (!engineHasSubcommand("nnmatch")) return engineTooOld("teffects nnmatch", "nnmatch");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTeffects.known,
                                            kVocabTeffects.unimplemented)) {
    return ojson(*err);
  }

  std::string genStub, metric, level, dtolerance;
  int nneighbor = 1, vceNn = 2;
  bool atet = false, sawAte = false, sawVce = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "generate" || k == "gen") genStub = strutil::trim(v.strValue);
    else if (k == "dtolerance") dtolerance = strutil::trim(v.strValue);
    else if (k == "nneighbor" || k == "nn") {
      const std::string t = strutil::trim(v.strValue);
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos)
        return aerErrorReason("teffects nnmatch: nneighbor(#) must be an integer", 198);
      nneighbor = std::stoi(t);
    } else if (k == "atet") atet = true;
    else if (k == "ate") sawAte = true;
    else if (k == "metric") {
      // `metric(matrix M)` names a user matrix; only the metric WORD decides
      // the refusal, and the reason string must stay a single token.
      metric = strutil::lower(strutil::trim(v.strValue));
      const auto sp = metric.find_first_of(" \t");
      if (sp != std::string::npos) metric = metric.substr(0, sp);
    }
    else if (k == "level") level = strutil::trim(v.strValue);
    else if (k == "vce") {
      sawVce = true;
      std::string why;
      if (!parseTeffectsVce(v.strValue, &vceNn, &why)) {
        // `vce(iid)` was captured once for the record and never verified, and
        // an unknown vcetype is a wrong ANSWER, not a wrong option name.
        for (char& c : why)
          if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_vce_" + why));
      }
    }
  }
  (void)sawVce;
  if (sawAte && atet)
    return aerSyntaxError("teffects nnmatch: options ate and atet are mutually exclusive");
  if (!metric.empty() && metric != "mahalanobis" && metric != "ivariance" &&
      metric != "euclidean")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_metric_" + metric));
  // `level(#)`: Stata's band, MEASURED at four points on this very command
  // (aer_estim2_oracle.log INFO|nn_level|l0_rc=198, |l9_rc=198, |l9999_rc=0,
  // |l101_rc=198, |labc_rc=198) -- 10.00 <= # <= 99.99, r(198) outside it and
  // r(198) for a non-number. The value is FORWARDED to the engine below; it is
  // not an accept-note, because it moves the reported interval.
  if (!level.empty()) {
    char* endp = nullptr;
    const double lv = std::strtod(level.c_str(), &endp);
    while (endp != nullptr && *endp != '\0' && std::isspace(static_cast<unsigned char>(*endp))) ++endp;
    if (endp == level.c_str() || (endp != nullptr && *endp != '\0') || !std::isfinite(lv) ||
        lv < 10.0 || lv > 99.99)
      return aerErrorReason("teffects nnmatch: level() must be between 10 and 99.99", 198);
  }

  std::vector<std::string> g1, g2;
  std::string why;
  if (!teffectsGroups(parsed.raw, &g1, &g2, &why))
    return aerSyntaxError("teffects nnmatch: syntax is `teffects nnmatch (ovar mvarlist) (tvar) "
                          "[if] [in] [, options]' -- " + why);
  if (g1.size() < 2)
    return aerSyntaxError("teffects nnmatch: the first group needs an outcome and at least one "
                          "matching variable");
  for (const auto& tok : g1) if (tok == ",")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_omvarlist_options"));
  // `(tvar , tlevel(1))` and the treatment-model varlist of ipw/aipw are other
  // commands' grammar; here the second group is exactly one variable.
  if (g2.size() != 1)
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_tvar_group_options"));

  const std::string y = g1[0], tvar = g2[0];
  // `mvars` is e(mvarlist): the matching variables in TYPED order with the
  // factor/continuous OPERATOR STRIPPED. Measured (aer_estim2_oracle.log
  // INFO|nn_level|mixed_mvarlist and |cdot_mvarlist, plus the 14-fixture
  // oracle.txt): `(y x1 i.a i.b)` -> `x1 a b`, `(y c.x1 i.a)` -> `x1 a`,
  // `(y i.a i.b)` -> `a b`. xs/fvs alone cannot rebuild that order, which is
  // why the list is accumulated here and not from the two split vectors.
  std::vector<std::string> xs, fvs, mvars;
  for (std::size_t i = 1; i < g1.size(); ++i) {
    std::string tok = g1[i];
    if (tok.find('#') != std::string::npos)
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_interaction_terms"));
    const std::string low = strutil::lower(tok);
    if (low.rfind("i.", 0) == 0) { fvs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (low.rfind("c.", 0) == 0) { xs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (tok.find('.') != std::string::npos)
      // `L.x`, `ib2.a`, `o.x` -- an operator whose meaning this build has not
      // measured for the matching metric. Named, not quietly dropped.
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_fv_operator"));
    else { xs.push_back(tok); mvars.push_back(tok); }
  }
  if (xs.empty() && fvs.empty())
    return aerSyntaxError("teffects nnmatch: at least one matching variable is required");

  // gen(stub) mints stub1..stubK where K = e(k_nnmax). Stata refuses r(110) on a
  // collision, but it does NOT scan the whole stub* family and it does not do it
  // all at once -- MEASURED, StataBE 19.5 2026-09-09 on the 200-row fixture
  // (default nneighbor(1), k_nnmax 2):
  //
  //   mm1 exists, nneighbor(1)  -> r(110) "variable mm1 already defined /
  //                                        in option generate(mm)"
  //   mm2 exists, nneighbor(1)  -> r(110) "maximum number of nearest neighbors
  //                                        is 2, but variable mm2 already exists"
  //   mm3 exists, nneighbor(1)  -> rc 0, mm1/mm2 created  (mm3 is NOT touched)
  //   mm3 exists, nneighbor(3)  -> r(110) "variable mm3 already defined ..."
  //
  // i.e. the stubs Stata can name BEFORE it estimates are stub1..stub(nneighbor)
  // -- that many are guaranteed -- and it carries a DIFFERENT text for the ones
  // only the fitted K reveals. The earlier 1..64 pre-scan refused the third case
  // too, which is an honest error where Stata answers: a divergence created by a
  // fix. Both halves below are the measured rule.
  if (!genStub.empty()) {
    if (!strutil::isValidStataIdentifier(genStub + "1"))
      return aerSyntaxError("teffects nnmatch: invalid generate() stub `" + genStub + "'");
    Datastore* ds = nullptr;
    try { ds = session.datastore(); } catch (const std::exception&) { ds = nullptr; }
    if (ds != nullptr)
      for (int k = 1; k <= nneighbor; ++k) {
        const std::string nm = genStub + std::to_string(k);
        if (ds->has(nm))
          return aerErrorReason("teffects nnmatch: variable " + nm + " already defined", 110);
      }
  }

  std::vector<std::string> args = {"nnmatch", "--data", *session.currentHbin(), "--y", y,
                                   "--t", tvar};
  if (!xs.empty()) { args.push_back("--x"); args.push_back(est_detail::joinCommaM4(xs)); }
  if (!fvs.empty()) { args.push_back("--fv"); args.push_back(est_detail::joinCommaM4(fvs)); }
  args.push_back("--nn"); args.push_back(std::to_string(nneighbor));
  args.push_back("--vce-nn"); args.push_back(std::to_string(vceNn));
  if (!metric.empty()) { args.push_back("--metric"); args.push_back(metric); }
  if (atet) { args.push_back("--stat"); args.push_back("atet"); }
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  // BUG-468: `dtolerance(#)`, the exact-match tolerance (default sqrt(c(epsdouble)),
  // e(dtolerance); StataBE 19.5 p-margins/s8.do: dtolerance(1e-7) and (0.05) give
  // the engine's ATE/SE/k_nnmax, dtolerance(0) is r(198) with this message).
  if (!dtolerance.empty()) {
    char* endp = nullptr;
    const double dt = std::strtod(dtolerance.c_str(), &endp);
    if (endp == dtolerance.c_str() || endp == nullptr || *endp != '\0' || !(dt > 0))
      return aerErrorReason("dtolerance(#) must be a real greater than zero", 198);
    args.push_back("--dtolerance");
    args.push_back(dtolerance);
  }
  // `if` restricts the ESTIMATION SAMPLE without renumbering the frame: the
  // engine takes it as a MASK over the original rows, so the ids it returns
  // stay frame `_n` -- which is what Stata does (INFO|nn_edge|ifodd_*) and what
  // paper 076's `merge 1:m match_id1` depends on. BUG-468: `in a/b` joins the
  // same mask as `_n` bounds, so the ids stay frame `_n` there too.
  std::string keepif;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) keepif = "(" + *parsed.ifExpr + ")";
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = nullptr;
    try { ds = session.datastore(); } catch (const std::exception&) { ds = nullptr; }
    const InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return inRangeRejectError(bad, *parsed.inRange, "teffects");
    }
    keepif += (keepif.empty() ? "" : " & ") + std::string("(_n>=") + std::to_string(rr.a) +
              " & _n<=" + std::to_string(rr.b) + ")";
  }
  if (!keepif.empty()) {
    args.push_back("--keepif");
    args.push_back(keepif);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    // Same rc discipline as oaxacaEngineError: an engine refusal carrying an
    // exact Stata rc outranks the coarse mapping, and the default engine_error
    // (which reads as rc 1) is never what comes back.
    const std::string msg = "teffects nnmatch: " + cobraErrorMessage(ce);
    static const std::regex reRc(R"(r\((\d+)\))");
    std::smatch mrc;
    if (std::regex_search(ce.detail, mrc, reRc))
      return aerErrorReason(msg, std::stoi(mrc.str(1)));
    if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
    if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
    return aerError(msg, ce.code);
  }

  // ---- gen(stub1..stubK) --------------------------------------------------
  // Ragged ids -> padded long columns, ORDER preserved, `.` beyond |Omega(i)|
  // and on every row the estimation sample excluded.
  if (!genStub.empty()) {
    const int kmax = raw.value("k_nnmax", 0);
    const auto& ids = raw["matches"]["ids"];
    // The second half of the measured collision rule (see the pre-estimation
    // guard above): stub(nneighbor+1)..stubK are the ones only the fitted K
    // names, and Stata's text for them is the one that quotes K. Checked for
    // the WHOLE range before a single column is written, so a collision at
    // stub2 does not leave stub1 minted behind an error.
    {
      Datastore* ds2 = nullptr;
      try { ds2 = session.datastore(); } catch (const std::exception&) { ds2 = nullptr; }
      if (ds2 != nullptr)
        for (int k = nneighbor + 1; k <= kmax; ++k) {
          const std::string nm = genStub + std::to_string(k);
          if (ds2->has(nm))
            return aerErrorReason("teffects nnmatch: maximum number of nearest neighbors is " +
                                      std::to_string(kmax) + ", but variable " + nm +
                                      " already exists",
                                  110);
        }
    }
    for (int k = 0; k < kmax; ++k) {
      std::vector<double> col;
      col.reserve(ids.size());
      for (const auto& row : ids)
        col.push_back(static_cast<int>(row.size()) > k
                          ? static_cast<double>(row[k].get<long long>())
                          : std::numeric_limits<double>::quiet_NaN());
      const std::string nm = genStub + std::to_string(k + 1);
      ojson err;
      if (!estimWriteColumn(session, nm, col, &err)) return err;
    }
  }

  // ---- e() ----------------------------------------------------------------
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("n0", static_cast<double>(raw.value("n0", 0)));
  session.eval.setEScalar("n1", static_cast<double>(raw.value("n1", 0)));
  session.eval.setEScalar("treated", raw.value("treated_level", 1.0));
  session.eval.setEScalar("control", raw.value("control_level", 0.0));
  session.eval.setEScalar("k_levels", 2.0);
  session.eval.setEScalar("k_nneighbor", static_cast<double>(raw.value("k_nneighbor", 1)));
  session.eval.setEScalar("k_nnmin", static_cast<double>(raw.value("k_nnmin", 0)));
  session.eval.setEScalar("k_nnmax", static_cast<double>(raw.value("k_nnmax", 0)));
  session.eval.setEScalar("k_robust", static_cast<double>(raw.value("k_robust", 2)));
  session.eval.setEScalar("dtolerance", raw.value("dtolerance", 1.4901161193847656e-8));
  // e(level) is deliberately NOT posted: the full `ereturn list` capture of
  // all 14 oracle fixtures (internal capture
  // oracle.txt SCALARS) has no `level`, and `display e(level)` after
  // `teffects nnmatch ..., level(90)` prints `.`
  // (aer_estim2_oracle.log INFO|nn_level|l90_e_level). Posting it was an
  // invention; the option's ONLY measured effect is on ci_lo/ci_hi.
  session.eval.setEString("cmd", "teffects");
  session.eval.setEString("subcmd", "nnmatch");
  session.eval.setEString("depvar", y);
  session.eval.setEString("tvar", tvar);
  {
    std::string mv;
    for (std::size_t i = 0; i < mvars.size(); ++i) mv += (i > 0 ? " " : "") + mvars[i];
    session.eval.setEString("mvarlist", mv);
  }
  session.eval.setEString("metric", raw.value("metric", std::string("mahalanobis")));
  session.eval.setEString("stat", atet ? "atet" : "ate");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("vcetype", "AI robust");
  session.eval.setEString("title", "Treatment-effects estimation");
  {
    // NOT the literal "0 1": measured on a 1/2-coded treatment
    // (aer_estim2_oracle.log INFO|nn_level|t12_tlevels) Stata answers `1 2`,
    // which is also what e(control)/e(treated)=1/2 and the colname
    // `ATE:r2vs1.t12` in the SAME e() set already said. The levels are
    // integers by construction (the engine refuses a non-integer treatment).
    auto lvl = [](double v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
      return std::string(buf);
    };
    session.eval.setEString("tlevels", lvl(raw.value("control_level", 0.0)) + " " +
                                           lvl(raw.value("treated_level", 1.0)));
  }
  session.eval.setEString("properties", "b V");
  if (!genStub.empty()) {
    std::string iv;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k)
      iv += (k > 1 ? " " : "") + genStub + std::to_string(k);
    session.eval.setEString("indexvar", iv);
  }
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };

  LastModel lm;
  lm.cobraCmd = "nnmatch";
  // BUG-458 publishes e(cmd)/e(depvar)/e(vce) out of the LastModel, and it
  // prefers stataCmd -- without this line e(cmd) would come back `nnmatch`,
  // which is the ENGINE verb, where Stata answers `teffects` (with `nnmatch`
  // in e(subcmd), posted separately above).
  lm.stataCmd = "teffects";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  for (const auto& f : fvs) lm.x.push_back("i." + f);
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);
  // eb-order T-P3 part 0: posted after the fresh setLastModel, as oaxaca's
  // (the block's one row is `ATE:r<t>vs<c>.<tvar>`, the raw name, so
  // `_b[ATE:r1vs0.t]` is unchanged, aer_estim2_oracle.log:832).
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }
  const ojson estBlock = est_detail::cobraToEstimationM4(ojson(raw), "nnmatch", &session);
  session.lastEstBlock = estBlock;

  ojson blocks = ojson::array();
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(estBlock);
  // The gen() columns are a DATASET MUTATION and mutation_result.h's contract
  // ("every handler TU that mutates the loaded dataset returns" this envelope)
  // binds here exactly as it does for the estimWriteColumn callers above
  // (runTsfilter / runLowess / runLpoly). Without it routes.cpp's
  // `varlist_after` comes back null and the console's Variables pane never
  // learns that stub1..stubK exist (app.js:1007 guards on it).
  if (!genStub.empty()) {
    std::vector<std::string> minted;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k) minted.push_back(genStub + std::to_string(k));
    if (!minted.empty()) blocks.push_back(mutationResult(session, "teffects", minted, {}));
  }
  return blocks;
}

}  // namespace piet::handlers
