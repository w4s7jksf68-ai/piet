// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table model context.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// Stored estimates: the bag, the e() scalars, and the staleness rule
// ===========================================================================

ojson emptyBag() {
  ojson b;
  b["scalars"] = ojson::object();
  b["locals"] = ojson::object();
  b["matrices"] = ojson::object();
  return b;
}


// Freeze the e() scalars a stored estimate should carry. `eScalarKey()` is the
// ONE table of Stata-spelling -> cobra-key (eval/macros.h); it was exposed at
// namespace scope for this call precisely so a third copy would not appear.
ojson buildEScalars(const ojson& est) {
  ojson out = ojson::object();
  if (!est.is_object()) return out;
  for (const auto& [stataName, engineKey] : eval::eScalarKey()) {
    if (out.contains(stataName)) continue;
    if (!est.contains(engineKey)) continue;
    const ojson& v = est.at(engineKey);
    if (!v.is_number()) continue;
    const double d = v.get<double>();
    if (std::isnan(d)) continue;
    // the engine's n_clusters 0 means "unclustered": Stata has no e(N_clust)
    // there, so the cell stays empty (BUG-1466; the e() resolver's own rule)
    if (stataName == "N_clust" && d == 0.0) continue;
    out[stataName] = d;
  }
  // `r2` is the one name whose snapshot spelling can differ from the block's:
  // makeSnapshot prefers r2_within when present, and a table asking for `r2`
  // must get the number the snapshot itself reports.
  return out;
}


// The snapshot's own `escalars`/`added` -- what `runEststo` attaches -- WIN.
// The fallback below is a backstop for an estimate that reached the store by
// some other route (a snapshot from before this bundle, or one built from
// LastModel's cached result), and it fires only when the shared fingerprint
// agrees. A mismatch discards rather than bleeds: attaching another model's
// additions is exactly the failure the fingerprint exists to stop.
Model materializeModel(SessionState& session, const std::string& name, const ojson& snap) {
  Model m;
  m.name = name;
  m.snap = snap;
  m.added = emptyBag();
  m.escalars = ojson::object();

  if (snap.contains("added") && snap["added"].is_object()) {
    m.added = snap["added"];
  } else if (!session.estAddedFp.empty() &&
             session.estAddedFp == SessionState::estAddFingerprint(snap)) {
    m.added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!m.added.contains(k) || !m.added[k].is_object()) m.added[k] = ojson::object();
  }

  if (snap.contains("escalars") && snap["escalars"].is_object()) {
    m.escalars = snap["escalars"];
  } else if (session.lastEstBlock.has_value() &&
             SessionState::estAddFingerprint(*session.lastEstBlock) ==
                 SessionState::estAddFingerprint(snap)) {
    m.escalars = buildEScalars(*session.lastEstBlock);
  }
  // N and r2 live on the snapshot itself, so they are available even when the
  // live block has moved on. They never OVERWRITE a frozen escalars entry.
  if (!m.escalars.contains("N") && snap.contains("n_obs") && snap["n_obs"].is_number()) {
    m.escalars["N"] = snap["n_obs"].get<double>();
  }
  if (!m.escalars.contains("r2") && snap.contains("r2") && snap["r2"].is_number()) {
    m.escalars["r2"] = snap["r2"].get<double>();
  }
  return m;
}


// Union of coefficient names in first-appearance order across models, then
// keep()/drop()/order() applied in Stata's own precedence: `order()` promotes,
// `keep()` restricts, `drop()` removes.
// The ONE coefficient-selection filter. There were two: `coefNames` for
// estout/outreg2 and a second copy inside `writeEsttabFile` for esttab, and the
// glob fix below reached only the first one -- `keep(*2*)` selected `x2` on
// estout and emptied the table on esttab in the same build. A command whose row
// selection is implemented twice is a command with two answers about what `*`
// means, which is the shadow-evaluator class this codebase keeps paying for.
void applyCoefSelection(std::vector<std::string>* names,
                        const std::optional<std::string>& keepOpt,
                        const std::optional<std::string>& dropOpt,
                        const std::optional<std::string>& orderOpt, std::string* errOut) {
  std::vector<std::string>& sel = *names;
  // An `eq:name` token matches the equation-qualified spelling as well as the
  // bare one, because this build's coeftable carries no equation for a
  // single-equation model and `keep(*:*eigh*)` still keeps `weight` there
  // (w1b_keepglob.log k7). So a token with a colon is tried against BOTH the
  // name and `:name`, which is what makes the corpus's `keep(3:0.MI*...)` reach
  // rows this engine names without an equation.
  // An interaction also matches in another atom order or with/without `c.`:
  // Stata's `1.treatment#c.age_BL` is the engine's `age_BL#1.treatment` (AER 007
  // Joint_test's keep(); cecon::postest::interaction_key, the rule test/lincom use).
  auto tokenHits = [](const std::string& tok, const std::string& name) {
    if (piet::strutil::globMatch(tok, name)) return true;
    if (tok.find_first_of("*?") == std::string::npos) {
      const std::string kt = cecon::postest::interaction_key(tok);
      if (!kt.empty() && kt == cecon::postest::interaction_key(name)) return true;
    }
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) return false;
    return piet::strutil::globMatch(tok.substr(colon + 1), name);
  };
  if (keepOpt.has_value()) {
    const std::vector<std::string> keep = splitWs(dequote(*keepOpt));
    for (const auto& k : keep) {
      bool any = false;
      for (const auto& n : sel) {
        if (tokenHits(k, n)) {
          any = true;
          break;
        }
      }
      if (!any && errOut != nullptr && errOut->empty()) {
        *errOut = "coefficient " + k + " not found";
      }
    }
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      for (const auto& k : keep) {
        if (tokenHits(k, n)) {
          filtered.push_back(n);
          break;
        }
      }
    }
    sel = filtered;
  }
  if (dropOpt.has_value()) {
    const std::vector<std::string> drop = splitWs(dequote(*dropOpt));
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      bool hit = false;
      for (const auto& d : drop) {
        if (tokenHits(d, n)) {
          hit = true;
          break;
        }
      }
      if (!hit) filtered.push_back(n);
    }
    sel = filtered;
  }
  if (orderOpt.has_value()) {
    const std::vector<std::string> order = splitWs(dequote(*orderOpt));
    std::vector<std::string> promoted;
    for (const auto& o : order) {
      for (const auto& n : sel) {
        if (!tokenHits(o, n)) continue;
        if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
      }
    }
    for (const auto& n : sel) {
      if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
    }
    sel = promoted;
  }
}


std::string labelFor(const std::string& name, const std::map<std::string, std::string>& explicitMap,
                     bool useVarLabels, SessionState& session) {
  auto it = explicitMap.find(name);
  if (it != explicitMap.end()) return it->second;
  if (useVarLabels) {
    const std::string lbl = session.eval.varLabels.get(name);
    if (!lbl.empty()) return lbl;
  }
  return name;
}


// BUG-486 removed this file's last caller of a `statLabel` auto-label map --
// estout labels a footer row with the key AS TYPED (`r2`, not `R-squared`;
// w1b_estout_defaults.log g3), and a pretty label comes from
// `stats(..., labels(...))`, which is the user's text and needs no map. The map
// is deleted rather than parked: a dead table of guesses is how a later reader
// concludes the guesses were measured. esttab's footer is built by its own
// writer and its labels were byte-verified by the 2026-09-09 capture.

// One footer statistic for one model: e() scalars first, then estadd's scalars,
// then estadd's locals (a TEXT row -- "Controls Yes"). Absent = blank cell, not
// zero: a missing statistic is not a statistic equal to nothing.
std::string statCell(const Model& m, const std::string& key, const std::string& fmt,
                     bool* isTextOut) {
  *isTextOut = false;
  // `ar2` is esttab's spelling of e(r2_a); both reach the same frozen scalar.
  std::vector<std::string> tryKeys{key};
  if (key == "ar2") tryKeys.push_back("r2_a");
  if (key == "r2_a") tryKeys.push_back("ar2");
  // W1a-5d: the pseudo-R2 arrives under TWO names depending on the estimator,
  // measured on this engine: `poisson` publishes `r2_p` (.0126126) while
  // `logit` and `probit` publish `r2_pseudo` (.0321488 / .0345996) and each
  // leaves the other missing; `nbreg` publishes neither. So a reader that
  // knows only one key answers `.` for two of the three families esttab's
  // `pr2` is actually used on -- and both live corpus uses are probit.
  if (key == "pr2" || key == "r2_p" || key == "r2_pseudo") {
    for (const char* alt : {"r2_p", "r2_pseudo"}) {
      if (std::find(tryKeys.begin(), tryKeys.end(), alt) == tryKeys.end()) tryKeys.push_back(alt);
    }
  }
  for (const auto& k : tryKeys) {
    if (m.escalars.contains(k) && m.escalars[k].is_number()) {
      // BUG-486: `N` is formatted by the CALLER's format like every other
      // statistic. estout renders `74` under %9.0g and `74.000` under the
      // fmt(3) its cells left running (w1b_estout_defaults.log g3 vs g4), so a
      // hard `fmtCount` here made one of those two impossible. esttab's writer
      // (the other caller) keeps its measured behaviour by passing `%9.0f` for
      // this key explicitly, which renders an integral count identically.
      return fmtNum(m.escalars[k].get<double>(), fmt);
    }
  }
  if (m.added["scalars"].contains(key)) {
    const ojson& v = m.added["scalars"][key];
    if (v.is_number()) return fmtNum(v.get<double>(), fmt);
  }
  if (m.added["locals"].contains(key)) {
    const ojson& v = m.added["locals"][key];
    *isTextOut = true;
    return v.is_string() ? v.get<std::string>() : std::string();
  }
  return "";
}




// The active e() as a Model, with the intercept renamed the way `makeSnapshot`
// does -- `_cons` is the spelling every table and every user writes.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut) {
  if (!session.lastEstBlock.has_value()) {
    if (session.lastModel().has_value() && session.lastModel()->result.has_value()) {
      // A fit that filled LastModel but not lastEstBlock (five TUs assign the
      // latter directly) still has its raw result cached.
      ojson snap = *session.lastModel()->result;
      out->name = cmd;
      out->snap = snap;
      out->added = emptyBag();
      out->escalars = buildEScalars(snap);
      return true;
    }
    *errOut = tblErrorRc(cmd + ": no estimation result in memory. Run a regression first.", 301);
    return false;
  }
  ojson snap = *session.lastEstBlock;
  if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
    for (auto& row : snap["coeftable"]) {
      if (row.contains("name") && row["name"].is_string() &&
          row["name"].get<std::string>() == "(intercept)") {
        row["name"] = "_cons";
      }
    }
  }
  out->name = cmd;
  out->snap = snap;
  out->added = emptyBag();
  if (session.estAddedFp == SessionState::estAddFingerprint(*session.lastEstBlock)) {
    out->added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!out->added.contains(k) || !out->added[k].is_object()) out->added[k] = ojson::object();
  }
  out->escalars = buildEScalars(snap);
  if (!out->escalars.contains("N") && snap.contains("n_obs")) {
    out->escalars["N"] = jsonNum(snap["n_obs"]);
  }
  if (snap.contains("coeftable")) out->snap["base_levels"] = factorBaseLevels(snap["coeftable"], session);
  return true;
}


// One place where a rendered table becomes a file. `append` reads the existing
// bytes FIRST and hands the concatenation to the atomic writer, so the
// destination is never opened for writing while it is still a source (the
// dest==source law, BUG-138).
ojson emitTable(const std::string& cmd, SessionState& session, const std::string& userPath,
                const std::string& bytes, bool replace, bool append) {
  const std::string resolved = resolveUserPath(session, userPath);
  ojson guard = guardOverwrite(cmd, resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  std::string payload = bytes;
  if (append && cecon::native_exists(resolved)) {
    bool ok = false;
    const std::string existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError(cmd + ": cannot read " + userPath + " to append to it", "data_io");
    payload = existing;
    if (!payload.empty() && payload.back() != '\n') payload += "\n";
    payload += bytes;
  }
  ojson werr = writeTextFileAtomic(cmd, resolved, userPath, payload);
  if (!werr.is_null()) return werr;
  // The USER's spelling, never the resolved absolute path: bundle D echoed the
  // resolved path and baked a build machine's workdir into a golden.
  return displayBlock("(output written to " + userPath + ")");
}
}
namespace piet::handlers {
using namespace table_detail;


// ===========================================================================
// esttab ... using FILE   (B4-W5)
//
// The dispatcher hands this the block `runEsttab` ALREADY built, so the file
// and the screen cannot disagree about a COEFFICIENT: every b/se/t/p cell below
// is read out of that block and nothing is re-estimated here. What the writer
// owns is esttab's own PRESENTATION contract -- which statistic sits in the
// parentheses, how many digits it carries, which rows exist and in what order,
// and the legend -- and that contract is now pinned to real StataBE output:
// tests/eval/golden/aer_b4/tables/t_*.txt|tex, 16 arms captured 2026-09-09 by
// pins_oracle_t.do / _t2.do against the installed esttab 2.1.4 + estout
// 3.33. Where this file and that capture disagree, the capture is right.
//
// The defaults the capture REFUTED (all four were live in this build, all four
// are silent -- the table renders, it is just not esttab's table):
//   * the parenthesised statistic is `t`, not the standard error
//     (t_plain.txt legend: "t statistics in parentheses"), esttab
//     `if "`tfmt'"=="" local tfmt 2` -> %11.2f, so (-0.57), not (86.156);
//   * the coefficient format is `a3`, estout's ADAPTIVE significant-digit
//     format (esttab `if "`bfmt'"=="" local bfmt a3`), not %9.3f:
//     -49.51 / 1.747 / 1946.1, three different decimal counts in one column;
//   * the default footer is N alone -- no R-squared row (t_plain.txt);
//   * `_cons` is the LAST coefficient row, not the first.
// The star ladder (* 0.05 ** 0.01 *** 0.001, esttab) was already right
// in `estoutStars()`; only the legend's ORDER was reversed against the capture.
//
// SCOPE NOTE, stated because silence is what this campaign removes: the JSON
// block `runEsttab` returns to the SCREEN still carries this build's older
// defaults (se in parentheses, %9.3f, an R-squared footer row, `_cons` first).
// Correcting it changes bytes pinned by internal capture esttab.json
// and coefplot.json, which are outside this task's write manifest. The FILE
// writer is the surface the 27 captured arms measure and the surface the AER
// corpus writes; the screen block's divergence is named in the W5 report as a
// re-pin request, not fixed here behind the maintainer's back.
// ===========================================================================

// ===========================================================================
// Factor base levels (W2AK BASELEV): Stata's e(b) after `i.var` carries the
// base level as a column (`0b.clinic`, b = 0 and var 0); this engine's
// coefficient table does not (EB-BASE). estout shows it by default as a `0` /
// `(.)` row before the first non-base level (estout `baselevels`, the
// `b.` stripped by estout_omitted_and_base). The base is the level of `var`
// present in e(sample) that has no coefficient -- read here from the data and
// the hidden e(sample) column while the fit is still the live one, so the
// stored snapshot carries it even after the data move on.
// ===========================================================================
ojson factorBaseLevels(const ojson& coeftable, SessionState& session) {
  ojson out = ojson::array();
  if (!coeftable.is_array() || !session.hasData()) return out;
  std::vector<std::string> vars;
  std::map<std::string, std::vector<std::pair<long long, std::string>>> levels;
  for (const auto& r : coeftable) {
    const std::string n = r.value("name", std::string());
    long long lv = 0;
    std::string var;
    if (!parseLevelName(n, &lv, &var)) continue;
    if (levels.count(var) == 0) vars.push_back(var);
    levels[var].emplace_back(lv, n);
  }
  if (vars.empty()) return out;
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return out;
    const double* mask = nullptr;
    double serial = 0.0;
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kColumn && ds->has(kEsampleColumn)) {
      mask = ds->col(kEsampleColumn).f64();
      serial = static_cast<double>(er.serial);
    }
    for (const auto& var : vars) {
      if (!ds->has(var) || ds->col(var).dtype == cecon::DType::Factor) continue;
      const cecon::Column& c = ds->col(var);
      const double* d = c.f64();
      const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
      std::set<long long> present;
      for (int64_t i = 0; i < n; ++i) {
        if (mask != nullptr && mask[i] != serial) continue;
        if (std::isnan(d[i]) || d[i] != std::floor(d[i])) continue;
        present.insert(static_cast<long long>(d[i]));
      }
      std::set<long long> coefLv;
      for (const auto& lv : levels[var]) coefLv.insert(lv.first);
      long long base = 0;
      bool found = false;
      for (long long v : present) {
        if (coefLv.count(v) == 0) {
          base = v;
          found = true;
          break;
        }
      }
      if (!found) continue;
      std::string before;
      long long bestAbove = 0;
      for (const auto& lv : levels[var]) {
        if (lv.first > base && (before.empty() || lv.first < bestAbove)) {
          before = lv.second;
          bestAbove = lv.first;
        }
      }
      ojson e;
      e["var"] = var;
      e["level"] = base;
      e["name"] = std::to_string(base) + "." + var;
      e["before"] = before;   // empty: after the last level
      e["after"] = levels[var].back().second;
      out.push_back(e);
    }
  } catch (const std::exception&) {
    return ojson::array();
  }
  return out;
}
}
