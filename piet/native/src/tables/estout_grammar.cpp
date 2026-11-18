// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// estout grammar.
#include "tables_internal.h"
#include "estout_internal.h"
#include <set>
namespace piet::handlers::table_detail {


// Split `list , subopts` at the first comma outside quotes and parentheses.
void eoSplitComma(const std::string& raw, std::string* lhs, std::string* rhs) {
  int depth = 0;
  bool inQ = false;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '"') inQ = !inQ;
    if (inQ) continue;
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      *lhs = raw.substr(0, i);
      *rhs = raw.substr(i + 1);
      return;
    }
  }
  *lhs = raw;
  rhs->clear();
}


// Stata `syntax` abbreviation: the CAPITALS of `spec` are the minimum, the
// whole word (lower-cased) the maximum.
bool eoAbbr(const std::string& given, const char* spec) {
  const std::string full = strutil::lower(spec);
  size_t minLen = 0;
  while (spec[minLen] != '\0' && std::isupper(static_cast<unsigned char>(spec[minLen]))) ++minLen;
  if (minLen == 0) minLen = full.size();
  return given.size() >= minLen && given.size() <= full.size() &&
         full.compare(0, given.size(), given) == 0;
}


std::string eoReplaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t at = 0;
  while ((at = s.find(from, at)) != std::string::npos) {
    s.replace(at, from.size(), to);
    at += to.size();
  }
  return s;
}


// estout Abbrev.
std::string eoAbbrev(int width, const std::string& value, bool abbrev) {
  if (!abbrev) return value;
  if (width > 32) return cecon::ident::truncateCodePoints(value, static_cast<size_t>(width));
  if (width > 0 && static_cast<int>(cecon::ident::codePointCount(value)) > width)
    return eoStataAbbrev(value, width);
  return value;
}


// Suboptions of one element (estout ParseValueSubopts).
bool eoParseElemSubopts(const std::string& inner, EoElem* el, std::string* err) {
  for (const auto& [nm, arg] : eoSubopts(inner)) {
    if (eoAbbr(nm, "Star")) {
      el->star = true;
    } else if (eoAbbr(nm, "NOStar")) {
      el->star = false;
    } else if (eoAbbr(nm, "Fmt")) {
      el->fmt = eoTokens(arg.value_or(""));
    } else if (eoAbbr(nm, "Label")) {
      const std::vector<std::string> t = eoTokens(arg.value_or(""));
      el->label = arg.has_value() ? (arg->find('"') == std::string::npos ? *arg
                                                                          : (t.empty() ? "" : t[0]))
                                  : "";
      el->labelSet = true;
    } else if (nm == "par") {
      el->hasPar = true;
      if (arg.has_value()) {
        el->par = eoTokens(*arg);
      }
    } else if (nm == "nopar") {
      el->hasPar = false;
    } else if (eoAbbr(nm, "Vacant")) {
      const std::vector<std::string> t = eoTokens(arg.value_or(""));
      el->vacant = t.empty() ? std::string() : t[0];
    } else if (eoAbbr(nm, "PVALue")) {
      el->pvalue = eoTrim(arg.value_or("p"));
    } else if (eoAbbr(nm, "PATtern")) {
      el->pattern = eoTokens(arg.value_or(""));
      for (const auto& p : el->pattern) {
        if (p != "0" && p != "1") {
          *err = "\"" + arg.value_or("") + "\" invalid in " + el->name + "(... pattern())";
          return false;
        }
      }
    } else if (nm == "abs") {
      el->abs = true;
    } else if (nm == "noabs") {
      el->abs = false;
    } else if (eoAbbr(nm, "Transpose") || eoAbbr(nm, "NOTranspose")) {
      // a 1xk matrix is read as a row either way in this build
    } else if (eoAbbr(nm, "Keep") || eoAbbr(nm, "Drop") || nm == "mrow") {
      *err = "estout: cells() element suboption " + nm + "() is not implemented in this build";
      return false;
    } else {
      *err = "option " + nm + " not allowed";
      return false;
    }
  }
  if (el->hasPar && el->par.empty()) {
    if (el->name == "ci") el->par = {"[", ",", "]"};
    else if (el->name == "ci_l") el->par = {"[", ""};
    else if (el->name == "ci_u") el->par = {"", "]"};
    else el->par = {"(", ")"};
  }
  return true;
}


// One row's text (`b se`, `b & se`, `b(fmt(3)) . se`): columns separated by
// blanks, elements of one column joined by `&`.
bool eoParseCellRow(const std::string& row, EoCells* out, std::string* err) {
  std::vector<std::vector<int>> cols;
  std::vector<int> cur;
  bool joinNext = false;
  bool haveCur = false;
  size_t i = 0;
  const size_t n = row.size();
  auto flush = [&]() {
    if (haveCur) cols.push_back(cur);
    cur.clear();
    haveCur = false;
  };
  while (i < n) {
    const char c = row[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '&') {
      joinNext = true;
      ++i;
      continue;
    }
    // one token: name[(...)]
    size_t j = i;
    while (j < n && row[j] != '(' && row[j] != ' ' && row[j] != '\t' && row[j] != '&') ++j;
    std::string name = row.substr(i, j - i);
    std::string inner;
    if (j < n && row[j] == '(') {
      int depth = 0;
      bool inQ = false;
      size_t k = j;
      for (; k < n; ++k) {
        if (row[k] == '"') inQ = !inQ;
        if (inQ) continue;
        if (row[k] == '(') ++depth;
        if (row[k] == ')' && --depth == 0) break;
      }
      if (k >= n) {
        *err = "unmatched parentheses in cells()";
        return false;
      }
      inner = row.substr(j + 1, k - j - 1);
      j = k + 1;
    }
    i = j;
    if (!joinNext) flush();
    joinNext = false;
    haveCur = true;
    if (name == ".") continue;  // the placeholder: an empty column
    if (name.find('[') != std::string::npos) {
      *err = "estout: cells() row selection `name[#]` is not implemented in this build";
      return false;
    }
    EoElem el;
    el.name = name;
    if (!eoParseElemSubopts(inner, &el, err)) return false;
    out->elems.push_back(el);
    cur.push_back(static_cast<int>(out->elems.size()) - 1);
  }
  flush();
  if (!cols.empty()) {
    out->ncols = std::max(out->ncols, cols.size());
    out->rows.push_back(cols);
  }
  return true;
}


// estout (the cells() parse; none / CellsCheck): an
// unquoted bound word is a ROW of one element; a quoted word (or a
// parenthesised group) is a row of side-by-side columns.
bool eoParseCells(const std::string& raw, EoCells* out, std::string* err) {
  const std::string s = eoTrim(raw);
  if (s == "none") {
    out->none = true;
    return true;
  }
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) break;
    if (s[i] == '"' || (s[i] == '`' && i + 1 < n && s[i + 1] == '"')) {
      const bool compound = s[i] == '`';
      size_t j = i + (compound ? 2 : 1);
      size_t end = std::string::npos;
      if (compound) {
        end = s.find("\"'", j);
      } else {
        end = s.find('"', j);
      }
      if (end == std::string::npos) end = n;
      if (!eoParseCellRow(s.substr(j, end - j), out, err)) return false;
      i = end + (compound ? 2 : 1);
      continue;
    }
    if (s[i] == '(') {
      int depth = 0;
      size_t k = i;
      for (; k < n; ++k) {
        if (s[k] == '(') ++depth;
        if (s[k] == ')' && --depth == 0) break;
      }
      if (k >= n) {
        *err = "unmatched parentheses in cells()";
        return false;
      }
      if (!eoParseCellRow(s.substr(i + 1, k - i - 1), out, err)) return false;
      i = k + 1;
      continue;
    }
    // an unquoted word bound through its parentheses
    size_t j = i;
    int depth = 0;
    bool inQ = false;
    while (j < n) {
      const char c = s[j];
      if (c == '"') inQ = !inQ;
      if (!inQ) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if ((c == ' ' || c == '\t') && depth == 0) break;
      }
      ++j;
    }
    std::string word = s.substr(i, j - i);
    i = j;
    // `b & se` written unquoted still joins: estout's gettoken parses `&`.
    if (word == "&") {
      if (out->rows.empty()) {
        *err = "invalid cells()";
        return false;
      }
      // join the next element into the last row's last column
      while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
      size_t k = i;
      depth = 0;
      inQ = false;
      while (k < n) {
        const char c = s[k];
        if (c == '"') inQ = !inQ;
        if (!inQ) {
          if (c == '(') ++depth;
          if (c == ')') --depth;
          if ((c == ' ' || c == '\t') && depth == 0) break;
        }
        ++k;
      }
      EoCells tmp;
      tmp.elems = out->elems;
      if (!eoParseCellRow(s.substr(i, k - i), &tmp, err)) return false;
      if (!tmp.rows.empty() && !tmp.rows.back().empty()) {
        auto& lastCol = out->rows.back().back();
        for (int idx : tmp.rows.back().front()) lastCol.push_back(idx);
        out->elems = tmp.elems;
      }
      i = k;
      continue;
    }
    if (!eoParseCellRow(word, out, err)) return false;
  }
  return true;
}


std::string eoStringArg(const std::optional<std::string>& a) {
  // `Prefix(string)`: syntax strips ONE pair of outer quotes.
  if (!a.has_value()) return std::string();
  const std::string t = eoTrim(*a);
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
  if (t.size() >= 4 && t.rfind("`\"", 0) == 0 && t.compare(t.size() - 2, 2, "\"'") == 0) {
    return t.substr(2, t.size() - 4);
  }
  return *a;
}


bool eoParseLabelOpt(const std::string& raw, EoLabelOpt* o, std::string* err) {
  o->given = true;
  if (eoTrim(raw) == "none") {
    o->none = true;
    return true;
  }
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  o->list = eoTokens(lhs);
  auto lines = [](const std::optional<std::string>& a) {
    const std::string v = a.value_or("");
    if (v.find('"') == std::string::npos) return std::vector<std::string>{v};
    return eoTokens(v);
  };
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "NUMbers")) o->numbers = true;
    else if (eoAbbr(nm, "NONUMbers")) o->numbers = false;
    else if (eoAbbr(nm, "TItles")) o->titles = true;
    else if (eoAbbr(nm, "NOTItles")) o->notitles = true;
    else if (eoAbbr(nm, "DEPvars")) o->depvars = true;
    else if (eoAbbr(nm, "NODEPvars")) o->nodepvars = true;
    else if (nm == "none") o->none = true;
    else if (nm == "nonone") o->none = false;
    else if (nm == "span") o->span = true;
    else if (nm == "nospan") o->span = false;
    else if (eoAbbr(nm, "Prefix")) o->prefix = eoStringArg(arg);
    else if (eoAbbr(nm, "Suffix")) o->suffix = eoStringArg(arg);
    else if (eoAbbr(nm, "Begin")) o->begin = lines(arg);
    else if (eoAbbr(nm, "End")) o->end = lines(arg);
    else if (eoAbbr(nm, "Replace")) o->replace = true;
    else if (eoAbbr(nm, "NOReplace")) o->replace = false;
    else if (eoAbbr(nm, "BList")) o->blist = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "EList")) o->elist = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "ERepeat")) o->erepeat = eoStringArg(arg);
    else if (eoAbbr(nm, "First")) o->first = 1;
    else if (eoAbbr(nm, "NOFirst")) o->first = 0;
    else if (eoAbbr(nm, "Last")) o->last = 1;
    else if (eoAbbr(nm, "NOLast")) o->last = 0;
    else if (nm == "lhs") o->lhs = eoStringArg(arg);
    else if (eoAbbr(nm, "PATtern")) o->pattern = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "Merge")) o->merge = true;
    else if (eoAbbr(nm, "NOMerge")) o->merge = false;
    else {
      *err = "option " + nm + " not allowed";
      return false;
    }
  }
  // estout: merge implies the `:` suffix unless one is given.
  if (o->merge && o->suffix.empty()) o->suffix = ":";
  return true;
}


bool eoIsCoefStat(const std::string& s) {
  static const std::set<std::string> k = {"b",  "se",   "var",   "t",     "z",     "p",
                                          "ci", "ci_l", "ci_u",  "_star", "_sign", "_sigsign"};
  return k.count(s) != 0;
}


// `star(* 0.10 ** 0.05)` -> pairs; false when the list is not symbol/level pairs.
bool eoParseStarPairs(const std::string& raw, std::vector<std::pair<std::string, std::string>>* out,
                      std::string* labelOut, std::string* delimOut, EoError* err) {
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  const std::vector<std::string> t = eoTokens(lhs);
  if (t.size() % 2 != 0) {
    err->msg = "unmatched list of significance symbols and levels";
    err->rc = 198;
    return false;
  }
  double prev = 1.0;
  for (size_t i = 0; i + 1 < t.size(); i += 2) {
    double lv = 0.0;
    try {
      size_t used = 0;
      lv = std::stod(t[i + 1], &used);
      if (used != t[i + 1].size()) throw std::invalid_argument("x");
    } catch (...) {
      err->msg = "'" + t[i + 1] + "' found where number expected";
      err->rc = 7;
      return false;
    }
    if (lv > prev || lv <= 0.0) {
      err->msg = "significance levels out of order or out of range (0,1]";
      err->rc = 198;
      return false;
    }
    prev = lv;
    out->emplace_back(t[i], t[i + 1]);
  }
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "Label") && labelOut != nullptr) *labelOut = eoStringArg(arg);
    if (eoAbbr(nm, "Delimiter") && delimOut != nullptr) *delimOut = eoStringArg(arg);
  }
  return true;
}


// estout's stats(keys, fmt() labels() star) (estout ParseStatsSubopts,
// ProcessStatslayout): per-key formats run forward, the default being the
// first cell element's first format.
bool eoParseStats(const std::string& raw, const std::string& defaultFmt, EoSpec* sp,
                  EoError* err) {
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  std::vector<std::string> keys;
  for (const auto& k : eoTokens(lhs)) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  std::vector<std::string> fmts;
  std::set<std::string> starred;
  bool nostar = false;
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "Fmt")) {
      fmts = eoTokens(arg.value_or(""));
    } else if (eoAbbr(nm, "Labels")) {
      if (!eoParseLabelOpt(arg.value_or(""), &sp->statslabels, &err->msg)) {
        err->rc = 198;
        return false;
      }
    } else if (nm == "star") {
      if (arg.has_value()) {
        for (const auto& s : eoTokens(*arg)) {
          starred.insert(s);
          if (std::find(keys.begin(), keys.end(), s) == keys.end()) keys.push_back(s);
        }
      } else if (!keys.empty()) {
        starred.insert(keys.front());
      }
    } else if (eoAbbr(nm, "NOStar")) {
      nostar = true;
    } else if (eoAbbr(nm, "LAYout") || eoAbbr(nm, "PChar")) {
      err->msg = "estout: stats(..., " + nm + "()) is not implemented in this build";
      err->rc = 198;
      return false;
    } else {
      err->msg = "option " + nm + " not allowed";
      err->rc = 198;
      return false;
    }
  }
  if (fmts.empty()) fmts.push_back(defaultFmt);
  std::string format = "%9.0g";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i < fmts.size() && !fmts[i].empty()) format = fmts[i];
    EoStatRow r;
    r.key = keys[i];
    r.fmt = format;
    r.star = !nostar && starred.count(keys[i]) != 0;
    sp->stats.push_back(r);
  }
  return true;
}


// vs-stata bank 2026-11-10 (StataBE 19.5, estout 3.x): `estout m1 m2 m3,
// var(mpg "Miles")` and the esttab twin r(121) "varwidth() invalid -- invalid
// number" (c8c0df3522257b53, 0af0f59c2e4c3dde; `var(30)` runs); `s(* 0.1 **
// 0.05)`, `st(..)`, `sta(..)` and `scalars(r2) s(%9.4f)` r(7) "* invalid name"
// (c369a328aab710a2, 9c7a522a41f163ce, c8eadb8f34764c47; `s(N r2)`, `st(mpg)`
// run); esttab `ma(b 2)` r(198) "margin(b 2) invalid" (a6800b29a2804408);
// esttab `nocons constant` r(198) "options constant and noconstant not both
// allowed" (2fac90176decdc9a).
std::optional<EoError> eoArgumentError(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool esttab) {
  // syntax matches each declared option once: an option that takes an argument and is given again
  // under another spelling (`cells("b se") c("Coef")`) matches nothing the second time and is r(198)
  // "option c() not allowed" (vs-stata bank 94c113e7, c61a22f8).
  {
    std::set<std::string> seen;
    for (const auto& [k, v] : opts) {
      if (!v.isBool && !seen.insert(k).second) return EoError{"option " + k + "() not allowed", 198};
    }
  }
  auto lowerOf = [](std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  // `key` spells `full` when it is a prefix of it at least `minLen` long.
  auto spells = [](const std::string& key, const std::string& full, size_t minLen) {
    return key.size() >= minLen && key.size() <= full.size() &&
           full.compare(0, key.size(), key) == 0;
  };
  auto isName = [](const std::string& t) {
    if (t.empty() || t.size() > 32) return false;
    const unsigned char c0 = static_cast<unsigned char>(t[0]);
    if (!(std::isalpha(c0) || c0 == '_')) return false;
    for (unsigned char c : t)
      if (!(std::isalnum(c) || c == '_')) return false;
    return true;
  };
  bool neg = false, pos = false;
  for (const auto& [kRaw, v] : opts) {
    const std::string k = lowerOf(kRaw);
    if (esttab) {
      if (spells(k, "noconstant", 6)) neg = true;
      if (spells(k, "constant", 4)) pos = true;
    }
    if (v.isBool) continue;
    const std::string arg = eoTrim(v.strValue);
    if (spells(k, "varwidth", 3)) {
      const std::vector<std::string> toks = eoTokens(arg);
      for (const auto& t : toks) {
        char* end = nullptr;
        const double d = std::strtod(t.c_str(), &end);
        if (t.empty() || end == nullptr || *end != '\0')
          return EoError{"varwidth() invalid -- invalid number", 121};
        if (d != std::floor(d))
          return EoError{"varwidth() invalid -- invalid numlist has noninteger elements", 126};
        if (d < 0)
          return EoError{"varwidth() invalid -- invalid numlist has elements outside of allowed range",
                         125};
      }
      if (toks.size() > 1)
        return EoError{"varwidth() invalid -- invalid numlist has too many elements", 123};
    } else if (spells(k, "stats", 1)) {
      std::string lhs, rhs;
      eoSplitComma(arg, &lhs, &rhs);
      for (const auto& t : eoTokens(lhs))
        if (!isName(t)) return EoError{t + " invalid name", 7};
    } else if (spells(k, "margin", 2)) {
      const std::string m = lowerOf(arg);
      if (!m.empty() && m != "margin" && m != "u" && m != "c" && m != "p")
        return EoError{"margin(" + arg + ") invalid", 198};
    }
  }
  if (neg && pos) return EoError{"options constant and noconstant not both allowed", 198};
  return std::nullopt;
}
}
