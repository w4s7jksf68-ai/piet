// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Likelihood support.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include <cmath>
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
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"
#include "extended_estimation_internal.h"  // stataRefusalM4b

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

namespace est_detail {  // R2: reopened — these cross the TU cut (runP7Mle & friends)



// Builds `CMD --y Y --x X1,X2 [--keepif E] [--vce ... [--cluster C]]`.
// `vceAllowed` names the vce spellings this verb actually implements; anything
// else is an honest reject rather than a silent downgrade to iid.
// BUG-333: forward Stata's `level(#)` to the engine. All six commands wired
// through this (nl/gmm, mixed, fracreg, ivprobit/ivtobit) register AND consume
// `--level` in cobra today (options_table.cpp:110-118); PIET was simply not
// passing it, so the option was an honest-but-unnecessary reject. Kept as a
// free function rather than a branch inside buildP7MleArgv because that builder
// is shared with commands whose engine half has NO --level (xtlogit), and a
// blanket push there would turn a working call into an E6 unconsumed-flag error.
// Same idiom as runTobit/runQreg/runNbreg: forward the string, let the engine
// own the [10, 99.99] range check and surface it through CobraError.
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      return;
    }
  }
}

// `in a/b` on an engine fit that filters with --keepif: the range becomes the
// observation-number condition `_n >= a & _n <= b` over the dataset in memory
// (the engine's keepif reads `_n` as the observation number, BUG-364), ANDed
// with any `if` -- Stata's rule -- so the fit, its e(sample) mask and every
// later reader see the same rows (vs-stata bank d6434408 `ologit rep78 mpg
// weight in 1/60`). A malformed or out-of-range bound is Stata's refusal.
std::optional<ojson> inRangeConditionM4(SessionState& session, const eval::ParsedCommand& parsed,
                                        std::optional<std::string>& ifOut, const std::string& stataCmd) {
  ifOut = parsed.ifExpr;
  if (!parsed.inRange.has_value() || parsed.inRange->empty()) return std::nullopt;
  Datastore* ds = session.datastore();
  InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
  if (!rr.ok) {
    InRangeFilterResult bad;
    bad.ok = false;
    bad.negative = rr.negative;
    bad.refusal = rr.refusal;
    return inRangeRejectError(bad, *parsed.inRange, stataCmd);
  }
  const std::string cond = "(_n >= " + std::to_string(rr.a) + " & _n <= " + std::to_string(rr.b) + ")";
  ifOut = parsed.ifExpr.has_value() && !parsed.ifExpr->empty() ? cond + " & (" + *parsed.ifExpr + ")" : cond;
  return std::nullopt;
}

std::optional<ojson> pushInRangeKeepifM4(SessionState& session, const eval::ParsedCommand& parsed,
                                         std::vector<std::string>& argv, const std::string& stataCmd) {
  if (!parsed.inRange.has_value() || parsed.inRange->empty()) return std::nullopt;
  std::optional<std::string> eff;
  if (auto err = inRangeConditionM4(session, parsed, eff, stataCmd)) return err;
  for (size_t i = 0; i + 1 < argv.size(); ++i)
    if (argv[i] == "--keepif") {  // the `if` buildP7MleArgv forwarded
      argv[i + 1] = *eff;
      return std::nullopt;
    }
  argv.push_back("--keepif");
  argv.push_back(*eff);
  return std::nullopt;
}

P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed) {
  P7MleArgv r;
  if (parsed.varlist.size() < 2) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "syntax_error: " + cobraCmd + " requires a dependent variable and at least one regressor",
        "syntax_error");
    return r;
  }
  const std::string y = parsed.varlist[0];
  const std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());
  r.argv = {cobraCmd, "--y", y, "--x", joinCommaM4(xs)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }

  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want;
  std::string cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // Stata's legacy bare `cluster(v)` is a synonym for vce(cluster v) — the
    // BUG-055 class. Honouring it here keeps these verbs from silently running
    // iid while the user believes they clustered. BUG-537: tested BEFORE the
    // bare `robust` flag -- `robust cluster(v)` together is cluster-robust in
    // Stata (w2_pweight.log R5_pw_robust_AND_cluster_equals_R4); robust-first
    // dropped the cluster. Same ordering as vcovFromOpts above.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;  // engine default (iid)

  bool allowed = false;
  for (const auto& s : vceAllowed)
    if (s == want) { allowed = true; break; }
  if (!allowed) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.",
        "unsupported_option");
    return r;
  }
  if (want == "cluster") {
    // A second cluster variable would be two-way clustering, which none of these
    // ML estimators implements — rejecting is parity, quietly using the first
    // variable is not.
    std::istringstream iss(cvar);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    if (toks.empty()) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                        "unsupported_option");
      return r;
    }
    if (toks.size() > 1) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4(
          "Two-way clustering is not implemented for `" + cobraCmd + "`.", "unsupported_option");
      return r;
    }
    r.argv.push_back("--vce");
    r.argv.push_back("cluster");
    r.argv.push_back("--cluster");
    r.argv.push_back(toks[0]);
  } else {
    r.argv.push_back("--vce");
    r.argv.push_back(want);
  }
  return r;
}

// rawOut, when given, receives the engine payload BEFORE cobraToEstimationM4
// narrows it to the known block keys — the ordered models need it because their
// cutpoints ride in a key the mapper does not carry through.
// `stataCmd` is the e(cmd) word LastModel records (cacheLastModelM4); empty
// publishes cobraCmd, as every runP7Mle caller does.
static ojson runP7MleFitM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd, bool recordSample) {
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  std::vector<std::string> fitArgs = cobraArgs;
  const std::string sample = recordSample ? esampleFitArgs(session, fitArgs, std::string(), 0) : std::string();
  ojson raw;
  try {
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!sample.empty()) cecon::native_unlink(sample);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (rawOut) *rawOut = raw;
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  fillCoeftableStatsM4(block);
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw, sample, stataCmd);
  return block;
}

ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                   SessionState& session, ojson* rawOut, const std::string& stataCmd) {
  return runP7MleFitM4(cobraCmd, std::move(cobraArgs), session, rawOut, stataCmd, false);
}

ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut, bool recordSample) {
  return runP7MleFitM4(cobraCmd, std::move(cobraArgs), session, rawOut, std::string(), recordSample);
}

// BUG-1196: the depvar equation of an engine coefficient array as Stata names
// it -- `depvar:name`, `(intercept)` spelled `_cons` and placed last. False when
// a name is not a plain variable name (a factor level, an interaction, a
// time-series operator): Stata's columns there (base levels, `o.` marks) are
// not the engine's, and nothing is posted.
bool depvarEqColsM4(const std::string& depvar, const ojson& coefs, const char* bKey,
                    std::vector<AncillaryEbColM4>& out) {
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (!std::regex_match(depvar, rePlain) || !coefs.is_array() || coefs.empty()) return false;
  std::vector<AncillaryEbColM4> cols;
  std::optional<AncillaryEbColM4> cons;
  for (const auto& c : coefs) {
    if (!c.is_object() || !c.contains("name") || !c["name"].is_string() || !c.contains(bKey) ||
        !c[bKey].is_number())
      return false;
    const std::string nm = c["name"].get<std::string>();
    AncillaryEbColM4 col{std::string(), nm, c[bKey].get<double>()};
    if (nm == "(intercept)" || nm == "_cons") {
      col.stata = depvar + ":_cons";
      cons = col;
      continue;
    }
    if (!std::regex_match(nm, rePlain)) return false;
    col.stata = depvar + ":" + nm;
    cols.push_back(std::move(col));
  }
  if (cons.has_value()) cols.push_back(*cons);
  out.insert(out.end(), cols.begin(), cols.end());
  return true;
}

// BUG-1196: e(b)/e(V) of a fit whose Stata stripe has equations or ancillary
// parameters that the coefficient table does not carry (tobit, ologit/oprobit,
// nbreg, mlogit), posted as e() matrices, which every e(b)/e(V) reader answers
// before the stripe table's refusal (EvalSession::resolveEMatrixFor). `cols`
// are Stata's columns in order: its name, the engine's `e_V` name for it ("" for
// a column Stata posts as a structural 0 -- mlogit's base outcome), and b. e(V)
// is the engine's `--emit-v` covariance, made exactly symmetric as
// canonicalEMatrix makes it. Posts nothing -- the refusal stands -- unless the
// reply's `e_V` names every column with finite cells.
bool postAncillaryEbM4(SessionState& session, const ojson& raw,
                       const std::vector<AncillaryEbColM4>& cols) {
  const auto it = raw.find("e_V");
  if (cols.empty() || it == raw.end() || !it->is_object() || !it->contains("names") ||
      !it->contains("matrix"))
    return false;
  const ojson& vn = (*it)["names"];
  const ojson& vm = (*it)["matrix"];
  if (!vn.is_array() || !vm.is_array() || vm.size() != vn.size()) return false;
  const size_t k = cols.size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if (!std::isfinite(cols[a].b)) return false;
    if (cols[a].engine.empty()) continue;
    for (size_t j = 0; j < vn.size(); ++j)
      if (vn[j].is_string() && vn[j].get<std::string>() == cols[a].engine) {
        at[a] = static_cast<long>(j);
        break;
      }
    if (at[a] < 0) return false;
  }
  const auto cell = [&vm, &vn](long r, long c, double* v) {
    const ojson& row = vm[static_cast<size_t>(r)];
    if (!row.is_array() || row.size() != vn.size() || !row[static_cast<size_t>(c)].is_number())
      return false;
    *v = row[static_cast<size_t>(c)].get<double>();
    return std::isfinite(*v);
  };
  eval::EvalMatrix mb, mv;
  mb.rownames = {"y1"};
  mb.data.emplace_back();
  mv.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a) {
    mb.colnames.push_back(cols[a].stata);
    mb.data[0].push_back(cols[a].b);
    for (size_t c = 0; c < k; ++c) {
      if (at[a] < 0 || at[c] < 0) continue;
      double v1 = 0, v2 = 0;
      if (!cell(at[a], at[c], &v1) || !cell(at[c], at[a], &v2)) return false;
      mv.data[a][c] = 0.5 * (v1 + v2);
    }
  }
  mv.rownames = mb.colnames;
  mv.colnames = mb.colnames;
  for (auto* m : {&mb, &mv}) {
    const std::string nm = m == &mb ? "b" : "V";
    bool put = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = *m;
        put = true;
      }
    if (!put) session.eval.eMatrices.emplace_back(nm, *m);
  }
  return true;
}


// ---- constraints (Stata's `constraint` command and the ml estimators'
// constraints() option). A constraint is a linear equation in the model's
// coefficients: terms `[eq]name`, `_b[eq:name]`, `[eq]_b[name]`, `name`,
// `/cut1`, each optionally multiplied by a number, and numbers, on either side
// of one `=`. It is parsed only when an estimator reads it (Stata checks its
// coefficient names against the model then); `constraint define` stores the text.
namespace {
bool cnsNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '#' || c == '~';
}
// One side of a constraint: sum of terms -> (terms, constant).
bool parseCnsSideM4(const std::string& s, double sign, std::vector<CnsTermM4>& terms, double& cst,
                    std::string& why) {
  size_t i = 0;
  const size_t n = s.size();
  auto skip = [&] { while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
  bool first = true;
  for (;;) {
    skip();
    if (i >= n) {
      if (first) { why = "empty side"; return false; }
      return true;
    }
    double tsign = 1.0;
    if (!first) {
      if (s[i] == '+') ++i;
      else if (s[i] == '-') { tsign = -1.0; ++i; }
      else { why = "expected + or -"; return false; }
      skip();
    }
    while (i < n && (s[i] == '+' || s[i] == '-')) {  // leading unary signs
      if (s[i] == '-') tsign = -tsign;
      ++i;
      skip();
    }
    first = false;
    double coef = 1.0;
    bool haveRef = false;
    CnsTermM4 t;
    for (;;) {  // factor ('*' factor)*
      skip();
      if (i >= n) { why = "incomplete term"; return false; }
      const char c = s[i];
      if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n &&
                                                          std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
        size_t used = 0;
        double v = 0.0;
        try {
          v = std::stod(s.substr(i), &used);
        } catch (const std::exception&) {
          why = "bad number";
          return false;
        }
        coef *= v;
        i += used;
      } else {
        if (haveRef) { why = "a term multiplies two coefficients"; return false; }
        haveRef = true;
        if (c == '[') {  // [eq]name or [eq]_b[name]
          const size_t close = s.find(']', i);
          if (close == std::string::npos) { why = "unmatched ["; return false; }
          t.eq = s.substr(i + 1, close - i - 1);
          t.eq.erase(0, t.eq.find_first_not_of(" \t"));
          t.eq.erase(t.eq.find_last_not_of(" \t") + 1);
          i = close + 1;
          skip();
          if (s.compare(i, 3, "_b[") == 0) {
            const size_t c2 = s.find(']', i + 3);
            if (c2 == std::string::npos) { why = "unmatched ["; return false; }
            t.name = s.substr(i + 3, c2 - i - 3);
            i = c2 + 1;
          } else {
            const size_t st = i;
            while (i < n && cnsNameChar(s[i])) ++i;
            t.name = s.substr(st, i - st);
          }
        } else if (s.compare(i, 3, "_b[") == 0) {  // _b[eq:name] / _b[name]
          const size_t close = s.find(']', i + 3);
          if (close == std::string::npos) { why = "unmatched ["; return false; }
          std::string in = s.substr(i + 3, close - i - 3);
          const size_t colon = in.find(':');
          if (colon != std::string::npos) {
            t.eq = in.substr(0, colon);
            in = in.substr(colon + 1);
          }
          t.name = in;
          i = close + 1;
        } else if (c == '/') {
          const size_t st = i;
          ++i;
          while (i < n && cnsNameChar(s[i])) ++i;
          t.name = s.substr(st, i - st);
        } else if (cnsNameChar(c)) {
          const size_t st = i;
          while (i < n && cnsNameChar(s[i])) ++i;
          t.name = s.substr(st, i - st);
        } else {
          why = std::string("unexpected '") + c + "'";
          return false;
        }
        t.name.erase(0, t.name.find_first_not_of(" \t"));
        t.name.erase(t.name.find_last_not_of(" \t") + 1);
        if (t.name.empty()) { why = "missing coefficient name"; return false; }
      }
      skip();
      if (i < n && s[i] == '*') {
        ++i;
        continue;
      }
      break;
    }
    if (haveRef) {
      t.coef = sign * tsign * coef;
      terms.push_back(std::move(t));
    } else {
      cst += sign * tsign * coef;
    }
  }
}
std::vector<int> parseCnsNumlistM4(const std::string& s, bool& ok) {
  std::vector<int> out;
  ok = true;
  std::istringstream is(s);
  std::string tok;
  while (is >> tok) {
    const size_t dash = tok.find_first_of("/-", 1);
    try {
      if (dash != std::string::npos) {
        const int a = std::stoi(tok.substr(0, dash)), b = std::stoi(tok.substr(dash + 1));
        if (a > b) { ok = false; return out; }
        for (int v = a; v <= b; ++v) out.push_back(v);
      } else {
        size_t used = 0;
        out.push_back(std::stoi(tok, &used));
        if (used != tok.size()) { ok = false; return out; }
      }
    } catch (const std::exception&) {
      ok = false;
      return out;
    }
  }
  return out;
}
}  // namespace

bool parseConstraintTextM4(const std::string& text, CnsDefM4& out, std::string& why) {
  const size_t eq = text.find('=');
  if (eq == std::string::npos || text.find('=', eq + 1) != std::string::npos) {
    why = "a constraint is one equation";
    return false;
  }
  double cst = 0.0;
  out.terms.clear();
  if (!parseCnsSideM4(text.substr(0, eq), 1.0, out.terms, cst, why)) return false;
  if (!parseCnsSideM4(text.substr(eq + 1), -1.0, out.terms, cst, why)) return false;
  out.rhs = cst == 0.0 ? 0.0 : -cst;
  return true;
}

bool readConstraintsM4(const eval::ParsedCommand& parsed, const SessionState& session,
                       std::vector<CnsDefM4>& out, ojson& err) {
  out.clear();
  for (const auto& [k, v] : parsed.opts) {
    if (k != "constraints") continue;
    bool ok = true;
    const std::vector<int> nums = parseCnsNumlistM4(v.isBool ? std::string() : v.strValue, ok);
    if (!ok || nums.empty()) {
      err = extended_fit_detail::stataRefusalM4b("invalid numlist in constraints()", 121);
      return false;
    }
    for (int num : nums) {
      const auto it = session.constraints.find(num);
      if (it == session.constraints.end()) continue;  // Stata skips an undefined number
      CnsDefM4 d;
      d.num = num;
      std::string why;
      if (!parseConstraintTextM4(it->second, d, why)) {
        err = extended_fit_detail::stataRefusalM4b("constraint (" + std::to_string(num) + ") is not a linear equation: " + why,
                              198);
        return false;
      }
      out.push_back(std::move(d));
    }
  }
  return true;
}

// Stata's display of a number in a constraint (`-.1`, `2`, `1.5e-08`).
std::string cnsNumberTextM4(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.8g", v);
  std::string s = buf;
  if (s.rfind("0.", 0) == 0) s.erase(0, 1);
  else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
  return s;
}

std::string cnsDisplayLineM4(const std::vector<std::pair<double, std::string>>& terms, double rhs) {
  std::string s;
  for (size_t i = 0; i < terms.size(); ++i) {
    double c = terms[i].first;
    if (i == 0) {
      if (c < 0) s += "-";
    } else {
      s += c < 0 ? " - " : " + ";
    }
    c = std::fabs(c);
    if (c != 1.0) s += cnsNumberTextM4(c) + "*";
    s += terms[i].second;
  }
  return s + " = " + cnsNumberTextM4(rhs);
}

}  // namespace est_detail

// `constraint [define] # exp`, `constraint drop numlist|_all`,
// `constraint dir|list [numlist|_all]`, `constraint free`, `constraint get #`.
ojson runConstraint(const eval::ParsedCommand& parsed, SessionState& session) {
  using extended_fit_detail::stataRefusalM4b;
  std::string rest = parsed.raw;
  {
    // the text after the command word, as typed
    const std::string low = piet::strutil::lower(rest);
    size_t at = low.find(piet::strutil::lower(parsed.cmd));
    if (at == std::string::npos) at = low.find("cons");
    if (at == std::string::npos) return stataRefusalM4b("invalid syntax", 198);
    rest = rest.substr(at + parsed.cmd.size());
    while (!rest.empty() && est_detail::cnsNameChar(rest[0])) rest.erase(0, 1);  // the rest of an abbreviation
  }
  auto trim = [](std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    return s;
  };
  rest = trim(rest);
  std::string word, tail;
  {
    const size_t sp = rest.find_first_of(" \t");
    word = rest.substr(0, sp);
    tail = sp == std::string::npos ? std::string() : trim(rest.substr(sp));
  }
  const std::string w = piet::strutil::lower(word);
  auto numsOf = [&](const std::string& t, bool& ok) -> std::vector<int> {
    std::vector<int> v;
    ok = true;
    if (trim(t).empty() || trim(t) == "_all") {
      for (const auto& kv : session.constraints) v.push_back(kv.first);
      return v;
    }
    return est_detail::parseCnsNumlistM4(t, ok);
  };
  if (w == "dir" || w == "list" || w == "l" || w == "li" || w == "lis") {
    bool ok = true;
    const std::vector<int> nums = numsOf(tail, ok);
    if (!ok) return stataRefusalM4b("invalid numlist", 121);
    std::string text;
    for (int nm : nums) {
      const auto it = session.constraints.find(nm);
      if (it == session.constraints.end()) continue;
      char head[16];
      std::snprintf(head, sizeof(head), "%6d:  ", nm);
      text += (text.empty() ? "" : "\n") + std::string(head) + it->second;
    }
    if (text.empty()) return ojson::array();
    ojson out;
    out["type"] = "echo";
    out["text"] = text;
    return out;
  }
  if (w == "drop") {
    bool ok = true;
    const std::vector<int> nums = numsOf(tail, ok);
    if (!ok) return stataRefusalM4b("invalid numlist", 121);
    for (int nm : nums) session.constraints.erase(nm);
    return ojson::array();
  }
  if (w == "free") {
    int f = 1;
    while (f <= 1999 && session.constraints.count(f) != 0) ++f;
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.rValues.set("free", f <= 1999 ? static_cast<double>(f) : eval::kStataMissingSentinel);
    return ojson::array();
  }
  if (w == "get") {
    int nm = 0;
    try {
      nm = std::stoi(tail);
    } catch (const std::exception&) {
      return stataRefusalM4b("invalid syntax", 198);
    }
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    const auto it = session.constraints.find(nm);
    session.eval.rValues.set("defined", it == session.constraints.end() ? 0.0 : 1.0);
    if (it != session.constraints.end()) session.eval.rStrings.set("contents", it->second);
    return ojson::array();
  }
  std::string body = rest;
  if (w == "define" || w == "def" || w == "defi" || w == "defin" || w == "de") body = tail;
  std::string numTok;
  {
    body = trim(body);
    const size_t sp = body.find_first_of(" \t");
    numTok = body.substr(0, sp);
    body = sp == std::string::npos ? std::string() : trim(body.substr(sp));
  }
  int num = 0;
  try {
    size_t used = 0;
    num = std::stoi(numTok, &used);
    if (used != numTok.size()) throw std::invalid_argument("n");
  } catch (const std::exception&) {
    return stataRefusalM4b("invalid syntax", 198);
  }
  if (num < 1 || num > 1999) return stataRefusalM4b("constraint number must be between 1 and 1,999", 198);
  if (body.empty()) return stataRefusalM4b("invalid syntax", 198);
  session.constraints[num] = body;
  return ojson::array();
}

}  // namespace piet::handlers
