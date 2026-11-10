// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table transform.
#include "tables_internal.h"
#include "piet/handlers/estimates_tables.h"
namespace piet::handlers::table_detail {


// ---------------------------------------------------------------------------
// `transform()`. Declared in piet/handlers.h; see there for the grammar and for
// why it is shared rather than per-writer.
// ---------------------------------------------------------------------------


// Stata's `gettoken` with no `bind` splits on WHITESPACE only, so `exp(@)` and
// `@/(-_b[weight])` are each one token and `pattern(1 1 0 0)` is FOUR. That last
// one is not a detail: it is why paper 083's `transform(@/M 1/M pattern(1 1 0 0)
// @*100 100 pattern(0 0 1 1))` -- no comma, so pattern() cannot be a suboption --
// reads `pattern(1` as a coefficient pattern and silently transforms nothing in
// Stata (w1b_transform.log tr_083_verb, rc 0 and byte-identical to the control).
std::vector<std::string> trTokens(const std::string& s) { return splitWs(s); }


// Split at the FIRST top-level comma: `transform(<list> [, pattern(...)])`.
void trSplitSubopts(const std::string& raw, std::string* list, std::string* subs) {
  int depth = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') --depth;
    else if (c == ',' && depth == 0) {
      *list = raw.substr(0, i);
      *subs = raw.substr(i + 1);
      return;
    }
  }
  *list = raw;
  subs->clear();
}


// estout `MatchCoef`. A token with no colon matches the NAME; one with
// a colon matches equation and name separately, and an empty half is `*`. This
// engine's coeftable carries no equation for a single-equation model, and
// `transform(*:*mpg* @*2 2)` still transforms `mpg` there (w1b_transform.log
// r9) -- `globMatch("*", "")` is true, so that falls out rather than needing a
// case.
bool trMatch(const std::string& pat, const std::string& eq, const std::string& name) {
  const std::string p = strutil::trim(pat);
  if (p.empty() || p == "*") return true;
  const size_t colon = p.find(':');
  std::string eqPat, xPat;
  if (colon == std::string::npos) {
    xPat = p;
  } else {
    eqPat = strutil::trim(p.substr(0, colon));
    xPat = strutil::trim(p.substr(colon + 1));
  }
  if (eqPat.empty()) eqPat = "*";
  if (xPat.empty()) xPat = "*";
  return strutil::globMatch(eqPat, eq) && strutil::globMatch(xPat, name);
}


// `@` stands for a MATRIX ELEMENT in the ado (`subinstr "@" "\`b'"`, where `b` is
// `bc[i,j]`), which is atomic -- so the substituted literal is PARENTHESISED.
// Without that, `@^2` on a negative coefficient would parse as -(x^2) here and
// as (+x)^2 there, and the sign of every squared transform would differ.
std::string trSubstAt(const std::string& expr, double at) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "(%.17g)", at);
  std::string out;
  for (char c : expr) {
    if (c == '@') out += buf;
    else out += c;
  }
  return out;
}
}
namespace piet::handlers {
using namespace table_detail;


bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err) {
  std::string list, subs;
  trSplitSubopts(dequote(raw), &list, &subs);
  if (!strutil::trim(subs).empty()) {
    // `splitCall` is declared further down this file, so the one suboption is
    // parsed here rather than reaching forward for it.
    const std::string sub = strutil::trim(subs);
    const size_t open = sub.find('(');
    const size_t close = sub.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        strutil::lower(strutil::trim(sub.substr(0, open))) != "pattern") {
      *err = "transform(): the only suboption is pattern(); `" + sub + "` names something else";
      return false;
    }
    const std::string inner = sub.substr(open + 1, close - open - 1);
    for (const auto& w : splitWs(inner)) {
      if (w != "0" && w != "1") {
        // estout says so in as many words, and it is r(198)
        // (w1b_transform2.log tr_pat_two).
        *err = "invalid pattern in transform(,pattern()) or eform()";
        return false;
      }
      out->pattern.push_back(w);
    }
  }
  // The ado's own loop, which needs no special case for one or two tokens: with
  // two, the first iteration already has an empty `df` and an empty rest, so
  // (coef, f) slides to (f, df) and the pattern becomes empty.
  const std::vector<std::string> toks = trTokens(list);
  size_t i = 0;
  while (i < toks.size()) {
    std::string coef = toks[i];
    std::string f = (i + 1 < toks.size()) ? toks[i + 1] : std::string();
    std::string df = (i + 2 < toks.size()) ? toks[i + 2] : std::string();
    const bool restEmpty = (i + 3 >= toks.size());
    if (df.empty() && restEmpty) {
      df = f;
      f = coef;
      coef.clear();
    }
    TransformGroup g;
    g.pat = coef;
    g.f = f;
    g.df = df;
    out->groups.push_back(g);
    i += 3;
  }
  return true;
}


TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session) {
  TransformCell out;
  if (spec.empty() || !spec.appliesToModel(modelIdx) || std::isnan(b)) return out;
  std::string eq;
  if (const ojson* row = coefRow(snap, coefName)) eq = row->value("eq", std::string());
  for (const auto& g : spec.groups) {
    if (!trMatch(g.pat, eq, coefName)) continue;
    out.matched = true;
    if (g.f.empty()) break;
    TrScopedModel guard(session.eval, snap);
    // `@` is the coefficient for BOTH halves: the se's multiplier is the
    // derivative evaluated at b, not at the se (estout reads
    // `bc[i,1]`), which is why `eform` spells itself `exp(@) exp(@)`.
    const std::optional<double> nb = piet::evalScalarExpr(trSubstAt(g.f, b), session);
    if (!nb.has_value() || !std::isfinite(*nb)) {
      out.bMissing = true;  // exp(1946) is `.` in the oracle too (u1's _cons)
    } else {
      out.b = *nb;
    }
    if (!g.df.empty()) {
      const std::optional<double> d = piet::evalScalarExpr(trSubstAt(g.df, b), session);
      if (d.has_value() && std::isfinite(*d)) out.seFactor = std::fabs(*d);
      else out.seFactor = std::numeric_limits<double>::quiet_NaN();
    }
    break;
  }
  return out;
}
}
