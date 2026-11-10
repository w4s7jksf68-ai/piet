// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table dataset export.
#include "tables_internal.h"
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
  if (auto lvErr = rejectForeignLevel(parsed.opts, "regsave")) return *lvErr;

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  std::vector<CoefRowView> rows = coefViews(m.snap);
  // regsave builds its table with svmat, i.e. FLOAT columns: b, se and r2
  // are float-rounded, and the interval is float(float(b) -/+ t*float(se))
  // with t the fit's own critical value (w2at wt_r04; regsave.ado's svmat).
  auto fl = [](double v) { return std::isnan(v) ? v : static_cast<double>(static_cast<float>(v)); };
  // regsave: t_ci = invttail(e(df_r), .025) exactly (the normal
  // quantile without df); the fit's own (b - lo)/se differs in the last bits
  // and flips the float rounding of a limit (w2at wt_r01_r5c4).
  double tExact = kEoNaN;
  if (m.escalars.contains("df_r") && m.escalars["df_r"].is_number()) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "invttail(%.17g, 0.025)", m.escalars["df_r"].get<double>());
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
  {
    DsColumn v{"var", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  {
    DsColumn& c = addNum("coef");
    for (const auto& r : rows) c.num.push_back(r.b);
  }
  {
    DsColumn& c = addNum("stderr");
    for (const auto& r : rows) c.num.push_back(r.se);
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
  {
    DsColumn& c = addNum("r2");
    const double r2 = m.escalars.contains("r2") ? m.escalars["r2"].get<double>()
                                                : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(fl(r2));
  }
  if (auto al = optStr(parsed.opts, "addlabel")) {
    for (const auto& [name, val] : parseCommaPairs(*al)) {
      DsColumn c{name, true, {}, {}};
      for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(val);
      cols.push_back(c);
    }
  }
  if (hasFlag(parsed.opts, "cmdline")) {
    DsColumn c{"cmdline", true, {}, {}};
    const std::string cl = m.snap.value("estimator", std::string()) + " " +
                           m.snap.value("depvar", std::string());
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(strutil::trim(cl));
    cols.push_back(c);
  }

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "regsave", cols, rows.size(), &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "regsave", hbin, usingPath, hasFlag(parsed.opts, "replace"),
                       hasFlag(parsed.opts, "append"), newVars);
}


ojson runParmest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabParmest.known,
                                            kVocabParmest.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  if (!activeModel(session, "parmest", &m, &err)) return err;
  if (auto lvErr = rejectForeignLevel(parsed.opts, "parmest")) return *lvErr;

  // `saving("f.dta", replace)` -- the replace lives INSIDE the parentheses,
  // which is parmest's own syntax and not the outer `replace` option.
  std::optional<std::string> savePath;
  bool saveReplace = hasFlag(parsed.opts, "replace");
  if (auto sv = optStr(parsed.opts, "saving")) {
    const std::vector<std::string> parts = splitTopLevel(*sv, ',');
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
      return tblErrorRc("file " + up + " already exists\nsaving(" + *optStr(parsed.opts, "saving") +
                            ") invalid",
                        498);
    }
  }
  const std::vector<CoefRowView> rows = coefViews(m.snap);
  const double dfr = m.escalars.contains("df_r") ? m.escalars["df_r"].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();

  std::vector<DsColumn> cols;
  {
    DsColumn v{"parm", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  { DsColumn& c = addNum("estimate"); for (const auto& r : rows) c.num.push_back(r.b); }
  { DsColumn& c = addNum("stderr");   for (const auto& r : rows) c.num.push_back(r.se); }
  {
    DsColumn& c = addNum("dof");
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(dfr);
  }
  if (hasFlag(parsed.opts, "omit")) {
    // `omit` ADDS a byte `omit` after dof, 1 on base and omitted parameters
    // (w2at wt_p02_v5); it removes nothing.
    DsColumn& c = addNum("omit");
    for (const auto& r : rows) c.num.push_back((r.b == 0.0 && (r.se == 0.0 || std::isnan(r.se))) ? 1.0 : 0.0);
  }
  { DsColumn& c = addNum("t");        for (const auto& r : rows) c.num.push_back(r.t); }
  { DsColumn& c = addNum("p");        for (const auto& r : rows) c.num.push_back(r.p); }
  // `min95`/`max95` are parmest's names for the fitted 95% limits; a foreign
  // level() was already refused above, so the names cannot lie about the level.
  { DsColumn& c = addNum("min95");    for (const auto& r : rows) c.num.push_back(r.lo); }
  { DsColumn& c = addNum("max95");    for (const auto& r : rows) c.num.push_back(r.hi); }
  if (hasFlag(parsed.opts, "label")) {
    DsColumn c{"label", true, {}, {}};
    for (const auto& r : rows) c.str.push_back(session.eval.varLabels.get(r.name));
    cols.push_back(c);
  }
  if (auto idn = optStr(parsed.opts, "idnum")) {
    DsColumn& c = addNum("idnum");
    double v = std::numeric_limits<double>::quiet_NaN();
    try {
      size_t pos = 0;
      v = std::stod(dequote(*idn), &pos);
      if (pos != dequote(*idn).size()) v = std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
    }
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(v);
  }
  if (auto ids = optStr(parsed.opts, "idstr")) {
    DsColumn c{"idstr", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(dequote(*ids));
    cols.push_back(c);
  }
  const size_t nRows = cols.empty() ? 0 : (cols[0].isString ? cols[0].str.size()
                                                            : cols[0].num.size());

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "parmest", cols, nRows, &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "parmest", hbin, savePath, saveReplace, false, newVars);
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
  if (mat == nullptr) return tblError("matrix " + dequote(*matName) + " not found");

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
