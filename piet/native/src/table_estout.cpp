// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table estout.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// estout's MatrixMode (estout; esttab hands the same namelist
// on): one model named after the matrix, one side-by-side cell column per
// matrix column labelled with its name, the rows the matrix's rows, fmt()
// cycling over the columns.
bool eoMatrixModel(SessionState& session, const std::string& spec, EoSpec* sp, std::string* err) {
  // `e(NAME[, subopts])` / `r(NAME[, subopts])` (MatrixMode's e()/r() options,
  // estout) wrap the same `name [, Fmt() Transpose]` that
  // matrix() takes (ParseMatrixOpt); the model is called `e(NAME)`.
  std::string inner = eoTrim(spec);
  char cls = 0;
  if (inner.size() >= 3 && (inner[0] == 'e' || inner[0] == 'r') && inner[1] == '(' &&
      inner.back() == ')') {
    cls = inner[0];
    inner = inner.substr(2, inner.size() - 3);
  }
  std::string mname = inner, mlhs, mrhs;
  eoSplitComma(mname, &mlhs, &mrhs);
  const std::string bareName = eoTrim(mlhs);
  const std::string matName = cls == 0 ? bareName : std::string(1, cls) + "(" + bareName + ")";
  std::vector<std::string> fmts;
  bool transpose = false;
  for (const auto& [nm, arg] : eoSubopts(mrhs)) {
    if (eoAbbr(nm, "Fmt")) fmts = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "Transpose")) transpose = true;
  }
  const eval::EvalMatrix* mat = nullptr;
  std::optional<eval::EvalMatrix> resolved;
  if (cls == 'e') {
    // `confirm matrix e(b)` (:4751) reads the live e(); e(b)/e(V) of a fit are
    // built by the one resolver every other matrix reader uses.
    int rc = 0;
    std::string why;
    resolved = session.eval.resolveEMatrix(bareName, &rc, &why);
    if (resolved.has_value()) mat = &*resolved;
  } else if (cls == 'r') {
    mat = session.eval.findClassMatrix('r', bareName);
  } else {
    for (const auto& kv : session.eval.matrices) {
      if (kv.first == matName) mat = &kv.second;
    }
  }
  if (mat == nullptr) {
    *err = "matrix " + matName + " not found";
    return false;
  }
  const size_t mr = mat->data.size();
  const size_t mc = mr == 0 ? 0 : mat->data[0].size();
  auto rowName = [&](size_t i) { return i < mat->rownames.size() ? mat->rownames[i] : "r" + std::to_string(i + 1); };
  auto colName = [&](size_t j) { return j < mat->colnames.size() ? mat->colnames[j] : "c" + std::to_string(j + 1); };
  auto at = [&](size_t i, size_t j) {
    return (i < mr && j < mat->data[i].size()) ? mat->data[i][j] : kEoNaN;
  };
  const size_t nr = transpose ? mc : mr;
  const size_t nc = transpose ? mr : mc;
  EoModel em;
  em.name = matName;
  sp->cells = EoCells{};
  sp->cells.rows.emplace_back();
  size_t fi = 0;
  for (size_t j = 0; j < nc; ++j) {
    EoElem el;
    el.name = "c" + std::to_string(j + 1);
    if (!fmts.empty()) {
      el.fmt = {fmts[std::min(fi, fmts.size() - 1)]};
      ++fi;
      if (fi >= fmts.size()) fi = 0;
    }
    el.label = transpose ? rowName(j) : colName(j);
    el.labelSet = true;
    sp->cells.elems.push_back(el);
    sp->cells.rows.back().push_back({static_cast<int>(j)});
    std::vector<EoKey> order;
    std::map<EoKey, double> vals;
    for (size_t i = 0; i < nr; ++i) {
      // A row's equation (`y:x1` of a system e(b)) is its own EoKey equation
      // (QuotedRowNames/roweq, :4759-4761; an empty one is "__", :4789).
      const std::string full = transpose ? colName(i) : rowName(i);
      const size_t colon = full.find(':');
      const EoKey key = colon == std::string::npos
                            ? EoKey{"__", full}
                            : EoKey{full.substr(0, colon), full.substr(colon + 1)};
      order.push_back(key);
      vals[key] = transpose ? at(j, i) : at(i, j);
    }
    em.extraOrder[el.name] = order;
    em.extra[el.name] = vals;
  }
  sp->cells.ncols = nc;
  for (auto& el : sp->cells.elems) {
    if (el.fmt.empty()) el.fmt = {"%9.0g"};
  }
  sp->models.push_back(em);
  return true;
}


// `matrix(A, fmt(...))` written where a namelist goes; returns its argument.
std::optional<std::string> eoMatrixNamelist(const std::vector<std::string>& words) {
  std::string joined;
  for (const auto& w : words) {
  if (!joined.empty()) joined += " ";
  joined += w;
  }
  const std::string t = eoTrim(joined);
  if (t.rfind("matrix(", 0) == 0 && t.back() == ')') return t.substr(7, t.size() - 8);
  // `estout e(b, transpose)` / `estout r(table)`: MatrixMode's e()/r() spelled
  // where the namelist goes (estout `MatrixMode, `anything''). Kept
  // wrapped so eoMatrixModel knows the class.
  if ((t.rfind("e(", 0) == 0 || t.rfind("r(", 0) == 0) && t.back() == ')') return t;
  return std::nullopt;
}
}
namespace piet::handlers {
using namespace table_detail;


// ===========================================================================
// estout (W2AK): estout.ado's option parse (estout) onto the shared
// writer above.
// ===========================================================================
ojson runEstout(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstout.known,
                                            kVocabEstout.unimplemented)) {
    return ojson(*err);
  }
  const auto& opts = parsed.opts;
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto eoFail = [](const EoError& e) { return tblErrorRc(e.msg, e.rc); };

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);

  EoSpec sp;
  sp.session = &session;
  EoError eerr;

  // ---- matrix mode (estout) -------------------------------------
  std::optional<std::string> matrixOpt;
  for (const char* k : {"matrix", "e", "r"}) {
    if (auto v = str(k)) {
      if (matrixOpt.has_value()) return tblErrorRc("only one of matrix(), e(), or r() allowed", 198);
      matrixOpt = (std::string(k) == "matrix") ? *v : std::string(k) + "(" + *v + ")";
    }
  }
  if (!matrixOpt.has_value()) matrixOpt = eoMatrixNamelist(words);
  std::vector<Model> models;
  if (!matrixOpt.has_value()) {
    ojson err;
    models = collectModels(session, "estout", words, &err);
    if (models.empty()) return err;
  }

  // ---- style defaults (estout) ----------------------------------------
  std::string style = str("style").has_value() ? strutil::lower(eoTrim(dequote(*str("style"))))
                                               : (usingPath.has_value() ? "" : "smcl");
  if (style == "esttab") style = "tab";
  if (style != "" && style != "tab" && style != "fixed" && style != "tex" && style != "smcl") {
    if (style == "html" || style == "mmd" || style == "md") {
      return ojson(eval::refusedCommand("estout", "no_" + style + "_writer"));
    }
    return tblErrorRc(style + " style not available (file estout_" + style + ".def not found)", 601);
  }
  const bool label = (has("label") || has("labels")) && !has("nolabel");
  bool abbrev = has("abbrev") && !has("noabbrev");
  std::vector<int> mwv;
  if (auto mwo = str("modelwidth")) mwv = eoIntList(*mwo);
  int varwidth = -1;
  if (auto vwo = str("varwidth")) {
    const std::vector<int> v = eoIntList(*vwo);
    if (!v.empty()) varwidth = v[0];
  }
  sp.delimiter = "\t";
  sp.interaction = " # ";
  if (auto v = str("delimiter")) sp.delimiter = eoStringArg(v);
  if (auto v = str("begin")) sp.begin = eoStringArg(v);
  if (auto v = str("end")) sp.end = eoStringArg(v);
  if (auto v = str("lastend")) {
    sp.lastend = eoStringArg(v);
    sp.lastendSet = true;
  }
  if (auto v = str("interaction")) sp.interaction = eoStringArg(v);
  const bool eqMerge = false;
  const double varwidthfactor = 1.0 + ((eqMerge && !has("unstack")) ? 0.5 : 0.0);
  if (style == "smcl" || style == "fixed" || style == "tex") {
    if (varwidth < 0) varwidth = static_cast<int>((label ? 20 : 12) * varwidthfactor);
    if (mwv.empty()) mwv = {12};
  }
  if (style == "smcl" || style == "fixed") {
    if (!has("noabbrev")) abbrev = true;
    if (!str("delimiter").has_value()) sp.delimiter = " ";
  } else if (style == "tex") {
    if (!str("delimiter").has_value()) sp.delimiter = "&";
    if (!str("end").has_value()) sp.end = "\\\\";
    if (!str("interaction").has_value()) sp.interaction = " $\\times$ ";
  }
  sp.varwidth = varwidth < 0 ? 0 : varwidth;
  sp.modelwidth = mwv.empty() ? std::vector<int>{0} : mwv;
  sp.abbrev = abbrev;
  sp.wrap = has("wrap") && !has("nowrap");
  sp.lz = !has("nolz");
  if (auto ic = str("incelldelimiter")) sp.incelldelimiter = eoStringArg(ic);
  if (auto hl = str("hlinechar")) sp.hlinechar = eoStringArg(hl);
  if (auto v = str("substitute")) {
    const std::vector<std::string> t = eoTokens(*v);
    for (size_t i = 0; i < t.size(); i += 2) sp.substitute.emplace_back(t[i], i + 1 < t.size() ? t[i + 1] : "");
  }
  sp.label = label;
  if (label) session.syncEvalVarLabels();
  sp.unstack = has("unstack") && !has("nounstack");
  sp.omitted = !has("noomitted");
  sp.baselevels = !has("nobaselevels");
  sp.legend = has("legend") && !has("nolegend");
  sp.title = str("title").has_value() ? eoStringArg(str("title")) : std::string();
  sp.note = str("note").has_value() ? eoStringArg(str("note")) : std::string();
  if (auto ph = str("prehead")) sp.prehead = eoHookLines(*ph);
  if (auto ph = str("posthead")) sp.posthead = eoHookLines(*ph);
  if (auto pf = str("prefoot")) sp.prefoot = eoHookLines(*pf);
  if (auto pf = str("postfoot")) sp.postfoot = eoHookLines(*pf);
  if (auto xc = str("extracols")) sp.extracols = eoIntList(*xc);

  // ---- cells (estout, 356-365, 398-407) ------------------------------------
  if (auto c = str("cells")) {
    std::string cerr;
    if (!eoParseCells(*c, &sp.cells, &cerr)) return tblErrorRc(cerr, 198);
  } else {
    // estout's default is cells(b); the bare se/t/p flags this build has long
    // accepted add a parenthesised second row (BUG-486's arms).
    EoElem b;
    b.name = "b";
    sp.cells.elems.push_back(b);
    sp.cells.rows.push_back({{0}});
    for (const char* f : {"se", "t", "p"}) {
      if (has(f)) {
        EoElem e;
        e.name = f;
        e.hasPar = true;
        e.par = {"(", ")"};
        sp.cells.elems.push_back(e);
        sp.cells.rows.push_back({{static_cast<int>(sp.cells.elems.size()) - 1}});
        break;
      }
    }
    sp.cells.ncols = 1;
    if (auto f = str("fmt")) sp.cells.elems[0].fmt = eoTokens(*f);
  }
  // Formats: the first element's fmt, default %9.0g, is every fmt-less element's.
  if (!sp.cells.elems.empty()) {
    if (sp.cells.elems[0].fmt.empty()) sp.cells.elems[0].fmt = {"%9.0g"};
    for (auto& el : sp.cells.elems) {
      if (el.fmt.empty()) el.fmt = sp.cells.elems[0].fmt;
    }
  }
  if (auto st = str("stats")) {
    const std::string firstFmt =
        (!sp.cells.elems.empty() && !sp.cells.elems[0].fmt.empty()) ? sp.cells.elems[0].fmt[0] : "%9.0g";
    if (!eoParseStats(*st, firstFmt, &sp, &eerr)) return eoFail(eerr);
  }
  bool anyStar = false;
  for (const auto& el : sp.cells.elems) {
    if (el.star) anyStar = true;
  }
  if (auto sl = str("starlevels")) {
    std::string lab = sp.starlevelsLabel, del = sp.starlevelsDelim;
    if (!eoParseStarPairs(*sl, &sp.starlevels, &lab, &del, &eerr)) return eoFail(eerr);
    sp.starlevelsLabel = lab;
    sp.starlevelsDelim = del;
  }
  (void)anyStar;

  // ---- labels -----------------------------------------------------------------------------
  auto parseLab = [&](const char* k, EoLabelOpt* o) -> bool {
    if (auto v = str(k)) return eoParseLabelOpt(*v, o, &eerr.msg);
    return true;
  };
  if (!parseLab("mlabels", &sp.mlabels) || !parseLab("collabels", &sp.collabels) ||
      !parseLab("eqlabels", &sp.eqlabels) || !parseLab("varlabels", &sp.varlabels) ||
      !parseLab("mgroups", &sp.mgroups)) {
    return tblErrorRc(eerr.msg, 198);
  }
  sp.numbers = has("numbers") && !has("nonumbers");
  if (auto nb = str("numbers")) {
    const std::vector<std::string> t = eoTokens(*nb);
    sp.numL = t.size() > 0 ? t[0] : std::string();
    sp.numR = t.size() > 1 ? t[1] : std::string();
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

  // ---- coefficient selection --------------------------------------------------------------
  sp.keep = str("keep");
  sp.drop = str("drop");
  sp.order = str("order");
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

  // ---- the models ---------------------------------------------------------------------------
  TransformSpec trSpec;
  if (auto tr = str("transform")) {
    std::string trErr;
    if (!parseTransformOpt(*tr, &trSpec, &trErr)) return tblSyntax(trErr);
  }
  if (matrixOpt.has_value()) {
    std::string merr;
    if (!eoMatrixModel(session, *matrixOpt, &sp, &merr)) return tblErrorRc(merr, 111);
  } else {
    for (size_t mi = 0; mi < models.size(); ++mi) {
      sp.models.push_back(eoModelFrom(models[mi], models[mi].name, session,
                                      trSpec.empty() ? nullptr : &trSpec, mi, !sp.omitted,
                                      sp.baselevels));
    }
    for (size_t mi = 0; mi < sp.models.size(); ++mi) sp.models[mi].model = &models[mi];
  }

  std::string text;
  int ccols = -1;
  if (!eoRender(sp, &text, &eerr, &ccols)) return eoFail(eerr);
  ojson result;
  if (!usingPath.has_value()) {
    result = echoBlock(text);
  } else {
    result = emitTable("estout", session, *usingPath, text, has("replace"), has("append"));
  }
  if (!isErrorBlock(result)) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "estout";
  }
  return result;
}
}
