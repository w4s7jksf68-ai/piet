// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Nonlinear estimation.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/display_hardening.h"  // resolveVarName
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"
#include "estimator_command_support.h"  // e3::setE*OnFit

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

// ===========================================================================
// gmm — Stata's interactive GMM (gmm.sthlp): one or more residual equations
// `([eqname:] <expr>)`, each written over parameters {name} / {eq:varlist}; the
// two-step estimator by default (onestep, igmm), winitial(unadjusted|identity
// [, independent]), wmatrix() and vce() of type robust, unadjusted or cluster,
// fweight/aweight/pweight/iweight. The arithmetic is the engine's (`cobra gmm`,
// src/estimators/nlgmm.cpp gmm_fit_ex); this side turns the command line into
// Stata's substitutable expressions and publishes Stata's e() (vs-stata bank
// 6858022a and its option cells).
// ===========================================================================
namespace {

ojson gmmRc(const std::string& msg, int rc, const std::string& code = "syntax_error") {
  return ojson(eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

std::string gmmTrim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> gmmWords(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> w;
  std::string t;
  while (iss >> t) w.push_back(t);
  return w;
}

bool gmmIsName(const std::string& s) {
  static const std::regex re(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  return std::regex_match(s, re);
}

// `word` is `full` abbreviated to at least `minLen` characters.
bool gmmAbbrev(const std::string& word, const std::string& full, size_t minLen) {
  const std::string w = lowerM4(word);
  return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
}

struct GmmEq {
  std::string name;        // "" when unnamed
  std::string sexp;        // Stata's e(sexp_#)
  std::string engineExpr;  // the same with {p=init} initial values
  std::vector<std::string> params;
};

// One substitutable expression -> Stata's canonical form: blanks dropped, a free
// parameter {b0} is {b0:_cons}, a linear combination {xb:mpg weight} is
// ({xb:mpg}*mpg+{xb:weight}*weight) (one variable: {xb:mpg}*mpg). A variable
// named in a combination must exist (r(111)).
std::optional<ojson> gmmCanon(const std::string& body, SessionState& session, GmmEq& eq) {
  std::string out, eng;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    if (c != '{') {
      out += c;
      eng += c;
      continue;
    }
    const size_t close = body.find('}', i);
    if (close == std::string::npos) return gmmRc("unmatched braces in the moment expression", 198);
    std::string in = gmmTrim(body.substr(i + 1, close - i - 1));
    i = close;
    std::string init;
    const size_t eqs = in.find('=');
    if (eqs != std::string::npos) {
      init = gmmTrim(in.substr(eqs + 1));
      in = gmmTrim(in.substr(0, eqs));
    }
    const size_t colon = in.find(':');
    auto addParam = [&](const std::string& p) {
      if (std::find(eq.params.begin(), eq.params.end(), p) == eq.params.end()) eq.params.push_back(p);
    };
    if (colon == std::string::npos) {
      if (!gmmIsName(in)) return gmmRc("invalid parameter name {" + in + "}", 198);
      const std::string p = in + ":_cons";
      addParam(p);
      out += "{" + p + "}";
      eng += "{" + p + (init.empty() ? "" : "=" + init) + "}";
      continue;
    }
    const std::string eqn = gmmTrim(in.substr(0, colon));
    std::vector<std::string> vars = gmmWords(in.substr(colon + 1));
    if (!gmmIsName(eqn) || vars.empty()) return gmmRc("invalid parameter specification {" + in + "}", 198);
    // `_cons` anywhere in the list is the combination's constant term: taken out of
    // the variables and appended last, as the bare parameter {eq:_cons}
    // (gmm.sthlp: "Specify the system variable _cons in varlist to include a
    // constant term"); an initial value needs exactly one variable.
    const bool hasCons = std::find(vars.begin(), vars.end(), "_cons") != vars.end();
    vars.erase(std::remove(vars.begin(), vars.end(), "_cons"), vars.end());
    if (!init.empty() && vars.size() > 1) return gmmRc("invalid parameter specification {" + in + "=" + init + "}", 198);
    if (vars.empty()) {
      addParam(eqn + ":_cons");
      out += "{" + eqn + ":_cons}";
      eng += "{" + eqn + ":_cons" + (init.empty() ? "" : "=" + init) + "}";
      continue;
    }
    std::string so, se;
    size_t terms = 0;
    for (size_t v = 0; v < vars.size(); ++v) {
      bool ambiguous = false;
      const auto full = resolveVarName(vars[v], session, &ambiguous);
      if (!full.has_value())
        return gmmRc("variable " + vars[v] + (ambiguous ? " ambiguous abbreviation" : " not found"), 111);
      const std::string p = eqn + ":" + *full;
      addParam(p);
      so += std::string(terms ? "+" : "") + "{" + p + "}*" + *full;
      se += std::string(terms ? "+" : "") + "{" + p + (init.empty() ? "" : "=" + init) + "}*" + *full;
      ++terms;
    }
    if (hasCons) {
      const std::string p = eqn + ":_cons";
      addParam(p);
      so += "+{" + p + "}";
      se += "+{" + p + (init.empty() ? "" : "=" + init) + "}";
      ++terms;
    }
    if (terms > 1) {
      so = "(" + so + ")";
      se = "(" + se + ")";
    }
    out += so;
    eng += se;
  }
  eq.sexp = out;
  eq.engineExpr = "(" + eng + ")";
  return std::nullopt;
}

// vce()/wmatrix() type: robust | unadjusted | cluster VAR, with an optional
// `, independent` suboption (at least "indep"); any other suboption is r(198).
std::optional<ojson> gmmVceType(const std::string& raw, const char* what, std::string& type,
                                std::string& clus, bool& indep, SessionState& session) {
  std::string spec = raw;
  indep = false;
  const std::string bad = std::string(what) + "() incorrectly specified";
  const size_t comma = spec.find(',');
  if (comma != std::string::npos) {
    const std::string sub = gmmTrim(spec.substr(comma + 1));
    spec = spec.substr(0, comma);
    if (!sub.empty()) {
      if (!gmmAbbrev(sub, "independent", 5)) return gmmRc("option " + bad, 198);
      indep = true;
    }
  }
  std::vector<std::string> w = gmmWords(spec);
  if (w.empty()) return gmmRc(bad, 198);
  if (gmmAbbrev(w[0], "robust", 1) && w.size() == 1) { type = "robust"; return std::nullopt; }
  if (gmmAbbrev(w[0], "unadjusted", 2) && w.size() == 1) { type = "unadjusted"; return std::nullopt; }
  if (gmmAbbrev(w[0], "cluster", 2)) {
    if (w.size() != 2) return gmmRc(bad, 198);
    bool ambiguous = false;
    const auto full = resolveVarName(w[1], session, &ambiguous);
    if (!full.has_value())
      return gmmRc("variable " + w[1] + (ambiguous ? " ambiguous abbreviation" : " not found"), 111);
    type = "cluster";
    clus = *full;
    return std::nullopt;
  }
  const std::string lw = lowerM4(w[0]);
  if (gmmAbbrev(w[0], "bootstrap", 4) || gmmAbbrev(w[0], "jackknife", 4) || lw == "hac")
    return engineErrorBlockM4(std::string("Option `") + what + "(" + raw +
                                  ")` is a valid Stata option that this version of PIET does not "
                                  "implement for `gmm`.",
                              "unsupported_option");
  return gmmRc(bad, 198);
}

}  // namespace

static ojson runGmmM4(const std::string& rawLine, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  // ---- equations -----------------------------------------------------------
  std::string s = trimM4(rawLine);
  size_t pos = s.find('(');
  if (pos == std::string::npos) return engineErrorBlockM4("syntax_error: gmm requires a parenthesised expression", "syntax_error");
  std::vector<GmmEq> eqs;
  while (pos < s.size() && s[pos] == '(') {
    int depth = 0;
    size_t rp = std::string::npos;
    for (size_t i = pos; i < s.size(); ++i) {
      if (s[i] == '(') ++depth;
      else if (s[i] == ')' && --depth == 0) { rp = i; break; }
    }
    if (rp == std::string::npos) return gmmRc("unmatched parentheses", 198);
    std::string body = s.substr(pos + 1, rp - pos - 1);
    GmmEq eq;
    static const std::regex reName(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:([^:].*)$)");
    std::smatch m;
    const size_t brace = body.find('{');
    if (std::regex_match(body, m, reName) && (brace == std::string::npos || static_cast<size_t>(m.position(2)) <= brace + 1)) {
      eq.name = m[1].str();
      body = m[2].str();
    }
    if (auto err = gmmCanon(body, session, eq)) return *err;
    eqs.push_back(std::move(eq));
    pos = rp + 1;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  }
  const size_t M = eqs.size();
  bool anyNamed = false;
  for (const auto& e : eqs) anyNamed = anyNamed || !e.name.empty();
  std::vector<std::string> eqnames;
  for (size_t m = 0; m < M; ++m)
    eqnames.push_back(eqs[m].name.empty() ? std::to_string(m + 1) : eqs[m].name);

  // ---- options -------------------------------------------------------------
  std::vector<std::vector<std::string>> inst(M);
  std::vector<bool> instSet(M, false);
  bool onestep = false, twostep = false, igmm = false;
  std::string winit = "unadjusted", winitLabel = "Unadjusted";
  bool winitIndep = false;
  std::string vceT, vceClus, wmT, wmClus, vceRaw, wmRaw;
  bool vceGiven = false, wmGiven = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "instruments") {
      std::string spec = v.isBool ? std::string() : v.strValue;
      if (spec.find(',') != std::string::npos)
        return engineErrorBlockM4("Option `instruments(" + spec + ")` with suboptions is a valid Stata option that this version of PIET does not implement for `gmm`.", "unsupported_option");
      std::vector<size_t> target;
      const size_t colon = spec.find(':');
      if (colon != std::string::npos) {
        for (const std::string& t : gmmWords(spec.substr(0, colon))) {
          size_t at = M;
          for (size_t m = 0; m < M; ++m)
            if (eqnames[m] == t) at = m;
          if (at == M) return gmmRc("equation " + t + " not found", 198);
          target.push_back(at);
        }
        spec = spec.substr(colon + 1);
      } else {
        for (size_t m = 0; m < M; ++m) target.push_back(m);
      }
      std::vector<std::string> vars;
      for (const std::string& t : gmmWords(spec)) {
        bool ambiguous = false;
        const auto full = resolveVarName(t, session, &ambiguous);
        if (!full.has_value())
          return gmmRc("variable " + t + (ambiguous ? " ambiguous abbreviation" : " not found"), 111);
        vars.push_back(*full);
      }
      for (size_t m : target) {
        for (const auto& x : vars) inst[m].push_back(x);
        instSet[m] = true;
      }
    } else if (k == "onestep") onestep = true;
    else if (k == "twostep") twostep = true;
    else if (k == "igmm") igmm = true;
    else if (k == "winitial") {
      std::string a = v.isBool ? std::string() : v.strValue;
      std::string sub;
      const size_t comma = a.find(',');
      if (comma != std::string::npos) {
        sub = gmmTrim(a.substr(comma + 1));
        a = a.substr(0, comma);
      }
      const std::vector<std::string> w = gmmWords(a);
      if (w.size() != 1) return gmmRc("winitial() incorrectly specified", 198);
      if (gmmAbbrev(w[0], "unadjusted", 2)) { winit = "unadjusted"; winitLabel = "Unadjusted"; }
      else if (gmmAbbrev(w[0], "identity", 1)) { winit = "identity"; winitLabel = "Identity"; }
      else
        return engineErrorBlockM4("Option `winitial(" + v.strValue + ")` is a valid Stata option that this version of PIET does not implement for `gmm`.", "unsupported_option");
      if (!sub.empty()) {
        if (!gmmAbbrev(sub, "independent", 3)) return gmmRc("winitial() incorrectly specified", 198);
        winitIndep = true;
      }
    } else if (k == "wmatrix") {
      wmRaw = v.isBool ? std::string() : v.strValue;
      wmGiven = true;
    } else if (k == "vce") {
      vceRaw = v.isBool ? std::string() : v.strValue;
      vceGiven = true;
    }
  }
  // ---- weights -------------------------------------------------------------
  std::string wtypeEng, wtypeMacro;
  const bool weighted = parsed.weightVar.has_value() && !parsed.weightVar->empty();
  if (weighted) {
    const std::string wt = lowerM4(parsed.weightType.value_or("aw"));
    if (wt == "aw" || wt == "aweight") { wtypeEng = "aw"; wtypeMacro = "aweight"; }
    else if (wt == "pw" || wt == "pweight") { wtypeEng = "pw"; wtypeMacro = "pweight"; }
    else if (wt == "fw" || wt == "fweight") { wtypeEng = "fw"; wtypeMacro = "fweight"; }
    else if (wt == "iw" || wt == "iweight") { wtypeEng = "iw"; wtypeMacro = "iweight"; }
    else return gmmRc("weight type not allowed", 101);
  }
  // Stata's checks, in its order: one estimator (r(184)); no wmatrix() with the
  // one-step estimator (r(198)); the vce()/wmatrix() specifications; `independent`
  // only on both or neither (r(198)); one cluster variable (r(498)); pweights need a
  // robust or cluster weight matrix, and with onestep a robust or cluster VCE (r(404)).
  if (static_cast<int>(onestep) + static_cast<int>(twostep) + static_cast<int>(igmm) > 1)
    return gmmRc("can specify only one of onestep, twostep, or igmm", 184);
  if (onestep && wmGiven) return gmmRc("cannot specify wmatrix() with one-step estimator", 198);
  bool vceIndep = false, wmIndep = false;
  if (vceGiven)
    if (auto err = gmmVceType(vceRaw, "vce", vceT, vceClus, vceIndep, session)) return *err;
  if (wmGiven)
    if (auto err = gmmVceType(wmRaw, "wmatrix", wmT, wmClus, wmIndep, session)) return *err;
  // Each of vce() and wmatrix() defaults to the other's type (bank e1b3d53d
  // vce(unadjusted) -> e(wmatrix) unadjusted, 9aa98f1b vce(cluster rep78) -> e(wmatrix)
  // "cluster rep78", 6948e4f7 wmatrix(unadjusted) -> e(vce) unadjusted); robust otherwise.
  if (vceT.empty() && !wmT.empty()) {
    vceT = wmT;
    vceClus = wmClus;
    vceIndep = wmIndep;
  }
  if (vceT.empty()) vceT = "robust";
  if (wmT.empty()) {
    wmT = vceT;
    wmClus = vceClus;
    wmIndep = vceIndep;
  }
  if ((vceIndep || wmIndep) && !(vceIndep && wmIndep))
    return gmmRc("option " + vceT + "() incorrectly specified", 198);
  if (vceIndep)
    return engineErrorBlockM4("Option `" + std::string(wmGiven ? "wmatrix(" + wmRaw : "vce(" + vceRaw) +
                                  ")` (the independent suboption) is a valid Stata option that this "
                                  "version of PIET does not implement for `gmm`.",
                              "unsupported_option");
  if (wmT == "cluster" && vceT == "cluster" && wmClus != vceClus)
    return gmmRc("cannot specify different cluster variables in wmat() and vce()", 498);
  if (wtypeEng == "pw") {
    if (wmT != "cluster" && wmT != "robust" && !onestep)
      return gmmRc("cannot use " + wmT + " weight matrix with pweights", 404);
    if (vceT != "cluster" && vceT != "robust" && onestep)
      return gmmRc("cannot use " + vceT + " VCE with pweights and one-step estimator", 404);
  }
  for (size_t m = 0; m < M; ++m)
    if (!instSet[m]) return gmmRc("no instruments specified for equation " + eqnames[m], 198);

  // ---- engine --------------------------------------------------------------
  std::vector<std::string> argv = {"gmm"};
  for (size_t m = 0; m < M; ++m) {
    argv.push_back("--expr");
    argv.push_back(eqs[m].engineExpr);
    argv.push_back("--instruments");
    argv.push_back(joinCommaM4(inst[m]));
  }
  {
    std::string en;
    for (size_t m = 0; m < M; ++m) en += (m ? "," : "") + eqnames[m];
    argv.push_back("--eqnames");
    argv.push_back(en);
  }
  if (onestep) argv.push_back("--onestep");
  if (igmm) argv.push_back("--igmm");
  argv.push_back("--winitial");
  argv.push_back(winit + (winitIndep ? "_independent" : ""));
  argv.push_back("--wmatrix");
  argv.push_back(wmT);
  if (wmT == "cluster") {
    argv.push_back("--wcluster");
    argv.push_back(wmClus);
  }
  argv.push_back("--vce");
  argv.push_back(vceT);
  if (vceT == "cluster") {
    argv.push_back("--cluster");
    argv.push_back(vceClus);
  }
  if (weighted) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wtypeEng);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (auto err = pushInRangeKeepifM4(session, parsed, argv, "gmm")) return *err;
  pushLevelIfGivenM4(parsed, argv);
  ojson raw;
  ojson block = runP7Mle("gmm", argv, session, &raw, /*recordSample=*/true);
  if (block.value("type", std::string()) == "error") {
    // Stata's own sentences and codes for the engine's refusals.
    const std::string msg = block.value("message", std::string());
    if (msg.find("model is not identified") != std::string::npos) {
      const size_t at = msg.find("model is not identified");
      return gmmRc(msg.substr(at), 481, "engine_error");
    }
    if (msg.find("initial weight matrix is not positive definite") != std::string::npos)
      return gmmRc("initial weight matrix is not positive definite; check your instruments() "
                   "specification or try another winitial() option", 506, "engine_error");
    return block;
  }
  if (!raw.contains("gmm") || !raw["gmm"].is_object()) return block;
  const ojson& g = raw["gmm"];
  const std::vector<std::string> pnames = g["params"].get<std::vector<std::string>>();
  const size_t k = pnames.size();
  const std::vector<double> b = g["b"].get<std::vector<double>>();
  const std::vector<double> init = g["init"].get<std::vector<double>>();
  const int L = g["n_moments"].get<int>();

  // ---- e() -----------------------------------------------------------------
  // Every e() item is written on top of the installed model, so each is recorded
  // (SessionState::noteEOnFit): the next estimation command drops gmm's e()
  // whole, as Stata replaces e() on each fit (a later regress must not answer
  // e(N), e(k), e(rank) or e(J) from this gmm, nor a later gmm e(ic)/e(N_clust)).
  auto& ev = session.eval;
  auto setS = [&session](const std::string& nm, double v) { e3::setEScalarOnFit(session, nm, v); };
  auto setM = [&session](const std::string& nm, const std::string& v) { e3::setEStringOnFit(session, nm, v); };
  auto postM = [&session](const std::string& nm, eval::EvalMatrix mtx) {
    session.noteEOnFit(nm);
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = std::move(mtx);
        return;
      }
    session.eval.eMatrices.emplace_back(nm, std::move(mtx));
  };
  auto numbered = [](const char* p, size_t n) {
    std::vector<std::string> v;
    for (size_t i = 0; i < n; ++i) v.push_back(std::string(p) + std::to_string(i + 1));
    return v;
  };
  auto sqm = [](const ojson& rows, const std::vector<std::string>& rn, const std::vector<std::string>& cn) {
    eval::EvalMatrix mtx;
    for (const auto& row : rows) mtx.data.push_back(row.get<std::vector<double>>());
    mtx.rownames = rn;
    mtx.colnames = cn;
    return mtx;
  };
  std::vector<std::string> instNames;
  for (size_t m = 0; m < M; ++m) {
    for (const auto& z : inst[m]) instNames.push_back(eqnames[m] + ":" + z);
    instNames.push_back(eqnames[m] + ":_cons");
  }
  {
    eval::EvalMatrix bm;
    bm.data.push_back(b);
    bm.rownames = {"y1"};
    bm.colnames = pnames;
    postM("b", bm);
    eval::EvalMatrix im;
    im.data.push_back(init);
    im.rownames = {"r1"};
    im.colnames = pnames;
    postM("init", im);
  }
  postM("V", sqm(g["V"], pnames, pnames));
  const bool unadjV = vceT == "unadjusted";
  postM("W", sqm(g["W"], instNames, instNames));
  postM("W0", sqm(g["W"], numbered("r", L), numbered("c", L)));
  postM("W_criterion", sqm(g["W"], numbered("r", L), numbered("c", L)));
  postM("G", sqm(g["G"], numbered("r", L), numbered("c", k)));
  if (!unadjV) {
    postM("S", sqm(g["S"], instNames, instNames));
    postM("V_modelbased", sqm(g["V_modelbased"], numbered("r", k), numbered("c", k)));
  } else {
    for (const char* nm : {"S", "V_modelbased"})
      for (size_t i = 0; i < ev.eMatrices.size(); ++i)
        if (ev.eMatrices[i].first == nm) {
          ev.eMatrices.erase(ev.eMatrices.begin() + static_cast<long>(i));
          break;
        }
  }
  // parameter equations, in order; the trailing run of _cons-only ones are the
  // ancillary parameters Stata shows as /name (e(k_aux))
  std::vector<std::string> peqs;
  std::map<std::string, std::vector<std::string>> peqCols;
  for (const auto& p : pnames) {
    const std::string e = p.substr(0, p.find(':'));
    if (std::find(peqs.begin(), peqs.end(), e) == peqs.end()) peqs.push_back(e);
    peqCols[e].push_back(p.substr(p.find(':') + 1));
  }
  size_t kAux = 0;
  for (size_t i = peqs.size(); i-- > 0;) {
    const auto& c = peqCols[peqs[i]];
    if (c.size() == 1 && c[0] == "_cons") ++kAux;
    else break;
  }
  const double N = g["N"].get<double>();
  const char* est = onestep ? "onestep" : (igmm ? "igmm" : "twostep");
  setS("N", N);
  setS("k", static_cast<double>(k));
  setS("k_eq", static_cast<double>(peqs.size()));
  setS("k_aux", static_cast<double>(kAux));
  setS("k_eq_model", 0);
  setS("n_eq", static_cast<double>(M));
  setS("n_moments", static_cast<double>(L));
  setS("rank", static_cast<double>(k));
  setS("Q", g["Q"].get<double>());
  setS("Q_criterion", g["Q_criterion"].get<double>());
  setS("J", g["J"].get<double>());
  setS("J_df", g["J_df"].get<double>());
  setS("converged", g.value("converged", true) ? 1.0 : 0.0);
  setS("has_xtinst", 0);
  setS("type", 1);
  setS("version", 19.5);
  if (igmm) setS("ic", g["ic"].get<double>());
  for (size_t m = 0; m < M; ++m) {
    const std::string sfx = "_" + std::to_string(m + 1);
    setS("k" + sfx, static_cast<double>(eqs[m].params.size()));
    std::string pl, il;
    for (const auto& p : eqs[m].params) pl += (pl.empty() ? "" : " ") + p;
    for (const auto& z : inst[m]) il += z + " ";
    setM("params" + sfx, pl);
    setM("inst" + sfx, il + "_cons");
    setM("sexp" + sfx, eqs[m].sexp);
  }
  {
    std::string pl, en;
    for (const auto& p : pnames) pl += (pl.empty() ? "" : " ") + p;
    for (const auto& e : eqnames) en += (en.empty() ? "" : " ") + e;
    setM("params", pl);
    setM("eqnames", en);
  }
  setM("cmd", "gmm");
  setM("estimator", est);
  setM("winit", winitLabel);
  setM("wmatrix", wmT == "cluster" ? "cluster " + wmClus : wmT);
  setM("vce", vceT);
  setM("vcetype", unadjV ? std::string() : std::string("Robust"));
  setM("clustvar", vceT == "cluster" ? vceClus : std::string());
  if (vceT == "cluster" && g.contains("N_clust")) setS("N_clust", g["N_clust"].get<double>());
  setM("technique", "gn");
  setM("estat_cmd", "gmm_estat");
  setM("predict", "gmm_p");
  setM("marginsok", "xb");
  setM("marginsnotok", "Residuals SCores");
  setM("marginsprop", "allcons nochainrule");
  setM("scorevers", "version 19.5:");
  setM("properties", "b V");
  setM("wtype", weighted ? wtypeMacro : std::string());
  setM("wexp", weighted ? "= " + parsed.weightExp.value_or(*parsed.weightVar) : std::string());

  // ---- the block -----------------------------------------------------------
  // Stata's table: the trailing free parameters as /name, the rest eq:name.
  if (block.contains("coeftable") && block["coeftable"].is_array())
    for (size_t j = 0; j < k && j < block["coeftable"].size(); ++j) {
      const std::string e = pnames[j].substr(0, pnames[j].find(':'));
      const size_t at = static_cast<size_t>(std::find(peqs.begin(), peqs.end(), e) - peqs.begin());
      block["coeftable"][j]["name"] = at + kAux >= peqs.size() ? "/" + e : pnames[j];
    }
  block["estimator"] = "gmm";
  block["gmm_estimator"] = est;
  block["vcetype"] = unadjV ? "" : "Robust";
  {
    ojson rc = ojson::array();
    const ojson& ct = block["coeftable"];
    for (size_t j = 0; j < k && j < ct.size(); ++j) {
      ojson c = ct[j];
      c["name"] = pnames[j];
      if (c.contains("t") && !c.contains("z")) c["z"] = c["t"];
      rc.push_back(c);
    }
    block["rtable_cols"] = rc;
  }
  session.lastEstBlock = block;
  return block;
}

ojson runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
               const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (cobraCmd == "gmm") return runGmmM4(rawLine, parsed, session);
  // Take the expression straight off the raw line: the tokenizer splits on
  // whitespace, so rejoining varlist tokens would silently normalise the
  // spacing inside the user's own formula.
  std::string s = trimM4(rawLine);
  size_t lp = s.find('(');
  if (lp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires a parenthesised expression",
                              "syntax_error");
  int depth = 0;
  size_t rp = std::string::npos;
  for (size_t i = lp; i < s.size(); ++i) {
    if (s[i] == '(') ++depth;
    else if (s[i] == ')' && --depth == 0) { rp = i; break; }
  }
  if (rp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " expression has no closing ')'",
                              "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--expr", s.substr(lp, rp - lp + 1)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "gmm") {
    std::string instruments;
    bool onestep = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "instruments" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> toks;
        std::string t;
        while (iss >> t) toks.push_back(t);
        instruments = joinCommaM4(toks);
      }
      else if (k == "onestep" && v.isBool && v.boolValue) onestep = true;
      else if (k == "twostep" && v.isBool && v.boolValue)
        return engineErrorBlockM4(
            "Option `twostep` is a valid Stata option that this version of PIET does not implement "
            "for `gmm` (only the one-step estimator is available).",
            "unsupported_option");
    }
    if (instruments.empty())
      return engineErrorBlockM4("syntax_error: gmm requires instruments()", "syntax_error");
    argv.push_back("--instruments");
    argv.push_back(instruments);
    if (!onestep)
      return engineErrorBlockM4(
          "`gmm` without `onestep` (two-step GMM) is a valid Stata command that this version of "
          "PIET does not implement.",
          "unsupported_command");
    argv.push_back("--onestep");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "robust") && cobraCmd == "nl")
      return engineErrorBlockM4(
          "Option `vce()` is a valid Stata option that this version of PIET does not implement for "
          "`nl` (the small-sample factor is not fixed by any oracle available here).",
          "unsupported_option");
  }
  // [aw=]/[pw=] (BUG-1283): the engine's weighted NLS -- aweights normalized to N
  // with the model-based VCE, pweights the robust sandwich (StataBE 19.5 _se[/b1]
  // .067819537723433465 / .071775268345285762). Stata notes the weight sum first.
  const bool weighted = cobraCmd == "nl" && parsed.weightVar.has_value() &&
                        !parsed.weightVar->empty();
  if (weighted) {
    const std::string wt = lowerM4(parsed.weightType.value_or("aw"));
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    const bool pw = wt == "pw" || wt == "pweight";
    argv.push_back("--wtype");
    argv.push_back(pw ? "pw" : "aw");
    if (pw) {  // e(vce) robust, e(vcetype) Robust, as Stata posts under a pweight
      argv.push_back("--vce");
      argv.push_back("robust");
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  ojson raw;
  ojson block = runP7Mle(cobraCmd, argv, session, &raw);
  // A model with no {parameters} fails inside nl's Gauss-Newton (Mata) with
  // r(3301) "subscript invalid" (vs-stata bank 3c5fc5ed `nl (mpg = 40*exp(-0.0003*weight))`).
  if (block.value("type", std::string()) == "error" &&
      block.value("message", std::string()).find("has no {parameters}") != std::string::npos)
    return gmmRc("subscript invalid", 3301, "engine_error");
  if (!weighted || block.value("type", std::string()) == "error" || !raw.contains("weight") ||
      !raw["weight"].is_object() || !raw["weight"].contains("sum_w") ||
      !raw["weight"]["sum_w"].is_number())
    return block;
  char buf[64];
  std::snprintf(buf, sizeof buf, "(sum of wgt is %.16g)", raw["weight"]["sum_w"].get<double>());
  ojson note;
  note["type"] = "display";
  note["text"] = buf;
  return ojson::array({note, block});
}

// ===========================================================================
// P7 — multilevel: `mixed depvar indepvars || GROUP:, vce(robust)`
//
// Only the random-INTERCEPT form is implemented. A random-slope list after the
// colon, or a second `||` level, is a different model and rejects rather than
// being silently fitted as an intercept-only one.


}  // namespace piet::handlers
