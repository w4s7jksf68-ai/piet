// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table text.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// Errors
// ===========================================================================

ojson tblError(const std::string& message, const std::string& code ) {
  return ojson(eval::engineError(message, code));
}


// A USER SYNTAX error. `syntax_error` is what coarseRc maps to rc 198; the
// default `engine_error` maps to rc 1, and bundle C had to repair six sites
// that had made exactly that substitution.
ojson tblSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}


// An error whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson tblErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}


ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}


ojson echoBlock(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}


bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}


// ===========================================================================
// Option access
// ===========================================================================

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}


bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  // A bare flag parses as isBool/true; `tex(frag)` parses as a STRING value but
  // still means "tex was given". Both forms answer yes -- reading only the bool
  // form is how `outreg2 ..., tex(frag)` would have quietly written plain text.
  return v->isBool ? v->boolValue : true;
}


std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return v->strValue;
}


// ===========================================================================
// Small text helpers
// ===========================================================================

std::string dequote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}


std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}


// Split at TOP-LEVEL whitespace only: `b(star fmt(3)) se(par)` is two items,
// not five. Quotes count as depth too, so `label("Std. err.")` survives.
std::vector<std::string> splitTopLevel(const std::string& s, char sep ) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQuote = false;
  for (char c : s) {
    if (c == '"') inQuote = !inQuote;
    if (!inQuote) {
      if (c == '(' || c == '[' || c == '{') ++depth;
      if (c == ')' || c == ']' || c == '}') --depth;
    }
    const bool isSep = (sep == ' ') ? (c == ' ' || c == '\t') : (c == sep);
    if (isSep && depth == 0 && !inQuote) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}


std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}


// The whole string is `r( NAME )`. NAME is read with core/ident.h's permissive
// tier (a byte >= 0x80 is a name byte), by hand rather than by regex: a narrow
// `char` bracket range over the high bytes is not portable across standard
// libraries.
bool parseWholeRName(const std::string& s, std::string* name) {
  if (s.size() < 4 || s[0] != 'r' || s[1] != '(') return false;
  size_t j = 2;
  while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
  const size_t b = j;
  if (j >= s.size() || !cecon::ident::isIdentStartByte(s[j])) return false;
  while (j < s.size() && cecon::ident::isIdentByte(s[j])) ++j;
  const size_t e = j;
  while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
  if (j + 1 != s.size() || s[j] != ')') return false;
  *name = s.substr(b, e - b);
  return true;
}


// `<integer>.<name>`: a factor-level coefficient (`2.قيمة`).
bool parseLevelName(const std::string& s, long long* level, std::string* var) {
  size_t i = 0;
  if (i < s.size() && s[i] == '-') ++i;
  const size_t d0 = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
  if (i == d0 || i >= s.size() || s[i] != '.') return false;
  const size_t dot = i++;
  if (i >= s.size() || !cecon::ident::isIdentStartByte(s[i])) return false;
  for (size_t k = i; k < s.size(); ++k)
    if (!cecon::ident::isIdentByte(s[k])) return false;
  try {
    *level = std::stoll(s.substr(0, dot));
  } catch (const std::exception&) {
    return false;
  }
  *var = s.substr(i);
  return true;
}


AdecCell outreg2AdecCell(double v, int adec) {
  AdecCell out;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", adec, v);
  const std::string plain(buf);
  if (plain.size() > 12) {
    // Stata's width overflow. The mantissa carries adec decimals up to five, and
    // adec(0) carries five as well: 1e12 is `1.0e+12` at adec(1), `1.000e+12` at
    // adec(3), `1.00000e+12` at adec(0), adec(5), adec(6) and adec(11)
    // (w1b_adec_rule4.log u1/u2/u4/u5/u6 and w1b_adec_rule3.log t6).
    const int edec = (adec >= 1 && adec <= 5) ? adec : 5;
    std::snprintf(buf, sizeof(buf), "%.*e", edec, v);
    out.text = buf;
    return out;
  }
  std::string sep = plain;
  const size_t dot = sep.find('.');
  const size_t start = (!sep.empty() && sep[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? sep.size() : dot;
  int inserted = 0;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    sep.insert(at, ",");
    ++inserted;
  }
  if (inserted > 0 && sep.size() <= 12) {
    out.text = sep;
    out.seps = inserted;
    return out;
  }
  out.text = plain;  // the separated spelling did not fit, so the ado drops it
  return out;
}


// estout/esttab's default ladder and outreg2's are DIFFERENT, and this is not
// cosmetic: the same p = 0.03 gets one star from estout and two from outreg2.
// The plan's Stata-semantics section fixes both explicitly for that reason.
const std::vector<StarLevel>& estoutStars() {
  static const std::vector<StarLevel> k = {{"***", 0.001}, {"**", 0.01}, {"*", 0.05}};
  return k;
}

const std::vector<StarLevel>& outreg2Stars() {
  static const std::vector<StarLevel> k = {{"***", 0.01}, {"**", 0.05}, {"*", 0.10}};
  return k;
}


// `using FILE` split out of a varlist. Returns the raw (still user-spelled)
// path, and erases `using` and everything after it from `words`.
std::optional<std::string> takeUsing(std::vector<std::string>& words) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (strutil::lower(words[i]) == "using") {
      std::optional<std::string> path;
      if (i + 1 < words.size()) {
        size_t j = i + 1;
        path = dequote(strutil::rejoinQuotedWord(words, j));  // a quoted path with blanks
      }
      words.erase(words.begin() + static_cast<long>(i), words.end());
      return path;
    }
  }
  return std::nullopt;
}


std::vector<Model> collectModels(SessionState& session, const std::string& cmd,
                                 const std::vector<std::string>& namesIn, ojson* errOut) {
  // W2 esttabnames (BUG-552): a written namelist goes through the ONE
  // shared expansion (SessionState::expandEstimatesNamelist): `A_*`, `A_?`,
  // `*`, `_all`, `.`, in STORE order; a token matching nothing is r(111)
  // "estimation result NAME not found" -- w2_smalls.log section G. The old
  // exact-name lookup answered `esttab A_*` with "model(s) not found" at rc 1.
  std::vector<std::string> names = namesIn.empty() ? session.estimatesNames() : namesIn;
  // With no namelist and nothing stored estout tables `.`, the active results,
  // and with none of those either it is r(301) "last estimates (.) not found"
  // (StataBE 19.5 `clear all` / `estout`, BUG-1248; it was rc 1).
  if (!namesIn.empty() || names.empty()) {
    SessionState::EstNamelist ex =
        session.expandEstimatesNamelist(names.empty() ? std::vector<std::string>{"."} : names);
    if (ex.rc != 0) {
      *errOut = tblErrorRc(ex.message, ex.rc);
      return {};
    }
    names = std::move(ex.names);
  }
  if (names.empty()) {  // `_all` over an empty store
    *errOut = tblError(cmd + ": no stored estimates. Run a regression, then `eststo m1` first.");
    return {};
  }
  std::vector<Model> out;
  for (const auto& n : names) {
    if (n == ".") {
      // The active results, carrying no store name: read straight from e().
      Model m;
      if (!activeModel(session, cmd, &m, errOut)) return {};
      m.name = ".";
      out.push_back(std::move(m));
      continue;
    }
    const ojson* snap = session.estimatesGet(n);
    if (snap == nullptr) {
      // Unreachable after the expansion (it returns stored names or "."); kept
      // so a future caller can never index a null snapshot.
      *errOut = tblErrorRc("estimation result " + n + " not found", 111);
      return {};
    }
    out.push_back(materializeModel(session, n, *snap));
  }
  return out;
}


// The coefficient rows of one model, keyed by name.
const ojson* coefRow(const ojson& snap, const std::string& name) {
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return nullptr;
  for (const auto& r : snap["coeftable"]) {
    if (r.value("name", std::string()) == name) return &r;
  }
  return nullptr;
}


// BUG-496: every one of `keep()`/`drop()`/`order()`'s tokens is a PATTERN, not a
// name. `keep(*romer*)` is how all four of paper 015's transform lines select
// their coefficients and `keep(3:0.MI*#c.romer*)` how another does, and exact
// matching answered them with an EMPTY body at rc 0. `keep(mp?)` is `mpg` too
// (w1b_keepglob.log ka), so it is Stata's `match()`, not a prefix rule. `errOut`
// carries the other half: a keep() token that matches nothing is r(111)
// "coefficient zz* not found" (kg_glob_none), not a table with no rows.
std::vector<std::string> coefNames(const std::vector<Model>& models,
                                   const std::optional<std::string>& keepOpt,
                                   const std::optional<std::string>& dropOpt,
                                   const std::optional<std::string>& orderOpt,
                                   std::string* errOut ) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const auto& m : models) {
    if (!m.snap.contains("coeftable") || !m.snap["coeftable"].is_array()) continue;
    for (const auto& r : m.snap["coeftable"]) {
      const std::string n = r.value("name", std::string());
      if (seen.insert(n).second) names.push_back(n);
    }
  }
  applyCoefSelection(&names, keepOpt, dropOpt, orderOpt, errOut);
  return names;
}


// `varlabels(x "Income" y "Age")` / `coeflabels(...)` -- pairs. Plus `label`,
// which asks for the DATASET's variable labels instead.
std::map<std::string, std::string> parseLabelPairs(const std::string& spec) {
  std::map<std::string, std::string> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec));
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out[toks[i]] = dequote(toks[i + 1]);
  }
  return out;
}
}
