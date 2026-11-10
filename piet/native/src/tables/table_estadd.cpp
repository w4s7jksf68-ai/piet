// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// table estadd.
#include "tables_internal.h"
#include "estout_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// Raw-argument recovery
//
// `parsed.varlist` is WHITESPACE-split (tokenizer.cpp:685), so
// `estadd local controls "Yes, with FE"` arrives as four tokens with the quotes
// broken across two of them. Every place below that needs a user STRING reads
// it out of `parsed.raw` instead. The comma cut uses the tokenizer's own
// quote-aware scanner rather than a plain find(','), because a comma inside the
// text is not an option separator.
// ===========================================================================

std::string argsAfterHead(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return {};
  const long long comma = eval::findUnquotedComma(raw);
  std::string body = (comma == -1) ? raw : raw.substr(0, static_cast<size_t>(comma));
  if (e >= body.size()) return {};
  return strutil::trim(body.substr(e));
}


// First `:` outside quotes and parens -- estadd's `... : m1 m2` namelist split.
long long findUnquotedColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i < quoted.size() && quoted[i]) continue;
    if (s[i] == '(' || s[i] == '[') ++depth;
    else if (s[i] == ')' || s[i] == ']') --depth;
    else if (s[i] == ':' && depth == 0) return static_cast<long long>(i);
  }
  return -1;
}


// Peel the leading word off a string, returning it and advancing `rest`.
std::string takeWord(std::string* rest) {
  const std::string s = strutil::trim(*rest);
  const size_t sp = s.find_first_of(" \t");
  if (sp == std::string::npos) {
    *rest = "";
    return s;
  }
  *rest = strutil::trim(s.substr(sp));
  return s.substr(0, sp);
}


// ===========================================================================
// Fit-sample column values (estadd ysumm)
//
// The estimation sample is not the loaded dataset: `regress y x if region<=4`
// fits on a subset, and Stata's `estadd ysumm` summarises e(sample), not the
// frame. LastModel records the two shapes this codebase uses for that -- a
// materialized scratch file (`fitDataPath`) and/or an engine-side `--keepif`
// expression -- and this reproduces the same reconstruction handlers_resample.
// cpp:480-500 does, rather than reading the head and being quietly wrong by a
// few hundred observations (BUG-163's measured failure was 3,106 vs 4,649).
// ===========================================================================

bool fitSampleValues(SessionState& session, const std::string& var,
                     std::vector<double>* out, std::string* errOut) {
  if (!session.currentHbin().has_value()) {
    *errOut = "no dataset in memory";
    return false;
  }
  std::string basePath = *session.currentHbin();
  std::string keepif;
  // BUG-1209: the fit's own e(sample) mask on the data, when it has one, IS
  // its sample; replaying a fit condition that reads e(sample) itself (`xtreg
  // y x1 if e(sample), fe`) on the live data selected no rows (StataBE: ymean
  // 2.2668824339561078 there).
  const SessionState::EsampleReading sample = session.esampleReading();
  if (sample.kind == SessionState::EsampleReading::Kind::kColumn) {
    keepif = std::string("(") + kEsampleColumn + "==" + std::to_string(sample.serial) + ")";
  } else if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    keepif = lm.fitKeepif;
  }
  if (!keepif.empty()) {
    const std::string filtered = mintScratchPath(session.workdir(), "estadd_ysumm");
    const CobraRunResult cr =
        runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
    if (!cr.ok || !cecon::native_exists(filtered)) {
      *errOut = "could not reconstruct the estimation sample";
      return false;
    }
    basePath = filtered;
  }
  try {
    Datastore ds(basePath);
    if (!ds.has(var)) {
      *errOut = "variable " + var + " not found";
      return false;
    }
    const cecon::Column& c = ds.col(var);
    if (c.dtype == cecon::DType::Factor) {
      *errOut = var + " is a string variable";
      return false;
    }
    const double* d = c.f64();
    const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
    out->clear();
    out->reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      if (!std::isnan(d[i])) out->push_back(d[i]);
    }
  } catch (const std::exception& e) {
    *errOut = e.what();
    return false;
  }
  return true;
}



// `r(name)` -> its number, for the expression estadd evaluates itself: the
// macro layer leaves bare r() on an estadd line alone (eval/macros.cpp), so
// `estadd r(mean)` and `estadd scalar r(sd)` reach the handler as written.
std::string estaddSubstR(const std::string& expr, SessionState& session) {
  std::string out;
  size_t i = 0;
  while (i < expr.size()) {
    // `r(` at a name boundary: the byte before is not a (Unicode-permissive)
    // identifier byte, so `دخلr(x)` is not an r() reference.
    size_t refEnd = 0;
    std::string name;
    if (expr[i] == 'r' && i + 1 < expr.size() && expr[i + 1] == '(' &&
        (i == 0 || !cecon::ident::isIdentByte(expr[i - 1]))) {
      size_t j = i + 2;
      while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
      const size_t b = j;
      if (j < expr.size() && cecon::ident::isIdentStartByte(expr[j])) {
        while (j < expr.size() && cecon::ident::isIdentByte(expr[j])) ++j;
        name = expr.substr(b, j - b);
        while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
        if (j < expr.size() && expr[j] == ')') refEnd = j + 1;
      }
    }
    if (refEnd == 0) {
      out += expr[i++];
      continue;
    }
    const double* v = session.eval.rValues.find(name);
    if (v != nullptr) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", *v);
      out += std::isnan(*v) ? std::string(".") : std::string(buf);
    } else {
      out += expr.substr(i, refEnd - i);
    }
    i = refEnd;
  }
  return out;
}
}
namespace piet::handlers {
using namespace table_detail;


// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// estadd (estout package)
// ===========================================================================

// ---------------------------------------------------------------------------
// BUG-408 / BUG-409 / BUG-421 — the ONE root cause behind all three.
//
// estadd and estpost published every addition into the TABLE bag
// (`session.estAdded` / the stored estimate's `added` object), which is what
// esttab/estout read — and into nothing else. The three surfaces that answer
// `e(NAME)` for a USER read a different store each:
//
//   `matrix M = e(mean)` / `matlist e(mean)`  -> session.eval.eMatrices
//   `display e(k)`                            -> display_hardening's resolver
//   `` `e(controls)' ``                       -> the macro layer's e() stores
//
// so estpost posted seven matrices nothing could copy (rc 111), estadd posted a
// scalar `display e(k)` answered `.` for, and `estadd local` posted a macro the
// macro layer could not see at all. The table was right and every other reader
// was blind: the two-layer divergence class this file's own header warns about.
//
// The fix is to publish into BOTH: the bag stays exactly as it was (every
// renderer, every frozen table fixture unchanged) and the session e() stores
// gain the same names. `pca` already does this — it is the shape a poster is
// supposed to have.
void mirrorIntoEStores(SessionState& session,
                       const std::vector<std::pair<std::string, double>>& scalars,
                       const std::vector<std::pair<std::string, std::string>>& locals,
                       const std::vector<std::pair<std::string, ojson>>& matrices) {
  // An estadd addition lives on top of the fitted e(): the next model install
  // drops it (SessionState::eOnFitNames), as Stata's new fit replaces e()
  // whole -- `estadd r(mean)` after one regress and `estadd mean` after the
  // next must find no e(mean) left over (w2ak_estout_family.log ek_ad_mean).
  for (const auto& [n, v] : scalars) session.noteEOnFit(n);
  for (const auto& [n, v] : locals) session.noteEOnFit(n);
  for (const auto& [n, m] : matrices) session.noteEOnFit(n);
  for (const auto& [n, v] : scalars) session.eval.setEScalar(n, v);
  for (const auto& [n, v] : locals) session.eval.setEString(n, v);
  for (const auto& [n, m] : matrices) {
    eval::EvalMatrix em;
    if (m.contains("rownames") && m["rownames"].is_array())
      em.rownames = m["rownames"].get<std::vector<std::string>>();
    if (m.contains("colnames") && m["colnames"].is_array())
      em.colnames = m["colnames"].get<std::vector<std::string>>();
    if (m.contains("data") && m["data"].is_array()) {
      for (const auto& row : m["data"]) {
        std::vector<double> r;
        if (row.is_array()) {
          for (const auto& cell : row) {
            // jsonNum's rule, transcribed rather than called: that helper is
            // defined further down this file, and a forward declaration for two
            // lines would be the wider change.
            r.push_back(cell.is_number() ? cell.get<double>()
                                         : std::numeric_limits<double>::quiet_NaN());
          }
        }
        em.data.push_back(std::move(r));
      }
    }
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == n) { kv.second = em; replaced = true; break; }
    }
    if (!replaced) session.eval.eMatrices.emplace_back(n, std::move(em));
  }
}


ojson runEstadd(const eval::ParsedCommand& parsedIn, SessionState& session) {
  // estadd `_on_colon_parse` splits at the colon BEFORE syntax runs,
  // so `estadd local fe "No", replace: n1` is options `replace` and the target
  // n1 (w2ak ek_ad_colon_repl, log:577). The generic option parser saw an
  // option named `replace:`; the options are re-read from the text before the
  // colon instead.
  eval::ParsedCommand parsed = parsedIn;
  std::string colonTargets;
  bool colonAfterComma = false;
  {
    const std::string full = parsed.raw;
    const long long comma = eval::findUnquotedComma(full);
    const long long colonAll = findUnquotedColon(full);
    if (comma >= 0 && colonAll > comma) {
      colonAfterComma = true;
      colonTargets = strutil::trim(full.substr(static_cast<size_t>(colonAll) + 1));
      const std::string optText =
          full.substr(static_cast<size_t>(comma) + 1, static_cast<size_t>(colonAll - comma - 1));
      parsed.opts.clear();
      for (const auto& w : splitTopLevel(optText)) {
        eval::OptValue ov;
        const size_t open = w.find('(');
        if (open != std::string::npos && w.back() == ')') {
          ov.isBool = false;
          ov.strValue = w.substr(open + 1, w.size() - open - 2);
          parsed.opts.emplace_back(strutil::lower(w.substr(0, open)), ov);
        } else {
          ov.isBool = true;
          ov.boolValue = true;
          parsed.opts.emplace_back(strutil::lower(w), ov);
        }
      }
      eval::canonicalizeForCommand(parsed.opts, "estadd");
    }
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstadd.known,
                                            kVocabEstadd.unimplemented)) {
    return ojson(*err);
  }

  std::string args = argsAfterHead(parsed.raw);
  if (colonAfterComma) args = strutil::trim(args) + " : " + colonTargets;
  if (args.empty()) {
    // A BARE `estadd` has no sub-verb at all, which is a syntax error rather
    // than an unimplemented feature -- kUnknownSubcommand would name "" as the
    // subcommand.
    return tblSyntax("estadd: a subcommand is required (local, scalar, matrix or ysumm)");
  }

  // `estadd ... : m1 m2` targets ALREADY-STORED estimates rather than the
  // active e(). Split it off before anything else so the sub-verb parser never
  // sees the namelist.
  std::vector<std::string> targets;
  const long long colon = findUnquotedColon(args);
  if (colon >= 0) {
    targets = splitWs(args.substr(static_cast<size_t>(colon) + 1));
    args = strutil::trim(args.substr(0, static_cast<size_t>(colon)));
    if (targets.empty()) {
      return tblSyntax("estadd: `:' must be followed by one or more stored estimate names");
    }
  }

  std::string sub = strutil::lower(takeWord(&args));
  // estadd: the abbreviations exist as programs of their own.
  if (sub == "loc" || sub == "loca") sub = "local";
  if (sub == "sca" || sub == "scal" || sub == "scala") sub = "scalar";
  if (sub == "mat" || sub == "matr" || sub == "matri") sub = "matrix";
  // estadd: `estadd r(name)` is `estadd scalar r(name)` or
  // `estadd matrix r(name)`, whichever r() holds; neither is r(111).
  {
    std::string nm;
    if (parseWholeRName(sub, &nm)) {
      if (session.eval.rValues.find(nm) != nullptr) {
        args = sub + (args.empty() ? std::string() : " " + args);
        sub = "scalar";
      } else if (session.eval.findClassMatrix('r', nm) != nullptr) {
        args = nm + " = " + sub + (args.empty() ? std::string() : " " + args);
        sub = "matrix";
      } else {
        return tblErrorRc(sub + " not found", 111);
      }
    } else if (!strutil::isValidStataIdentifier(sub)) {
      return tblSyntax("invalid subcommand");
    }
  }

  // The sub-verbs that are real estadd features this build does not compute.
  // `unsupportedCommand(..., kNotImplemented)` -- NOT a bare engineError, whose
  // default code answers rc 1 instead of 199.
  static const std::set<std::string> kNotImplementedSubs = {"beta", "vif",
                                                            "expb",  "summ", "sd",
                                                            "svy",   "margins"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (sub != "local" && sub != "scalar" && sub != "matrix" && sub != "ysumm" &&
      sub != "ysummarize" && sub != "mean") {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // estadd confirm_esample: the e(sample) subcommands stop with r(498)
  // when the active results carry none (w2ak ek_ad_ysumm_noest, after
  // `ereturn clear`).
  if ((sub == "ysumm" || sub == "ysummarize" || sub == "mean") && targets.empty() &&
      !session.lastEstBlock.has_value()) {
    return tblErrorRc("e(sample) information not available", 498);
  }
  if (!session.lastEstBlock.has_value() && targets.empty()) {
    // Stata answers rc 301 ("last estimates not found") here, which coarseRc's
    // message rules do not know, so it is stated through `reason`.
    return tblErrorRc("estadd: no estimation result in memory. Run a regression first.", 301);
  }

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::string fp =
      session.lastEstBlock.has_value()
          ? SessionState::estAddFingerprint(*session.lastEstBlock)
          : std::string();

  // ---- gather what this sub-verb wants to add -----------------------------
  // Collected into a staging bag first so a failure adds NOTHING: a half-applied
  // `estadd ysumm` would leave e() carrying four of its eight scalars with no
  // diagnostic about the other four.
  std::vector<std::pair<std::string, double>> addScalars;
  std::vector<std::pair<std::string, std::string>> addLocals;
  std::vector<std::pair<std::string, ojson>> addMatrices;

  if (sub == "local") {
    const std::string name = takeWord(&args);
    if (name.empty()) return tblSyntax("estadd local: a name is required");
    if (!strutil::isValidStataIdentifier(name)) {
      return tblSyntax("estadd local: invalid name " + name);
    }
    addLocals.emplace_back(name, dequote(strutil::trim(args)));
  } else if (sub == "scalar") {
    // estadd. `estadd scalar k = e(F)`, `estadd scalar k e(F)`,
    // `estadd scalar NAME` (copies the scalar NAME) and `estadd scalar r(sd)`
    // (posts e(sd) = r(sd)).
    std::string rest = args;
    std::string name, expr;
    const std::string restTrim = strutil::trim(rest);
    if (parseWholeRName(restTrim, &name)) {
      if (session.eval.rValues.find(name) == nullptr) return tblSyntax("invalid syntax");
      expr = restTrim;
    } else {
      const size_t eq = rest.find('=');
      if (eq == std::string::npos) {
        name = takeWord(&rest);
        expr = strutil::trim(rest);
      } else {
        name = strutil::trim(rest.substr(0, eq));
        expr = strutil::trim(rest.substr(eq + 1));
      }
      if (name.empty() || !strutil::isValidStataIdentifier(name)) {
        return tblSyntax("invalid syntax");
      }
      if (expr.empty()) {
        // `estadd scalar NAME` -> scalar(NAME)
        const std::string* sv = session.eval.scalars.find(name);
        if (sv == nullptr) return tblErrorRc("scalar " + name + " not found", 111);
        expr = *sv;
      }
    }
    if (auto pf = optStr(parsed.opts, "prefix")) name = strutil::trim(*pf) + name;
    expr = estaddSubstR(expr, session);
    // ONE e() reader for the whole bundle: `evalScalarExpr` is the chain
    // `display` already runs (whole-token resolution, then e()/_b[]
    // substitution, then arithmetic). A second reader here is how the two
    // e()-scalar tables in this tree started.
    std::optional<double> v = piet::evalScalarExpr(expr, session);
    if (!v.has_value()) {
      // evalScalarExpr answers "no value" for a MISSING result on purpose (a
      // table cell must not print it as 0). estadd stores missing as missing:
      // StataBE `estadd scalar bc = 0.098/0.000` is rc 0 with e(bc) = .
      // (scratch-r150/uni-oracle es.do; 007's tables divide by a mean shown as
      // 0.000). Only an expression that evaluates cleanly to a missing token
      // takes this path; anything that fails to evaluate is still refused.
      std::string resolveErr;
      const std::string joined = eval::resolveEstScalarRefs(expr, session.eval, &resolveErr);
      if (resolveErr.empty()) {
        std::string raised;
        bool isString = false;
        const auto r = eval::evalArith(joined, &session.eval, &raised, &isString);
        if (r.has_value() && !isString && raised.empty()) {
          const std::string t = strutil::trim(*r);
          if (t == "." || (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z'))
            v = std::numeric_limits<double>::quiet_NaN();
        }
      }
    }
    if (!v.has_value()) {
      return tblError("estadd scalar: could not evaluate `" + expr + "'");
    }
    addScalars.emplace_back(name, *v);
  } else if (sub == "matrix") {
    // estadd: `estadd matrix NAME` copies the matrix NAME,
    // `estadd matrix NAME [=] M` copies M (w2ak ek_e20 `estadd matrix qval :
    // m2 m6`).
    std::string rest = args;
    const size_t eq = rest.find('=');
    std::string name, src;
    if (eq == std::string::npos) {
      name = takeWord(&rest);
      src = strutil::trim(rest);
      if (src.empty()) src = name;
    } else {
      name = strutil::trim(rest.substr(0, eq));
      src = strutil::trim(rest.substr(eq + 1));
    }
    if (name.empty() || src.empty() || !strutil::isValidStataIdentifier(name)) {
      return tblSyntax("invalid syntax");
    }
    if (auto pf = optStr(parsed.opts, "prefix")) name = strutil::trim(*pf) + name;
    const eval::EvalMatrix* mat = session.eval.findMatrix(src);
    if (mat == nullptr) {
      // coarseRc maps "matrix ... not found" through its "not found" rule to
      // rc 111, which is Stata's own code for an unknown matrix name.
      return tblError("matrix " + src + " not found");
    }
    ojson m;
    m["rownames"] = mat->rownames;
    m["colnames"] = mat->colnames;
    ojson data = ojson::array();
    for (const auto& row : mat->data) data.push_back(row);
    m["data"] = data;
    addMatrices.emplace_back(name, m);
  } else if (sub == "mean") {
    // estadd estadd_mean: e(<prefix>mean) is a copy of e(b) whose
    // column j is the e(sample) mean of variable j (`su VAR [aw] if e(sample),
    // meanonly`, pweight read as aweight), .z where the column is not a
    // numeric variable (_cons, a factor level) and . where summarize fails
    // (w2ak_estout_family.log ek_ad_mean `0|3|5.175|2.25|.z|`).
    if (!session.lastModel().has_value()) {
      return tblErrorRc("e(sample) information not available", 498);
    }
    std::string prefix;
    if (auto pf = optStr(parsed.opts, "prefix")) prefix = strutil::trim(dequote(*pf));
    int brc = 0;
    std::string bwhy;
    const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b", &brc, &bwhy);
    if (!eb.has_value() || eb->data.empty()) {
      return tblErrorRc(bwhy.empty() ? std::string("matrix e(b) not found") : bwhy,
                        brc != 0 ? brc : 111);
    }
    std::string wtype;
    if (const std::string* wt = session.eval.eStrings.find("wtype")) wtype = *wt;
    if (wtype == "pweight") wtype = "aweight";
    std::string wexp;
    if (const std::string* we = session.eval.eStrings.find("wexp")) wexp = *we;
    const std::string wclause = wtype.empty() ? std::string() : " [" + wtype + wexp + "]";
    // The variables in memory and whether each is numeric: the resident card
    // when it is current, else the datastore (the card avoids writing a dirty
    // virtual head to disk just to read names).
    std::map<std::string, bool> numericOf;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) numericOf[vi.name] = vi.dtype != "factor";
    } else if (session.hasData()) {
      if (Datastore* ds = session.datastore())
        for (const auto& nm : ds->names()) numericOf[nm] = ds->dtype(nm) != "factor";
    }
    std::vector<double> means;
    for (const auto& full : eb->colnames) {
      const size_t colon = full.find(':');
      const std::string var = colon == std::string::npos ? full : full.substr(colon + 1);
      const auto nit = numericOf.find(var);
      const bool numericVar = nit != numericOf.end() && nit->second;
      if (!numericVar) {
        means.push_back(eval::stataMissingWithKind(26));  // .z, the matrix layer's spelling
        continue;
      }
      const ojson r =
          piet::dispatch("quietly summarize " + var + wclause + " if e(sample), meanonly", session);
      bool failed = false;
      if (r.is_array()) {
        for (const auto& b : r)
          if (b.is_object() && b.value("type", std::string()) == "error") failed = true;
      }
      const double* m = session.eval.rValues.find("mean");
      means.push_back(failed || m == nullptr ? std::numeric_limits<double>::quiet_NaN() : *m);
    }
    ojson m;
    m["rownames"] = eb->rownames;
    m["colnames"] = eb->colnames;
    ojson data = ojson::array();
    data.push_back(means);
    m["data"] = data;
    addMatrices.emplace_back(prefix + "mean", m);
  } else {  // ysumm
    if (!session.lastModel().has_value() && !session.lastEstBlock.has_value()) {
      return tblErrorRc("estadd ysumm: no estimation result in memory", 301);
    }
    // eb-order T-P3: a posting with no fitted model behind it (estpost) has no
    // e(sample): Stata's estadd stops with r(498) "e(sample) information not
    // available" (w2v_estpost_ematrix.log ep14_estadd_ysumm, ep14n_estadd_ysumm).
    // BUG-1210: so do results read by `estimates use`, which carry no e(sample).
    if (!session.lastModel().has_value() ||
        session.lastModel()->esample == LastModel::EsampleState::kNone) {
      return tblErrorRc("e(sample) information not available", 498);
    }
    std::string depvar;
    if (session.lastEstBlock.has_value()) depvar = session.lastEstBlock->value("depvar", "");
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->depvar;
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->y;
    if (depvar.empty()) {
      return tblError("estadd ysumm: the last estimation does not name a dependent variable");
    }
    std::vector<double> vals, wts;
    std::string ferr;
    const LastModel& lmw = *session.lastModel();
    if (!lmw.fitWeight.empty()) {
      // estadd: `summarize ... [aw]` over e(sample) after a
      // weighted fit (pweight read as aweight). The sample is rebuilt as the
      // complete cases of the model's own variables in the data in memory and
      // CHECKED against e(N) (the beta rule); a fit with an if/in filter is
      // refused rather than summarised over the wrong rows.
      if (!lmw.fitKeepif.empty()) {
        return tblError("estadd ysumm: a weighted fit with an if/in restriction is not "
                        "summarised in this build");
      }
      try {
        Datastore& ds = *session.datastore();
        std::vector<std::string> need{depvar, lmw.fitWeight};
        for (const auto& x : lmw.x) {
          if (ds.has(x)) need.push_back(x);
        }
        std::vector<const double*> cols;
        int64_t nrows = -1;
        for (const auto& v : need) {
          if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) {
            return tblError("estadd ysumm: variable " + v + " not found");
          }
          const cecon::Column& c = ds.col(v);
          const int64_t nn = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (nrows < 0) nrows = nn;
          cols.push_back(c.f64());
        }
        for (int64_t i = 0; i < nrows; ++i) {
          bool ok = true;
          for (const double* c : cols) {
            if (std::isnan(c[i])) ok = false;
          }
          if (ok) {
            vals.push_back(cols[0][i]);
            wts.push_back(cols[1][i]);
          }
        }
      } catch (const std::exception& e) {
        return tblError(std::string("estadd ysumm: ") + e.what());
      }
      double nObs = 0.0;
      if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("n_obs") &&
          (*session.lastEstBlock)["n_obs"].is_number()) {
        nObs = (*session.lastEstBlock)["n_obs"].get<double>();
      }
      if (nObs > 0 && static_cast<double>(vals.size()) != nObs) {
        return tblError("estadd ysumm: the estimation sample cannot be reproduced from the data "
                        "in memory");
      }
    } else if (!fitSampleValues(session, depvar, &vals, &ferr)) {
      return tblError("estadd ysumm: " + ferr);
    }
    const double n = static_cast<double>(vals.size());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // summarize's weights: aweights rescale to sum to N (mean = sum(wx)/sum(w),
    // var = sum(w(x-m)^2)/sum(w) * N/(N-1)); fweights count observations.
    const bool fw = lmw.fitWtype == "fweight" || lmw.fitWtype == "fw";
    double sw = 0.0, sum = 0.0;
    for (size_t i = 0; i < vals.size(); ++i) {
      const double wi = wts.empty() ? 1.0 : wts[i];
      sw += wi;
      sum += wi * vals[i];
    }
    const double mean = sw > 0 ? sum / sw : nan;
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (size_t i = 0; i < vals.size(); ++i) {
      const double wi = wts.empty() ? 1.0 : wts[i] * (fw ? 1.0 : n / sw);
      const double d = vals[i] - mean;
      m2 += wi * d * d;
      m3 += wi * d * d * d;
      m4 += wi * d * d * d * d;
    }
    if (!wts.empty() && !fw) sum = mean * n;  // aweights: r(sum) is over rescaled weights
    const double nEff = (fw && !wts.empty()) ? sw : n;
    // Stata's summarize reports the SAMPLE variance (n-1) but POPULATION
    // skewness/kurtosis (n divisor, kurtosis NOT excess -- a normal draw
    // reports ~3, not ~0). Mixing those two conventions is the classic silent
    // wrong number here, so both are spelled out.
    const double var = nEff > 1 ? m2 / (nEff - 1) : nan;
    const double sd = n > 1 ? std::sqrt(var) : nan;
    const double m2p = nEff > 0 ? m2 / nEff : nan;
    const double skew = (nEff > 0 && m2p > 0) ? (m3 / nEff) / std::pow(m2p, 1.5) : nan;
    const double kurt = (nEff > 0 && m2p > 0) ? (m4 / nEff) / (m2p * m2p) : nan;
    double mn = nan, mx = nan;
    if (!vals.empty()) {
      mn = *std::min_element(vals.begin(), vals.end());
      mx = *std::max_element(vals.begin(), vals.end());
    }
    // estadd: the statistics asked for (default `sd max min
    // mean`), posted in the ado's order -- iqr semean cv range, then the
    // Stats list p99 ... sum mean filtered -- under prefix() (default `y`).
    std::string prefix = "y";
    if (auto pf = optStr(parsed.opts, "prefix")) prefix = strutil::trim(dequote(*pf));
    auto flag = [&](const char* f) { return hasFlag(parsed.opts, f); };
    const bool all = flag("all");
    std::vector<std::string> stats;
    const std::vector<std::string> kStats = {"p99", "p95", "p90", "p75", "p50", "p25", "p10", "p5",
                                             "p1",  "kurtosis", "skewness", "var", "sd", "max",
                                             "min", "sum", "mean"};
    bool p25 = flag("p25"), p50 = flag("p50"), p75 = flag("p75");
    if (flag("q")) p25 = p50 = p75 = true;
    if (flag("median")) p50 = true;
    for (const auto& st : kStats) {
      bool on = all || flag(st.c_str());
      if (st == "p25") on = all || p25;
      if (st == "p50") on = all || p50;
      if (st == "p75") on = all || p75;
      if (on) stats.push_back(st);
    }
    const bool iqr = all || flag("iqr"), semean = all || flag("semean"), cv = all || flag("cv"),
               range = all || flag("range");
    if (stats.empty() && !range && !cv && !semean && !iqr) stats = {"sd", "max", "min", "mean"};
    for (const auto& st : stats) {
      if (st[0] == 'p' && st != "p") {
        return ojson(eval::unsupportedOption(
            st, "estadd ysumm: the percentile statistics are not implemented in this build"));
      }
    }
    if (iqr) {
      return ojson(eval::unsupportedOption(
          "iqr", "estadd ysumm: the percentile statistics are not implemented in this build"));
    }
    std::map<std::string, double> val = {{"mean", mean}, {"sd", sd},   {"min", mn},
                                         {"max", mx},    {"sum", sum}, {"var", var},
                                         {"skewness", skew}, {"kurtosis", kurt}};
    if (semean) addScalars.emplace_back(prefix + "semean", n > 0 ? sd / std::sqrt(n) : nan);
    if (cv) addScalars.emplace_back(prefix + "cv", sd / mean);
    if (range) addScalars.emplace_back(prefix + "range", mx - mn);
    for (const auto& st : stats) addScalars.emplace_back(prefix + st, val[st]);
  }

  // ---- decide WHERE it lands ----------------------------------------------
  // `: m1 m2` writes into each named STORED estimate's own bag (a mutable
  // handle, which is why SessionState grew `estimatesGetMutable`); everything
  // else writes the live bag beside the active e().
  std::vector<ojson*> bags;
  if (!targets.empty()) {
    // W2 esttabnames (BUG-552): `estadd ... : A_*` -- the target list is a
    // namelist too (estadd.ado expands it against the stored names). The
    // active results (`.`) are not a stored bag; refused by name below.
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(targets);
    if (ex.rc != 0) return tblErrorRc(ex.message, ex.rc);
    for (const auto& t : ex.names) {
      if (t == ".") {
        return tblError("estadd: `.` in the `: namelist` (the active, unstored results) is not "
                        "supported in this version of PIET; run `estadd` without the colon to "
                        "add to the active results");
      }
      ojson* snap = session.estimatesGetMutable(t);
      if (snap == nullptr) {
        return tblError("estadd: no stored estimation result named " + t);
      }
      if (!snap->contains("added") || !(*snap)["added"].is_object()) {
        (*snap)["added"] = emptyBag();
      }
      for (const char* k : {"scalars", "locals", "matrices"}) {
        if (!(*snap)["added"].contains(k)) (*snap)["added"][k] = ojson::object();
      }
      bags.push_back(&(*snap)["added"]);
    }
  } else {
    if (session.estAddedFp != fp) {
      // A DIFFERENT estimation has replaced e() since the bag was opened.
      // Discarding is the rule; keeping would attach one model's additions to
      // another model's table -- a plausible number on the right-looking row.
      session.estAddedClear();
    }
    // The bag shape is EXACTLY {scalars, locals, matrices}: `runEststo` copies
    // it verbatim into the snapshot and every renderer here reads those three
    // keys, so a fourth key or a missing one is a silently empty table cell.
    for (const char* k : {"scalars", "locals", "matrices"}) {
      if (!session.estAdded.contains(k) || !session.estAdded[k].is_object()) {
        session.estAdded[k] = ojson::object();
      }
    }
    bags.push_back(&session.estAdded);
    // W2AK: when the active results ARE a stored model (`eststo p1: ...` then a
    // bare `estadd`), the stored copy carries the addition too -- `esttab p1,
    // stats(postst)` prints it (w2ak ek_ad_after_tab L11, log:637; the 069
    // shape ek_d01 `eststo:` then `estadd scalar mean = r(mean)`). estadd.ado
    // runs the subcommand on e() with e(_estimates_name) set, and the store is
    // what esttab reads back.
    if (!session.activeEstimatesName.empty() && session.lastEstBlock.has_value()) {
      ojson* snap = session.estimatesGetMutable(session.activeEstimatesName);
      if (snap != nullptr) {
        if (!snap->contains("added") || !(*snap)["added"].is_object()) (*snap)["added"] = emptyBag();
        for (const char* k : {"scalars", "locals", "matrices"}) {
          if (!(*snap)["added"].contains(k)) (*snap)["added"][k] = ojson::object();
        }
        bags.push_back(&(*snap)["added"]);
      }
    }
  }

  // ---- collision check across EVERY target, before writing ANY of them -----
  if (!replace) {
    for (ojson* bag : bags) {
      for (const auto& [n, v] : addScalars) {
        (void)v;
        if ((*bag)["scalars"].contains(n)) {
          // "already defined" is one of coarseRc's own measured message rules
          // (-> rc 110, Stata's confirm-new-name code), so this wording is the
          // rc as much as it is the sentence.
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addLocals) {
        (void)v;
        if ((*bag)["locals"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addMatrices) {
        (void)v;
        if ((*bag)["matrices"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
    }
  }

  for (ojson* bag : bags) {
    for (const auto& [n, v] : addScalars) (*bag)["scalars"][n] = v;
    for (const auto& [n, v] : addLocals) (*bag)["locals"][n] = v;
    for (const auto& [n, v] : addMatrices) (*bag)["matrices"][n] = v;
  }
  // BUG-408/409/421: the live e() gains the same names, so `display e(k)`,
  // `` `e(controls)' `` and `matrix M = e(mean)` can read what the table reads.
  // Only for the LIVE bag: `estadd ... : m1 m2` writes stored estimates, whose
  // e() is not the active one.
  if (targets.empty()) mirrorIntoEStores(session, addScalars, addLocals, addMatrices);
  // MIRROR the scalars into e() itself, so `display e(k)` answers 7 rather than
  // `.`. Stata's estadd genuinely ADDS to e() -- the bag is this build's way of
  // carrying the addition to `eststo`, not a substitute for e() being readable,
  // and a table row the macro layer cannot see is the pangram's own stated test
  // (S28 reads `e(s28k)` back immediately after adding it).
  //
  // Only names the estimate does NOT already carry: `estadd scalar r2 = 5` must
  // not overwrite the fitted R-squared behind `display e(r2)`. The addition
  // still reaches `stats(r2)` through the bag, so nothing is lost by declining
  // to shadow an engine key.
  // The two stores are written TOGETHER on purpose: `eval.lastModelResult` is
  // what macros.cpp's e() resolver reads and `lastEstBlock` is what the table
  // path reads, and updating one without the other is the two-layer divergence
  // this codebase keeps paying for.
  if (targets.empty() && session.lastEstBlock.has_value() && !addScalars.empty()) {
    ojson blk = *session.lastEstBlock;
    for (const auto& [n, v] : addScalars) {
      if (!blk.contains(n)) blk[n] = v;
    }
    session.lastEstBlock = blk;
    if (session.eval.lastModelResult.has_value()) {
      ojson res = *session.eval.lastModelResult;
      for (const auto& [n, v] : addScalars) {
        if (!res.contains(n)) res[n] = v;
      }
      session.eval.lastModelResult = res;
    }
  }
  // Re-stamp the fingerprint from the SHARED helper, unconditionally. It is
  // `SessionState::estAddFingerprint` and not a private copy on purpose:
  // `runEststo` READS it to decide whether the bag still belongs to the active
  // estimate, and two copies differing by one field would make the bag never
  // match -- an `estadd`ed row quietly missing from the table, at rc 0. The
  // mirror above cannot move it: the fingerprint is built from depvar,
  // estimator, n_obs and the coeftable LENGTH, none of which a new scalar key
  // touches.
  if (targets.empty() && session.lastEstBlock.has_value()) {
    session.estAddedFp = SessionState::estAddFingerprint(*session.lastEstBlock);
  }

  // One block per addition, as an ARRAY -- legal here because the arm routes
  // through `pushEnvResult` (which flattens) AND every element is a real block
  // object with a "type" key. See defect class 1 in this file's header.
  ojson out = ojson::array();
  if (!hasFlag(parsed.opts, "quietly")) {
    for (const auto& [n, v] : addScalars) {
      out.push_back(displayBlock("added scalar: e(" + n + ") = " + fmtNum(v, "%9.7g")));
    }
    for (const auto& [n, v] : addLocals) {
      out.push_back(displayBlock("added macro:  e(" + n + ") : \"" + v + "\""));
    }
    for (const auto& [n, v] : addMatrices) {
      (void)v;
      out.push_back(displayBlock("added matrix: e(" + n + ")"));
    }
  }
  return out;
}
}
