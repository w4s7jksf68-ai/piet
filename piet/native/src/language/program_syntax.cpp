// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/program.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"
#include "program_internal.h"

namespace piet::program {

using namespace program_detail;

// ---------------------------------------------------------------------------
// `syntax` -- Stata's argument parser for a program's `0' (the arguments as
// typed), after [P] syntax:
//   syntax [varlist|varname|newvarlist|newvarname|namelist|name|anything[(spec)]]
//          [if[/]] [in[/]] [using[/]] [=[/]exp] [weight-types[/]] [, options]
// each element optionally in [ ] (optional), and the options either all inside
// `[, ...]' or, after a bare `,', required words and optional [ ] groups.
//
// Bound locals: varlist (+ typlist for new variables), namelist/anything (or
// the name= override), if, in, using, exp, weight, and one local per option.
// A spec StataBE itself rejects is its r(197) "invalid syntax" (truth bank:
// `syntax varlist(bogus)`, `syntax [, opt(bogus)]`, an optional `Level(integer)`
// without a default, `GEN(newvarname)` as an option type). What this binder
// cannot do (the `generate` specifier of a new varlist) is refused BY NAME --
// never a partial bind (w2j_e3_ritest.do `_e3_inv`).
namespace {

ojson synErr(const std::string& msg, int rc, const std::string& code = "syntax_error") {
  return ojson(eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson specErr() { return synErr("invalid syntax", 197); }

struct SynItem {
  std::string text;
  bool optional = false;
};

// Top-level items of a spec (or of an argument string's option tail): `[...]`
// groups, a lone `,', and words with their `(...)' attached.
std::vector<SynItem> synItems(const std::string& s) {
  std::vector<SynItem> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    if (s[i] == ',') {
      out.push_back({",", false});
      ++i;
      continue;
    }
    if (s[i] == '[') {
      int depth = 0;
      bool inq = false;
      size_t j = i;
      for (; j < s.size(); ++j) {
        const char c = s[j];
        if (c == '"') inq = !inq;
        if (inq) continue;
        if (c == '[' || c == '(') ++depth;
        if (c == ']' || c == ')') {
          --depth;
          if (depth == 0) break;
        }
      }
      out.push_back({s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1), true});
      i = j < s.size() ? j + 1 : s.size();
      continue;
    }
    size_t j = i;
    int depth = 0;
    bool inq = false;
    for (; j < s.size(); ++j) {
      const char c = s[j];
      if (c == '"') inq = !inq;
      if (inq) continue;
      if (c == '(') ++depth;
      else if (c == ')') --depth;
      else if (depth == 0 && (std::isspace(static_cast<unsigned char>(c)) || c == '[' || c == ','))
        break;
    }
    out.push_back({s.substr(i, j - i), false});
    i = j;
  }
  return out;
}

// Blank-separated words, a "..." group kept whole (`id="equation name"`).
std::vector<std::string> specWords(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool inq = false;
  for (char c : s) {
    if (c == '"') inq = !inq;
    if (!inq && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string unquote(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

bool parseCount(const std::string& w, const char* key, long long* out) {
  const size_t n = std::strlen(key);
  if (w.compare(0, n, key) != 0) return false;
  char* end = nullptr;
  const long long v = std::strtoll(w.c_str() + n, &end, 10);
  if (end == w.c_str() + n || *end != '\0') return false;
  *out = v;
  return true;
}

bool parseNumber(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  *out = std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0';
}

bool isStorageType(const std::string& w, bool* isStr) {
  static const std::regex reStr(R"(^str([0-9]+|L)$)");
  if (w == "byte" || w == "int" || w == "long" || w == "float" || w == "double") {
    *isStr = false;
    return true;
  }
  if (std::regex_match(w, reStr)) {
    *isStr = true;
    return true;
  }
  return false;
}

// `w` spells `full` down to at least `minLen` letters (syntax.sthlp writes
// these as int:eger, str:ing, num:eric, gen:erate, asc:ending, miss:ingokay,
// fw:eight ...).
bool abbrevOf(const std::string& w, const char* full, size_t minLen) {
  const size_t n = std::strlen(full);
  return w.size() >= minLen && w.size() <= n && std::strncmp(full, w.c_str(), w.size()) == 0;
}

// The spec as syntax's own parser reads it, in one canonical spelling:
//  * no blank between a keyword and its `(` (`varlist (default=none)`);
//  * inside parentheses, no blank around `=` (`max = 1`, `name = vce`) and none
//    between a comparison and its number (`>= 0`, `<= 16000`), while a
//    comparison glued to a word starts a new word (`integer>0`);
//  * outside them, `/` belongs to the word before it (`using /`, `[fw iw /]`)
//    and `=` to the word after it (`= exp`, `= /exp`).
// All of these are spellings shipped StataNow ado-files use.
std::string normalizeSpec(const std::string& s) {
  std::string out;
  int depth = 0;
  bool inq = false;
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '#';
  };
  auto popBlanks = [&]() {
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  };
  auto skipBlanks = [&](size_t& i) {
    while (i + 1 < s.size() && std::isspace(static_cast<unsigned char>(s[i + 1]))) ++i;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') inq = !inq;
    if (inq) {
      out += c;
      continue;
    }
    if (c == '(') {
      size_t k = out.size();
      while (k > 0 && std::isspace(static_cast<unsigned char>(out[k - 1]))) --k;
      if (k > 0 && k < out.size() && isWordChar(out[k - 1])) out.resize(k);
      ++depth;
      out += c;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      out += c;
      continue;
    }
    if (depth > 0) {
      if (c == '=' && !(!out.empty() && (out.back() == '<' || out.back() == '>'))) {
        popBlanks();
        out += c;
        skipBlanks(i);
        continue;
      }
      if (c == '<' || c == '>') {
        if (!out.empty() && isWordChar(out.back())) out += ' ';
        out += c;
        if (i + 1 < s.size() && s[i + 1] == '=') {
          out += '=';
          ++i;
        }
        skipBlanks(i);
        continue;
      }
    } else {
      if (c == '/') {
        popBlanks();
        out += c;
        continue;
      }
      if (c == '=') {
        out += c;
        skipBlanks(i);
        continue;
      }
    }
    out += c;
  }
  return out;
}

// One option of the spec.
struct SynOpt {
  std::string full;     // lower-case name without its leading `no' (the local's name)
  size_t minLen = 0;    // of `full`: the capitalised prefix, or all of it
  bool noForm = false;  // noNAME: an off-switch
  std::string type;     // "" (switch), integer, real, cilevel, numlist, varlist, varname,
                        // namelist, name, string, passthru
  std::string deflt;
  bool required = false;
  bool star = false;
  bool asis = false;
  bool numeric = false, stringOnly = false, fvts = false, localNames = false;
  long long min = -1, max = -1;
  bool ascending = false, descending = false, integer = false, missingOk = false, sort = false;
  std::optional<double> gt, ge, lt, le;
};

// A numlist bound (`>0`, `>=1`, `<1e+9`).
bool parseBound(const std::string& w, SynOpt* o) {
  double d = 0;
  if (w.compare(0, 2, ">=") == 0 && parseNumber(w.substr(2), &d)) o->ge = d;
  else if (w.compare(0, 2, "<=") == 0 && parseNumber(w.substr(2), &d)) o->le = d;
  else if (w[0] == '>' && parseNumber(w.substr(1), &d)) o->gt = d;
  else if (w[0] == '<' && parseNumber(w.substr(1), &d)) o->lt = d;
  else return false;
  return true;
}

// A `str#`/`strL` restriction (a bare `str` is the abbreviation of string).
bool isStrHash(const std::string& w) {
  static const std::regex reStr(R"(^str([0-9]+|L)$)");
  return std::regex_match(w, reStr);
}

// False = the descriptor is not one Stata accepts (r(197)).
bool parseSynOpt(const std::string& word, bool optional, SynOpt* o) {
  if (word == "*") {
    o->star = true;
    return true;
  }
  const size_t lp = word.find('(');
  const std::string name = lp == std::string::npos ? word : word.substr(0, lp);
  if (name.empty()) return false;
  for (char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  size_t start = 0;
  // [P] syntax "optionally off": `no' then the name (noREPLACE, nodetail); a
  // name that merely begins with those letters capitalises its N (Notice).
  if (lp == std::string::npos && name.size() > 2 && name[0] == 'n' && name[1] == 'o') {
    o->noForm = true;
    start = 2;
  }
  size_t caps = 0;
  while (start + caps < name.size() && std::isupper(static_cast<unsigned char>(name[start + caps])))
    ++caps;
  std::string base = lower(name.substr(start));
  o->minLen = caps == 0 ? base.size() : caps;
  if (base.size() > 31) base = base.substr(0, 31);  // the local is the first 31 letters
  o->full = base;
  o->required = !optional;
  if (lp == std::string::npos) return true;
  const size_t rp = word.rfind(')');
  if (rp == std::string::npos || rp < lp) return false;
  const std::vector<std::string> ws = specWords(word.substr(lp + 1, rp - lp - 1));
  if (ws.empty()) return false;
  std::string t = ws[0];
  if (t == "asis") {  // kept from the first binder: `Opt(asis)` reads as `Opt(string asis)`
    t = "string";
    o->asis = true;
  }
  // The type word, with the abbreviations shipped ado-files use: int:eger
  // (`Bin(int -1)`), str:ing (`FORmat(str)`, `touse(strin)`), pass:thru
  // (`eform1(passthr)`).
  if (abbrevOf(t, "integer", 3)) t = "integer";
  else if (abbrevOf(t, "string", 3)) t = "string";
  else if (abbrevOf(t, "passthru", 4)) t = "passthru";
  static const std::vector<std::string> kTypes = {"integer", "real",    "cilevel",  "crlevel",
                                                  "numlist", "varlist", "varname",  "namelist",
                                                  "name",    "string",  "passthru"};
  if (std::find(kTypes.begin(), kTypes.end(), t) == kTypes.end()) return false;
  o->type = t;
  if (t == "varname" || t == "name") o->max = 1;
  bool haveDefault = false;
  for (size_t k = 1; k < ws.size(); ++k) {
    const std::string& w = ws[k];
    double d = 0;
    long long cnt = 0;
    if (t == "integer" || t == "real") {
      if (haveDefault) return false;
      if (w != "." && !parseNumber(w, &d)) return false;
      if (t == "integer" && (w == "." || d != std::floor(d))) return false;
      o->deflt = w;
      haveDefault = true;
    } else if (t == "numlist") {
      // asc:ending, desc:ending, int:eger, miss:ingokay ([P] syntax).
      if (abbrevOf(w, "ascending", 3)) o->ascending = true;
      else if (abbrevOf(w, "descending", 4)) o->descending = true;
      else if (abbrevOf(w, "integer", 3)) o->integer = true;
      else if (abbrevOf(w, "missingokay", 4)) o->missingOk = true;
      else if (w == "sort") o->sort = true;
      else if (parseCount(w, "min=", &cnt)) o->min = cnt;
      else if (parseCount(w, "max=", &cnt)) o->max = cnt;
      else if (!parseBound(w, o)) return false;
    } else if (t == "varlist" || t == "varname") {
      if (abbrevOf(w, "numeric", 3)) o->numeric = true;
      else if (abbrevOf(w, "string", 3) || isStrHash(w)) o->stringOnly = true;
      else if (w == "fv" || w == "ts") o->fvts = true;
      else if (w == "broken") {
      } else if (parseCount(w, "min=", &cnt)) o->min = cnt;
      else if (parseCount(w, "max=", &cnt)) o->max = cnt;
      else return false;
    } else if (t == "namelist" || t == "name") {
      if (w == "local") o->localNames = true;
      else if (parseCount(w, "min=", &cnt)) o->min = cnt;
      else if (parseCount(w, "max=", &cnt)) o->max = cnt;
      else return false;
    } else if (t == "string") {
      if (w == "asis") o->asis = true;
      else return false;
    } else {
      return false;  // cilevel, passthru: no modifiers
    }
  }
  // An optional integer/real option needs its default ([P] syntax: "# (unless
  // the option is required)"); without one StataBE answers r(197).
  if ((t == "integer" || t == "real") && optional && !haveDefault) return false;
  return true;
}

// A name as syntax's namelist checks it: Stata's identifier rule, or with
// `local' the local-macro rule (may begin with a digit, at most 31 bytes).
bool validName(const std::string& w, bool local) {
  if (!local) return strutil::isValidStataIdentifier(w);
  if (w.empty() || w.size() > 31) return false;
  for (char c : w)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
        static_cast<unsigned char>(c) < 0x80)
      return false;
  return true;
}

bool isStringVar(SessionState& session, const std::string& v) {
  if (!session.hasData()) return false;
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(v) && ds->dtype(v) != "f64";
}

// Variable names for a varname/varlist binding: wildcards and ranges through
// loops::expandVarlist, then every name checked (an unambiguous abbreviation
// becomes its variable; an unknown or ambiguous one is r(111)).
bool synVars(const std::string& text, SessionState& session, bool numeric, bool stringOnly,
             std::vector<std::string>* out, ojson* err) {
  loops::VarlistExpandResult ex = loops::expandVarlist(text, session);
  if (ex.isError) {
    *err = synErr(ex.error.value("message", std::string("variable not found")), 111);
    return false;
  }
  if (session.hasData()) {
    if (auto bad = loops::checkKnownVarlist(ex.tokens, session)) {
      *err = synErr(bad->value("message", std::string("variable not found")), 111);
      return false;
    }
  }
  for (const auto& v : ex.tokens) {
    const bool isStr = isStringVar(session, v);
    if (numeric && isStr) {
      *err = synErr("string variables not allowed in varlist;\n" + v + " is a string variable", 109);
      return false;
    }
    if (stringOnly && !isStr) {
      *err = synErr("numeric variables not allowed in varlist;\n" + v + " is a numeric variable",
                    109);
      return false;
    }
  }
  *out = ex.tokens;
  return true;
}

std::string joinWords(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& w : v) s += (s.empty() ? "" : " ") + w;
  return s;
}

// A varlist as syntax reads one, word by word: a factor-variable or
// time-series token (`i.foreign`, `L.price`) is kept as typed when the
// element allows it (`fv`/`ts`) and is r(101) otherwise; `pre~suf` names the
// ONE variable that begins with `pre` and ends with `suf` (StataBE: `pr~e` is
// price; two candidates are "ambiguous abbreviation", none "not found", both
// r(111)); runs of plain names go through synVars together, so a blank-spaced
// range `a - b` stays one range.
bool synVarWords(const std::string& text, SessionState& session, bool allowFv, bool numeric,
                 bool stringOnly, std::vector<std::string>* out, ojson* err) {
  std::vector<std::string> plain;
  auto flush = [&]() {
    if (plain.empty()) return true;
    std::vector<std::string> ex;
    if (!synVars(joinWords(plain), session, numeric, stringOnly, &ex, err)) return false;
    out->insert(out->end(), ex.begin(), ex.end());
    plain.clear();
    return true;
  };
  // Words split at blanks outside parentheses, so an operator group
  // (`i.(foreign rep78)`, `c.(mpg weight)#i.foreign`) stays one token.
  std::vector<std::string> words;
  {
    std::string cur;
    int depth = 0;
    for (char c : text) {
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
  }
  for (std::string w : words) {
    if (w.find_first_of(".#") != std::string::npos) {
      if (!allowFv) {
        *err = synErr("factor-variable and time-series operators not allowed", 101);
        return false;
      }
      if (!flush()) return false;
      // A time-series operator (L., F2., D., S., LD., L(1/2).) needs the data
      // tsset: StataBE `syntax varlist(fv ts)` given `i(1 2).rep78 L.price`
      // is r(111) "time variable not set" (bank a5fb0337c3227033).
      if (!session.timevar.has_value() || session.timevar->empty()) {
        static const std::regex reTsOp(R"(^[LFDSlfds][0-9LFDSlfds]*(\([^()]*\))?$)");
        size_t start = 0;
        bool tsOp = false;
        for (size_t k = 0; k <= w.size() && !tsOp; ++k) {
          if (k < w.size() && w[k] != '.' && w[k] != '#') continue;
          if (k < w.size() && w[k] == '.') tsOp = std::regex_match(w.substr(start, k - start), reTsOp);
          start = k + 1;
        }
        if (tsOp) {
          *err = synErr("time variable not set", 111);
          return false;
        }
      }
      out->push_back(w);
      continue;
    }
    const size_t tilde = w.find('~');
    if (tilde != std::string::npos && session.hasData()) {
      const std::string pre = w.substr(0, tilde), suf = w.substr(tilde + 1);
      loops::VarlistExpandResult all = loops::expandVarlist("_all", session);
      std::vector<std::string> hits;
      for (const auto& v : all.tokens)
        if (v.size() >= pre.size() + suf.size() && v.compare(0, pre.size(), pre) == 0 &&
            v.compare(v.size() - suf.size(), suf.size(), suf) == 0)
          hits.push_back(v);
      if (hits.size() != 1) {
        *err = synErr(hits.empty() ? "variable " + w + " not found" : w + " ambiguous abbreviation",
                      111);
        return false;
      }
      w = hits.front();
    }
    plain.push_back(w);
  }
  return flush();
}

std::string fmtNum(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// A numlist option's argument expanded ([P] numlist): `#`, `.`, `#/#`,
// `#(#)#`, `#[#]#`, `# # : #` and `# # to #`, blank- or comma-separated.
// `name` is the option's full name, for the message.
bool synNumlist(const SynOpt& o, const std::string& text, std::string* out, ojson* err) {
  const std::string pre = o.full + "() invalid -- invalid numlist";
  auto fail = [&](const std::string& what, int rc) {
    *err = synErr(pre + what, rc);
    return false;
  };
  std::string norm;
  for (char c : text) {
    if (c == ',') norm += ' ';
    else if (c == '[') norm += '(';
    else if (c == ']') norm += ')';
    else if (c == ':') norm += " : ";
    else norm += c;
  }
  std::vector<std::string> words;
  {
    std::istringstream is(norm);
    for (std::string w; is >> w;) words.push_back(w);
  }
  std::vector<std::string> elems;  // %15.0g text, "." for a missing value
  std::vector<bool> plain;         // elems[i] came from a single number (for `a b : c`)
  for (size_t k = 0; k < words.size(); ++k) {
    const std::string& w = words[k];
    if (w == ":" || lower(w) == "to") {
      if (elems.size() < 2 || !plain[elems.size() - 1] || !plain[elems.size() - 2] ||
          k + 1 >= words.size())
        return fail("", 121);
      double a = 0, b = 0, c = 0;
      if (!parseNumber(elems[elems.size() - 2], &a) || !parseNumber(elems.back(), &b) ||
          !parseNumber(words[k + 1], &c) || a == b)
        return fail("", 121);
      std::vector<std::string> run;
      if (!loops::expandNumlist(fmtNum(a) + "(" + fmtNum(b - a) + ")" + fmtNum(c), &run))
        return fail("", 121);
      elems.resize(elems.size() - 2);
      plain.resize(plain.size() - 2);
      for (auto& r : run) {
        elems.push_back(r);
        plain.push_back(false);
      }
      ++k;
      continue;
    }
    if (w == "." || (w.size() == 2 && w[0] == '.' && w[1] >= 'a' && w[1] <= 'z')) {
      elems.push_back(w);
      plain.push_back(false);
      continue;
    }
    std::vector<std::string> run;
    if (!loops::expandNumlist(w, &run) || run.empty()) return fail("", 121);
    const bool single = run.size() == 1 && w.find_first_of("/(") == std::string::npos;
    for (auto& r : run) {
      elems.push_back(r);
      plain.push_back(single);
    }
  }
  std::vector<double> vals;
  bool anyMissing = false;
  for (const auto& e : elems) {
    double d = 0;
    if (e[0] == '.' && (e.size() == 1 || std::isalpha(static_cast<unsigned char>(e[1])))) {
      anyMissing = true;
      vals.push_back(NAN);
      continue;
    }
    if (!parseNumber(e, &d)) return fail("", 121);
    vals.push_back(d);
  }
  if (anyMissing && !o.missingOk) return fail(" has missing values", 127);
  for (double d : vals)
    if (!std::isnan(d) && o.integer && d != std::floor(d))
      return fail(" has noninteger elements", 126);
  for (double d : vals) {
    if (std::isnan(d)) continue;
    if ((o.gt && !(d > *o.gt)) || (o.ge && !(d >= *o.ge)) || (o.lt && !(d < *o.lt)) ||
        (o.le && !(d <= *o.le)))
      return fail(" has elements outside of allowed range", 125);
  }
  for (size_t k = 1; k < vals.size(); ++k) {
    if (o.ascending && !(vals[k] > vals[k - 1])) return fail(" has elements out of order", 124);
    if (o.descending && !(vals[k] < vals[k - 1])) return fail(" has elements out of order", 124);
  }
  if (o.min >= 0 && static_cast<long long>(elems.size()) < o.min)
    return fail(" has too few elements", 122);
  if (o.max >= 0 && static_cast<long long>(elems.size()) > o.max)
    return fail(" has too many elements", 123);
  if (o.sort) {
    std::vector<size_t> idx(elems.size());
    for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
      const bool ma = std::isnan(vals[a]), mb = std::isnan(vals[b]);
      if (ma != mb) return mb;
      return !ma && vals[a] < vals[b];
    });
    std::vector<std::string> sorted;
    for (size_t k : idx) sorted.push_back(elems[k]);
    elems = sorted;
  }
  *out = joinWords(elems);
  return true;
}

// New variable names as typed: `[type] name`, `type(name name)`, `stub1-stub3`.
bool synNewVars(const std::string& text, SessionState& session, std::vector<std::string>* names,
                std::vector<std::string>* types, ojson* err) {
  std::string spaced;
  for (char c : text) {
    if (c == '(' || c == ')') {
      spaced += ' ';
      spaced += c;
      spaced += ' ';
    } else {
      spaced += c;
    }
  }
  const std::string deflt =
      session.defaultStorageType.empty() ? std::string("float") : session.defaultStorageType;
  std::string pending;
  bool inGroup = false;
  std::istringstream is(spaced);
  static const std::regex reRange(R"(^([A-Za-z_]+)([0-9]+)-([A-Za-z_]+)([0-9]+)$)");
  for (std::string w; is >> w;) {
    bool isStr = false;
    if (w == "(") {
      if (pending.empty() || inGroup) {
        *err = synErr("invalid syntax", 198);
        return false;
      }
      inGroup = true;
      continue;
    }
    if (w == ")") {
      if (!inGroup) {
        *err = synErr("invalid syntax", 198);
        return false;
      }
      inGroup = false;
      pending.clear();
      continue;
    }
    if (!inGroup && isStorageType(w, &isStr)) {
      pending = w;
      continue;
    }
    std::vector<std::string> expanded;
    std::smatch m;
    if (std::regex_match(w, m, reRange) && m.str(1) == m.str(3)) {
      const long long a = std::atoll(m.str(2).c_str()), b = std::atoll(m.str(4).c_str());
      if (a > b) {
        *err = synErr(w + " invalid name", 198);
        return false;
      }
      for (long long k = a; k <= b; ++k) expanded.push_back(m.str(1) + std::to_string(k));
    } else {
      expanded.push_back(w);
    }
    for (const auto& nm : expanded) {
      if (!strutil::isValidStataIdentifier(nm)) {
        *err = synErr(nm + " invalid name", 198);
        return false;
      }
      Datastore* ds = session.hasData() ? session.datastore() : nullptr;
      if (ds != nullptr && ds->has(nm)) {
        *err = synErr("variable " + nm + " already defined", 110);
        return false;
      }
      names->push_back(nm);
      types->push_back(pending.empty() ? deflt : pending);
    }
    if (!inGroup) pending.clear();
  }
  if (inGroup || !pending.empty()) {
    *err = synErr("invalid syntax", 198);
    return false;
  }
  return true;
}

// The weight word the user typed -> its type ("weight" = the spec's default,
// "" = unknown); the spellings eval::tokenize accepts for built-in commands.
std::string synWeightWord(const std::string& word) {
  struct Row {
    const char* full;
    size_t minLen;
    const char* type;
  };
  static constexpr Row kRows[] = {
      {"fweight", 2, "fweight"}, {"frequency", 4, "fweight"}, {"aweight", 2, "aweight"},
      {"cellsize", 4, "aweight"}, {"pweight", 2, "pweight"},  {"iweight", 2, "iweight"},
      {"weight", 1, "weight"},
  };
  for (const auto& row : kRows) {
    const std::string full = row.full;
    if (word.size() >= row.minLen && word.size() <= full.size() &&
        full.compare(0, word.size(), word) == 0)
      return row.type;
  }
  return std::string();
}

// The argument string's pre-comma part, split at its top-level elements.
struct SynArgs {
  std::string lead, ifText, inText, usingText, expText, weightText;
  bool hasIf = false, hasIn = false, hasUsing = false, hasExp = false, hasWeight = false;
};

SynArgs splitArgs(const std::string& s, bool everything, bool equalok) {
  SynArgs a;
  enum { kLead, kIf, kIn, kUsing, kExp, kAfter } cur = kLead;
  std::string after;  // text after a weight that no keyword claimed
  std::string* dst = &a.lead;
  static const std::regex reWeightBody(R"(^\s*[A-Za-z]+\s*=(?!=))");
  int depth = 0;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      const size_t q = s.find('"', i + 1);
      const size_t e = q == std::string::npos ? s.size() : q + 1;
      *dst += s.substr(i, e - i);
      i = e;
      continue;
    }
    if (c == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      int nest = 0;
      size_t j = i;
      for (; j + 1 < s.size(); ++j) {
        if (s[j] == '`' && s[j + 1] == '"') ++nest;
        else if (s[j] == '"' && s[j + 1] == '\'' && --nest == 0) break;
      }
      const size_t e = j + 1 < s.size() ? j + 2 : s.size();
      *dst += s.substr(i, e - i);
      i = e;
      continue;
    }
    if (depth == 0 && c == '[') {
      size_t j = i;
      int d = 0;
      for (; j < s.size(); ++j) {
        if (s[j] == '[') ++d;
        if (s[j] == ']' && --d == 0) break;
      }
      const std::string inner = s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1);
      // A weight is `[type = exp]`; any other bracket (`[3]`, `x[1]`) is text.
      if (!a.hasWeight && std::regex_search(inner, reWeightBody)) {
        a.hasWeight = true;
        a.weightText = inner;
        i = j < s.size() ? j + 1 : s.size();
        cur = kAfter;
        dst = &after;
        continue;
      }
    }
    if (c == '(' || c == '[') ++depth;
    if ((c == ')' || c == ']') && depth > 0) --depth;
    const bool atWord = i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]));
    auto kw = [&](const char* w) {
      const size_t n = std::strlen(w);
      return depth == 0 && atWord && s.compare(i, n, w) == 0 &&
             (i + n == s.size() || std::isspace(static_cast<unsigned char>(s[i + n])));
    };
    // `if(foreign)`: an `if` glued to its parenthesised condition is still
    // the if qualifier (bank 8295405e500388f9).
    const bool ifGlued = depth == 0 && atWord && s.compare(i, 3, "if(") == 0;
    if (!everything && !a.hasIf && (kw("if") || ifGlued)) {
      cur = kIf;
      a.hasIf = true;
      dst = &a.ifText;
      i += 2;
      continue;
    }
    if (!everything && !a.hasIn && kw("in")) {
      cur = kIn;
      a.hasIn = true;
      dst = &a.inText;
      i += 2;
      continue;
    }
    if (!everything && !a.hasUsing && kw("using")) {
      cur = kUsing;
      a.hasUsing = true;
      dst = &a.usingText;
      i += 5;
      continue;
    }
    // `=` opens the =exp unless it belongs to a relational operator: `==`,
    // `>=`, `<=`, `!=`, `~=` (`an1 price >= 5000` is all `anything`).
    if (depth == 0 && c == '=' && cur == kLead && !equalok && !a.hasExp &&
        !(i + 1 < s.size() && s[i + 1] == '=') &&
        !(i > 0 && std::strchr("=<>!~", s[i - 1]) != nullptr)) {
      cur = kExp;
      a.hasExp = true;
      dst = &a.expText;
      ++i;
      continue;
    }
    *dst += c;
    ++i;
  }
  a.lead = trim(a.lead);
  // `if` keeps the text as typed up to the next element, its blank included
  // (StataBE: `pso price mpg if foreign [aw=weight] in 1/50` binds `if foreign `).
  while (!a.ifText.empty() && std::isspace(static_cast<unsigned char>(a.ifText.front())))
    a.ifText.erase(a.ifText.begin());
  a.inText = trim(a.inText);
  a.usingText = trim(a.usingText);
  while (!a.expText.empty() && std::isspace(static_cast<unsigned char>(a.expText.front())))
    a.expText.erase(a.expText.begin());
  if (!trim(after).empty()) a.lead += (a.lead.empty() ? "" : " ") + trim(after);
  return a;
}

const char* weightAdjective(const std::string& type) {
  if (type == "fweight") return "frequency";
  if (type == "aweight") return "analytic";
  if (type == "pweight") return "sampling";
  return "importance";
}

}  // namespace

namespace program_detail {

// Bind `syntax SPEC` against the local `0'. True = bound (locals set; *note
// may hold a display block such as "(analytic weights assumed)"); false =
// *err holds Stata's error or the refusal.
bool bindSyntax(const std::string& specIn, SessionState& session, ojson* err, ojson* note) {
  std::string spec = specIn;
  if (eval::hasMacroRefs(spec)) {
    eval::ExpandResult ex = eval::expandPass(spec, session.eval);
    if (!ex.isError) spec = ex.line;
  }
  spec = normalizeSpec(spec);
  const std::string argstr = session.eval.locals.get("0", "");

  // ---- the spec ----
  enum class Lead { kNone, kVarlist, kVarname, kNewVarlist, kNewVarname, kNamelist, kName, kAnything };
  Lead lead = Lead::kNone;
  std::string leadWord;
  bool leadOptional = false, leadNumeric = false, leadString = false, leadFv = false,
       leadDefaultNone = false, leadLocalNames = false, leadEqualok = false,
       leadEverything = false;
  long long leadMin = -1, leadMax = -1;
  std::string leadLocal, leadId;
  bool allowIf = false, allowIn = false, allowUsing = false, allowExp = false, allowWeight = false;
  bool ifRequired = false, inRequired = false, usingRequired = false, expRequired = false;
  bool ifSlash = false, inSlash = false, usingSlash = false, expSlash = false, weightSlash = false;
  std::vector<std::string> weightTypes;
  std::vector<SynOpt> opts;
  bool inOptions = false;
  auto refuse = [&](const std::string& what) {
    *err = synErr("syntax: " + what +
                      " is not implemented in this version of PIET; the program was not run "
                      "further rather than bound partially",
                  198, "unsupported_option");
    return false;
  };
  // One option descriptor; false = *err holds r(197).
  std::function<bool(const std::string&, bool)> addOption = [&](const std::string& word,
                                                                bool optional) -> bool {
    // `OR*`, `PPVALues*`: the `*` glued to a name is the rest-collector after
    // that option (fracreg's Replay reads `or' and `options'; bayesstats
    // reports `options' as an unknown subcommand).
    if (word.size() > 1 && word.back() == '*' && word.find('(') == std::string::npos) {
      const std::string nm = word.substr(0, word.size() - 1);
      bool plain = true;
      for (char c : nm) plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
      if (plain) return addOption(nm, optional) && addOption("*", optional);
    }
    SynOpt o;
    if (!parseSynOpt(word, optional, &o)) {
      *err = specErr();
      return false;
    }
    opts.push_back(o);
    return true;
  };
  auto addOptions = [&](const std::string& text, bool optional) -> bool {
    for (const SynItem& it : synItems(text)) {
      if (it.text == ",") continue;
      if (it.optional) {
        for (const SynItem& in2 : synItems(it.text)) {
          if (in2.text == ",") continue;
          if (!addOption(in2.text, true)) return false;
        }
        continue;
      }
      if (!addOption(it.text, optional)) return false;
    }
    return true;
  };
  // fw:eight aw:eight pw:eight iw:eight ([P] syntax), `/` on the last one.
  auto weightType = [](std::string w) -> std::string {
    if (!w.empty() && w.back() == '/') w.pop_back();
    for (const char* full : {"fweight", "aweight", "pweight", "iweight"})
      if (abbrevOf(w, full, 2)) return full;
    return std::string();
  };
  for (const SynItem& it : synItems(spec)) {
    std::string t = trim(it.text);
    if (inOptions) {
      if (it.optional) {
        if (!addOptions(t, true)) return false;
      } else if (!addOption(t, false)) {
        return false;
      }
      continue;
    }
    if (t == ",") {
      inOptions = true;
      continue;
    }
    if (it.optional && !t.empty() && t[0] == ',') {
      if (!addOptions(t.substr(1), true)) return false;
      // What follows an optional-options group is more options: a required
      // one (`[, lag_list(string)] n_esample(string)`) or a bare `*`.
      inOptions = true;
      continue;
    }
    // A weight group: every word a weight type, the last one may carry `/`.
    {
      std::vector<std::string> ws = specWords(t);
      bool allWeights = it.optional && !ws.empty();
      for (const auto& w : ws) allWeights = allWeights && !weightType(w).empty();
      if (allWeights) {
        allowWeight = true;
        for (const std::string& w : ws) {
          if (w.back() == '/') weightSlash = true;
          weightTypes.push_back(weightType(w));
        }
        continue;
      }
    }
    std::string head = t;
    std::string inner;
    const size_t lp = t.find('(');
    if (lp != std::string::npos) {
      head = t.substr(0, lp);
      const size_t rp = t.rfind(')');
      if (rp == std::string::npos || rp < lp || rp + 1 != t.size()) {
        *err = specErr();
        return false;
      }
      inner = t.substr(lp + 1, rp - lp - 1);
    }
    const std::string h = head;
    if (h == "if" || h == "if/") {
      allowIf = true;
      ifRequired = !it.optional;
      ifSlash = h == "if/";
    } else if (h == "in" || h == "in/") {
      allowIn = true;
      inRequired = !it.optional;
      inSlash = h == "in/";
    } else if (h == "using" || h == "using/" || h == "usin" || h == "usin/") {  // usin:g
      allowUsing = true;
      usingRequired = !it.optional;
      usingSlash = h.back() == '/';
    } else if (h == "=exp" || h == "=/exp") {
      allowExp = true;
      expRequired = !it.optional;
      expSlash = h == "=/exp";
    } else if (h == "varlist" || h == "varname" || h == "newvarlist" || h == "newvarname" ||
               h == "namelist" || h == "name" || h == "anything") {
      if (lead != Lead::kNone) {
        *err = specErr();
        return false;
      }
      lead = h == "varlist"      ? Lead::kVarlist
             : h == "varname"    ? Lead::kVarname
             : h == "newvarlist" ? Lead::kNewVarlist
             : h == "newvarname" ? Lead::kNewVarname
             : h == "namelist"   ? Lead::kNamelist
             : h == "name"       ? Lead::kName
                                 : Lead::kAnything;
      leadWord = h;
      leadOptional = it.optional;
      leadLocal = (lead == Lead::kAnything)                              ? "anything"
                  : (lead == Lead::kNamelist || lead == Lead::kName)     ? "namelist"
                                                                         : "varlist";
      leadId = lead == Lead::kAnything ? "something" : h;
      if (lead == Lead::kVarname || lead == Lead::kNewVarname || lead == Lead::kName) leadMax = 1;
      const bool varKind = lead == Lead::kVarlist || lead == Lead::kVarname ||
                           lead == Lead::kNewVarlist || lead == Lead::kNewVarname;
      const bool newKind = lead == Lead::kNewVarlist || lead == Lead::kNewVarname;
      const bool nameKind = lead == Lead::kNamelist || lead == Lead::kName;
      for (const std::string& w : specWords(inner)) {
        long long cnt = 0;
        // num:eric, str:ing, str#, strL, fv, ts, broken, default=none,
        // min=#, max=#, gen:erate ([P] syntax); `default=empty` (the st
        // commands' spelling) leaves the list empty as none does; `ts`/`fv`
        // on a new varlist (arima's predict) are accepted.
        if (varKind && abbrevOf(w, "numeric", 3)) leadNumeric = true;
        else if (varKind && (abbrevOf(w, "string", 3) || isStrHash(w))) leadString = true;
        else if (varKind && (w == "fv" || w == "ts")) leadFv = leadFv || !newKind;
        else if (varKind && !newKind && w == "broken") {
        } else if (varKind && (w == "default=none" || w == "default=empty")) leadDefaultNone = true;
        else if ((varKind || lead == Lead::kNamelist) && parseCount(w, "min=", &cnt)) leadMin = cnt;
        else if ((varKind || lead == Lead::kNamelist) && parseCount(w, "max=", &cnt)) leadMax = cnt;
        else if (newKind && abbrevOf(w, "generate", 3))
          return refuse("`" + h + "(generate)'");
        else if (lead == Lead::kVarlist && w == "fvanova")  // anova's own, undocumented
          return refuse("`varlist(fvanova)'");
        else if ((nameKind || lead == Lead::kAnything) && w.compare(0, 5, "name=") == 0 &&
                 w.size() > 5)
          leadLocal = w.substr(5);
        else if ((nameKind || lead == Lead::kAnything) && w.compare(0, 3, "id=") == 0)
          leadId = unquote(w.substr(3));
        else if (nameKind && w == "local") leadLocalNames = true;
        else if (lead == Lead::kAnything && w == "equalok") leadEqualok = true;
        else if (lead == Lead::kAnything && w == "everything") leadEverything = true;
        else {
          *err = specErr();
          return false;
        }
      }
    } else {
      *err = specErr();
      return false;
    }
  }

  // ---- the arguments ----
  const long long comma = eval::findUnquotedComma(argstr);
  const std::string pre = comma < 0 ? argstr : argstr.substr(0, static_cast<size_t>(comma));
  const std::string optTail = comma < 0 ? std::string() : argstr.substr(static_cast<size_t>(comma) + 1);
  const SynArgs a = splitArgs(pre, leadEverything, leadEqualok);

  if (a.hasIf && !allowIf) {
    *err = synErr("if not allowed", 101);
    return false;
  }
  if (a.hasIn && !allowIn) {
    *err = synErr("in range not allowed", 101);
    return false;
  }
  if (a.hasUsing && !allowUsing) {
    *err = synErr("using not allowed", 101);
    return false;
  }
  if (a.hasExp && !allowExp) {
    *err = synErr("=exp not allowed", 101);
    return false;
  }
  if (a.hasWeight && !allowWeight) {
    *err = synErr("weights not allowed", 101);
    return false;
  }

  std::vector<std::pair<std::string, std::string>> binds;
  // the leading element
  if (lead == Lead::kNone) {
    if (!a.lead.empty()) {
      *err = synErr("varlist not allowed", 101);
      return false;
    }
  } else if (lead == Lead::kAnything) {
    if (a.lead.empty() && !leadOptional) {
      *err = synErr(leadId + " required", 100);
      return false;
    }
    binds.emplace_back(leadLocal, a.lead);
  } else if (lead == Lead::kNamelist || lead == Lead::kName) {
    std::vector<std::string> names;
    std::istringstream is(a.lead);
    for (std::string w; is >> w;) {
      if (!validName(w, leadLocalNames)) {
        *err = synErr(w + " invalid name", 198);
        return false;
      }
      names.push_back(w);
    }
    if (names.empty() && !leadOptional) {
      *err = synErr(leadId + " required", 100);
      return false;
    }
    const long long n = static_cast<long long>(names.size());
    if (n > 0 && leadMin >= 0 && n < leadMin) {
      *err = synErr(leadId + ":  too few specified", 102);
      return false;
    }
    if (leadMax >= 0 && n > leadMax) {
      *err = synErr(leadId + ":  too many specified", 103);
      return false;
    }
    binds.emplace_back(leadLocal, joinWords(names));
  } else if (lead == Lead::kNewVarlist || lead == Lead::kNewVarname) {
    std::vector<std::string> names, types;
    if (!synNewVars(a.lead, session, &names, &types, err)) return false;
    if (names.empty() && !leadOptional) {
      *err = synErr("varlist required", 100);
      return false;
    }
    for (size_t k = 0; k < names.size(); ++k) {
      const bool isStr = types[k].compare(0, 3, "str") == 0;
      if (leadNumeric && isStr) {
        *err = synErr("string variables not allowed in varlist;\n" + names[k] +
                          " is a string variable",
                      109);
        return false;
      }
      if (leadString && !isStr) {
        *err = synErr("numeric variables not allowed in varlist;\n" + names[k] +
                          " is a numeric variable",
                      109);
        return false;
      }
    }
    const long long n = static_cast<long long>(names.size());
    if (n > 0 && leadMin >= 0 && n < leadMin) {
      *err = synErr("too few variables specified", 102);
      return false;
    }
    if (leadMax >= 0 && n > leadMax) {
      *err = synErr("too many variables specified", 103);
      return false;
    }
    binds.emplace_back("varlist", joinWords(names));
    binds.emplace_back("typlist", joinWords(types));
  } else {  // varlist / varname
    std::vector<std::string> bound;
    if (a.lead.empty()) {
      if (!leadOptional) {
        *err = synErr("varlist required", 100);
        return false;
      }
      if (lead == Lead::kVarlist && !leadDefaultNone && session.hasData()) {
        ojson e2;
        if (!synVars("_all", session, false, false, &bound, &e2)) bound.clear();
      }
    } else if (!synVarWords(a.lead, session, leadFv, leadNumeric, leadString, &bound, err)) {
      return false;
    }
    const long long n = static_cast<long long>(bound.size());
    if (leadMin >= 0 && n < leadMin && !(a.lead.empty() && leadOptional && n == 0)) {
      *err = synErr("too few variables specified", 102);
      return false;
    }
    if (leadMax >= 0 && n > leadMax) {
      *err = synErr("too many variables specified", 103);
      return false;
    }
    binds.emplace_back("varlist", joinWords(bound));
  }

  if (allowIf) {
    if (a.hasIf && trim(a.ifText).empty()) {
      *err = synErr("invalid syntax", 198);
      return false;
    }
    if (ifRequired && !a.hasIf) {
      *err = synErr("if required", 100);
      return false;
    }
    binds.emplace_back("if", !a.hasIf ? std::string() : ifSlash ? trim(a.ifText) : "if " + a.ifText);
  }
  if (allowIn) {
    if (inRequired && !a.hasIn) {
      *err = synErr("in range required", 100);
      return false;
    }
    std::string inBind;
    if (a.hasIn) {
      int64_t nRows = 0;
      if (session.hasData()) {
        if (Datastore* ds = session.datastore()) nRows = ds->nRows();
      }
      const handlers::InRangeBounds r = handlers::parseInRangeBounds(a.inText, nRows);
      if (!r.ok) {
        *err = synErr(r.refusal.empty() ? std::string("invalid syntax") : r.refusal, 198);
        return false;
      }
      const std::string range = std::to_string(r.a) + "/" + std::to_string(r.b);
      inBind = inSlash ? range : "in " + range;
    }
    binds.emplace_back("in", inBind);
  }
  if (allowUsing) {
    if (usingRequired && !a.hasUsing) {
      *err = synErr("using required", 100);
      return false;
    }
    std::string u = a.usingText;
    if (usingSlash && u.size() >= 2 && u.front() == '"' && u.back() == '"') u = u.substr(1, u.size() - 2);
    binds.emplace_back("using", a.hasUsing ? (usingSlash ? u : "using " + a.usingText) : std::string());
  }
  if (allowExp) {
    if (expRequired && !a.hasExp) {
      *err = synErr("=exp required", 100);
      return false;
    }
    if (a.hasExp && trim(a.expText).empty()) {
      *err = synErr("invalid syntax", 198);
      return false;
    }
    binds.emplace_back("exp", !a.hasExp ? std::string() : expSlash ? a.expText : "= " + a.expText);
  }
  if (allowWeight) {
    std::string wtype, wexp;
    if (a.hasWeight) {
      const size_t eq = a.weightText.find('=');
      const std::string word = lower(trim(a.weightText.substr(0, eq)));
      std::string full = synWeightWord(word);
      if (full.empty()) {
        *err = synErr(word + " unknown weight type", 198);
        return false;
      }
      if (full == "weight") {
        full = weightTypes.front();
        if (note != nullptr) {
          ojson b;
          b["type"] = "display";
          b["text"] = std::string("(") + weightAdjective(full) + " weights assumed)";
          *note = b;
        }
      }
      if (std::find(weightTypes.begin(), weightTypes.end(), full) == weightTypes.end()) {
        *err = synErr(full + "s not allowed", 101);
        return false;
      }
      std::string e = a.weightText.substr(eq + 1);
      while (!e.empty() && std::isspace(static_cast<unsigned char>(e.front()))) e.erase(e.begin());
      if (trim(e).empty()) {
        *err = synErr("invalid syntax", 198);
        return false;
      }
      wtype = full;
      wexp = weightSlash ? e : "= " + e;
    }
    binds.emplace_back("weight", wtype);
    binds.emplace_back("exp", wexp);
  }

  // ---- the options ----
  // Typed options match case-sensitively, each spec option at most once (a
  // repeat is "option d not allowed", StataBE 19.5); a switch also takes its
  // opposite spelling and then binds empty (`Detail` given `nod`, `noDETail`
  // given `detail`, `CONStant` given `nocons`).
  std::vector<bool> seen(opts.size(), false);
  std::vector<std::string> values(opts.size());
  std::string starText;
  const int starIdx = [&]() {
    for (size_t i = 0; i < opts.size(); ++i)
      if (opts[i].star) return static_cast<int>(i);
    return -1;
  }();
  auto prefixOf = [](const std::string& key, const std::string& full, size_t minLen) {
    return key.size() >= minLen && key.size() <= full.size() && full.compare(0, key.size(), key) == 0;
  };
  for (const SynItem& it : synItems(optTail)) {
    if (it.text == ",") continue;
    const std::string tok = it.optional ? "[" + it.text + "]" : it.text;
    const size_t lp = tok.find('(');
    const std::string key = lp == std::string::npos ? tok : tok.substr(0, lp);
    std::string arg;
    const bool hasArg = lp != std::string::npos;
    if (hasArg) {
      const size_t rp = tok.rfind(')');
      if (rp == std::string::npos || rp < lp) {
        *err = synErr(") required", 100);
        return false;
      }
      arg = tok.substr(lp + 1, rp - lp - 1);
    }
    int hit = -1;
    bool negated = false;
    for (size_t i = 0; i < opts.size(); ++i) {
      const SynOpt& o = opts[i];
      if (o.star || seen[i]) continue;
      if (prefixOf(key, o.full, o.minLen)) {
        hit = static_cast<int>(i);
        negated = o.noForm;  // the positive spelling of an off-switch
        break;
      }
      if (o.type.empty() && key.size() > 2 && key.compare(0, 2, "no") == 0 &&
          prefixOf(key.substr(2), o.full, o.minLen)) {
        hit = static_cast<int>(i);
        negated = !o.noForm;
        break;
      }
    }
    if (hit < 0) {
      if (starIdx >= 0) {
        starText += (starText.empty() ? "" : " ") + tok;
        continue;
      }
      *err = synErr("option " + key + (hasArg ? "()" : "") + " not allowed", 198);
      return false;
    }
    const SynOpt& o = opts[static_cast<size_t>(hit)];
    seen[static_cast<size_t>(hit)] = true;
    if (o.type.empty()) {
      if (hasArg) {
        *err = synErr("option " + key + "() not allowed", 198);
        return false;
      }
      values[static_cast<size_t>(hit)] = negated ? std::string() : (o.noForm ? "no" + o.full : o.full);
      continue;
    }
    if (!hasArg) {
      *err = synErr("option " + o.full + "() required", 198);
      return false;
    }
    std::string v = trim(arg);
    if (o.type == "integer") {
      double d = 0;
      if (!parseNumber(v, &d) || d != std::floor(d) || std::fabs(d) > 2147483647.0) {
        *err = synErr("option " + o.full + "() incorrectly specified", 198);
        return false;
      }
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.0f", d);
      v = buf;
    } else if (o.type == "real") {
      double d = 0;
      const bool missing = v == "." || (v.size() == 2 && v[0] == '.' && v[1] >= 'a' && v[1] <= 'z');
      if (!missing && !parseNumber(v, &d)) {
        *err = synErr("option " + o.full + "() incorrectly specified", 198);
        return false;
      }
    } else if (o.type == "cilevel" || o.type == "crlevel") {
      double d = 0;
      if (!parseNumber(v, &d)) {
        *err = synErr("option " + o.full + "() incorrectly specified", 198);
        return false;
      }
      if (d < 10 || d > 99.99) {
        *err = synErr("level() must be between 10 and 99.99 inclusive", 198);
        return false;
      }
    } else if (o.type == "numlist") {
      if (!v.empty() && !synNumlist(o, v, &v, err)) return false;
    } else if (o.type == "varname" || o.type == "varlist") {
      std::vector<std::string> ex;
      if (!v.empty()) {
        if (!synVarWords(v, session, o.fvts, o.numeric, o.stringOnly, &ex, err)) {
          (*err)["message"] = (*err).value("message", std::string()) + "\n(error in option " +
                              o.full + "())";
          return false;
        }
      }
      const long long n = static_cast<long long>(ex.size());
      if (o.min >= 0 && n < o.min) {
        *err = synErr("option " + o.full + "():  too few variables specified", 102);
        return false;
      }
      if (o.max >= 0 && n > o.max) {
        *err = synErr(ex.empty() ? "variable name required" : "too many variables specified",
                      ex.empty() ? 100 : 103);
        return false;
      }
      v = joinWords(ex);
    } else if (o.type == "name" || o.type == "namelist") {
      std::vector<std::string> names;
      std::istringstream is(v);
      for (std::string w; is >> w;) {
        if (!validName(w, o.localNames)) {
          *err = synErr("option " + o.full + "():  " + w + " invalid name", 198);
          return false;
        }
        names.push_back(w);
      }
      const long long n = static_cast<long long>(names.size());
      if (o.min >= 0 && n < o.min) {
        *err = synErr("option " + o.full + "():  too few names specified", 102);
        return false;
      }
      if (o.max >= 0 && n > o.max) {
        *err = synErr("option " + o.full + "():  too many names specified", 103);
        return false;
      }
      v = joinWords(names);
    } else if (o.type == "passthru") {
      v = o.full + "(" + arg + ")";
    } else if (o.type == "string" && o.asis) {
      v = arg;
    } else if (o.type == "string") {
      // Several quoted pieces each lose their quotes and are joined by one
      // blank (BUG-1765, StataBE 19.5: title("x" "y") binds `x y`, sub2(`"a"'
      // `"b"') binds `a b`); one piece is the case below.
      std::vector<std::string> pieces;
      bool allQuoted = true;
      std::string rest = v;
      while (allQuoted) {
        size_t s = 0;
        while (s < rest.size() && std::isspace(static_cast<unsigned char>(rest[s]))) ++s;
        if (s >= rest.size()) break;
        const std::string r = rest.substr(s);
        if (!(r[0] == '"' || r.compare(0, 2, "`\"") == 0)) {
          allQuoted = false;
          break;
        }
        auto [piece, after] = eval::scanOneToken(r, /*bind=*/true);
        if (after.size() >= r.size()) {
          allQuoted = false;
          break;
        }
        pieces.push_back(piece);
        rest = after;
      }
      if (allQuoted && pieces.size() > 1) v = joinWords(pieces);
      else if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      // BUG-1049: the compound form loses its quotes too (title(`"a "b" c"')
      // binds a "b" c in StataBE).
      else if (v.compare(0, 2, "`\"") == 0) {
        auto [innerTok, after] = eval::scanOneToken(v, /*bind=*/true);
        if (after.empty()) v = innerTok;
      }
    }
    values[static_cast<size_t>(hit)] = v;
  }
  for (size_t i = 0; i < opts.size(); ++i) {
    const SynOpt& o = opts[i];
    if (o.star) {
      binds.emplace_back("options", starText);
      continue;
    }
    if (!seen[i]) {
      if (o.required) {
        *err = synErr("option " + o.full + (o.type.empty() ? "" : "()") + " required", 198);
        return false;
      }
      if (o.type == "cilevel") values[i] = std::to_string(session.level);
      // c(clevel), which this build does not let `set` move from its default.
      else if (o.type == "crlevel") values[i] = "95";
      else values[i] = o.deflt;
    }
    binds.emplace_back(o.full, values[i]);
  }
  for (const auto& [k, v] : binds) session.eval.locals.set(k, v);
  return true;
}

}  // namespace program_detail

}  // namespace piet::program
