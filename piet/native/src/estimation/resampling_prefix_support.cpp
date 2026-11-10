// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Helpers shared by the resampling prefixes (bootstrap, jackknife/permute, rolling,
// statsby): engine-error and scratch-path helpers, exp_list parsing and evaluation,
// coefficient-table cell formatting, the e() matrix post and the prefix title.
// Shared design note of the family: the head of jackknife_permute_prefix.cpp.
#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "bsample_draws.h"       // bsample's Stata draws (shared with the bsample command)
#include "core/ident.h"          // the shared Unicode name rule
#include "core/distributions.h"  // t_critical / t_twosided_pvalue / z_critical (shared with cobra)
#include "core/rng.h"            // Philox4x32-10, the engine's own counter-based RNG (permute)
#include "estimator_command_support.h"  // postBV / installEsampleMask / invsym / setE*OnFit
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "resampling_prefix_internal.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace resampling_detail {

ojson rsError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// BUG-389: the tree's one minting body, so every replicate frame is in the mint
// registry and the exit sweep reclaims what no ledger owns (the probe's census
// counted 151 ORPHAN replicate files per bootstrap with the old local copy).
std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}

std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// ---------------------------------------------------------------------------
// exp_list parsing. Stata's grammar is `NAME=EXPR [NAME=EXPR ...]`, and an EXPR
// may itself contain spaces and brackets (`_b[x1]`, `r(mean)`), so the split is
// on the `NAME=` boundaries, not on whitespace.
std::vector<ExpItem> parseExpList(const std::string& raw, std::string* err) {
  std::vector<ExpItem> out;
  std::string s = raw;
  size_t i = 0;
  auto skipWs = [&]() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
  while (true) {
    skipWs();
    if (i >= s.size()) break;
    const size_t nameStart = i;
    while (i < s.size() && cecon::ident::isIdentByte(s[i])) ++i;  // bytes >= 0x80: Unicode names
    const std::string name = s.substr(nameStart, i - nameStart);
    skipWs();
    if (name.empty() || i >= s.size() || s[i] != '=') {
      *err = "expression list must be `name=expression [name2=expression2 ...]` (got `" + raw + "`)";
      return {};
    }
    ++i;  // '='
    // The expression runs to the next `NAME=` at bracket depth zero, or to the end.
    const size_t exprStart = i;
    int depth = 0;
    size_t exprEnd = s.size();
    size_t j = i;
    while (j < s.size()) {
      const char c = s[j];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
        // Look ahead: is the next token a `NAME=`? If so, this space ends the
        // expression; otherwise it is a space INSIDE the expression.
        size_t k = j;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        size_t m = k;
        while (m < s.size() && cecon::ident::isIdentByte(s[m])) ++m;
        size_t n = m;
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (m > k && n < s.size() && s[n] == '=' && (n + 1 >= s.size() || s[n + 1] != '=')) {
          exprEnd = j;
          break;
        }
      }
      ++j;
    }
    if (j >= s.size()) exprEnd = s.size();
    std::string expr = s.substr(exprStart, exprEnd - exprStart);
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) expr.pop_back();
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) expr.erase(0, 1);
    if (expr.empty()) {
      *err = "empty expression for `" + name + "`";
      return {};
    }
    out.push_back(ExpItem{name, expr});
    i = exprEnd;
  }
  if (out.empty()) *err = "expression list required";
  return out;
}

// Evaluate one exp_list element AGAINST THE CURRENT SESSION STATE — i.e. after
// the inner command has just run and posted its r()/e()/_b[]. This is the same
// resolver `display r(mean)` uses, which is exactly the point: the exp_list can
// say anything a display can.
std::optional<double> evalStat(const std::string& expr, SessionState& session) {
  // TWO resolvers, in this order, because they cover different names and
  // neither is a superset of the other:
  //   expandMacros  — locals/globals, r(), e(), s()
  //   evalArith     — the estimation scalars `_b[...]`/`_se[...]`
  //                   (resolveEstScalarRefs), matrix subscripts, arithmetic
  // Using only the first leaves `_b[x1]` unresolved; using only the second
  // leaves `r(mean)` unresolved. An exp_list may hold either, so it gets both.
  std::string text = expr;
  if (eval::hasMacroRefs(text)) {
    eval::ExpandResult r = eval::expandMacros(text, session.eval);
    if (r.isError) return std::nullopt;
    text = r.line;
  }
  std::optional<std::string> folded = eval::evalArith(text, &session.eval);
  const std::string& out = folded.has_value() ? *folded : text;
  try {
    size_t pos = 0;
    const double v = std::stod(out, &pos);
    while (pos < out.size() && std::isspace(static_cast<unsigned char>(out[pos]))) ++pos;
    if (pos != out.size()) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

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


}  // namespace resampling_detail

namespace {

// --- Stata table rendering ---------------------------------------------------
// The oracle log IS the layout spec: these reproduce the column widths and the
// rules of `bootstrap`/`jackknife`/`permute` output.
std::string g9(double v) { return fmtStataDefaultG(v); }

}  // namespace

namespace resampling_detail {

// BUG-183: a COEFFICIENT-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer — the coefficient table reserves the sign column, so the
// digits get 8 characters where `display` gets 9. `fmtStataDefaultG` is
// calibrated for `display` (its own doc comment records
// `2.157789219986937 -> "2.1577892"`, nine characters) and is correct THERE, so
// the fix is a second, narrower entry point at the table call sites rather than
// a change to the shared renderer — moving the shared one would break `display`.
//
// Exposure is narrow and was measured, not assumed: the two rules coincide below
// |1| (".2071479" is eight characters either way) and, above it, whenever the 9th
// character would be a trailing zero, which `%g` strips. So this only prints
// differently when |value| >= 1 AND its 8th significant digit is nonzero — which
// is why the P6 answerbook stayed byte-identical (its jackknife CI upper is
// `1.099479`, exactly the trailing-zero case) and why this was filed LATENT.
//
// Kept verbatim in sync with `g9cell` in engine_report_support.cpp, which is where the P-QE
// table arc first hit this wall (rendering did_imputation, whose tau values are
// all > 1). That file owns the original; this is the documented
// duplicate-and-keep-in-sync pattern this codebase already uses for TU-local
// helpers that cannot be shared without widening a header (see strutil.h's note).
// Oracle anchors for the rule itself: did_imputation tau0 1.2081958740803342
// prints `1.208196` not `1.2081959` (p3_stata.log:326); P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64).
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
std::string fixed2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return std::string(buf);
}
std::string fixed3(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return std::string(buf);
}
std::string fixed4(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return std::string(buf);
}

// A posted e() matrix (replaces one of the same name).
void setEMatrix(SessionState& session, const std::string& name, eval::EvalMatrix m) {
  for (auto& kv : session.eval.eMatrices) {
    if (kv.first == name) {
      kv.second = std::move(m);
      return;
    }
  }
  session.eval.eMatrices.emplace_back(name, std::move(m));
}

// _prefix_title: the inner e(title) when it has one, else bootstrap.ado's
// table of titles for a few commands, else "Bootstrap results".
std::string bootstrapTitle(const std::string& innerTitle, const std::string& cmdname) {
  if (!innerTitle.empty()) return innerTitle;
  static const std::vector<std::pair<std::string, std::string>> k = {
      {"cnreg", "Censored normal regression"},   {"logit", "Logistic regression"},
      {"mlogit", "Multinomial logistic regression"}, {"ologit", "Ordered logistic regression"},
      {"oprobit", "Ordered probit regression"},  {"probit", "Probit regression"},
      {"regress", "Linear regression"},          {"tobit", "Tobit regression"}};
  for (const auto& [c, t] : k)
    if (c == cmdname) return t;
  return "Bootstrap results";
}

}  // namespace resampling_detail

}  // namespace handlers
}  // namespace piet
