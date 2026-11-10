// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Iv specification.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation_internal.h"
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

namespace est_detail {  // R2: reopened — splitTokensM4/parseAtNumlistM4 cross to TU c

// Splits a plain string on whitespace AND commas — the same token grammar
// the tokenizer's own splitWs already applied to ParsedCommand::varlist
// (each `(endog = instruments)`/reg3-equation substring below is a plain
// std::string this file reconstructs BY HAND from already-split varlist
// tokens, joined back with single spaces — see parseIvParenM4's doc comment
// — so it needs the identical space-or-comma splitter applied a second
// time, not a paren-aware re-tokenize).
std::vector<std::string> splitTokensM4(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string joinSpaceM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// P7: Stata's `at()` value-list grammar, `at(v1=(a b c) v2=#)`. Returns ONE
// engine `--at` spec per point in the cartesian product, each spelled
// `v=val[,v2=val2]` with the value at %.17g. The grammar is the kernel's own
// (cecon::margins_parse_at, the parser the margins grid uses): this function
// used to carry a second one that closed a parenthesised list at its first
// `)`, so after logit/probit/poisson `at(c1=(1(1)3))` was the one point
// `c1=1(1` and `at(c1=(1/3))` the one point `c1=1/3`, both at rc 0 (Stata: three
// points, w2v_margins_siblings.log:2961-3012 F_range_numlist_lg, :3018-3069
// F_range_slash_lg). A list the kernel rejects gives no spec; the caller
// refuses it by name with the kernel's rc (runMargins, marginsAtRefusalM4).
std::vector<std::string> parseAtNumlistM4(const std::string& at) {
  const cecon::MarginsAtParse p = cecon::margins_parse_at(at);
  std::vector<std::string> out;
  if (p.rc != 0) return out;
  for (const auto& pt : p.points) {
    std::string spec;
    for (const auto& [var, val] : pt.fix) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", val);
      spec += (spec.empty() ? "" : ",") + var + "=" + buf;
    }
    out.push_back(spec);
  }
  return out;
}

// BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog = instruments)` /
// `ivreghdfe depvar [exog...] (endog = instruments), absorb(F)` / the
// `reghdfe depvar (endog=instruments), absorb(F)` paren form all share this
// ONE varlist shape: [depvar, exog...]  "(" endog "=" instruments ")"
// [exog...] (the trailing list only where exogAfterParen says so).
// splitWs() (eval/tokenizer.cpp) is whitespace-only, so it already broke
// "(beertax = spirits unemp)" into SEPARATE varlist tokens ("(beertax", "=",
// "spirits", "unemp)") — this rejoins them with single spaces (Stata's own
// whitespace inside the parens carries no meaning) and re-splits on the
// paren/equals structure instead.


IvParenResult parseIvParenM4(const std::vector<std::string>& tokens, bool exogAfterParen) {
  IvParenResult r;
  std::string joined = joinSpaceM4(tokens);
  size_t lp = joined.find('(');
  if (lp == std::string::npos) {
    r.errored = true;
    r.errorMsg = "requires an endogenous-regressors block: (endog = instruments)";
    return r;
  }
  size_t rpLast = joined.rfind(')');
  if (rpLast == std::string::npos || rpLast < lp) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing closing ')'";
    return r;
  }
  std::string beforeStr = joined.substr(0, lp);
  std::string insideStr = joined.substr(lp + 1, rpLast - lp - 1);
  std::string afterStr = joined.substr(rpLast + 1);
  // Only ONE parenthesised block is valid ivregress/ivreghdfe syntax — a
  // stray extra '('/')' anywhere else (before/inside/after) means either a
  // second block or an unbalanced paren; reject rather than silently
  // mis-slicing the wrong substring as the endog/instrument list.
  if (beforeStr.find(')') != std::string::npos || insideStr.find('(') != std::string::npos ||
      insideStr.find(')') != std::string::npos || afterStr.find('(') != std::string::npos ||
      afterStr.find(')') != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed syntax: only one (endog = instruments) block is supported";
    return r;
  }
  std::vector<std::string> before = splitTokensM4(beforeStr);
  std::vector<std::string> after = splitTokensM4(afterStr);
  if (before.empty()) {
    r.errored = true;
    r.errorMsg = "requires a dependent variable before the (endog = instruments) block";
    return r;
  }
  if (!after.empty() && !exogAfterParen) {
    r.errored = true;
    r.errorMsg = "unexpected tokens after the (endog = instruments) block: " + joinSpaceM4(after);
    return r;
  }
  size_t eq = insideStr.find('=');
  if (eq == std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing '='";
    return r;
  }
  if (insideStr.find('=', eq + 1) != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: only one '=' is allowed";
    return r;
  }
  std::vector<std::string> endog = splitTokensM4(insideStr.substr(0, eq));
  std::vector<std::string> instr = splitTokensM4(insideStr.substr(eq + 1));
  if (endog.empty() || instr.empty()) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: need at least one endogenous regressor "
                 "and at least one instrument";
    return r;
  }
  r.y = before[0];
  r.exog.assign(before.begin() + 1, before.end());
  // Exogenous regressors typed AFTER the block join the ones typed before it,
  // in typed order: w2b_estim.log:923-945 `ivregress 2sls y (x1 = z1) x2` fits
  // the `y x2 (x1 = z1)` model (b_x1 1.940390224456596 both ways, "Exogenous:
  // x2 z1"), and :1185/:1243 do the same for ivreghdfe.
  r.exog.insert(r.exog.end(), after.begin(), after.end());
  r.endog = endog;
  r.instruments = instr;
  return r;
}

}  // namespace est_detail

}  // namespace piet::handlers
