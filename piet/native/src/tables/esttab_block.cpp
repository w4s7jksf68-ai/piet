// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
#include "piet/handlers/estimates_tables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "results/stored_estimates_internal.h"

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
const piet::eval::OptVocab& kVocabEsttab = piet::eval::registerOptVocab(
    {"esttab"},
    // AER-SCOPE1 bundle E (TABLES, 2026-09-04): the pre-bundle set was the five
    // options this handler READS. That was honest while `esttab` could not write
    // a file, but the corpus spells `esttab m1 using t.tex, replace booktabs
    // label` and every one of those four answered "option not allowed". The
    // known set is now the scope table's, and the ones PIET does not honour are
    // listed as UNIMPLEMENTED so they say so rather than vanishing -- the
    // distinction matters because `rejectUnknownOptions` accepts an abbreviated
    // prefix of a known name, so a name missing from BOTH lists is the silent
    // wrong answer bundle A shipped (`ivreg ..., ro` ran without robust SEs).
    {{"nostar", "b", "se", "t", "star",
      "replace", "append", "booktabs", "label", "title", "mtitles", "nonumbers",
      "nomtitles", "stats", "scalars", "keep", "drop", "order", "r2", "ar2",
      "noobs", "nonotes", "addnotes", "fragment", "coeflabels", "csv",
      "compress", "nogaps", "plain",
      // B4-W5 (2026-09-09). Each of these is CONTENT -- it changes what a cell
      // comparator reads out of the written table -- and each is pinned to one
      // of the 16 captured arms in tests/eval/golden/aer_b4/tables/. They are
      // honoured by `writeEsttabFile` (table_esttab.cpp), which is the
      // `using` half of this command; see the scope note at the head of that
      // function for what the SCREEN block still does with them.
      "beta", "not", "ci", "p", "level", "starlevels", "substitute", "rename",
      "mlabels", "collabels", "nodepvars", "mgroups", "refcat", "tex",
      "prehead", "posthead", "prefoot", "postfoot",
      // LAYOUT-ONLY, accepted and NOT applied. They are here rather than in
      // `unimplemented` because refusing a corpus line over a column width
      // stops a paper that would otherwise reproduce; the writer says so once,
      // in the block it returns, so the acceptance is never silent. `nolegend`
      // is the one to watch: t_noleg.txt shows it also drops the STAR LEGEND,
      // i.e. it is content, not layout -- named as a W5 follow-up rather than
      // half-implemented here.
      "nolegend", "nolines", "page", "align", "width", "wrap", "modelwidth",
      "varwidth", "extracols",
      // W1a-3 (2026-09-17): the DISPLAY layer, every row measured against a
      // plain control in tests/eval/golden/aer_w1/w1b_esttab_display.log --
      // which is the point, because four of these turned out to change NOTHING
      // and a name is a bad reason to implement one.
      //   nocons/noconstant  drop the `_cons` rows. The two spellings are
      //                      byte-identical to each other (x6 vs the control).
      //   noomit/noomitted   drop a collinear coefficient's row (x13 vs x14).
      //   nobaselevels       a validated NO-OP here, and not because it was
      //                      too hard: this engine's coeftable does not carry
      //                      a factor's base level at all (`i.grp` gives
      //                      2.grp/3.grp and no 1.grp), so the `0.foreign 0 /
      //                      (.)` row Stata's default prints and this option
      //                      removes never exists to be removed. The absence
      //                      is the divergence and it is BUG-492's, not this
      //                      option's.
      //   type · obslast     MEASURED no-ops: byte-identical to the control,
      //                      `obslast` even with `s(N r2)` -- esttab's default
      //                      already puts N last. Implementing either on the
      //                      strength of its NAME would have been a divergence.
      //   style(...)         `fixed` is the default (byte-identical, x4);
      //                      `tex` reaches the same writer `tex`/`booktabs`
      //                      already do. Any other style is refused below.
      //   noabbrev           turns OFF the stub abbreviation (BUG-490).
      //   varlabels()        estout's spelling of `coeflabels()` -- same
      //                      name/label pairs, same map.
      "nocons", "noconstant", "noomit", "noomitted", "nobaselevels", "type",
      "obslast", "noabbrev", "style", "varlabels",
      // W1a-2 (2026-09-17, BUG-407): `cells` leaves the unimplemented list.
      // esttab has no cell renderer of its own -- esttab builds a
      // cells() spec out of main/aux when cells() is absent and hands it to
      // estout -- so `main`/`aux` join it as the same option wearing two other
      // names, and `writeEsttabFile` now reads all three through the one
      // grammar that already parses this syntax (estout's parseCells).
      // `wide` stays UNIMPLEMENTED on purpose: it is the quoted side-by-side
      // form (esttab), which this writer's coefficient-row-over-aux-row
      // model cannot render, and accepting it would be the exact
      // accept-and-ignore BUG-407 was filed against.
      "cells", "main", "aux",
      // W1a-5b (2026-09-18). `s` is estout's one-character minimum for
      // `Stats()` (estout `Stats(string asis)`), measured rc 0 in
      // w1b_stats_grammar.log s7 and written by 18 of the 32 obslast uses and
      // 13 of the 82 sfmt uses. It is added as an EXACT name: `resolveOption
      // AbbrevAcross` floors abbreviation at two characters, so a one-letter
      // key resolves only by exact membership and cannot start standing for
      // `se`/`star`/`style`.
      // `nolabel` cancels the label TEXT and not the WIDTH -- s12 keeps the
      // 20-column stub `label` set while printing `mpg`/`price` again.
      // `sfmt()` formats the `scalars()` rows, positionally, last-repeating --
      // and NOTHING else: twelve arms across three captures show it inert
      // against stats() keys, N, r2, the r2/ar2 flags, `plain` and `b()/se()`.
      // The identifying pair is w1b_sfmt_rule.log q5 vs q4.
      // W1a-5c. `gaps` is the DEFAULT and therefore a validated no-op (y5);
      // `alignment()` is the long spelling of `align()`, and both now reach the
      // tex column letter instead of being accepted and dropped.
      // W1a-5d: `pr2`/`pr2(#)` is the pseudo-R2 footer row. The engine
      // publishes that number under `r2_p` after poisson and `r2_pseudo` after
      // logit/probit, so the reader tries both (statCell).
      // W1a-5e: `labcol2(values, title(T))` -- one extra column between the
      // stub and the models, 456 corpus uses across 14 spellings.
      "s", "nolabel", "sfmt", "gaps", "alignment", "pr2", "labcol2",
      // `transform()` -- 6 corpus uses here and 12 on estout, and ONE
      // implementation shared with it (piet/handlers.h). `eform` stays
      // unimplemented beside it: it is `transform(exp(@) exp(@))` PLUS its own
      // removal of `_cons` (w1b_transform.log u2 has no intercept row where u1
      // does), so accepting it as an alias would drop that half in silence.
      "transform",
      // W1a-4. `indicate()` replaces the coefficients it names with one Yes/No
      // row (~47 corpus uses over 5 papers). `eqlabels()` and `unstack` are
      // MEASURED no-ops: on a single-equation table -- which is every table this
      // engine can produce, its coeftable carrying no equation at all -- all of
      // `eqlabels(none)`, `eqlabels("",none)`, `eqlabels("Estab." "HQ")` and
      // `unstack` are byte-identical to the control (w1b_indicate.log e1/e2/e3
      // and u1). Accepted as no-ops with the selftest pinning the SAMENESS, the
      // way W1a-3 handled `type`/`obslast`/`style(fixed)`; if the engine ever
      // carries equations these two have to be revisited, and that limit is the
      // reason it is written here.
      // `transpose` stays OUT: esttab answers r(198) "option transpose not
      // allowed" for it in Stata too (w1b_indicate.log tr_pose), so the six
      // corpus lines that spell it do not run there either and this build
      // refusing them is PARITY -- the `table, contents()` shape again.
      "indicate", "eqlabels", "unstack",
      // W2AK (2026-10-21): esttab is now derived from esttab.ado's
      // translation onto ONE estout writer (estout_format.cpp eoRender),
      // so the rest of its option list is honoured as the ado reads it --
      // modes (esttab), the switches of _more_syntax and the
      // estout pass-throughs of _estout_options.
      "tab", "scsv", "fixed", "z", "abs", "staraux", "constant", "aic", "bic", "wide",
      "brackets", "parentheses", "noparentheses", "depvars", "notes", "numbers", "lines",
      "noeqlines", "eqlines", "float", "nofloat", "note", "begin", "delimiter", "end",
      "incelldelimiter", "abbrev", "legend", "stardetach", "interaction", "hlinechar",
      "nolz", "lz", "noisily", "outfilenoteoff", "nooutfilenoteoff", "notype", "omitted",
      "baselevels", "longtable", "standalone", "fonttbl", "attributes", "onecell"},
     // UNIMPLEMENTED, and `cells`/`p` are here deliberately. This handler reads
     // nostar/b/se/t/star and nothing else from the cell layer, so listing
     // either as KNOWN would accept it and silently ignore it -- the class
     // bundle A shipped as `ivreg ..., ro` (accepted, ran without robust SEs,
     // said nothing). `estout` DOES implement cells(); esttab does not, and the
     // difference is per command, not per option name. Tightening this is a
     // named follow-up, not a silent one: BUG-407.
     // `p` moved OUT of this set in W5: `esttab, p` now selects the p-value as
     // the parenthesised statistic in the written table, the same way `se`/`t`
     // already did. `cells` stays -- runEsttab still does not read a cells()
     // spec, and accepting one would silently render the default layout under a
     // different request (BUG-407).
     {"rtf", "html", "html5", "md", "mmd", "smcl", "equations", "eform", "margin",
      "discrete", "dropped", "lastend", "dmarker", "msign", "stardrop", "starkeep",
      "topfile", "bottomfile"}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

// ===========================================================================
// esttab

namespace {

std::vector<std::pair<std::string, double>> parseStarOpt(const std::optional<std::string>& starStr) {
  static const std::vector<std::pair<std::string, double>> kDefault = {
      {"***", 0.01}, {"**", 0.05}, {"*", 0.10}};
  if (!starStr.has_value() || starStr->empty()) return kDefault;
  std::istringstream iss(*starStr);
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  std::vector<std::pair<std::string, double>> result;
  size_t i = 0;
  while (i + 1 < tokens.size()) {
    try {
      size_t pos;
      double threshold = std::stod(tokens[i + 1], &pos);
      if (pos != tokens[i + 1].size()) return kDefault;
      result.emplace_back(tokens[i], threshold);
      i += 2;
    } catch (...) {
      return kDefault;
    }
  }
  if (result.empty()) return kDefault;
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
  return result;
}

std::string starsFor(const ojson& p, const std::vector<std::pair<std::string, double>>& thresholds) {
  if (p.is_null()) return "";
  double pv;
  try {
    pv = p.get<double>();
  } catch (...) {
    return "";
  }
  if (std::isnan(pv)) return "";
  for (const auto& [label, cutoff] : thresholds) {
    if (pv < cutoff) return label;
  }
  return "";
}

// _fmt_b: Stata-style %fmt formatting (only %9.3f-shaped formats are
// exercised by the corpus; matches the oracle's regex-based %-?(\d+)?\.(\d+)
// ([fg]) extraction with a 3-decimal fallback).
std::string fmtB(const ojson& val, const std::string& fmt) {
  if (val.is_null()) return "";
  double v;
  try {
    v = val.get<double>();
  } catch (...) {
    return "";
  }
  if (std::isnan(v)) return "";
  static const std::regex re(R"(%-?(\d+)?\.(\d+)([fg]))");
  std::smatch m;
  int prec = 3;
  char kind = 'f';
  if (std::regex_search(fmt, m, re)) {
    prec = std::stoi(m[2].str());
    kind = m[3].str()[0];
  }
  char buf[64];
  if (kind == 'f') {
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
  }
  return std::string(buf);
}

}  // namespace

ojson runEsttab(const std::vector<std::string>& modelNamesIn,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                SessionState& session) {
  // W2 esttabnames (BUG-552): a written namelist is EXPANDED (namelist
  // semantics: `A_*`, `A_?`, `*`, `_all`, `.`), in store order, before any
  // lookup -- w2_smalls.log section G. Every name the expansion returns is
  // stored except the literal ".", which is the active results when they carry
  // no store name; that column is built from e() below.
  std::vector<std::string> modelNames;
  if (!modelNamesIn.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(modelNamesIn);
    if (ex.rc != 0) return estNamelistError(ex);
    modelNames = std::move(ex.names);
  } else {
    modelNames = session.estimatesNames();
  }

  // AER-SCOPE1 bundle E (TABLES, 2026-09-04): fall back to the ACTIVE estimate
  // when the caller named no models and the store is empty. `estpost summarize
  // x y` followed by a bare `esttab, cells(mean sd)` is estout's own documented
  // idiom and the shape the corpus writes -- estpost posts to e() and never
  // stores, so before this the bare esttab answered "no stored estimates" with
  // the estimate sitting right there in e(). Plain `regress y x` + `esttab`
  // reaches the same path.
  //
  // Deliberately does NOT call `session.eststo(...)`: a read must not have a
  // side effect, and silently minting an `m1` would change what a LATER
  // `eststo` numbers itself. The snapshot is built and carried locally instead.
  // Strict addition -- the only behaviour this replaces is an error.
  auto liveSnapshot = [&session]() {
    ojson snap = makeSnapshot(*session.lastEstBlock);
    snap["escalars"] = escalarsFrom(*session.lastEstBlock);
    const std::string fp = SessionState::estAddFingerprint(*session.lastEstBlock);
    snap["added"] = (!session.estAddedFp.empty() && session.estAddedFp == fp)
                        ? session.estAdded
                        : emptyAddedBag();
    return snap;
  };
  std::optional<ojson> adhocSnapshot;
  if (modelNames.empty() && session.lastEstBlock.has_value()) {
    std::string label = session.lastEstBlock->value("depvar", std::string());
    if (label.empty()) label = "est1";
    adhocSnapshot = liveSnapshot();
    modelNames.push_back(label);
  }

  if (modelNames.empty()) {
    // BUG-1248: nothing stored and no active estimates is estout's `.` not found,
    // r(301) (StataBE 19.5 `clear all` / `esttab`); it was rc 1.
    if (!session.eval.eLive())
      return ojson(eval::errorBlock("engine_error", "last estimates (.) not found", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(301)")));
    return engineErrorBlockEt(
        "esttab: no models specified and no stored estimates. "
        "Run a regression, then `eststo m1` first.");
  }

  std::vector<ojson> snapshots;
  if (adhocSnapshot.has_value()) {
    snapshots.push_back(*adhocSnapshot);
  } else {
    for (const auto& nm : modelNames) {
      if (nm == ".") {
        // The active results, unstored. A fit that filled only LastModel (no
        // e() display block) has no snapshot to render -- refused by name
        // rather than shown as an empty column.
        if (!session.lastEstBlock.has_value()) {
          return engineErrorBlockEt(
              "esttab: the active estimation results (.) carry no coefficient table this "
              "version of PIET can render; store them first (`estimates store NAME`)");
        }
        snapshots.push_back(liveSnapshot());
        continue;
      }
      const ojson* snap = session.estimatesGet(nm);
      if (snap == nullptr) {
        // Unreachable after the expansion above (it returns stored names or
        // "."), kept so a future caller cannot index a null snapshot.
        return estNamelistError({{}, 111, "estimation result " + nm + " not found"});
      }
      snapshots.push_back(*snap);
    }
  }

  // parse_esttab_opts.
  bool showSe = true;
  bool nostar = false;
  std::string bFmt = "%9.3f";
  std::optional<std::string> starOpt;
  for (const auto& [k, v] : opts) {
    if (k == "nostar" && v.isBool && v.boolValue) nostar = true;
    if (k == "b" && !v.isBool) bFmt = v.strValue;
    if (k == "se" && v.isBool && v.boolValue) showSe = true;
    if (k == "t" && v.isBool && v.boolValue) showSe = false;
    if (k == "star" && !v.isBool) starOpt = v.strValue;
  }
  // BS-097: nostar/b/se/t/star are the only esttab options this handler
  // reads (real Stata esttab also has title/mtitles/label/wide/ar2/scalars/
  // keep()/drop()/order()/compress/nogaps/plain/booktabs/replace/append/
  // using, not implemented here) — a leftover key now reads as an honest
  // reject instead of a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kEsttabKnownOpts = kVocabEsttab.known;
  if (auto err = eval::rejectUnknownOptions(opts, kEsttabKnownOpts)) {
    return *err;
  }
  auto thresholds = parseStarOpt(starOpt);

  // Union of coefficient names in appearance order across models.
  std::vector<std::string> unionNames;
  std::set<std::string> seen;
  for (const auto& snap : snapshots) {
    if (!snap.contains("coeftable")) continue;
    for (const auto& row : snap["coeftable"]) {
      std::string name = row.value("name", std::string());
      if (seen.insert(name).second) unionNames.push_back(name);
    }
  }

  // `transform()`: applied where the CELL is built, so esttab's screen block, its
  // csv and every file writer downstream see the same numbers. One semantics
  // with estout -- the function is shared (piet/handlers.h) rather than copied,
  // which is the BUG-496 lesson applied before the fact.
  TransformSpec trSpec;
  {
    std::string trRaw, trErr;
    for (const auto& [k, v] : opts) {
      if (k == "transform" && !v.isBool) trRaw = v.strValue;
    }
    if (!trRaw.empty() && !parseTransformOpt(trRaw, &trSpec, &trErr)) {
      // r(198): estout answers `invalid pattern in transform(,pattern()) or
      // eform()` there (w1b_transform2.log tr_pat_two), and `engine_error`
      // would make it rc 1.
      return engineErrorBlockEt("esttab: " + trErr, "syntax_error");
    }
  }
  ojson coefRows = ojson::array();
  for (const auto& coefName : unionNames) {
    ojson cells = ojson::array();
    for (size_t trMi = 0; trMi < snapshots.size(); ++trMi) {
      const ojson& snap = snapshots[trMi];
      ojson found;
      bool foundFlag = false;
      if (snap.contains("coeftable")) {
        for (const auto& row : snap["coeftable"]) {
          if (row.value("name", std::string()) == coefName) {
            found = row;
            foundFlag = true;
            break;
          }
        }
      }
      if (!foundFlag) {
        cells.push_back(nullptr);
        continue;
      }
      ojson b = found.contains("b") ? found["b"] : ojson(nullptr);
      ojson se = found.contains("se") ? found["se"] : ojson(nullptr);
      ojson t = found.contains("t") ? found["t"] : ojson(nullptr);
      ojson p = found.contains("p") ? found["p"] : ojson(nullptr);
      bool omitted = b.is_null();
      std::string starStr = (nostar || omitted) ? "" : starsFor(p, thresholds);
      ojson cell;
      cell["b"] = b;
      cell["se"] = se;
      cell["t"] = t;
      cell["p"] = p;
      cell["stars"] = starStr;
      cell["omitted"] = omitted;
      // The stars above come from the UNTRANSFORMED p, which is what the oracle
      // shows: `transform(@*2 2)` with stars on keeps `3.493**` (w1b_transform
      // .log s6), and t/p are in neither of the ado's two branches, so a linear
      // rescaling leaves the whole parenthesised row alone (s1/s2/s3).
      if (!trSpec.empty() && !omitted && b.is_number()) {
        const TransformCell tc =
            transformOneCell(trSpec, trMi, coefName, b.get<double>(), snap, session);
        if (tc.matched) {
          if (tc.bMissing) cell["b"] = ojson(nullptr);
          else if (tc.b.has_value()) cell["b"] = *tc.b;
          if (tc.seFactor.has_value() && se.is_number()) {
            if (std::isnan(*tc.seFactor)) cell["se"] = ojson(nullptr);
            else cell["se"] = se.get<double>() * *tc.seFactor;
          }
        }
      }
      cells.push_back(cell);
    }
    ojson cr;
    cr["name"] = coefName;
    cr["cells"] = cells;
    coefRows.push_back(cr);
  }

  // Footer rows: N and R2.
  ojson nVals = ojson::array(), r2Vals = ojson::array();
  for (const auto& snap : snapshots) {
    ojson n = snap.contains("n_obs") ? snap["n_obs"] : ojson(nullptr);
    ojson r2 = snap.contains("r2") ? snap["r2"] : ojson(nullptr);
    if (n.is_null()) {
      nVals.push_back(nullptr);
    } else {
      try {
        nVals.push_back(std::to_string(static_cast<long long>(n.get<double>())));
      } catch (...) {
        nVals.push_back(nullptr);
      }
    }
    if (r2.is_null()) {
      r2Vals.push_back(nullptr);
    } else {
      try {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", r2.get<double>());
        r2Vals.push_back(std::string(buf));
      } catch (...) {
        r2Vals.push_back(nullptr);
      }
    }
  }
  ojson footers = ojson::array();
  {
    ojson f1;
    f1["label"] = "N";
    f1["values"] = nVals;
    footers.push_back(f1);
    ojson f2;
    f2["label"] = "R²";
    f2["values"] = r2Vals;
    footers.push_back(f2);
  }

  // CSV export.
  std::string csv = ",";
  for (size_t i = 0; i < modelNames.size(); ++i) {
    if (i) csv += ",";
    csv += modelNames[i];
  }
  for (const auto& cr : coefRows) {
    std::string bLine = cr["name"].get<std::string>();
    std::string seLine;
    for (const auto& cell : cr["cells"]) {
      bLine += ",";
      seLine += ",";
      if (cell.is_null()) continue;
      if (cell.value("omitted", false)) {
        bLine += "(omitted)";
        continue;
      }
      std::string starStr = cell.value("stars", std::string());
      bLine += fmtB(cell["b"], bFmt) + starStr;
      ojson parenVal = showSe ? cell["se"] : cell["t"];
      if (!parenVal.is_null()) seLine += "(" + fmtB(parenVal, bFmt) + ")";
    }
    csv += "\n" + bLine + "\n" + seLine;
  }
  for (const auto& f : footers) {
    csv += "\n" + f["label"].get<std::string>();
    for (const auto& v : f["values"]) {
      csv += ",";
      if (!v.is_null()) csv += v.get<std::string>();
    }
  }

  ojson out;
  out["type"] = "estimates_table";
  ojson namesJ = ojson::array();
  for (const auto& n : modelNames) namesJ.push_back(n);
  out["names"] = namesJ;
  out["coef_rows"] = coefRows;
  out["footers"] = footers;
  out["csv"] = csv;
  ojson options;
  options["se"] = showSe;
  options["nostar"] = nostar;
  options["b_fmt"] = bFmt;
  out["options"] = options;
  return out;
}

}  // namespace piet::handlers
