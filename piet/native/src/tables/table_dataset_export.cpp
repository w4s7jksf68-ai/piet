// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table dataset export.
#include "tables_internal.h"
#include "estout_internal.h"
#include "outreg2_internal.h"
#include "piet/handlers/io.h"
#include "piet/user_paths.h"
#include "piet/dispatcher.h"
#include "piet/eval/macros.h"
namespace piet::handlers::table_detail {


// CSV escaping for the convert step. `cobra convert --delim ,` reads a plain
// RFC4180 file; a variable name or label containing a comma would otherwise
// shift every later column by one.
std::string csvCell(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}


// Build the .hbin. Returns the path, or an error block through `errOut`.
bool materializeDataset(SessionState& session, const std::string& cmd,
                        const std::vector<DsColumn>& cols, size_t nRows, std::string* pathOut,
                        ojson* errOut) {
  if (cols.empty() || nRows == 0) {
    *errOut = tblError(cmd + ": the estimation has no coefficients to save");
    return false;
  }
  // mintScratchPath always names a ".hbin"; the CSV beside it has to be
  // registered under its OWN name or the scratch census counts it as an orphan
  // (BUG-389 -- sweepOrphanScratch only removes paths it knows).
  const std::string hbin = mintScratchPath(session.workdir(), cmd + "_ds");
  std::string csv = mintScratchPath(session.workdir(), cmd + "_csv");
  forgetScratchMint(csv);
  csv = csv.substr(0, csv.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      *errOut = tblError(cmd + ": could not write the intermediate table", "data_io");
      return false;
    }
    for (size_t c = 0; c < cols.size(); ++c) {
      if (c) f << ",";
      f << csvCell(cols[c].name);
    }
    f << "\n";
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (size_t r = 0; r < nRows; ++r) {
      for (size_t c = 0; c < cols.size(); ++c) {
        if (c) f << ",";
        if (cols[c].isString) {
          f << csvCell(r < cols[c].str.size() ? cols[c].str[r] : std::string());
        } else {
          const double v = r < cols[c].num.size() ? cols[c].num[r]
                                                  : std::numeric_limits<double>::quiet_NaN();
          // An EMPTY field is how the loader spells a missing value; writing
          // "nan" would land a literal token in a numeric column and writing 0
          // would invent an estimate.
          if (!std::isnan(v)) f << v;
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      *errOut = tblError(cmd + ": failed writing the intermediate table (disk full?)", "data_io");
      return false;
    }
  }
  const CobraRunResult cr =
      runCobra({"convert", "--data", csv, "--out", hbin, "--delim", ","});
  if (!cr.ok || !cecon::native_exists(hbin)) {
    *errOut = tblError(cmd + ": could not build the result dataset");
    return false;
  }
  *pathOut = hbin;
  return true;
}


// Common tail: `using`/`saving()` writes a .dta and leaves the session alone;
// otherwise the built table BECOMES the dataset in memory.
ojson finishDataset(SessionState& session, const std::string& cmd, const std::string& hbin,
                    const std::optional<std::string>& outUser, bool replace, bool appendUsing,
                    const std::vector<std::string>& newVars) {
  if (outUser.has_value() && !outUser->empty()) {
    const std::string userPath = ensureExt(*outUser, ".dta");
    const std::string resolved = resolveUserPath(session, userPath);
    // `append` is a legitimate reason for the destination to already exist, so
    // it counts as permission here -- without this an `append` onto the file it
    // was asked to extend errors with "already exists".
    ojson guard = guardOverwrite(cmd, resolved, userPath, replace, appendUsing);
    if (!guard.is_null()) return guard;
    std::string src = hbin;
    if (appendUsing && cecon::native_exists(resolved)) {
      // `append` stacks onto the EXISTING saved table -- one row block per
      // model across a do-file, which is what regsave's append means.
      //
      // The existing file is a `.dta`, and `cobra append --using` opens its
      // argument with Dataset::open, which reads `.hbin` ONLY (src/io/
      // columnar.cpp:724 parses an hbin header). Handing it the .dta directly
      // fails; worse, a failure here after the export below would already have
      // replaced the file the append was supposed to extend. So it is imported
      // first, and every step is checked before anything touches the
      // destination.
      const std::string prior = mintScratchPath(session.workdir(), cmd + "_prior");
      const CobraRunResult imp =
          runCobra({"import-dta", "--data", resolved, "--out", prior});
      if (!imp.ok || !cecon::native_exists(prior)) {
        return tblError(cmd + ": cannot read " + userPath +
                        " to append to it (append needs a .dta this build can read)");
      }
      const std::string merged = mintScratchPath(session.workdir(), cmd + "_app");
      const CobraRunResult cr =
          runCobra({"append", "--data", prior, "--using", src, "--out", merged});
      if (!cr.ok || !cecon::native_exists(merged)) {
        return tblError(cmd + ": could not append to " + userPath);
      }
      src = merged;
    }
    // export-dta writes the destination directly, so it goes to a sibling temp
    // first and is renamed into place -- the same guarantee every other writer
    // in this file gives, and the reason a failed export cannot destroy the
    // file it was meant to replace.
    const std::string tmp = resolved + ".piet_tmp";
    const CobraRunResult cr = runCobra({"export-dta", "--data", src, "--out", tmp});
    if (!cr.ok || !cecon::native_exists(tmp)) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": could not write " + userPath +
                      " (this build needs a cobra engine with `export-dta')");
    }
    std::error_code mvEc;
    std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
    if (mvEc) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath, "data_io");
    }
    return displayBlock("file " + userPath + " saved");
  }
  // No destination: the table replaces the dataset in memory. `droppedVars` is
  // every column that was there before, because none of them survive.
  std::vector<std::string> dropped;
  if (session.datastore() != nullptr) dropped = session.datastore()->names();
  // W2AK TW-DS: through a .dta and the `use` path, so the columns carry
  // Stata storage types -- str# for the name column, byte for counts, float
  // for float-exact values -- exactly as the saved file does (w2at wt_p03 /
  // wt_r01 v1..v8: `: type parm` was r(101) on the CSV-built frame).
  {
    const std::string tmpDta = mintScratchPath(session.workdir(), cmd + "_mem");
    std::string dta = tmpDta.substr(0, tmpDta.size() - 5) + ".dta";
    forgetScratchMint(tmpDta);
    registerScratchMint(dta);
    const CobraRunResult cr = runCobra({"export-dta", "--data", hbin, "--out", dta});
    if (cr.ok && cecon::native_exists(dta)) {
      const ojson u = runUse(dta, session);
      if (!isErrorBlock(u)) return mutationResult(session, cmd, newVars, dropped);
    }
  }
  session.load(hbin);
  return mutationResult(session, cmd, newVars, dropped);
}


// The rows in e(b)'s order and with e(b)'s names (eval::stataEbColumns): `_cons`
// last, and a factor's base level as a row of its own, b 0 and se 0 with t and
// p missing and both limits 0 (w2at_table_writers.log:4726-4773 wt_p01 `x1 x2
// 0b.g 1.g 2.g _cons`, row 3 `0|0|.|.|0|0`; regsave wt_r01 row 3 `0b.g` 0 0 0 0).
std::vector<CoefRowView> coefViews(const ojson& snap) {
  std::vector<CoefRowView> out;
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return out;
  for (const auto& col : eval::stataEbColumns(snap)) {
    CoefRowView v;
    v.name = col.name;
    if (col.row == nullptr) {
      v.b = 0.0;
      v.se = 0.0;
      v.lo = 0.0;
      v.hi = 0.0;
      out.push_back(v);
      continue;
    }
    const ojson& r = *col.row;
    if (r.contains("b")) v.b = jsonNum(r["b"]);
    if (r.contains("se")) v.se = jsonNum(r["se"]);
    if (r.contains("t")) v.t = jsonNum(r["t"]);
    if (r.contains("p")) v.p = jsonNum(r["p"]);
    if (r.contains("ci_lo")) v.lo = jsonNum(r["ci_lo"]);
    if (r.contains("ci_hi")) v.hi = jsonNum(r["ci_hi"]);
    out.push_back(v);
  }
  // e(b)'s order: `_cons` last (w2at wt_p06 reads x1, x2, _cons).
  std::stable_partition(out.begin(), out.end(), [](const CoefRowView& v) { return v.name != "_cons"; });
  // The factor base levels e(b) carries as `0b.g` columns (b 0, var 0: t and p
  // missing, the limits 0; w2at wt_p01_r3*, wt_r01_r3*). The engine's table has
  // no such column (EB-BASE); activeModel read the base level from the live
  // data and e(sample) (factorBaseLevels), the same rows esttab shows.
  if (snap.contains("base_levels") && snap["base_levels"].is_array()) {
    for (const auto& e : snap["base_levels"]) {
      CoefRowView v;
      v.name = std::to_string(e.value("level", 0LL)) + "b." + e.value("var", std::string());
      // e(b) itself carries the base column when the fit recorded its stripe
      // (eval::stataEbColumns, eb_stripe): it is the single source then.
      if (std::any_of(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == v.name; }))
        continue;
      v.b = 0.0;
      v.se = 0.0;
      v.lo = 0.0;
      v.hi = 0.0;
      const std::string before = e.value("before", std::string());
      const std::string after = e.value("after", std::string());
      auto pos = out.end();
      if (!before.empty()) {
        pos = std::find_if(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == before; });
      } else if (!after.empty()) {
        pos = std::find_if(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == after; });
        if (pos != out.end()) ++pos;
      }
      out.insert(pos, v);
    }
  }
  return out;
}


// `level(#)` other than the level the model was FITTED at cannot be honoured
// without an inverse-t this build does not have. Refused by name; the fix the
// message names (refit with level(#)) produces the right interval.
std::optional<ojson> rejectForeignLevel(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& cmd) {
  const std::optional<std::string> lv = optStr(opts, "level");
  if (!lv.has_value()) return std::nullopt;
  const std::string s = strutil::trim(dequote(*lv));
  if (s == "95" || s == "95.0") return std::nullopt;
  return tblError(cmd + ": level(" + s +
                  ") is not implemented in this build; the confidence limits come from the fit, "
                  "so refit the model with level(" + s + ") instead");
}
}
namespace piet::handlers {
using namespace table_detail;


ojson runRegsave(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRegsave.known,
                                            kVocabRegsave.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  // regsave reads e(b): with none it is r(111) "matrix e(b) not found"
  // (w2at wt_r05, log 5348-5358).
  if (!activeModel(session, "regsave", &m, &err)) return tblErrorRc("matrix e(b) not found", 111);
  // level(#): the interval is coef -/+ t * stderr with t the (1 - level/100)/2
  // upper-tail critical value of t(e(df_r)), or of the normal when the fit has
  // no residual degrees of freedom; the range is regsave's own check.
  double cilevel = 95.0;
  bool foreignLevel = false;
  if (const auto lv = optStr(parsed.opts, "level")) {
    const std::string txt = strutil::trim(dequote(*lv));
    char* end = nullptr;
    const double v = std::strtod(txt.c_str(), &end);
    if (txt.empty() || end == nullptr || *end != '\0') {
      return tblErrorRc("invalid 'level' option: " + txt, 198);
    }
    if (v < 10 || v > 99.99) return tblErrorRc("level() must be between 10 and 99.99 inclusive", 198);
    cilevel = v;
    foreignLevel = v != 95.0;
  }

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  std::vector<CoefRowView> rows = coefViews(m.snap);
  std::vector<size_t> keptIdx(rows.size());
  for (size_t i = 0; i < keptIdx.size(); ++i) keptIdx[i] = i;
  // regsave [coeflist]: only the named coefficients are kept, in e(b)'s order.
  // Every name must be a column of e(b) (equation prefix aside); a wildcard or
  // range is first expanded against the variables in memory, and kept literally
  // when it expands to nothing, so it then fails the same check.
  if (!words.empty()) {
    std::vector<std::string> wanted;
    for (const auto& w : words) {
      if (w.find_first_of("*?~-") != std::string::npos && session.datastore() != nullptr) {
        auto vr = loops::expandVarlist(w, session);
        if (!vr.isError && !vr.tokens.empty()) {
          for (const auto& t : vr.tokens) wanted.push_back(t);
          continue;
        }
      }
      wanted.push_back(w);
    }
    bool eqnames = false;
    for (const auto& r : rows) {
      if (r.name.find(':') != std::string::npos) eqnames = true;
    }
    auto bare = [](const std::string& n) {
      const size_t c = n.find(':');
      return c == std::string::npos ? n : n.substr(c + 1);
    };
    std::vector<bool> keep(rows.size(), false);
    for (const auto& want : wanted) {
      bool match = false;
      for (const auto& r : rows) {
        if (bare(r.name) == want) match = true;
      }
      if (!match) return tblErrorRc("No estimation results available for variable " + want, 111);
      for (size_t i = 0; i < rows.size(); ++i) {
        if (eqnames ? rows[i].name.find(want) != std::string::npos : rows[i].name == want) keep[i] = true;
      }
    }
    std::vector<CoefRowView> kept;
    std::vector<size_t> keptNew;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (keep[i]) {
        kept.push_back(rows[i]);
        keptNew.push_back(i);
      }
    }
    rows = std::move(kept);
    keptIdx = std::move(keptNew);
  }
  // regsave builds its table with svmat, i.e. FLOAT columns: b, se and r2
  // are float-rounded, and the interval is float(float(b) -/+ t*float(se))
  // with t the fit's own critical value (w2at wt_r04; regsave.ado's svmat).
  // `double` keeps every table column in double precision instead of float.
  const bool dbl = hasFlag(parsed.opts, "double");
  auto fl = [dbl](double v) {
    return (dbl || std::isnan(v)) ? v : static_cast<double>(static_cast<float>(v));
  };
  // regsave: t_ci = invttail(e(df_r), .025) exactly (the normal
  // quantile without df); the fit's own (b - lo)/se differs in the last bits
  // and flips the float rounding of a limit (w2at wt_r01_r5c4).
  double tExact = kEoNaN;
  const double tailProb = (1.0 - cilevel / 100.0) / 2.0;
  if (m.escalars.contains("df_r") && m.escalars["df_r"].is_number()) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "invttail(%.17g, %.17g)", m.escalars["df_r"].get<double>(),
                  foreignLevel ? tailProb : 0.025);
    if (auto v = piet::evalScalarExpr(buf, session)) tExact = *v;
  } else if (foreignLevel) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "invnormal(%.17g)", 1.0 - tailProb);
    if (auto v = piet::evalScalarExpr(buf, session)) tExact = *v;
  }
  for (auto& r : rows) {
    // `gen t_ci = ...` without `double` is a FLOAT column too (regsave).
    const double tcrit = !std::isnan(tExact)
                             ? fl(tExact)
                             : ((!std::isnan(r.lo) && r.se > 0) ? (r.b - r.lo) / r.se : kEoNaN);
    // regsave: svmat writes e(V)'s diagonal into a FLOAT column
    // and then `replace stderr = sqrt(stderr)` -- the root of the float
    // variance, rounded to float again (w2at wt_r01_r6c3 `+1.3b8862X-002`).
    const double fb = fl(r.b), fse = std::isnan(r.se) ? r.se : fl(std::sqrt(fl(r.se * r.se)));
    r.b = fb;
    r.se = fse;
    if (!std::isnan(tcrit)) {
      r.lo = fl(fb - tcrit * fse);
      r.hi = fl(fb + tcrit * fse);
    }
    r.t = fl(r.t);
    r.p = fl(r.p);
  }

  std::vector<DsColumn> cols;
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  auto scalar = [&](const std::string& expr) {
    const auto v = piet::evalScalarExpr(expr, session);
    return v.has_value() ? *v : std::numeric_limits<double>::quiet_NaN();
  };
  {
    DsColumn v{"var", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  {
    DsColumn& c = addNum("coef");
    for (const auto& r : rows) c.num.push_back(r.b);
  }
  if (!hasFlag(parsed.opts, "nose")) {
    DsColumn& c = addNum("stderr");
    for (const auto& r : rows) c.num.push_back(r.se);
  }
  // covar(varlist): the covariance of every coefficient with each named one (a column of
  // e(V)), named covar_<name> with the dots taken out.
  if (auto cv = optStr(parsed.opts, "covar")) {
    const std::optional<eval::EvalMatrix> V = session.eval.resolveEMatrix("V");
    for (const auto& want : splitWs(dequote(*cv))) {
      int col = -1;
      if (V.has_value()) {
        for (size_t j = 0; j < V->colnames.size(); ++j) {
          const std::string& nm = V->colnames[j];
          const size_t colon = nm.find(':');
          const std::string bare = colon == std::string::npos ? nm : nm.substr(colon + 1);
          if (bare == want) col = static_cast<int>(j);
        }
      }
      if (col < 0) return tblErrorRc(want + " is not a valid regressor", 111);
      std::string nm;
      for (char ch : want) {
        if (ch != '.') nm += ch;
      }
      DsColumn& c = addNum("covar_" + nm);
      for (size_t k : keptIdx) {
        const double x = (k < V->data.size() && static_cast<size_t>(col) < V->data[k].size())
                             ? V->data[k][static_cast<size_t>(col)]
                             : std::numeric_limits<double>::quiet_NaN();
        c.num.push_back(fl(x));
      }
    }
  }
  if (hasFlag(parsed.opts, "tstat")) {
    DsColumn& c = addNum("tstat");
    for (const auto& r : rows) c.num.push_back(r.t);
  }
  if (hasFlag(parsed.opts, "pval")) {
    DsColumn& c = addNum("pval");
    for (const auto& r : rows) c.num.push_back(r.p);
  }
  if (hasFlag(parsed.opts, "ci")) {
    DsColumn& lo = addNum("ci_lower");
    for (const auto& r : rows) lo.num.push_back(r.lo);
    DsColumn& hi = addNum("ci_upper");
    for (const auto& r : rows) hi.num.push_back(r.hi);
  }
  {
    DsColumn& c = addNum("N");
    const double n = m.escalars.contains("N") ? m.escalars["N"].get<double>()
                                              : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(n);
  }
  // r2 only when the estimates carry e(r2): after logit (no e(r2)) the saved
  // table is var coef stderr ... N (vs-stata bank 7b260327, regsave, pval ci: 7
  // variables; after regress, 8 with r2).
  if (m.escalars.contains("r2") && m.escalars["r2"].is_number()) {
    DsColumn& c = addNum("r2");
    const double r2 = m.escalars["r2"].get<double>();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(fl(r2));
  }
  if (hasFlag(parsed.opts, "cmdline")) {
    DsColumn c{"cmdline", true, {}, {}};
    const std::string cl = m.snap.value("estimator", std::string()) + " " +
                           m.snap.value("depvar", std::string());
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(strutil::trim(cl));
    cols.push_back(c);
  }
  // detail(scalars|macros|all): every e() scalar / macro as a column of its own, in e()'s
  // order, skipping a name the table already has (`cap gen` fails there).
  if (auto dt = optStr(parsed.opts, "detail")) {
    const std::string which = strutil::lower(strutil::trim(dequote(*dt)));
    if (which != "all" && which != "scalars" && which != "macros") {
      return tblErrorRc("The only valid choices for the detail option are \"all\", \"scalars\", or \"macros\"", 198);
    }
    auto taken = [&](const std::string& n) {
      for (const auto& c : cols) {
        if (c.name == n) return true;
      }
      return false;
    };
    if (which == "all" || which == "scalars") {
      const auto names = session.eNameOrderFor("scalars");
      if (!names.has_value()) {
        return ojson(eval::unsupportedOption(
            "detail", "regsave: the list of e() scalars after this estimator is not available in "
                      "this version of PIET"));
      }
      for (const auto& n : *names) {
        if (taken(n)) continue;
        DsColumn& c = addNum(n);
        // An integer in -32767..32740 is stored as it is (an int column); any other scalar goes
        // into a column of the session's default type -- float, so rounded to float -- unless
        // the `double' option asked for full precision.
        double v = scalar("e(" + n + ")");
        const bool smallInt = !std::isnan(v) && v == std::floor(v) && v >= -32767.0 && v <= 32740.0;
        if (!smallInt) v = fl(v);
        for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(v);
      }
    }
    if (which == "all" || which == "macros") {
      const auto names = session.eNameOrderFor("macros");
      if (!names.has_value()) {
        return ojson(eval::unsupportedOption(
            "detail", "regsave: the list of e() macros after this estimator is not available in "
                      "this version of PIET"));
      }
      for (const auto& n : *names) {
        if (taken(n)) continue;
        DsColumn c{n, true, {}, {}};
        const auto ex = eval::expandMacros("`e(" + n + ")'", session.eval);
        for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(ex.isError ? std::string() : ex.line);
        cols.push_back(c);
      }
    }
  }
  // autoid: a regression-number column _id (1; with `append` the ado renumbers
  // it against the rows already in the file, which this build does not do).
  if (hasFlag(parsed.opts, "autoid")) {
    if (hasFlag(parsed.opts, "append")) {
      return tblError("regsave: autoid together with append is not implemented in this build");
    }
    DsColumn& c = addNum("_id");
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(1.0);
  }
  if (auto al = optStr(parsed.opts, "addlabel")) {
    for (const auto& [name, val] : parseCommaPairs(*al)) {
      DsColumn c{name, true, {}, {}};
      for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(val);
      cols.push_back(c);
    }
  }

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "regsave", cols, rows.size(), &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  ojson done = finishDataset(session, "regsave", hbin, usingPath, hasFlag(parsed.opts, "replace"),
                             hasFlag(parsed.opts, "append"), newVars);
  if (!isErrorBlock(done)) {
    // `return clear` then r(N) = observations and r(k) = variables of the table.
    // With `append` the table is the one just saved: the rows already in the file
    // and the new ones, over the union of their variables.
    double nOut = static_cast<double>(rows.size()), kOut = static_cast<double>(cols.size());
    if (hasFlag(parsed.opts, "append") && usingPath.has_value()) {
      const std::string saved = resolveUserPath(session, ensureExt(*usingPath, ".dta"));
      const std::string back = mintScratchPath(session.workdir(), "regsave_back");
      const CobraRunResult imp = runCobra({"import-dta", "--data", saved, "--out", back});
      if (imp.ok && cecon::native_exists(back)) {
        try {
          Datastore ds(back);
          nOut = static_cast<double>(ds.nRows());
          kOut = 0;
          for (const auto& n : ds.names()) {
            if (!isHiddenColumn(n)) kOut += 1;
          }
        } catch (const std::exception&) {
        }
      }
    }
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", nOut);
    session.eval.rValues.set("k", kOut);
  }
  return done;
}


ojson runParmest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabParmest.known,
                                            kVocabParmest.unimplemented)) {
    return ojson(*err);
  }
  const auto& opts = parsed.opts;
  const bool norestore = hasFlag(opts, "norestore") || hasFlag(opts, "fast");
  const std::optional<std::string> listOpt = optStr(opts, "list");
  const std::optional<std::string> savingOpt = optStr(opts, "saving");
  // parmest needs somewhere to put the table: a listing, a file, or the memory.
  if (!listOpt.has_value() && !savingOpt.has_value() && !norestore) {
    return tblErrorRc(
        "You must specify at least one of the five options:\n"
        "list(), frame(), saving(), norestore, and fast.\n"
        "If you specify list(), then the output variables specified are listed.\n"
        "If you specify saving(), then the new data set is output to a disk file.\n"
        "If you specify norestore and/or fast, then the new data set is created in the current "
        "data frame,\nand any existing data set in the current data frame is destroyed.",
        498);
  }
  // bmatrix()/vmatrix() name the matrices the table is read from; only the defaults,
  // e(b) and e(V), are read here, and any other matrix is refused by name.
  std::string bmatrixTxt = "e(b)", vmatrixTxt = "e(V)";
  if (auto bm = optStr(opts, "bmatrix")) {
    bmatrixTxt = strutil::trim(dequote(*bm));
    if (bmatrixTxt != "e(b)") {
      return ojson(eval::unsupportedOption("bmatrix", "parmest: bmatrix(" + bmatrixTxt +
                                           ") is not implemented in this version of PIET; only e(b) is read"));
    }
  }
  if (auto vm = optStr(opts, "vmatrix")) {
    vmatrixTxt = strutil::trim(dequote(*vm));
    if (vmatrixTxt != "e(V)") {
      return ojson(eval::unsupportedOption("vmatrix", "parmest: vmatrix(" + vmatrixTxt +
                                           ") is not implemented in this version of PIET; only e(V) is read"));
    }
  }
  Model m;
  ojson err;
  if (!activeModel(session, "parmest", &m, &err)) return err;
  {
    const std::string ecmd = session.eval.eStrings.get("cmd");
    if (ecmd == "exlogistic" || ecmd == "expoisson") {
      return tblErrorRc("parmest and parmby cannot work after exlogistic or expoisson", 498);
    }
  }

  // level(numlist): every level gets its own min#/max# pair, named by the level
  // as typed (a `.` becomes `_`), in ascending order.
  std::vector<std::string> levelTexts;
  std::vector<double> levels;
  {
    std::vector<std::string> toks;
    if (auto lv = optStr(opts, "level")) {
      std::string t = dequote(*lv);
      for (char& c : t) {
        if (c == ',') c = ' ';
      }
      toks = splitWs(t);
    } else {
      toks = {"95"};
    }
    std::vector<std::pair<double, std::string>> ordered;
    for (const auto& t : toks) {
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (t.empty() || *end != '\0' || v < 0 || v >= 100) {
        return tblErrorRc("invalid level(" + t + ")", 125);
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.15g", v);
      ordered.emplace_back(v, buf);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [v, txt] : ordered) {
      levels.push_back(v);
      std::string name = txt;
      for (char& c : name) {
        if (c == '.') c = '_';
      }
      levelTexts.push_back(name);
    }
  }

  // `saving("f.dta", replace)` -- the replace lives INSIDE the parentheses,
  // which is parmest's own syntax and not an outer option.
  std::optional<std::string> savePath;
  bool saveReplace = false;
  if (savingOpt.has_value()) {
    const std::vector<std::string> parts = splitTopLevel(*savingOpt, ',');
    if (!parts.empty()) savePath = dequote(strutil::trim(parts[0]));
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string w = strutil::lower(strutil::trim(parts[i]));
      if (w == "replace" || w == "repl") saveReplace = true;
    }
  }
  // parmest checks saving() before it builds anything: an existing file
  // without replace is r(498) with two lines (w2at wt_p07, log 5130-5138).
  if (savePath.has_value() && !saveReplace) {
    const std::string up = ensureExt(*savePath, ".dta");
    if (cecon::native_exists(resolveUserPath(session, up))) {
      return tblErrorRc("file " + up + " already exists\nsaving(" + *savingOpt + ") invalid", 498);
    }
  }

  // The parameters, with the equation each e(b) column sits in: the e(b) stripe
  // names it (`foreign:mpg` after logit, bare after regress).
  const std::vector<CoefRowView> rows = coefViews(m.snap);
  std::vector<std::string> eqs(rows.size()), parms(rows.size());
  {
    std::vector<std::string> stripe;
    if (const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b")) stripe = eb->colnames;
    const bool useStripe = stripe.size() == rows.size();
    for (size_t i = 0; i < rows.size(); ++i) {
      const std::string& nm = useStripe ? stripe[i] : rows[i].name;
      const size_t colon = nm.find(':');
      eqs[i] = colon == std::string::npos ? std::string("_") : nm.substr(0, colon);
      parms[i] = colon == std::string::npos ? nm : nm.substr(colon + 1);
    }
  }
  bool anyEq = false;
  for (const auto& e : eqs) {
    if (e != "_") anyEq = true;
  }
  const bool eform = hasFlag(opts, "eform");
  const double nullvalue = eform ? 1.0 : 0.0;
  const double pnull = eform ? std::log(nullvalue) : nullvalue;
  const bool dofpres = m.escalars.contains("df_r") && m.escalars["df_r"].is_number();
  const double dfr = dofpres ? m.escalars["df_r"].get<double>() : std::numeric_limits<double>::quiet_NaN();
  auto scalar = [&](const std::string& expr) {
    const auto v = piet::evalScalarExpr(expr, session);
    return v.has_value() ? *v : std::numeric_limits<double>::quiet_NaN();
  };
  auto fp = [](const char* f, double a, double b) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, a, b);
    return std::string(buf);
  };

  std::vector<DsColumn> cols;
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  if (auto idn = optStr(opts, "idnum")) {
    DsColumn& c = addNum("idnum");
    double v = std::numeric_limits<double>::quiet_NaN();
    const std::string t = strutil::trim(dequote(*idn));
    char* end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    if (!t.empty() && *end == '\0') v = x;
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(v);
  }
  if (auto ids = optStr(opts, "idstr")) {
    DsColumn c{"idstr", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(dequote(*ids));
    cols.push_back(c);
  }
  // idnum and idstr lead the table, idnum first (parmest `order idstr`, `order idnum`).
  if (anyEq) {
    DsColumn c{"eq", true, {}, {}};
    c.str = eqs;
    cols.push_back(c);
  }
  {
    DsColumn c{"parm", true, {}, {}};
    c.str = parms;
    cols.push_back(c);
  }
  if (hasFlag(opts, "label")) {
    session.syncEvalVarLabels();
    DsColumn c{"label", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) {
      std::string lab;
      const std::string& p = parms[i];
      if (p == "_cons") {
        lab = "Constant";
      } else if (p.find('#') == std::string::npos) {
        // `2.rep78`, `1b.foreign`, `o.x`: the label of the variable behind it.
        std::string v = p;
        const size_t dot = v.find('.');
        if (dot != std::string::npos) v = v.substr(dot + 1);
        lab = session.eval.varLabels.get(v);
      }
      c.str.push_back(lab);
    }
    cols.push_back(c);
  }
  std::vector<double> est(rows.size()), se(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    est[i] = rows[i].b;
    se[i] = rows[i].se;
  }
  // t/z, p and the limits are computed on the estimate's own scale first; eform
  // exponentiates the estimate and the limits afterwards, and rescales the
  // standard error (parmest, parmcip).
  std::vector<double> stat(rows.size()), pval(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const double z = (se[i] == 0.0 || std::isnan(se[i])) ? std::numeric_limits<double>::quiet_NaN()
                                                          : (est[i] - pnull) / se[i];
    stat[i] = z;
    if (std::isnan(z)) {
      pval[i] = std::numeric_limits<double>::quiet_NaN();
    } else if (dofpres) {
      pval[i] = scalar(fp("2*ttail(%.17g, abs(%.17g))", dfr, z));
    } else {
      pval[i] = scalar(fp("2*normal(-abs(%.17g))", z, 0));
    }
  }
  std::vector<std::vector<double>> lo(levels.size(), std::vector<double>(rows.size())),
      hi(levels.size(), std::vector<double>(rows.size()));
  for (size_t k = 0; k < levels.size(); ++k) {
    const double alpha = (100.0 - levels[k]) / 100.0;
    double crit = dofpres ? scalar(fp("invttail(%.17g, %.17g)", dfr, alpha / 2))
                          : scalar(fp("invnormal(1-%.17g)", alpha / 2, 0));
    for (size_t i = 0; i < rows.size(); ++i) {
      const double hw = se[i] * crit;
      lo[k][i] = est[i] - hw;
      hi[k][i] = est[i] + hw;
    }
  }
  if (eform) {
    for (size_t i = 0; i < rows.size(); ++i) {
      est[i] = std::exp(est[i]);
      se[i] = se[i] * est[i];
      for (size_t k = 0; k < levels.size(); ++k) {
        lo[k][i] = std::exp(lo[k][i]);
        hi[k][i] = std::exp(hi[k][i]);
      }
    }
  }
  { DsColumn& c = addNum("estimate"); c.num = est; }
  { DsColumn& c = addNum("stderr");   c.num = se; }
  if (dofpres) {
    DsColumn& c = addNum("dof");
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(dfr);
  }
  if (hasFlag(opts, "omit")) {
    // `omit` ADDS a byte `omit` after dof, 1 on base and omitted parameters
    // (w2at wt_p02_v5); it removes nothing.
    DsColumn& c = addNum("omit");
    for (const auto& r : rows) c.num.push_back((r.b == 0.0 && (r.se == 0.0 || std::isnan(r.se))) ? 1.0 : 0.0);
  }
  { DsColumn& c = addNum(dofpres ? "t" : "z"); c.num = stat; }
  { DsColumn& c = addNum("p"); c.num = pval; }
  if (auto st = optStr(opts, "stars")) {
    // stars(numlist): one `*` for every threshold the p-value is at or below.
    std::string t = dequote(*st);
    for (char& c : t) {
      if (c == ',') c = ' ';
    }
    std::vector<double> thr;
    for (const auto& w : splitWs(t)) {
      char* end = nullptr;
      const double v = std::strtod(w.c_str(), &end);
      if (w.empty() || *end != '\0') return tblErrorRc("invalid stars(" + dequote(*st) + ")", 121);
      thr.push_back(v);
    }
    // stars(numlist descending >=0 <=1): thresholds outside [0,1] are r(125); a list that is not
    // strictly descending (repeats included) is r(124).
    for (double a : thr) {
      if (a < 0.0 || a > 1.0) {
        return tblErrorRc("stars(" + dequote(*st) + ") invalid -- invalid numlist has elements outside allowed range", 125);
      }
    }
    for (size_t k = 1; k < thr.size(); ++k) {
      if (!(thr[k] < thr[k - 1])) {
        return tblErrorRc("stars(" + dequote(*st) + ") invalid -- invalid numlist has elements out of order", 124);
      }
    }
    DsColumn c{"stars", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) {
      std::string stars;
      if (!std::isnan(pval[i])) {
        for (double a : thr) {
          if (pval[i] <= a) stars += "*";
        }
      }
      c.str.push_back(stars);
    }
    cols.push_back(c);
  }
  for (size_t k = 0; k < levels.size(); ++k) {
    { DsColumn& c = addNum("min" + levelTexts[k]); c.num = lo[k]; }
    { DsColumn& c = addNum("max" + levelTexts[k]); c.num = hi[k]; }
  }
  if (auto es = optStr(opts, "escal")) {
    size_t i = 0;
    for (const auto& name : splitWs(dequote(*es))) {
      DsColumn& c = addNum("es_" + std::to_string(++i));
      const double v = scalar("e(" + name + ")");
      for (size_t r = 0; r < rows.size(); ++r) c.num.push_back(v);
    }
  }
  if (auto em = optStr(opts, "emac")) {
    size_t i = 0;
    for (const auto& name : splitWs(dequote(*em))) {
      DsColumn c{"em_" + std::to_string(++i), true, {}, {}};
      const auto ex = eval::expandMacros("`e(" + name + ")'", session.eval);
      for (size_t r = 0; r < rows.size(); ++r) c.str.push_back(ex.isError ? std::string() : ex.line);
      cols.push_back(c);
    }
  }
  // rename(old new ...): a name that is not a column, or a new name already in
  // use, is a warning and the pair is skipped.
  std::vector<std::string> notes;
  if (auto rn = optStr(opts, "rename")) {
    std::vector<std::string> t = splitWs(dequote(*rn));
    if (t.size() % 2) {
      notes.push_back("Warning: odd number of variable names in rename list - last one ignored");
      t.pop_back();
    }
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
      const std::string &from = t[i], &to = t[i + 1];
      auto find = [&](const std::string& n) {
        for (size_t c = 0; c < cols.size(); ++c) {
          if (cols[c].name == n) return static_cast<int>(c);
        }
        return -1;
      };
      const int at = find(from);
      if (at < 0 || find(to) >= 0 || !strutil::isValidStataIdentifier(to)) {
        notes.push_back("Warning: it is not possible to rename " + from + " to " + to);
      } else {
        cols[static_cast<size_t>(at)].name = to;
      }
    }
  }
  const size_t nRows = rows.size();

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "parmest", cols, nRows, &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);

  ojson out = ojson::array();
  for (const auto& n : notes) out.push_back(displayBlock(n));
  auto push = [&](const ojson& b) {
    if (b.is_array()) {
      for (const auto& x : b) out.push_back(x);
    } else if (!b.is_null()) {
      out.push_back(b);
    }
  };
  // format(varlist %fmt ...): display formats of the new variables, applied in
  // the table itself so the saved file and the listing carry them.
  const std::optional<std::string> fmtOpt = optStr(opts, "format");
  const bool inMemory = fmtOpt.has_value() || listOpt.has_value();
  auto setR = [&]() {
    eval::replaceRClass(session.eval);
    session.eval.rStrings.set("dfmatrix", "");
    session.eval.rStrings.set("vmatrix", vmatrixTxt);
    session.eval.rStrings.set("bmatrix", bmatrixTxt);
    if (eform) session.eval.rStrings.set("eform", "eform");
    session.eval.rStrings.set("level", strutil::trim(levelTexts.empty() ? std::string("95") : [&] {
      std::string t = levelTexts[0];
      for (char& c : t) {
        if (c == '_') c = '.';
      }
      return t;
    }()));
    session.eval.rValues.set("nullvalue", nullvalue);
    session.eval.rValues.set("nparm", static_cast<double>(nRows));
    session.eval.rValues.set("dof", dofpres ? dfr : 0.0);
    session.eval.rValues.set("dofpres", dofpres ? 1.0 : 0.0);
  };
  if (!inMemory) {
    // Straight to the file and/or the memory.
    if (savePath.has_value()) {
      ojson saved = finishDataset(session, "parmest", hbin, savePath, saveReplace, false, newVars);
      if (isErrorBlock(saved)) return saved;
      push(saved);
      if (norestore) {
        push(finishDataset(session, "parmest", hbin, std::nullopt, false, false, newVars));
      }
    } else {
      push(finishDataset(session, "parmest", hbin, std::nullopt, false, false, newVars));
    }
  } else {
    // The table goes into the memory for the formats and the listing; unless
    // norestore it is a loan, and the data set that was there comes back.
    const bool hadData = session.hasData();
    if (!norestore && hadData) push(piet::dispatch("preserve", session));
    ojson loaded = finishDataset(session, "parmest", hbin, std::nullopt, false, false, newVars);
    if (isErrorBlock(loaded)) return loaded;
    if (norestore) push(loaded);
    if (fmtOpt.has_value()) {
      std::string vl;
      for (const auto& x : splitWs(dequote(*fmtOpt))) {
        if (x.empty()) continue;
        if (x[0] != '%') {
          vl += " " + x;
        } else {
          ojson fr = piet::dispatch("format" + vl + " " + x, session);
          vl.clear();
          if (!fr.is_null()) push(fr);
        }
      }
    }
    if (listOpt.has_value()) push(piet::dispatch("list " + dequote(*listOpt), session));
    if (savePath.has_value()) {
      std::string line = "save \"" + *savePath + "\"";
      if (saveReplace) line += ", replace";
      ojson sv = piet::dispatch(line, session);
      push(sv);
    }
    if (!norestore && hadData) push(piet::dispatch("restore", session));
  }
  if (savePath.has_value() && !inMemory) {
    // finishDataset already reported the file.
  }
  setR();
  if (const auto fl = optStr(opts, "flist"); fl.has_value() && savePath.has_value()) {
    const std::string gname = strutil::trim(dequote(*fl));
    std::string fname = ensureExt(*savePath, ".dta");
    if (fname.find(' ') != std::string::npos) fname = "\"" + fname + "\"";
    const std::string cur = session.eval.globals.get(gname);
    session.eval.globals.set(gname, cur.empty() ? fname : cur + " " + fname);
  }
  return out;
}


// Portions derived from mat2txt 1.1.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// mat2txt
// ===========================================================================

ojson runMat2txt(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMat2txt.known,
                                            kVocabMat2txt.unimplemented)) {
    return ojson(*err);
  }
  const std::optional<std::string> matName = optStr(parsed.opts, "matrix");
  if (!matName.has_value()) return tblSyntax("mat2txt: matrix(NAME) is required");
  const std::optional<std::string> saving = optStr(parsed.opts, "saving");
  if (!saving.has_value()) return tblSyntax("mat2txt: saving(FILENAME) is required");

  const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*matName));
  // A matrix that does not exist is r(111) "NAME not found" (bank mat2txt cell
  // fd7b33a486db1640).
  if (mat == nullptr) return tblErrorRc(dequote(*matName) + " not found", 111);

  // W2AK TW-MAT2TXT: mat2txt.ado 1.1.2, derived. The header is a tab and
  // each column name followed by a tab; every row is its name, a tab, then each
  // cell `file write %fmt (x) _tab` -- PADDED to the format's width (default
  // %10.0g) with a trailing tab (w2at wt_m01 L2 `r1~t        12~t...`). The
  // format list is per column with the last one repeating.
  std::vector<std::string> fmts = {"%10.0g"};
  if (auto f = optStr(parsed.opts, "format")) {
    fmts = eoTokens(*f);
    if (fmts.empty()) fmts = {"%10.0g"};
  }
  std::string path = dequote(*saving);
  if (path.find('.') == std::string::npos) path += ".txt";
  auto cellText = [&](double v, const std::string& fmt) {
    std::string val = std::isnan(v) ? std::string(".") : eoTrim(eval::applyStataFmt(v, fmt));
    static const std::regex reW(R"(^%(-?)0?(\d+))");
    std::smatch m;
    if (std::regex_search(fmt, m, reW)) {
      const int w = std::stoi(m[2].str());
      val = m[1].length() > 0 ? eoPadR(val, w) : eoPadL(val, w);
    }
    return val;
  };
  std::string bytes;
  if (auto ti = optStr(parsed.opts, "title")) bytes += eoStringArg(ti) + "\n";
  bytes += "\t";
  for (const auto& cn : mat->colnames) bytes += cn + "\t";
  bytes += "\n";
  for (size_t r = 0; r < mat->data.size(); ++r) {
    bytes += (r < mat->rownames.size() ? mat->rownames[r] : ("r" + std::to_string(r + 1))) + "\t";
    for (size_t c = 0; c < mat->data[r].size(); ++c) {
      const std::string& fmt = fmts[std::min(c, fmts.size() - 1)];
      bytes += cellText(mat->data[r][c], fmt) + "\t";
    }
    bytes += "\n";
  }
  if (auto n = optStr(parsed.opts, "note")) bytes += eoStringArg(n) + "\n";
  return emitTable("mat2txt", session, path, bytes, hasFlag(parsed.opts, "replace"),
                   hasFlag(parsed.opts, "append"));
}
}
