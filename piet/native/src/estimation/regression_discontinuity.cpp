// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/estimation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

#include "engine_report_support.h"
#include "graph/graph_point_layers.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabRdrobust = piet::eval::registerOptVocab(
    {"rdrobust"}, {{"c", "p", "q", "h", "b", "kernel", "nnmatch", "level"},
     {"covs", "fuzzy", "bwselect", "vce", "scalepar", "scaleregul", "masspoints",
      "bwcheck", "bwrestrict", "stdvars", "all", "weights", "deriv", "rho", "sharpbw"}});
const piet::eval::OptVocab& kVocabRddensity = piet::eval::registerOptVocab(
    {"rddensity"}, {{"c", "p", "q", "h", "kernel"},
     {"vce", "fitselect", "bwselect", "all", "plot", "masspoints", "bino", "nobino",
      "binomw", "binomn", "binomv", "level"}});
const piet::eval::OptVocab& kVocabRdplot = piet::eval::registerOptVocab(
    {"rdplot"}, {{"c", "p", "h", "nbins", "kernel"},
     {"covs", "binselect", "scale", "weights", "ci", "shade", "hide", "genvars",
      "graph_options", "support", "masspoints"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace engine_report_detail;

namespace {

// --- the two RD header-block geometries --------------------------------------
// rdrobust / rddensity share ONE geometry (label right-aligned in 18, " |",
// then a 10- and a 12-wide numeric column = 42 columns); rdplot's label field
// is three wider (21 + " |", still 10/12 after it = 45). The right-hand
// "Number of obs = ..." panel differs per command in both its start column and
// its value width, so it is a parameter rather than a constant.

struct TwoColGeom {
  size_t label;  // width of the right-aligned row label
  size_t c1;     // width of the "Left of c" column
  size_t c2;     // width of the "Right of c" column
};
constexpr TwoColGeom kRdGeom{18, 10, 12};      // rdrobust, rddensity
constexpr TwoColGeom kRdplotGeom{21, 10, 12};  // rdplot

std::string twoColRow(const TwoColGeom& g, const std::string& label, const std::string& a,
                      const std::string& b) {
  return padLeft(label, g.label) + " |" + padLeft(a, g.c1) + padLeft(b, g.c2);
}
std::string twoColRule(const TwoColGeom& g) {
  return std::string(g.label + 1, '-') + "+" + std::string(g.c1 + g.c2, '-');
}

// The right-hand info panel: `pad` blanks, then key padded to keyW, "=", then
// the value right-aligned in valW.
std::string infoCell(size_t pad, const std::string& key, size_t keyW, const std::string& value,
                     size_t valW) {
  return std::string(pad, ' ') + padRight(key, keyW) + "=" + padLeft(value, valW);
}

//
// SHAPE MISMATCH. The engine's own plot arrays are `[{"x":.., "y":..}, ...]`
// (cmds_rd.cpp's plot_pairs / cmds_binscatter.cpp's per-bin objects) — the
// graph layer's own `points` field is positional `[[x,y], ...]`
// (buildSvgTwoway indexes `pts[i][0]`/`pts[i][1]`). The two converters below
// are the entire translation.

// `[{"x":.., "y":..}, ...]` -> `[[x,y], ...]`. A non-numeric or malformed
// entry is dropped rather than crashing or inventing a zero — the same
// never-throw discipline num()/str() above use for the table half.
ojson xyObjArrayToPoints(const ojson& arr) {
  ojson out = ojson::array();
  if (!arr.is_array()) return out;
  for (const auto& p : arr) {
    if (p.is_object() && p.contains("x") && p.contains("y") && p.at("x").is_number() &&
        p.at("y").is_number()) {
      out.push_back(ojson::array({p.at("x").get<double>(), p.at("y").get<double>()}));
    }
  }
  return out;
}

// rdplot's `bins`/`fit` keys are each `{"left":[...], "right":[...]}` —
// reach one side of one of those two groups and convert it in one call.
ojson qeSidePoints(const ojson& r, const char* group, const char* side) {
  if (!r.contains(group) || !r.at(group).is_object()) return ojson::array();
  const ojson& g = r.at(group);
  if (!g.contains(side) || !g.at(side).is_array()) return ojson::array();
  return xyObjArrayToPoints(g.at(side));
}

}  // namespace

// ===========================================================================
// rdrobust y running [if], c(#) p(#) q(#) h(#) b(#) kernel() nnmatch(#) level(#)
//
// Oracle: p3_stata.log:153-175.
ojson runRdrobust(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdrobust requires exactly two variables — `rdrobust depvar runvar, c(#)`.",
        "syntax_error");
  }
  // Stata's rdrobust has a much wider option surface than cobra's CLI. The
  // ones cobra has no flag for are named explicitly as "not yet implemented"
  // rather than being silently dropped (a dropped bwselect() would answer a
  // DIFFERENT specification with rc 0).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdrobust.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdrobust.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdrobust", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"b", "--b"},
                                   {"kernel", "--kernel"},
                                   {"nnmatch", "--nnmatch"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const bool mass = (str(r, "masspoints") == "adjust");
  if (mass) {
    say("Mass points detected in the running variable.");
    say("");
  }
  const std::string design = str(r, "design", "sharp");
  say((design == "fuzzy" ? std::string("Fuzzy") : std::string("Sharp")) +
      " RD estimates using local polynomial regression.");
  say("");

  const auto& G = kRdGeom;
  // Header row + the right-hand info panel (10 blanks are folded into the
  // 12-space gap the oracle shows between the two halves: 12 blanks, key
  // padded to 14, "=", value right-aligned in 11 => column 80).
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(12, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(12, "BW type", 14, str(r, "bw_type"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(12, "Kernel", 14, str(r, "kernel"), 11));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(12, "VCE method", 14, str(r, "vce"), 11));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "BW bias (b)", f3(num(r, "b_left")), f3(num(r, "b_right"))));
  say(twoColRow(G, "rho (h/b)", f3(num(r, "rho")), f3(num(r, "rho"))));
  say(twoColRow(G, "Unique obs", std::to_string(inum(r, "n_unique_left")),
                std::to_string(inum(r, "n_unique_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  // The estimate table. 80 columns: label 18 + " |", an 8-wide estimate cell
  // padded out to 15, "|", then z (7) / p (13) / CI (12 + 12).
  const std::string rule80(80, '-');
  const std::string sep = std::string(19, '-') + "+" + std::string(60, '-');
  const std::string lvl = levelWord(num(r, "level", 95.0));
  say(rule80);
  say(padLeft("", 18) + " |" + padRight(" Point", 15) + "| Robust Inference");
  say(padLeft("", 18) + " |" + padRight(" Estimate", 15) + "|" + padLeft("z-stat", 7) +
      padLeft("P>|z|", 13) + padLeft("[" + lvl + "% Conf. Interval]", 24));
  say(sep);
  say(padLeft("RD Effect", 18) + " |" + padRight(padLeft(g9(num(r, "effect")), 8), 15) + "|" +
      padLeft(f4(num(r, "z")), 7) + padLeft(f3(num(r, "pvalue")), 13) +
      padLeft(g9(num(r, "ci_lo")), 12) + padLeft(g9(num(r, "ci_hi")), 12));
  say(rule80);
  if (mass) say("Estimates adjusted for mass points in the running variable.");

  // e15d_list_eb (w2g_ebmat.log:2302-2312): rdrobust.ado posts
  // `ereturn post b V, esample(touse)` with b = (tau_cl, tau_bc,
  // tau_bc) striped `y1:Conventional Bias-corrected Robust` and a
  // diagonal V whose first two cells are V_tau_cl ("Bias-corrected: SE =
  // sqrt(V_tau_cl)", :1015) and the last V_tau_rb. The engine's three rows are
  // exactly those (cmds_rd.cpp: se_bc = se_conventional), so they are posted
  // as e(b)/e(V) and as the coefficient table that answers _b[]/_se[].
  ojson e;
  eval::EvalMatrix eb, eV;
  eb.rownames = {"y1"};
  eb.data.emplace_back();
  e["coeftable"] = ojson::array();
  std::vector<double> ses;
  if (r.contains("rows") && r.at("rows").is_array()) {
    for (const auto& row : r.at("rows")) {
      const std::string name = str(row, "method");
      eb.colnames.push_back(name);
      eb.data[0].push_back(num(row, "coef"));
      ses.push_back(num(row, "se"));
      e["coeftable"].push_back(coefRow(name, num(row, "coef"), num(row, "se"), num(row, "z"),
                                       num(row, "p"), num(row, "ci_lo"), num(row, "ci_hi")));
    }
  }
  eV.rownames = eV.colnames = eb.colnames;
  eV.data.assign(ses.size(), std::vector<double>(ses.size(), 0.0));
  for (size_t i = 0; i < ses.size(); ++i) eV.data[i][i] = ses[i] * ses[i];
  e["estimator"] = "rdrobust";
  e["depvar"] = str(r, "y");
  e["n_obs"] = inum(r, "n_obs");
  e["tau_cl"] = num(r, "effect");
  e["se_tau_rb"] = num(r, "se");
  e["pv_rb"] = num(r, "pvalue");
  e["ci_l_rb"] = num(r, "ci_lo");
  e["ci_r_rb"] = num(r, "ci_hi");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  e["b_l"] = num(r, "b_left");
  e["b_r"] = num(r, "b_right");
  e["N_h_l"] = inum(r, "n_eff_left");
  e["N_h_r"] = inum(r, "n_eff_right");
  postEClass(session, "rdrobust", str(r, "y"), *session.currentHbin(), e);
  // After the fresh setLastModel (which clears the previous fit's block and
  // e(b)/e(V)), as every fit that has an e(b) does.
  if (!eb.colnames.empty()) {
    session.lastEstBlock = e;
    session.eval.eMatrices.emplace_back("b", std::move(eb));
    session.eval.eMatrices.emplace_back("V", std::move(eV));
  }

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rddensity runvar [if], c(#) p(#) q(#) h(#) kernel()
//
// Oracle: p3_stata.log:186-222. Note the manipulation test takes ONLY the
// running variable — there is no depvar.
ojson runRddensity(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 1) {
    return qeErrorBlocks(
        "syntax_error: rddensity takes exactly one variable (the running variable) — "
        "`rddensity runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRddensity.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRddensity.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  // rddensity.ado's own guards, run before any fit and leaving e() untouched
  // (:94-105): over the if/in sample with the running variable non-missing,
  // a cutoff not strictly inside (min, max) is r(125), and fewer than 10 rows
  // on either side (x < c, x >= c) is r(2001) (w2v_estpost_ematrix.log
  // ep15d_rddensity `2001` on the 20-row fixture with c(.5); the earlier
  // regress e(b) then survives, ep15d_b_shape).
  {
    double c = 0.0;
    bool cOk = true;
    for (const auto& [k, v] : parsed.opts) {
      if (k != "c" || v.isBool) continue;
      const std::string t = strutil::trim(v.strValue);
      char* end = nullptr;
      c = std::strtod(t.c_str(), &end);
      cOk = !t.empty() && end != nullptr && *end == '\0';
    }
    if (cOk) {
      try {
        const InRangeFilterResult fr = applyIfAndInFilter(session, *session.currentHbin(),
                                                          parsed.ifExpr, parsed.inRange);
        // No if/in: the live head (possibly a resident frame not on disk);
        // otherwise the filtered scratch copy applyIfAndInFilter wrote.
        std::unique_ptr<Datastore> owned;
        const Datastore* dsp = nullptr;
        if (fr.ok && fr.dataPath == *session.currentHbin()) {
          dsp = session.datastore();
        } else if (fr.ok) {
          owned = std::make_unique<Datastore>(fr.dataPath);
          dsp = owned.get();
        }
        if (dsp != nullptr) {
          const Datastore& ds = *dsp;
          const std::string& xn = parsed.varlist[0];
          if (ds.has(xn) && ds.col(xn).dtype == cecon::DType::F64 && ds.col(xn).f64() != nullptr) {
            const double* xv = ds.col(xn).f64();
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            long long nl = 0, nr = 0;
            for (int64_t i = 0; i < ds.nRows(); ++i) {
              if (std::isnan(xv[i])) continue;
              lo = std::min(lo, xv[i]);
              hi = std::max(hi, xv[i]);
              if (xv[i] < c) ++nl;
              else ++nr;
            }
            if (nl + nr > 0) {
              if (c <= lo || c >= hi)
                return ojson::array({eval::errorBlock(
                    "syntax_error", "c() should be set within the range of " + xn + ".",
                    std::nullopt, std::nullopt, std::nullopt, "r(125)")});
              if (nl < 10 || nr < 10)
                return ojson::array({eval::errorBlock(
                    "syntax_error", "Not enough observations to perform calculations.",
                    std::nullopt, std::nullopt, std::nullopt, "r(2001)")});
            }
          }
        }
      } catch (const std::exception&) {
        // An unreadable sample leaves the engine's own checks to answer.
      }
    }
  }

  std::vector<std::string> argv = {"rddensity", "--x", parsed.varlist[0]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  say("Computing data-driven bandwidth selectors.");
  say("");
  if (str(r, "masspoints") == "adjust") {
    say("Point estimates and standard errors have been adjusted for repeated observations.");
    say("(Use option nomasspoints to suppress this adjustment.)");
    say("");
  }
  say("RD Manipulation test using local polynomial density estimation.");
  say("");

  const auto& G = kRdGeom;
  // rddensity's cutoff label is `c = ` + a %.3f value in its own 10-wide cell,
  // not the trimmed spelling rdrobust uses.
  const std::string cutoffLabel = padLeft("c =", 8) + padLeft(f3(num(r, "cutoff")), 10);
  say(cutoffLabel + " |" + padLeft("Left of c", G.c1) + padLeft("Right of c", G.c2) +
      infoCell(10, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 13));
  say(twoColRule(G) + infoCell(10, "Model", 14, str(r, "model"), 13));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(10, "BW method", 14, str(r, "bw_method"), 13));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(10, "Kernel", 14, str(r, "kernel"), 13));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))) +
      infoCell(10, "VCE method", 14, str(r, "vce"), 13));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say("");
  say("Running variable: " + str(r, "x") + ".");

  const std::string rule42(42, '-');
  say(rule42);
  // Stata's own header spacing here is not column-aligned with the values
  // below it (the "T" sits three columns left of where the value ends); the
  // oracle is the spec, so the header is reproduced literally.
  say(padLeft("Method", 18) + " |" + "      T          P>|T|");
  say(twoColRule(G));
  say(twoColRow(G, "Robust", f4(num(r, "T")), f4(num(r, "pvalue"))));
  say(rule42);
  say("");

  say("P-values of binomial tests. (H0: prob = .5)");
  const std::string rule53(53, '-');
  say(rule53);
  say(padLeft("Window Length / 2", 18) + " |" + padLeft("<c", 9) + padLeft(">=c", 12) + " |" +
      padLeft("P>|T|", 10));
  say(std::string(19, '-') + "+" + std::string(22, '-') + "+" + std::string(10, '-'));
  if (r.contains("binomial") && r.at("binomial").is_array()) {
    for (const auto& row : r.at("binomial")) {
      say(padLeft(f3(num(row, "window")), 18) + " |" +
          padLeft(std::to_string(inum(row, "n_left")), 9) +
          padLeft(std::to_string(inum(row, "n_right")), 12) + " |" +
          padLeft(f4(num(row, "p")), 10));
    }
  }
  say(rule53);

  // e15d_eststo2/e15d_esttab2 (w2g_ebmat.log:2497-2513): rddensity is
  // eclass but never runs `ereturn post`: `ereturn clear`, then scalars and
  // the macros runningvar/kernel/bwmethod/vce (rddensity) -- no
  // e(cmd), e(depvar) or e(N). The e() stays live (`_b[x1]` and `matrix list
  // e(b)` are r(111), log:2486-2495), but it is not an estimation result:
  // `estimates store` exits 301 on the empty e(cmd),
  // and eststo's "." fill still reaches the hold step,
  // which refuses an e() that was never posted. So
  // the model is marked not storable (runEststo) and e(cmd)/e(depvar) are
  // erased (an empty depvar erases; setEString("") erases).
  ojson e;
  e["N_l"] = inum(r, "n_left");
  e["N_r"] = inum(r, "n_right");
  e["c"] = num(r, "cutoff");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  postEClass(session, "rddensity", "", *session.currentHbin(), e, /*ePosted=*/false);
  session.eval.setEString("cmd", "");

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rdplot y running [if], c(#) p(#) h(#) nbins(# #) kernel()
//
// Oracle: p3_stata.log:233-261 (note the ONE trailing blank line rdplot prints
// after its rule — every other command here prints none).
//
// SCOPE: table only. The JSON's `bins`/`fit`/`fit_coef` arrays are the plot,
// and drawing them is a sibling arc's GRAPH half; they ride out on the
// structured block untouched so that arc has them.
ojson runRdplot(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdplot requires exactly two variables — `rdplot depvar runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdplot.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdplot.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdplot", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  // Stata writes `nbins(# #)` space-separated; the engine wants `L,R`.
  if (auto nb = optStr(parsed, "nbins"); nb.has_value() && !nb->empty()) {
    argv.push_back("--nbins");
    argv.push_back(joinComma(splitWsOrComma(*nb)));
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  if (str(r, "masspoints") == "adjust") {
    say("Mass points detected in the running variable.");
    say("");
  }
  // Only the `esmv` phrasing is oracle-verified (p3_stata.log:235); the manual
  // form is the plain evenly-spaced wording rdplot uses when the bin count is
  // given rather than selected.
  const std::string binselect = str(r, "binselect", "esmv");
  std::string what = "evenly spaced mimicking variance number of bins";
  if (binselect == "manual" || binselect == "es") what = "evenly spaced number of bins";
  say("RD Plot with " + what + " using polynomial regression.");
  say("");

  const auto& G = kRdplotGeom;
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(8, "Number of obs", 15, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(8, "Kernel", 15, str(r, "kernel"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))));
  say(twoColRow(G, "Order poly. fit (p)", std::to_string(inum(r, "p_left")),
                std::to_string(inum(r, "p_right"))));
  say(twoColRow(G, "BW poly. fit (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "Number of bins scale", f3(num(r, "scale_left")), f3(num(r, "scale_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  const std::string rule45(45, '-');
  say(rule45);
  say(twoColRow(G, "", "Left of c", "Right of c"));
  say(twoColRule(G));
  say(twoColRow(G, "Bins selected", std::to_string(inum(r, "bins_left")),
                std::to_string(inum(r, "bins_right"))));
  say(twoColRow(G, "Average bin length", f3(num(r, "avg_bin_length_left")),
                f3(num(r, "avg_bin_length_right"))));
  say(twoColRow(G, "Median bin length", f3(num(r, "med_bin_length_left")),
                f3(num(r, "med_bin_length_right"))));
  say(twoColRule(G));
  say(twoColRow(G, "IMSE-optimal bins", std::to_string(inum(r, "bins_imse_left")),
                std::to_string(inum(r, "bins_imse_right"))));
  say(twoColRow(G, "Mimicking Var. bins", std::to_string(inum(r, "bins_mv_left")),
                std::to_string(inum(r, "bins_mv_right"))));
  say(twoColRule(G));
  // Stata prints this section label with a trailing space after the pipe.
  say(padLeft("Rel. to IMSE-optimal:", G.label) + " | ");
  say(twoColRow(G, "Implied scale", f3(num(r, "implied_scale_left")),
                f3(num(r, "implied_scale_right"))));
  say(twoColRow(G, "WIMSE var. weight", f3(num(r, "wimse_var_weight")),
                f3(num(r, "wimse_var_weight"))));
  say(twoColRow(G, "WIMSE bias weight", f3(num(r, "wimse_bias_weight")),
                f3(num(r, "wimse_bias_weight"))));
  say(rule45);
  say("");  // rdplot's own trailing blank line (oracle line 261)

  // rdplot is r-class in Stata, not e-class: lastModel is deliberately left
  // alone so a preceding regression's e() keeps answering, exactly as it would
  // in Stata.

  // Arc G: the picture. Four layers — one binned-scatter + one fitted-curve
  // per side of the cutoff — built from the SAME bins/fit arrays the table
  // above already read (no second engine call, no recomputation). The two
  // sides never share a layer, so there is no line connecting the last left
  // bin to the first right bin — that IS the "visually separated at the
  // cutoff" requirement, for free, just from drawing two independent
  // layers rather than one continuous one. layerIdx 0/1 also gives each
  // side its own palette color (scatter and its own fit curve share a
  // color), so the two sides read as two series even before the cutoff
  // line is looked at.
  std::vector<ojson> rdplotLayers = {
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "left"), 0),
      qeGraphLayer("line", qeSidePoints(r, "fit", "left"), 0),
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "right"), 1),
      qeGraphLayer("line", qeSidePoints(r, "fit", "right"), 1),
  };
  ojson rdplotGraph = qeGraphBlock(rdplotLayers, std::string(), str(r, "x"), str(r, "y"), session,
                                   num(r, "cutoff"));
  say.lines.push_back(registerQeGraph(std::move(rdplotGraph), session));

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
