// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table esttab.
#include "tables_internal.h"
namespace piet::handlers::table_detail {



ojson esttabImpl(const ojson& esttabBlock, const std::string& path,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, bool toFile) {
  if (!esttabBlock.is_object() || !esttabBlock.contains("coef_rows")) {
    return tblError("esttab: nothing to write");
  }
  std::string userPath = dequote(path);
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto eoFail = [](const EoError& e) { return tblErrorRc(e.msg, e.rc); };

  // ---- models (unchanged resolution: runEsttab already expanded the namelist) --
  std::vector<std::string> names;
  if (esttabBlock.contains("names") && esttabBlock["names"].is_array()) {
    for (const auto& n : esttabBlock["names"]) {
      if (n.is_string()) names.push_back(n.get<std::string>());
    }
  }
  // `runEsttab` falls back to the ACTIVE estimate when the caller named no
  // models and the store is empty; the label it reports for that is not a
  // stored name, so the active estimate is resolved directly here.
  // esttab MatrixMode: `esttab matrix(A, fmt())` tabulates a
  // matrix instead of estimates (the dispatcher hands the spec over).
  const bool matrixmode = esttabBlock.contains("matrix") && esttabBlock["matrix"].is_string();
  std::vector<Model> models;
  bool allStored = !names.empty();
  for (const auto& n : names) {
    if (n != "." && session.estimatesGet(n) == nullptr) allStored = false;
  }
  if (matrixmode) {
    // no estimates
  } else if (allStored) {
    ojson err;
    models = collectModels(session, "esttab", names, &err);
    if (models.empty()) return err;
  } else {
    Model m;
    ojson err;
    if (!activeModel(session, "esttab", &m, &err)) return err;
    if (!names.empty()) m.name = names[0];
    models.push_back(m);
  }

  // ---- format modes (esttab) --------------------------------------------
  std::vector<std::string> modes;
  for (const char* md : {"smcl", "fixed", "tab", "csv", "scsv", "rtf", "html", "html5", "tex",
                         "booktabs", "md", "mmd"}) {
    if (has(md)) modes.push_back(md);
  }
  if (modes.size() > 1) {
    return tblErrorRc("only one allowed of smcl, fixed, tab, csv, scsv, rtf, html, html5, tex, "
                      "booktabs, md, or mmd",
                      198);
  }
  std::string suffix;
  {
    // _getfilesuffix: the text after the last `.` of the file name, kept as typed.
    const size_t slash = userPath.find_last_of("/\\");
    const size_t dot = userPath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
      suffix = userPath.substr(dot);
    }
  }
  std::string mode = modes.empty() ? std::string() : modes[0];
  if (!toFile) {
    // The screen form (esttab `else local mode smcl`): smcl's widths,
    // delimiter and abbreviation are fixed mode's; the text itself is
    // the UI block's, so only the checks and r() matter here.
    if (mode.empty() || mode == "smcl" || mode == "rtf" || mode == "html" || mode == "html5" ||
        mode == "md" || mode == "mmd") {
      mode = "fixed";
    }
  } else if (mode.empty()) {
    if (suffix == ".html" || suffix == ".htm") mode = "html";
    else if (suffix == ".tex") mode = "tex";
    else if (suffix == ".csv") mode = "csv";
    else if (suffix == ".rtf") mode = "rtf";
    else if (suffix == ".smcl") mode = "smcl";
    else if (suffix == ".md") mode = "md";
    else if (suffix == ".mmd") mode = "mmd";
    else mode = "fixed";
  }
  if (toFile && suffix.empty()) {
    // esttab: a `using` name with no suffix gets the mode's.
    if (mode == "fixed" || mode == "tab") suffix = ".txt";
    else if (mode == "csv" || mode == "scsv") suffix = ".csv";
    else if (mode == "rtf") suffix = ".rtf";
    else if (mode == "html" || mode == "html5") suffix = ".html";
    else if (mode == "tex" || mode == "booktabs") suffix = ".tex";
    else if (mode == "smcl") suffix = ".smcl";
    else if (mode == "md") suffix = ".md";
    else if (mode == "mmd") suffix = ".mmd";
    userPath += suffix;
  }
  if (mode == "smcl" || mode == "rtf" || mode == "html" || mode == "html5" || mode == "md" ||
      mode == "mmd") {
    return ojson(eval::unsupportedOption(
        mode, "esttab: the " + mode + " writer is not implemented in this build; fixed, tab, csv, "
              "scsv, tex and booktabs are"));
  }

  // ---- option locals (esttab) ------------------------------------------------
  const bool plain = has("plain");
  const bool label = has("label");
  const bool fragment = has("fragment");
  const bool wide = has("wide");
  const bool noT = has("not");
  const bool zOn = has("z") || str("z").has_value();
  const bool seOn = has("se") || str("se").has_value();
  const bool pOn = has("p") || str("p").has_value();
  const bool ciOn = has("ci") || str("ci").has_value();
  const bool betaOn = has("beta") || str("beta").has_value();
  const bool staraux = has("staraux");
  const bool brackets = has("brackets");
  const bool cellsGiven = str("cells").has_value();
  const bool nocellsopt = !cellsGiven;
  const bool stardetach = has("stardetach");
  int level = 95;
  if (auto lv = str("level")) {
    try {
      level = std::stoi(dequote(*lv));
    } catch (...) {
      return tblErrorRc("level(" + *lv + ") invalid", 198);
    }
    if (level < 10 || level > 99) return tblErrorRc("level(" + *lv + ") invalid", 198);
    if (level != 95) {
      // The interval is READ from the estimator (it carries its own level), so
      // another level cannot be honoured; refused rather than mislabelled.
      return ojson(eval::unsupportedOption(
          "level", "esttab reports the confidence interval the estimation stored (95% by "
                   "default); this build cannot re-derive it at level(" + dequote(*lv) + ")"));
    }
  }
  std::optional<std::string> bfmtOpt = str("b");
  std::string bfmt = bfmtOpt.has_value() ? eoTrim(dequote(*bfmtOpt)) : std::string();
  std::string tfmt = str("t").has_value() ? eoTrim(dequote(*str("t"))) : std::string();
  std::string zfmt = str("z").has_value() ? eoTrim(dequote(*str("z"))) : std::string();
  std::string sefmt = str("se").has_value() ? eoTrim(dequote(*str("se"))) : std::string();
  std::string pfmt = str("p").has_value() ? eoTrim(dequote(*str("p"))) : std::string();
  std::string cifmt = str("ci").has_value() ? eoTrim(dequote(*str("ci"))) : std::string();
  std::string betafmt = str("beta").has_value() ? eoTrim(dequote(*str("beta"))) : std::string();
  const bool r2On = has("r2") || str("r2").has_value();
  const bool ar2On = has("ar2") || str("ar2").has_value();
  const bool pr2On = has("pr2") || str("pr2").has_value();
  const bool aicOn = has("aic") || str("aic").has_value();
  const bool bicOn = has("bic") || str("bic").has_value();
  std::string r2fmt = str("r2").value_or(""), ar2fmt = str("ar2").value_or(""),
              pr2fmt = str("pr2").value_or(""), aicfmt = str("aic").value_or(""),
              bicfmt = str("bic").value_or("");
  bool star = has("star") || str("star").has_value();
  if (star && has("nostar")) return tblErrorRc("options star and nostar not both allowed", 198);
  const bool noconstant = (has("noconstant") || has("nocons") ||
                           (betaOn && !has("constant")));

  EtMode md = esttabMode(mode, label, plain, level);
  const std::string mode0 = md.mode0;
  const std::string csvlhs = plain ? "\"" : "=\"";

  // widths (esttab)
  int modeVarwidth = md.varwidth, modeModelwidth = md.modelwidth;
  if (has("compress")) {
    if (modeModelwidth >= 0) modeModelwidth -= 3;
    if (modeVarwidth >= 0) modeVarwidth -= label ? 4 : 2;
  }
  EoSpec sp;
  sp.session = &session;
  sp.level = level;
  if (auto mwo = str("modelwidth")) {
    sp.modelwidth = eoIntList(*mwo);
    if (sp.modelwidth.empty()) sp.modelwidth = {0};
  } else if (nocellsopt && modeModelwidth >= 0 && ciOn) {
    if (wide) sp.modelwidth = {modeModelwidth, 2 * modeModelwidth - 2};
    else sp.modelwidth = {2 * modeModelwidth - 2};
  } else {
    sp.modelwidth = {modeModelwidth >= 0 ? modeModelwidth : 0};
  }
  if (auto vwo = str("varwidth")) {
    const std::vector<int> v = eoIntList(*vwo);
    sp.varwidth = v.empty() ? 0 : v[0];
  } else {
    sp.varwidth = modeVarwidth >= 0 ? modeVarwidth : 0;
  }

  // switches (esttab)
  bool depvars = has("depvars"), numbers = has("numbers"), parentheses = has("parentheses"),
       notes = has("notes"), gaps = has("gaps"), lines = has("lines"), eqlines = has("eqlines");
  if (!plain && !matrixmode) {
    if (!has("nostar")) star = true;
    if (!has("nodepvars")) depvars = true;
    if (!has("nonumbers")) numbers = true;
    if (!has("noparentheses")) parentheses = true;
    if (!has("nonotes")) notes = true;
    if (!wide && (!noT || zOn || seOn || pOn || ciOn || str("aux").has_value()) && nocellsopt &&
        !has("nogaps")) {
      gaps = true;
    }
  }
  if (!plain && !has("nolines")) lines = true;
  if (lines && !has("noeqlines")) eqlines = true;
  if (mode0 == "tab" || mode0 == "csv") {
    lines = false;
    eqlines = false;
  }
  const bool legend = notes && !has("nolegend") && nocellsopt && !matrixmode;
  if (plain) {
    if (bfmt.empty()) bfmt = "%9.0g";
    if (tfmt.empty()) tfmt = bfmt;
    if (zfmt.empty()) zfmt = bfmt;
    if (sefmt.empty()) sefmt = bfmt;
    if (pfmt.empty()) pfmt = bfmt;
    if (cifmt.empty()) cifmt = bfmt;
    if (betafmt.empty()) betafmt = bfmt;
  }

  // ---- cells (esttab) ----------------------------------------------------------
  std::vector<std::string> thenote;
  if (bfmt.empty()) bfmt = "a3";
  EoError eerr;
  if (matrixmode) {
    std::string merr;
    if (!eoMatrixModel(session, esttabBlock["matrix"].get<std::string>(), &sp, &merr)) {
      return tblErrorRc(merr, 111);
    }
  } else if (cellsGiven) {
    std::string cerr;
    if (!eoParseCells(*str("cells"), &sp.cells, &cerr)) return tblErrorRc(cerr, 198);
    // esttab hands cells() to estout unchanged, and estout gives
    // every fmt-less element the FIRST element's format (default %9.0g):
    // w2ak_estout_family.log ek_e20_cells_L7 `[0.500]` for qval(par([ ])) after
    // b(star fmt(3)).
    if (!sp.cells.elems.empty()) {
      if (sp.cells.elems[0].fmt.empty()) sp.cells.elems[0].fmt = {"%9.0g"};
      for (auto& el : sp.cells.elems) {
        if (el.fmt.empty()) el.fmt = sp.cells.elems[0].fmt;
      }
    }
  } else {
    std::string note1;
    auto addElem = [&](EoElem el, int row, bool sideBySide) {
      sp.cells.elems.push_back(el);
      const int idx = static_cast<int>(sp.cells.elems.size()) - 1;
      if (sideBySide && !sp.cells.rows.empty()) {
        sp.cells.rows.back().push_back({idx});
      } else {
        sp.cells.rows.push_back({{idx}});
      }
      (void)row;
      sp.cells.ncols = 0;
      for (const auto& r : sp.cells.rows) sp.cells.ncols = std::max(sp.cells.ncols, r.size());
    };
    EoElem mainEl;
    const bool bstar = star && !staraux;
    if (betaOn) {
      if (str("main").has_value()) return tblErrorRc("beta() and main() not allowed both", 198);
      if (betafmt.empty()) betafmt = "3";
      mainEl.name = "beta";
      mainEl.fmt = {betafmt};
      mainEl.star = bstar;
      if (notes) note1 = "Standardized beta coefficients";
    } else if (auto mo = str("main")) {
      const std::vector<std::string> t = eoTokens(*mo);
      mainEl.name = t.empty() ? std::string("b") : t[0];
      mainEl.fmt = {t.size() > 1 ? t[1] : bfmt};
      mainEl.star = bstar;
      if (notes) note1 = mainEl.name + " coefficients";
    } else {
      mainEl.name = "b";
      mainEl.fmt = {bfmt};
      mainEl.star = bstar;
    }
    addElem(mainEl, 0, false);
    if (!noT || zOn || seOn || pOn || ciOn || str("aux").has_value()) {
      if (has("onecell")) {
        return ojson(eval::unsupportedOption("onecell", "esttab: onecell is not implemented in this build"));
      }
      std::string auxname, auxfmt;
      if (auto ao = str("aux")) {
        const std::vector<std::string> t = eoTokens(*ao);
        if (!t.empty()) auxname = t[0];
        if (t.size() > 1) auxfmt = t[1];
      }
      std::vector<std::string> auxl;
      if (zOn) auxl.push_back("z");
      if (seOn) auxl.push_back("se");
      if (pOn) auxl.push_back("p");
      if (ciOn) auxl.push_back("ci");
      if (!auxname.empty()) auxl.push_back(auxname);
      if (auxl.size() > 1) return tblErrorRc("only one allowed of z, se, p, ci, and aux()", 198);
      const std::string aux = auxl.empty() ? std::string("t") : auxl[0];
      bool abs = has("abs");
      if (aux != "t" && aux != "z") abs = false;
      EoElem auxEl;
      auxEl.name = aux;
      bool bracketsOn = brackets;
      if (parentheses || brackets) {
        auxEl.hasPar = true;
        if (aux == "ci") {
          bracketsOn = true;
          auxEl.par = {"[", ",", "]"};
        } else if (brackets) {
          auxEl.par = {"[", "]"};
        } else {
          auxEl.par = {"(", ")"};
        }
      }
      if (notes) {
        std::string tn = note1;
        if (!tn.empty()) tn += "; ";
        if (!auxname.empty()) {
          tn += auxname;
        } else if (aux == "t" || aux == "z") {
          if (abs) tn += "Absolute ";
          tn += (aux == "t") ? md.tstatlab : md.zstatlab;
        } else if (aux == "se") {
          tn += "Standard errors";
        } else if (aux == "p") {
          tn += md.pvallab;
        } else if (aux == "ci") {
          tn += md.cilab;
        }
        if (!parentheses) tn += wide ? " in second column" : " in second row";
        else if (bracketsOn) tn += " in brackets";
        else tn += " in parentheses";
        note1 = tn;
      }
      if (tfmt.empty()) tfmt = "2";
      if (zfmt.empty()) zfmt = "2";
      if (sefmt.empty()) sefmt = bfmt;
      if (pfmt.empty()) pfmt = "3";
      if (cifmt.empty()) cifmt = bfmt;
      if (auxfmt.empty()) auxfmt = bfmt;
      if (auxname.empty()) {
        auxfmt = aux == "t" ? tfmt : aux == "z" ? zfmt : aux == "se" ? sefmt : aux == "p" ? pfmt : cifmt;
      }
      auxEl.fmt = {auxfmt};
      auxEl.abs = abs;
      auxEl.star = staraux && star;
      addElem(auxEl, 1, wide);
    }
    if (!note1.empty()) thenote.push_back(note1);
  }

  // ---- stats (esttab) -------------------------------------------------------------
  std::string sfmt = str("sfmt").value_or("");
  std::optional<std::string> statsOpt = str("stats");
  if (!statsOpt.has_value()) statsOpt = str("s");  // estout `Stats()`: `s` is its minimum
  if (matrixmode) {
    // esttab builds no stats() in matrix mode
  } else if (auto st = statsOpt) {
    const std::string firstFmt =
        (!sp.cells.elems.empty() && !sp.cells.elems[0].fmt.empty()) ? sp.cells.elems[0].fmt[0] : "%9.0g";
    if (!eoParseStats(*st, firstFmt, &sp, &eerr)) return eoFail(eerr);
  } else {
    if (sfmt.empty()) sfmt = bfmt;
    if (r2fmt.empty()) r2fmt = plain ? bfmt : "3";
    if (ar2fmt.empty()) ar2fmt = plain ? bfmt : "3";
    if (pr2fmt.empty()) pr2fmt = plain ? bfmt : "3";
    if (aicfmt.empty()) aicfmt = bfmt;
    if (bicfmt.empty()) bicfmt = bfmt;
    std::vector<std::string> stalabs = label ? md.lsl : md.ssl;
    const std::string obslab = stalabs.empty() ? std::string("N") : stalabs[0];
    std::vector<std::string> sta, stalab, stafmt;
    const bool noobs = has("noobs");
    const bool obslast = has("obslast");
    if (!noobs && !obslast) {
      sta.push_back("N");
      stalab.push_back(obslab);
      stafmt.push_back("%18.0g");
    }
    const char* flagKeys[] = {"r2", "r2_a", "r2_p", "aic", "bic"};
    const bool flagOn[] = {r2On, ar2On, pr2On, aicOn, bicOn};
    const std::string flagFmt[] = {r2fmt, ar2fmt, pr2fmt, aicfmt, bicfmt};
    for (int i = 0; i < 5; ++i) {
      if (!flagOn[i]) continue;
      sta.push_back(flagKeys[i]);
      stalab.push_back(static_cast<size_t>(i + 1) < stalabs.size() ? stalabs[static_cast<size_t>(i) + 1] : std::string());
      stafmt.push_back(eoTrim(dequote(flagFmt[i])));
    }
    if (auto sc = str("scalars")) {
      const std::vector<std::string> sfl = eoTokens(sfmt);
      size_t i = 0;
      for (const auto& addstat : eoTokens(*sc)) {
        ++i;
        std::string name, lab;
        {
          const std::string t = eoTrim(addstat);
          const size_t sp1 = t.find_first_of(" \t");
          name = sp1 == std::string::npos ? t : t.substr(0, sp1);
          lab = sp1 == std::string::npos ? std::string() : t.substr(sp1 + 1);
        }
        if (std::find(sta.begin(), sta.end(), name) != sta.end()) continue;
        if (name == "N" && !noobs && obslast) continue;
        if (eoTrim(lab).empty()) lab = name;
        std::string f = (i <= sfl.size()) ? sfl[i - 1] : (sfl.empty() ? std::string() : sfl.back());
        sta.push_back(name);
        stalab.push_back(lab);
        stafmt.push_back(f);
      }
    }
    if (!noobs && obslast) {
      sta.push_back("N");
      stalab.push_back(obslab);
      stafmt.push_back("%18.0g");
    }
    for (size_t i = 0; i < sta.size(); ++i) {
      EoStatRow r;
      r.key = sta[i];
      r.fmt = stafmt[i].empty() ? std::string("%9.0g") : stafmt[i];
      sp.stats.push_back(r);
    }
    sp.statslabels.given = true;
    sp.statslabels.list = stalab;
  }

  // ---- table header (esttab) ---------------------------------------------------------
  std::string mspanPrefix, mspanSuffix;
  bool mspan = false;
  if (auto ml = str("mlabels")) {
    if (!eoParseLabelOpt(*ml, &sp.mlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else {
    if (mode0 == "tex") {
      mspan = true;
      mspanPrefix = "\\multicolumn{@span}{c}{";
      mspanSuffix = "}";
    }
    auto setSpan = [&]() {
      sp.mlabels.span = mspan;
      sp.mlabels.prefix = mspanPrefix;
      sp.mlabels.suffix = mspanSuffix;
    };
    if (depvars) {
      sp.mlabels.given = true;
      sp.mlabels.depvars = true;
      setSpan();
    }
    if (has("nomtitles")) {
      sp.mlabels = EoLabelOpt{};
      sp.mlabels.given = true;
      sp.mlabels.none = true;
    }
    if (has("mtitles")) {
      sp.mlabels = EoLabelOpt{};
      sp.mlabels.given = true;
      sp.mlabels.titles = true;
      if (auto mt = str("mtitles")) sp.mlabels.list = eoTokens(*mt);
      setSpan();
    }
  }
  if (auto cl = str("collabels")) {
    if (!eoParseLabelOpt(*cl, &sp.collabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else if (nocellsopt && !plain && !matrixmode) {
    sp.collabels.given = true;
    sp.collabels.none = true;
  }
  sp.numbers = numbers && !has("nonumbers");
  if (mode0 == "tex" && sp.numbers) {
    sp.numL = "\\multicolumn{@span}{c}{(";
    sp.numR = ")}";
  }
  if (auto mg = str("mgroups")) {
    if (!eoParseLabelOpt(*mg, &sp.mgroups, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  }

  // ---- notes and the legend (esttab) ----------------------------------------------------
  sp.title = str("title").has_value() ? eoStringArg(str("title")) : std::string();
  if (auto nt = str("note")) {
    sp.note = eoStringArg(nt);
    thenote = {"@note"};
  }
  if (auto an = str("addnotes")) {
    if (an->find('"') == std::string::npos) {
      thenote.push_back(*an);
    } else {
      for (const auto& t : eoTokens(*an)) thenote.push_back(t);
    }
  }
  if (legend && (star || !nocellsopt)) thenote.push_back("@starlegend");

  // star levels and the mode strings (esttab)
  if (auto sl = str("starlevels")) {
    std::string lab = sp.starlevelsLabel, del = sp.starlevelsDelim;
    if (!eoParseStarPairs(*sl, &sp.starlevels, &lab, &del, &eerr)) return eoFail(eerr);
    sp.starlevelsLabel = lab;
    sp.starlevelsDelim = del;
  } else if (star) {
    std::vector<std::pair<std::string, std::string>> lv = md.starlevels;
    if (auto s2 = str("star")) {
      lv.clear();
      if (!eoParseStarPairs(*s2, &lv, nullptr, nullptr, &eerr)) return eoFail(eerr);
      if (mode0 == "tex") {
        for (auto& p : lv) p.first = "\\sym{" + p.first + "}";
      }
    }
    sp.starlevels = lv;
    if (md.starlevlabSet) sp.starlevelsLabel = md.starlevlab;
  }
  if (auto v = str("begin")) sp.begin = eoStringArg(v);
  else if (md.beginSet) sp.begin = md.begin;
  if (auto v = str("delimiter")) sp.delimiter = eoStringArg(v);
  else if (md.delimiterSet) sp.delimiter = md.delimiter;
  if (auto v = str("end")) sp.end = eoStringArg(v);
  else if (md.endSet) sp.end = md.end;
  if (auto v = str("substitute")) {
    const std::vector<std::string> t = eoTokens(*v);
    for (size_t i = 0; i < t.size(); i += 2) sp.substitute.emplace_back(t[i], i + 1 < t.size() ? t[i + 1] : "");
  } else {
    sp.substitute = md.substitute;
  }
  sp.interaction = str("interaction").has_value() ? eoStringArg(str("interaction")) : md.interaction;
  if (has("noabbrev")) sp.abbrev = false;
  else if (has("abbrev")) sp.abbrev = true;
  else sp.abbrev = md.abbrev;
  sp.wrap = has("wrap");
  if (auto ic = str("incelldelimiter")) sp.incelldelimiter = eoStringArg(ic);
  sp.lz = !has("nolz");
  if (auto hl = str("hlinechar")) sp.hlinechar = eoStringArg(hl);

  // estout's own style defaults on top (estout): `style(esttab)` is
  // estout's `tab`; a user style(tex)/style(fixed) fills what esttab left empty.
  {
    std::string style = str("style").has_value() ? strutil::lower(eoTrim(dequote(*str("style")))) : "esttab";
    if (style == "esttab") style = "tab";
    if (style == "tex") {
      if (!str("end").has_value() && !md.endSet) sp.end = "\\\\";
      if (!str("interaction").has_value() && md.interaction == " # " && false) sp.interaction = " $\\times$ ";
    } else if (style == "fixed") {
      if (!has("noabbrev")) sp.abbrev = true;
    } else if (style != "tab") {
      return ojson(eval::unsupportedOption(
          "style", "esttab: style(" + style + ") has no writer in this build; tab, fixed and tex do"));
    }
  }

  // ---- pre-/posthead, pre-/postfoot (esttab) ---------------------------------------------------
  std::vector<std::string> opening, closing, toprule, bottomrule, topgap, bottomgap;
  std::string open2, close2;
  if (!fragment) {
    if (mode0 == "tex") {
      const bool title = !sp.title.empty();
      const bool floatOn = (title || has("float")) && !has("nofloat");
      if (floatOn) opening.insert(opening.end(), md.open.begin(), md.open.end());
      else if (star) opening.push_back("{");
      if (star) opening.push_back("\\def\\sym#1{\\ifmmode^{#1}\\else\\(^{#1}\\)\\fi}");
      if (title) opening.insert(opening.end(), md.caption.begin(), md.caption.end());
      const std::string lstub = str("labcol2").has_value() ? "lc" : "l";
      const auto widthOpt = str("width");
      const std::string extracolsep =
          widthOpt.has_value() ? "@{\\hskip\\tabcolsep\\extracolsep\\fill}" : "";
      open2 = widthOpt.has_value() ? "\\begin{tabular*}{" + eoStringArg(widthOpt) + "}"
                                   : std::string("\\begin{tabular}");
      close2 = widthOpt.has_value() ? "\\end{tabular*}" : "\\end{tabular}";
      std::string colspec;
      std::optional<std::string> alignOpt = str("alignment");
      if (!alignOpt.has_value()) alignOpt = str("align");  // ALIGNment: `align` is its minimum
      if (matrixmode) {
        // MakeTeXColspecMat (esttab): `*{cols}{c}`, or the alignment
        // given, written without the `*{@E}{...}` wrapper.
        colspec.clear();
      } else if (auto al = alignOpt) {
        colspec = eoStringArg(al);
      } else if (nocellsopt) {
        // MakeTeXColspec (esttab)
        colspec = (star && stardetach && !staraux) ? "r@{}l" : "c";
        if (wide && !noT) colspec += (star && stardetach && staraux) ? "r@{}l" : "c";
      } else {
        // MakeTeXColspecAlt (esttab): one `c` per item of the widest row.
        size_t cnt = 1;
        for (const auto& r : sp.cells.rows) {
          size_t n = 0;
          for (const auto& c : r) n += std::max<size_t>(c.size(), 1);
          cnt = std::max(cnt, n);
        }
        colspec = std::string(cnt, 'c');
      }
      if (matrixmode) {
        const std::string mcs = alignOpt.has_value()
                                    ? eoStringArg(alignOpt)
                                    : "*{" + std::to_string(sp.cells.ncols) + "}{c}";
        opening.push_back(open2 + "{" + extracolsep + lstub + mcs + "}");
      } else {
        opening.push_back(open2 + "{" + extracolsep + lstub + "*{@E}{" + colspec + "}}");
      }
      // notes
      for (const auto& ch : thenote) closing.push_back("\\multicolumn{@span}{l}{\\footnotesize " + ch + "}\\\\");
      closing.push_back(close2);
      if (floatOn) closing.insert(closing.end(), md.close.begin(), md.close.end());
      else if (star) closing.push_back("}");
    } else {
      opening.insert(opening.end(), md.open.begin(), md.open.end());
      if (!sp.title.empty()) opening.insert(opening.end(), md.caption.begin(), md.caption.end());
      for (const auto& ch : thenote) {
        closing.push_back(mode0 == "csv" ? csvlhs + ch + "\"" : ch);
      }
      closing.insert(closing.end(), md.close.begin(), md.close.end());
    }
    toprule = md.toprule;
    bottomrule = md.bottomrule;
    topgap = md.topgap;
    bottomgap = md.bottomgap;
  }
  const std::vector<std::string> midrule = md.midrule, midgap = md.midgap;
  if (auto ph = str("prehead")) {
    sp.prehead = eoHookLines(*ph);
  } else {
    if (lines) opening.insert(opening.end(), toprule.begin(), toprule.end());
    else if (gaps) opening.insert(opening.end(), topgap.begin(), topgap.end());
    sp.prehead = opening;
  }
  if (auto ph = str("posthead")) {
    sp.posthead = eoHookLines(*ph);
  } else if (lines) {
    sp.posthead = midrule;
  } else if (gaps) {
    sp.posthead = midgap;
  }
  if (auto pf = str("prefoot")) {
    sp.prefoot = eoHookLines(*pf);
  } else if (!sp.stats.empty() && !sp.cells.none) {
    if (lines) sp.prefoot = midrule;
    else if (gaps) sp.prefoot = midgap;
  }
  if (auto pf = str("postfoot")) {
    sp.postfoot = eoHookLines(*pf);
  } else {
    std::vector<std::string> c2;
    if (lines) c2 = bottomrule;
    else if (gaps) c2 = bottomgap;
    c2.insert(c2.end(), closing.begin(), closing.end());
    sp.postfoot = c2;
  }

  // varlabels (esttab)
  if (auto vl = str("varlabels")) {
    if (!eoParseLabelOpt(*vl, &sp.varlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else {
    std::vector<std::string> pairs;
    if (auto cl = str("coeflabels")) pairs = eoTokens(*cl);
    if (label) {
      pairs.push_back("_cons");
      pairs.push_back("Constant");
    }
    sp.varlabels.list = pairs;
    if (gaps) {
      sp.varlabels.end = {""};
      sp.varlabels.end.insert(sp.varlabels.end.end(), midgap.begin(), midgap.end());
      sp.varlabels.last = 0;
    }
    sp.varlabels.given = !pairs.empty() || gaps;
  }
  // eqlabels (esttab)
  if (auto el = str("eqlabels")) {
    if (!eoParseLabelOpt(*el, &sp.eqlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  }
  if ((eqlines || gaps) && !has("unstack") && !sp.eqlabels.none) {
    const bool ok = sp.eqlabels.begin.empty() && !sp.eqlabels.replace && sp.eqlabels.first < 0;
    if (ok) {
      if (eqlines && md.hasEqrule) {
        sp.eqlabels.begin = md.eqruleBegin;
      } else if (gaps) {
        sp.eqlabels.begin = midgap;
        sp.eqlabels.begin.push_back("");
      }
      sp.eqlabels.first = 0;
    }
  }
  sp.label = label && !has("nolabel");
  sp.unstack = has("unstack");
  sp.omitted = !has("noomitted") && !has("noomit");
  sp.baselevels = !has("nobaselevels") && !has("nobase");

  // coefficient selection (esttab for noconstant)
  sp.keep = str("keep");
  sp.order = str("order");
  if (auto d = str("drop")) {
    sp.drop = d;
  } else if (noconstant) {
    sp.drop = std::string("_cons");
    sp.dropRelax = true;
  }
  if (auto rc = str("refcat")) {
    std::string lhs, rhs;
    eoSplitComma(*rc, &lhs, &rhs);
    const std::vector<std::string> t = eoTokens(lhs);
    for (size_t i = 0; i + 1 < t.size(); i += 2) sp.refcat.emplace_back(t[i], t[i + 1]);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "NOLabel")) sp.refcatLabel.clear();
      else if (eoAbbr(nm, "Label")) sp.refcatLabel = eoStringArg(arg);
      else if (eoAbbr(nm, "Below")) sp.refcatBelow = true;
    }
  }
  if (auto ind = str("indicate")) {
    std::string lhs, rhs;
    eoSplitComma(*ind, &lhs, &rhs);
    for (const auto& g : eoTokens(lhs)) {
      EoIndicate ig;
      const size_t eq = g.find('=');
      if (eq == std::string::npos) {
        ig.vars = g;
      } else {
        ig.name = eoTrim(g.substr(0, eq));
        ig.vars = g.substr(eq + 1);
      }
      sp.indicate.push_back(ig);
    }
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Labels")) {
        const std::vector<std::string> t = eoTokens(arg.value_or(""));
        if (!t.empty()) sp.indicateYes = t[0];
        if (t.size() > 1) sp.indicateNo = t[1];
      }
    }
  }
  if (auto lc = str("labcol2")) {
    std::string lhs, rhs;
    eoSplitComma(*lc, &lhs, &rhs);
    sp.labcol2 = true;
    sp.labcol2Vals = eoTokens(lhs);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Title")) {
        const std::string a = arg.value_or("");
        sp.labcol2Title = a.find('"') == std::string::npos ? std::vector<std::string>{a} : eoTokens(a);
      } else if (eoAbbr(nm, "Width")) {
        const std::vector<int> v = eoIntList(arg.value_or(""));
        if (!v.empty()) sp.labcol2Width = v[0];
      }
    }
  }
  if (auto xc = str("extracols")) sp.extracols = eoIntList(*xc);
  if (auto rn = str("rename")) {
    std::string rl = *rn;
    std::replace(rl.begin(), rl.end(), ',', ' ');
    const std::vector<std::string> t = eoTokens(rl);
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
      if (t[i + 1].find(':') != std::string::npos || t[i + 1].empty()) {
        return tblErrorRc("invalid rename()", 198);
      }
      sp.rename.emplace_back(t[i], t[i + 1]);
    }
  }
  if (str("equations").has_value()) {
    return ojson(eval::unsupportedOption("equations", "esttab: equations() is not implemented in this build"));
  }

  // ---- the models' rows --------------------------------------------------------------------------------------
  TransformSpec trSpec;
  if (auto tr = str("transform")) {
    std::string trErr;
    if (!parseTransformOpt(*tr, &trSpec, &trErr)) return tblSyntax("esttab: " + trErr);
  }
  std::vector<std::map<std::string, double>> betas;
  if (betaOn) {
    std::string berr;
    if (!esttabBetaVals(session, models, &betas, &berr)) {
      return ojson(eval::unsupportedOption("beta", berr));
    }
  }
  if (sp.label) session.syncEvalVarLabels();
  for (size_t mi = 0; mi < models.size() && !matrixmode; ++mi) {
    EoModel em = eoModelFrom(models[mi], models[mi].name, session, trSpec.empty() ? nullptr : &trSpec,
                             mi, !sp.omitted, sp.baselevels);
    if (betaOn) em.beta = betas[mi];
    sp.models.push_back(em);
  }
  if (!matrixmode) {
    for (auto& em : sp.models) em.model = nullptr;
    for (size_t mi = 0; mi < sp.models.size(); ++mi) sp.models[mi].model = &models[mi];
  }

  std::string text;
  int ccols = -1;
  if (!eoRender(sp, &text, &eerr, &ccols)) return eoFail(eerr);
  if (!toFile) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "esttab";
    // BUG-407: a cells()/main()/aux() layout is one the UI's estimates_table
    // block cannot carry, so the screen shows the rendered table itself: smcl's
    // text is fixed mode's (esttab), after the blank line esttab
    // prints first (StataBE 19.5 `esttab m1, cells(b)` and five other layouts
    // match this text line for line).
    if (cellsGiven || str("main").has_value() || str("aux").has_value()) {
      std::string body = text;
      while (!body.empty() && body.back() == '\n') body.pop_back();
      return displayBlock("\n" + body);
    }
    return ojson();
  }
  ojson result = emitTable("esttab", session, userPath, text, has("replace"), has("append"));
  if (!isErrorBlock(result)) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "esttab";
  }
  // Options this build still accepts without honouring: said once, never silent.
  std::vector<std::string> seen;
  for (const char* n : {"page", "longtable", "standalone", "fonttbl", "attributes"}) {
    if (has(n) || str(n).has_value()) seen.push_back(n);
  }
  if (!seen.empty() && !isErrorBlock(result)) {
    ojson arr = ojson::array();
    arr.push_back(result);
    arr.push_back(displayBlock("(note: " + joinWith(seen, ", ") +
                               " accepted and not applied by this build)"));
    return arr;
  }
  return result;
}
}
namespace piet::handlers {
using namespace table_detail;


ojson writeEsttabFile(const ojson& esttabBlock, const std::string& path,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                      SessionState& session) {
  return esttabImpl(esttabBlock, path, opts, session, /*toFile=*/true);
}


ojson esttabScreen(const ojson& esttabBlock,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   SessionState& session) {
  return esttabImpl(esttabBlock, std::string(), opts, session, /*toFile=*/false);
}
}
