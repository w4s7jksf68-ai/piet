// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vendor/nlohmann/json.hpp"

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

namespace piet {
namespace handlers {

namespace engine_report_detail {

// --- error / engine plumbing -------------------------------------------------

ojson qeError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// Handlers here return an ARRAY, so a rejection has to be an array too.
ojson qeErrorBlocks(const std::string& message, const std::string& code) {
  return ojson::array({qeError(message, code)});
}

// --- formatting helpers ------------------------------------------------------
// Deliberately local and tiny: the SHARED piece is fmtStataDefaultG (the %9.0g
// numeric renderer), which is what "do not write a second formatter" is about.
// padLeft/padRight/fixedN are three-line column primitives, identical in shape
// to the column primitives of resampling_prefix_support.cpp.

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string padLeft(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}
std::string padRight(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}
std::string g9(double v) { return fmtStataDefaultG(v); }

// Stata's coefficient-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer: the coefficient table reserves the sign column, so the
// digits get 8 characters where `display` gets 9. fmtStataDefaultG is
// calibrated for the display context — its own doc comment records
// `2.157789219986937 -> "2.1577892"`, a NINE-character result — and that is
// correct there; it is only the table cell that is narrower. Measured on the
// oracle twice over: did_imputation's tau0 (1.2081958740803342) prints
// `1.208196`, not `1.2081959` (p3_stata.log:326), and P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64). Below |1| the two rules COINCIDE —
// ".2071479" is eight characters either way — which is exactly why the shared
// renderer already reproduces every eventstudyinteract row byte for byte and
// only the >=1 magnitudes need this pass. The shared renderer still decides
// the digits whenever its answer fits; this only re-runs the same %g ladder
// one notch tighter when it does not.
std::string g9cell(double v) {
  std::string s = fmtStataDefaultG(v);
  const size_t sign = (!s.empty() && s[0] == '-') ? 1u : 0u;
  if (s.size() - sign <= 8) return s;
  for (int prec = 7; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t sg = (!t.empty() && t[0] == '-') ? 1u : 0u;
    if (t.size() > sg + 1 && t.compare(sg, 2, "0.") == 0) t.erase(sg, 1);
    if (t.size() - sg <= 8) return t;
  }
  return s;
}

std::string fixedN(double v, int n) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", n, v);
  return std::string(buf);
}
std::string f2(double v) { return fixedN(v, 2); }
std::string f3(double v) { return fixedN(v, 3); }
std::string f4(double v) { return fixedN(v, 4); }
std::string f5(double v) { return fixedN(v, 5); }

std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

// JSON readers that never throw: a field the engine did not send prints as
// Stata's own missing marker rather than crashing or inventing a zero.
double num(const ojson& j, const char* key, double fallback) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return j.at(key).get<double>();
}
int64_t inum(const ojson& j, const char* key, int64_t fallback) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return static_cast<int64_t>(j.at(key).get<double>());
}
std::string str(const ojson& j, const char* key, const std::string& fallback) {
  if (!j.contains(key) || !j.at(key).is_string()) return fallback;
  return j.at(key).get<std::string>();
}
// Stata's standard 78-column coefficient-table column header, e.g.
//   " Coefficient  std. err.      t    P>|t|     [95% conf. interval]"
// `errWord` is "std. err." (eventstudyinteract, under a Robust banner) or
// "Std. err." (did_imputation); `stat` is "t" or "z".
std::string coefHeader(const std::string& errWord, const std::string& stat,
                       const std::string& lvl) {
  return " Coefficient  " + errWord + "      " + stat + "    P>|" + stat + "|     [" + lvl +
         "% conf. interval]";
}

// A nested engine map (`coef`, `se`, ...) keyed by coefficient name.
std::optional<double> mapNum(const ojson& j, const char* obj, const std::string& key) {
  if (!j.contains(obj) || !j.at(obj).is_object()) return std::nullopt;
  const auto& m = j.at(obj);
  if (!m.contains(key) || !m.at(key).is_number()) return std::nullopt;
  return m.at(key).get<double>();
}

// The confidence-level word Stata stamps into a CI header: 95 -> "95".
std::string levelWord(double level) { return g9(level); }

// --- e-class posting ---------------------------------------------------------
//
// BUG-163's rule, applied: a command that posts e() in Stata must post OUR
// numbers, never leave the previous model's e() reachable under its name. The
// three estimation commands (eventstudyinteract / did_imputation / sdid) build
// a real coeftable so `_b[F2]` / `_se[tau0]` / `e(N)` answer from the fit that
// just ran. rdrobust posts its 1x3 e(b)/e(V) (Conventional, Bias-corrected,
// Robust; rdrobust). rddensity is e-class too but never runs
// `ereturn post`: it posts scalars with NO e(cmd) and no coeftable, so
// `_b[...]` is "not found" rather than answering from whatever regression ran
// three lines earlier, and estimates store refuses it (runRddensity).
//
// rdplot is NOT e-class in Stata (it is r-class), so it deliberately leaves
// lastModel untouched — clearing it would be its own wrong answer. binscatter
// IS e-class (binscatter `eclass`, `ereturn post`): runBinscatter
// replaces the whole of e() itself (FU-9's binscatter half).
//
// `result` carries the engine's key names where they already match what
// macros.cpp's resolver wants (n_obs -> e(N), n_clusters -> e(N_clust), F, r2,
// adj_r2, rmse, p_F -> e(p)); anything we do not compute is simply absent, so
// e(ll) and friends answer "." instead of a fabricated number.
void postEClass(SessionState& session, const std::string& cobraCmd, const std::string& depvar,
                const std::string& dataPath, ojson result, bool ePosted,
                const std::string& weightVar, const std::string& wtype) {
  LastModel lm;
  lm.ePosted = ePosted;
  lm.fitWeight = weightVar;  // e(wtype)/e(wexp)
  lm.fitWtype = wtype;
  lm.cobraCmd = cobraCmd;
  lm.y = depvar;
  lm.depvar = depvar;
  lm.dataPath = dataPath;
  lm.result = std::move(result);
  session.setLastModel(lm);
}

ojson coefRow(const std::string& name, std::optional<double> b, std::optional<double> se,
              std::optional<double> t, std::optional<double> p, std::optional<double> lo,
              std::optional<double> hi) {
  ojson r;
  r["name"] = name;
  auto put = [&](const char* k, const std::optional<double>& v) {
    if (v.has_value()) r[k] = *v; else r[k] = nullptr;
  };
  put("b", b);
  put("se", se);
  put("t", t);
  put("p", p);
  put("ci_lo", lo);
  put("ci_hi", hi);
  return r;
}

// --- shared option plumbing --------------------------------------------------

std::optional<std::string> optStr(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}
bool optFlag(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name) return true;
  }
  return false;
}
std::vector<std::string> splitWsOrComma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
// ROW ORDER IS NOT THE ENGINE'S KEY ORDER. runCobraJson returns
// nlohmann::json (NOT ordered_json), whose object is a std::map — so the
// engine's `coef` object arrives SORTED BY NAME, not in the order the model
// was specified. did_imputation showed this immediately: `pre1 pre2 tau0..`
// instead of Stata's `tau0..tau3 pre1 pre2`. Every table below therefore
// builds its own row order from the REQUEST (the varlist / horizons /
// pretrends the user actually asked for), then appends any leftover key the
// order missed so a coefficient can never be silently dropped.
std::vector<std::string> orderedCoefNames(const ojson& r, const std::vector<std::string>& wanted) {
  std::vector<std::string> out;
  auto has = [&](const std::string& n) {
    return r.contains("coef") && r.at("coef").is_object() && r.at("coef").contains(n);
  };
  for (const auto& n : wanted) {
    if (has(n)) out.push_back(n);
  }
  if (r.contains("coef") && r.at("coef").is_object()) {
    for (auto it = r.at("coef").begin(); it != r.at("coef").end(); ++it) {
      bool seen = false;
      for (const auto& n : out) {
        if (n == it.key()) { seen = true; break; }
      }
      if (!seen) out.push_back(it.key());
    }
  }
  return out;
}

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// Forward a scalar-valued option straight through under an engine flag.
void pushOpt(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
             const std::string& stataOpt, const std::string& flag) {
  auto v = optStr(parsed, stataOpt);
  if (v.has_value() && !v->empty()) {
    argv.push_back(flag);
    argv.push_back(*v);
  }
}

// `level(#)` -> --level, honouring `set level` when the command carries none
// (the same fallback every other estimator in this codebase uses).
void pushLevel(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
               const SessionState& session) {
  auto v = optStr(parsed, "level");
  if (v.has_value() && !v->empty()) {
    argv.push_back("--level");
    argv.push_back(*v);
  } else if (session.level != 95) {
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }
}

void pushKeepif(std::vector<std::string>& argv, const eval::ParsedCommand& parsed) {
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
}

EngineCall callEngine(std::vector<std::string> argv) {
  EngineCall r;
  if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");
  try {
    r.raw = runCobraJson(argv);
    r.ok = true;
  } catch (const CobraError& ce) {
    r.error = qeError(cobraErrorMessage(ce), ce.code);
  }
  return r;
}

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
