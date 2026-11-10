// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/program.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>   // BUG-360: ScopeGuard reaps this call's tempfiles
#include <regex>
#include <sstream>
#include <system_error>

#include "piet/cclass.h"  // BUG-1330: kVersionLocal
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"
#include "piet/datastore.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-360 reap)
#include "program_internal.h"

namespace piet::program {

using namespace program_detail;

// ---------------------------------------------------------------------------
// `syntax` -- Stata's argument parser for a program's `0' (the arguments as
// typed). The spec grammar (help syntax):
//   syntax [varlist|varname|newvarlist|newvarname|namelist|name|anything[(spec)]]
//          [if] [in] [using/] [weight-types] [, options]
// each element optionally in [ ] (optional) and the options either all inside
// `[, ...]' or, after a bare `,', required words and optional [ ] groups.
// Bound locals: varlist (namelist/anything for those elements, or the name=
// override), if ("if EXP"), in ("in RANGE"), using, weight + exp, and one local
// per option (bool -> its name or empty, noNAME -> "noname", NAME(type) -> the
// argument, passthru -> "name(arg)", `*' -> options). What this parser cannot
// bind (=exp, numlist and newvar elements, unknown types) is refused BY NAME --
// never a partial bind (w2j_e3_ritest.do `_e3_inv`: `syntax [if] [in],
// T(varname numeric) [Y(varname numeric) REF(varname numeric) ST(varname
// numeric)]`).
namespace {

ojson synErr(const std::string& msg, int rc, const std::string& code = "syntax_error") {
  return ojson(eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

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

struct SynOpt {
  std::string full;     // lower-case name ("dots" for noDOTS)
  size_t minLen = 0;    // of the TYPED key (noDOTS: of "nodots")
  bool noForm = false;  // noNAME: the local is NAME, its value "noname"
  std::string type;     // "" (switch), integer, real, string, name, varname, varlist,
                        // passthru, cilevel, namelist
  std::string deflt;    // integer/real default
  bool numeric = false;
  bool asis = false;
  bool required = false;
  bool star = false;
};

bool parseSynOpt(const std::string& word, bool optional, SynOpt* o, std::string* why) {
  if (word == "*") {
    o->star = true;
    return true;
  }
  const size_t lp = word.find('(');
  std::string name = lp == std::string::npos ? word : word.substr(0, lp);
  if (name.empty()) {
    *why = "an option without a name";
    return false;
  }
  size_t caps = 0;
  size_t start = 0;
  if (name.size() > 2 && name[0] == 'n' && name[1] == 'o' &&
      std::isupper(static_cast<unsigned char>(name[2]))) {
    o->noForm = true;
    start = 2;
  }
  while (start + caps < name.size() && std::isupper(static_cast<unsigned char>(name[start + caps])))
    ++caps;
  const std::string base = lower(name.substr(start));
  o->full = base;
  const size_t typedFull = (o->noForm ? 2 : 0) + base.size();
  o->minLen = caps == 0 ? typedFull : (o->noForm ? 2 : 0) + caps;
  o->required = !optional;
  if (lp != std::string::npos) {
    const size_t rp = word.rfind(')');
    std::istringstream is(word.substr(lp + 1, (rp == std::string::npos ? word.size() : rp) - lp - 1));
    std::string t;
    is >> t;
    t = lower(t);
    static const std::vector<std::string> kTypes = {"integer", "real",     "string",  "name",
                                                    "varname", "varlist",  "passthru", "cilevel",
                                                    "namelist", "asis"};
    if (std::find(kTypes.begin(), kTypes.end(), t) == kTypes.end()) {
      *why = "option " + base + "(" + t + ")";
      return false;
    }
    o->type = t == "asis" ? "string" : t;
    if (t == "asis") o->asis = true;
    for (std::string w; is >> w;) {
      const std::string lw = lower(w);
      if (lw == "numeric") o->numeric = true;
      else if (lw == "asis") o->asis = true;
      else if ((o->type == "integer" || o->type == "real") && o->deflt.empty()) o->deflt = w;
      else if (lw == "ts" || lw == "fv" || lw == "min=1" || lw == "max=1" || lw == "everything") {
      } else {
        *why = "option " + base + "(" + t + " " + w + ")";
        return false;
      }
    }
  }
  return true;
}

// Variable names for a varname/varlist binding: wildcards, ranges and
// abbreviations through loops::expandVarlist; an unknown name is r(111).
bool synVars(const std::string& text, SessionState& session, bool numeric,
             std::vector<std::string>* out, ojson* err) {
  loops::VarlistExpandResult ex = loops::expandVarlist(text, session);
  if (ex.isError) {
    const std::string msg = ex.error.value("message", std::string("variable not found"));
    *err = synErr(msg, msg.find("ambiguous") != std::string::npos ? 111 : 111);
    return false;
  }
  if (numeric) {
    Datastore* ds = session.datastore();
    for (const auto& v : ex.tokens) {
      if (ds != nullptr && ds->has(v) && ds->dtype(v) != "f64") {
        *err = synErr("string variables not allowed in varlist;\n" + v + " is a string variable",
                      109);
        return false;
      }
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

// Split an argument string's pre-comma part into the varlist text and the
// qualifier texts (if/in/using/weight), at top-level words.
struct SynArgs {
  std::string lead, ifText, inText, usingText, weightText;
  bool hasIf = false, hasIn = false, hasUsing = false, hasWeight = false;
};

SynArgs splitArgs(const std::string& s) {
  SynArgs a;
  enum { kLead, kIf, kIn, kUsing } cur = kLead;
  std::string* dst = &a.lead;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '[' ) {
      int depth = 0;
      size_t j = i;
      for (; j < s.size(); ++j) {
        if (s[j] == '[') ++depth;
        if (s[j] == ']' && --depth == 0) break;
      }
      // a weight is `[type = exp]` at top level, outside the if expression's
      // own subscripts (x[1]): it follows whitespace or starts the string.
      const bool wordStart = i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]));
      if (wordStart && cur != kIf) {
        a.hasWeight = true;
        a.weightText = s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1);
        i = j < s.size() ? j + 1 : s.size();
        continue;
      }
      if (wordStart && cur == kIf) {
        const std::string inner = s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1);
        if (inner.find('=') != std::string::npos) {
          a.hasWeight = true;
          a.weightText = inner;
          i = j < s.size() ? j + 1 : s.size();
          continue;
        }
      }
    }
    if (c == '"') {
      const size_t q = s.find('"', i + 1);
      const size_t e = q == std::string::npos ? s.size() : q + 1;
      *dst += s.substr(i, e - i);
      i = e;
      continue;
    }
    const bool atWord = (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])));
    auto kw = [&](const char* w) {
      const size_t n = std::strlen(w);
      return atWord && s.compare(i, n, w) == 0 &&
             (i + n == s.size() || std::isspace(static_cast<unsigned char>(s[i + n])));
    };
    if (kw("if") && cur == kLead) {
      cur = kIf;
      a.hasIf = true;
      dst = &a.ifText;
      i += 2;
      continue;
    }
    if (kw("in") && (cur == kLead || cur == kIf)) {
      cur = kIn;
      a.hasIn = true;
      dst = &a.inText;
      i += 2;
      continue;
    }
    if (kw("using")) {
      cur = kUsing;
      a.hasUsing = true;
      dst = &a.usingText;
      i += 5;
      continue;
    }
    *dst += c;
    ++i;
  }
  a.lead = trim(a.lead);
  a.ifText = trim(a.ifText);
  a.inText = trim(a.inText);
  a.usingText = trim(a.usingText);
  a.weightText = trim(a.weightText);
  return a;
}
}  // namespace

namespace program_detail {

// Bind `syntax SPEC` against the local `0'. True = bound (locals set); false =
// *err holds the refusal or Stata's own error.
bool bindSyntax(const std::string& specIn, SessionState& session, ojson* err) {
  std::string spec = specIn;
  if (eval::hasMacroRefs(spec)) {
    eval::ExpandResult ex = eval::expandPass(spec, session.eval);
    if (!ex.isError) spec = ex.line;
  }
  const std::string argstr = session.eval.locals.get("0", "");

  // ---- the spec ----
  enum class Lead { kNone, kVarlist, kVarname, kNamelist, kName, kAnything };
  Lead lead = Lead::kNone;
  bool leadOptional = false, leadNumeric = false, leadFv = false, leadDefaultNone = false;
  long long leadMin = -1, leadMax = -1;
  std::string leadLocal;
  bool allowIf = false, allowIn = false, allowUsing = false, allowWeight = false;
  bool ifRequired = false, inRequired = false, usingRequired = false, usingSlash = false;
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
  auto addOptions = [&](const std::string& text, bool optional) -> bool {
    for (const SynItem& it : synItems(text)) {
      if (it.text == ",") continue;
      if (it.optional) {
        for (const SynItem& in2 : synItems(it.text)) {
          SynOpt o;
          std::string why;
          if (!parseSynOpt(in2.text, true, &o, &why)) return refuse(why);
          opts.push_back(o);
        }
        continue;
      }
      SynOpt o;
      std::string why;
      if (!parseSynOpt(it.text, optional, &o, &why)) return refuse(why);
      opts.push_back(o);
    }
    return true;
  };
  for (const SynItem& it : synItems(spec)) {
    std::string t = trim(it.text);
    if (inOptions) {
      if (it.optional) {
        if (!addOptions(t, true)) return false;
      } else {
        SynOpt o;
        std::string why;
        if (!parseSynOpt(t, false, &o, &why)) return refuse(why);
        opts.push_back(o);
      }
      continue;
    }
    if (t == ",") {
      inOptions = true;
      continue;
    }
    if (it.optional && !t.empty() && t[0] == ',') {
      if (!addOptions(t.substr(1), true)) return false;
      continue;
    }
    std::string head = t;
    std::string inner;
    const size_t lp = t.find('(');
    if (lp != std::string::npos) {
      head = t.substr(0, lp);
      const size_t rp = t.rfind(')');
      inner = t.substr(lp + 1, (rp == std::string::npos ? t.size() : rp) - lp - 1);
    }
    const std::string h = lower(head);
    if (h == "if") {
      allowIf = true;
      ifRequired = !it.optional;
    } else if (h == "in") {
      allowIn = true;
      inRequired = !it.optional;
    } else if (h == "using" || h == "using/") {
      allowUsing = true;
      usingRequired = !it.optional;
      usingSlash = h == "using/";
    } else if (h == "fweight" || h == "aweight" || h == "pweight" || h == "iweight" ||
               h == "fw" || h == "aw" || h == "pw" || h == "iw") {
      allowWeight = true;
      std::istringstream is(t);
      for (std::string w; is >> w;) weightTypes.push_back(lower(w).substr(0, 1) + "weight");
    } else if (h == "varlist" || h == "varname" || h == "namelist" || h == "name" ||
               h == "anything") {
      lead = h == "varlist"    ? Lead::kVarlist
             : h == "varname"  ? Lead::kVarname
             : h == "namelist" ? Lead::kNamelist
             : h == "name"     ? Lead::kName
                               : Lead::kAnything;
      leadOptional = it.optional;
      leadLocal = (lead == Lead::kVarlist || lead == Lead::kVarname) ? "varlist"
                  : lead == Lead::kAnything                          ? "anything"
                                                                     : "namelist";
      std::istringstream is(inner);
      for (std::string w; is >> w;) {
        const std::string lw = lower(w);
        if (lw == "numeric") leadNumeric = true;
        else if (lw == "fv" || lw == "ts") leadFv = true;
        else if (lw.rfind("min=", 0) == 0) leadMin = std::atoll(lw.c_str() + 4);
        else if (lw.rfind("max=", 0) == 0) leadMax = std::atoll(lw.c_str() + 4);
        else if (lw == "default=none") leadDefaultNone = true;
        else if (lw.rfind("name=", 0) == 0) leadLocal = w.substr(5);
        else if (lw == "equalok" || lw == "everything" || lw.rfind("id=", 0) == 0) {
        } else {
          return refuse("`" + h + "(" + inner + ")'");
        }
      }
    } else {
      return refuse("the element `" + t + "'");
    }
  }

  // ---- the arguments ----
  const long long comma = eval::findUnquotedComma(argstr);
  const std::string pre = comma < 0 ? argstr : argstr.substr(0, static_cast<size_t>(comma));
  const std::string optTail = comma < 0 ? std::string() : argstr.substr(static_cast<size_t>(comma) + 1);
  const SynArgs a = splitArgs(pre);

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
  if (a.hasWeight && !allowWeight) {
    *err = synErr("weights not allowed", 101);
    return false;
  }
  if (ifRequired && !a.hasIf) {
    *err = synErr("if required", 100);
    return false;
  }
  if (inRequired && !a.hasIn) {
    *err = synErr("in range required", 100);
    return false;
  }
  if (usingRequired && !a.hasUsing) {
    *err = synErr("using required", 100);
    return false;
  }

  std::vector<std::pair<std::string, std::string>> binds;
  // the leading element
  if (lead == Lead::kNone) {
    if (!a.lead.empty()) {
      *err = synErr("varlist not allowed", 101);
      return false;
    }
  } else if (a.lead.empty()) {
    if (!leadOptional) {
      *err = synErr(lead == Lead::kVarlist || lead == Lead::kVarname ? "varlist required"
                                                                      : "nothing found where name expected",
                    100);
      return false;
    }
    std::string v;
    if ((lead == Lead::kVarlist) && !leadDefaultNone) {
      std::vector<std::string> all;
      ojson e2;
      if (session.hasData() && synVars("_all", session, false, &all, &e2)) v = joinWords(all);
    }
    binds.emplace_back(leadLocal, v);
  } else if (lead == Lead::kVarlist || lead == Lead::kVarname) {
    std::vector<std::string> plain;
    std::vector<std::string> words;
    std::istringstream is(a.lead);
    for (std::string w; is >> w;) words.push_back(w);
    std::vector<std::string> bound;
    for (const auto& w : words) {
      if (w.find_first_of(".#") != std::string::npos) {
        if (!leadFv) {
          *err = synErr("factor-variable and time-series operators not allowed", 101);
          return false;
        }
        bound.push_back(w);
        continue;
      }
      std::vector<std::string> ex;
      if (!synVars(w, session, leadNumeric, &ex, err)) return false;
      bound.insert(bound.end(), ex.begin(), ex.end());
    }
    const long long n = static_cast<long long>(bound.size());
    const long long mx = lead == Lead::kVarname ? 1 : leadMax;
    if (leadMin >= 0 && n < leadMin) {
      *err = synErr("too few variables specified", 102);
      return false;
    }
    if (mx >= 0 && n > mx) {
      *err = synErr("too many variables specified", 103);
      return false;
    }
    binds.emplace_back(leadLocal, joinWords(bound));
  } else {
    binds.emplace_back(leadLocal, a.lead);
  }
  if (allowIf) binds.emplace_back("if", a.hasIf ? "if " + a.ifText : std::string());
  if (allowIn) binds.emplace_back("in", a.hasIn ? "in " + a.inText : std::string());
  if (allowUsing) {
    std::string u = a.usingText;
    if (usingSlash && u.size() >= 2 && u.front() == '"' && u.back() == '"') u = u.substr(1, u.size() - 2);
    binds.emplace_back("using", a.hasUsing ? (usingSlash ? u : "using " + a.usingText) : std::string());
  }
  if (allowWeight) {
    std::string wtype, wexp;
    if (a.hasWeight) {
      const size_t eq = a.weightText.find('=');
      if (eq == std::string::npos) {
        *err = synErr("invalid weight", 198);
        return false;
      }
      std::string ty = lower(trim(a.weightText.substr(0, eq)));
      const std::string full = ty.empty() ? std::string() : ty.substr(0, 1) + "weight";
      if (std::find(weightTypes.begin(), weightTypes.end(), full) == weightTypes.end()) {
        *err = synErr(full + "s not allowed", 101);
        return false;
      }
      wtype = full;
      wexp = "= " + trim(a.weightText.substr(eq + 1));
    }
    binds.emplace_back("weight", wtype);
    binds.emplace_back("exp", wexp);
  }

  // ---- the options ----
  std::vector<bool> seen(opts.size(), false);
  std::vector<std::string> values(opts.size());
  std::string starText;
  const int starIdx = [&]() {
    for (size_t i = 0; i < opts.size(); ++i)
      if (opts[i].star) return static_cast<int>(i);
    return -1;
  }();
  for (const SynItem& it : synItems(optTail)) {
    if (it.text == ",") continue;
    const std::string tok = it.optional ? "[" + it.text + "]" : it.text;
    const size_t lp = tok.find('(');
    const std::string key = lower(lp == std::string::npos ? tok : tok.substr(0, lp));
    std::string arg;
    const bool hasArg = lp != std::string::npos;
    if (hasArg) {
      const size_t rp = tok.rfind(')');
      arg = tok.substr(lp + 1, (rp == std::string::npos || rp < lp ? tok.size() : rp) - lp - 1);
    }
    int hit = -1;
    for (size_t i = 0; i < opts.size(); ++i) {
      const SynOpt& o = opts[i];
      if (o.star) continue;
      const std::string typedFull = (o.noForm ? "no" : "") + o.full;
      if (key.size() >= o.minLen && key.size() <= typedFull.size() &&
          typedFull.compare(0, key.size(), key) == 0) {
        hit = static_cast<int>(i);
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
        *err = synErr("option " + o.full + "() not allowed", 198);
        return false;
      }
      values[static_cast<size_t>(hit)] = o.noForm ? "no" + o.full : o.full;
      continue;
    }
    if (!hasArg) {
      *err = synErr("option " + o.full + "() required", 198);
      return false;
    }
    std::string v = trim(arg);
    if (o.type == "integer" || o.type == "real" || o.type == "cilevel") {
      char* end = nullptr;
      const double d = std::strtod(v.c_str(), &end);
      if (v.empty() || end == nullptr || *end != '\0' ||
          (o.type == "integer" && d != std::floor(d))) {
        *err = synErr("option " + o.full + "() incorrectly specified", 198);
        return false;
      }
    } else if (o.type == "varname" || o.type == "varlist") {
      std::vector<std::string> ex;
      if (!synVars(v, session, o.numeric, &ex, err)) return false;
      if (o.type == "varname" && ex.size() != 1) {
        *err = synErr(ex.empty() ? "variable name required" : "too many variables specified",
                      ex.empty() ? 100 : 103);
        return false;
      }
      v = joinWords(ex);
    } else if (o.type == "name") {
      if (!strutil::isValidStataIdentifier(v)) {
        *err = synErr("option " + o.full + "() incorrectly specified", 198);
        return false;
      }
    } else if (o.type == "passthru") {
      v = o.full + "(" + arg + ")";
    } else if (o.type == "string" && !o.asis) {
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      // BUG-1049: the compound form loses its quotes too (title(`"a "b" c"')
      // binds a "b" c in StataBE).
      else if (v.compare(0, 2, "`\"") == 0) {
        auto [inner, after] = eval::scanOneToken(v, /*bind=*/true);
        if (after.empty()) v = inner;
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
        *err = synErr("option " + o.full + "() required", 198);
        return false;
      }
      if (o.type == "cilevel") values[i] = std::to_string(session.level);
      else values[i] = o.deflt;
    }
    binds.emplace_back(o.full, values[i]);
  }
  for (const auto& [k, v] : binds) session.eval.locals.set(k, v);
  return true;
}

}  // namespace

}  // namespace piet::program
