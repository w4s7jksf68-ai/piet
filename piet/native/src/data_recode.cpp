// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data recode.
#include "data_mutation_internal.h"
namespace piet::handlers::data_mutation_detail {


// ---------------------------------------------------------------------------
// recode varlist (rule) [(rule) ...] [if] [in] [, generate(newvars) prefix(str)
//        label(name) copyrest test]
// recode varname rule [rule ...] [if] [in] [, ...]
// Behaviour per [D] recode (syntax, rules, options, the min/max keywords) and
// the StataBE 19.5 measurements.
// A recode is a nested conditional assignment ([D] recode, Remarks): each
// source is rewritten (in place) or copied into a new variable through one
// dispatched `replace`/`generate` whose right-hand side is a cond() chain built
// from the rules, so storage-type promotion and float storage stay those
// commands' own. The rule set is also evaluated here, row by row, wherever a
// number has to be known before or apart from the store: min/max, the change
// count r(N), `test`'s notes and the type a new variable keeps.


// The values of one numeric column of the current head: the §38 column fetch
// when the head is resident (no whole-frame flush), else the datastore.
std::optional<std::vector<double>> headColumnValues(SessionState& session, const std::string& col) {
  std::string projPath;
  std::unique_ptr<Datastore> proj = fetchColumns(session, {col}, session.workdir(), &projPath);
  std::optional<std::vector<double>> out;
  Datastore* d = proj ? proj.get() : session.datastore();
  if (d != nullptr && d->has(col) && d->dtype(col) == "f64") {
    const double* p = d->col(col).f64();
    out = std::vector<double>(p, p + d->nRows());
  }
  if (proj) {
    proj.reset();
    cecon::native_unlink(projPath);
  }
  return out;
}


// The smallest integer type that holds every non-missing value (compress on a
// long), "" when a value is not an integer or leaves long's range.
std::string compressedIntegerType(const std::vector<double>& v) {
  double lo = 0.0, hi = 0.0;
  for (double x : v) {
    if (std::isnan(x)) continue;
    if (x != std::floor(x)) return std::string();
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo >= -127.0 && hi <= 100.0) return "byte";
  if (lo >= -32767.0 && hi <= 32740.0) return "int";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return std::string();
}


// Blank-separated words; `(`, `)`, `/` and `=` stand alone; "..." and the
// compound `"..."' are one token each, without their outer quotes.
std::optional<std::vector<RcTok>> recodeLex(const std::string& s) {
  std::vector<RcTok> out;
  std::string word;
  auto flush = [&]() {
    if (!word.empty()) out.push_back({word, false});
    word.clear();
  };
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == ' ' || c == '\t') {
      flush();
      ++i;
    } else if (c == '(' || c == ')' || c == '/' || c == '=') {
      flush();
      out.push_back({std::string(1, c), false});
      ++i;
    } else if (c == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      flush();
      // Compound quotes nest: find the matching "' .
      int depth = 1;
      size_t j = i + 2;
      while (j < s.size()) {
        if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
          ++depth;
          j += 2;
        } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
          if (--depth == 0) break;
          j += 2;
        } else {
          ++j;
        }
      }
      if (depth != 0) return std::nullopt;
      out.push_back({s.substr(i + 2, j - (i + 2)), true});
      i = j + 2;
    } else if (c == '"') {
      flush();
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) return std::nullopt;
      out.push_back({s.substr(i + 1, close - i - 1), true});
      i = close + 1;
    } else {
      word.push_back(c);
      ++i;
    }
  }
  flush();
  return out;
}


std::optional<RcEl> recodeParseEl(const RcTok& t) {
  RcEl e;
  e.typed = t.text;
  if (t.quoted) return std::nullopt;
  const std::string& s = t.text;
  if (s == ".") {
    e.kind = RcEl::Miss;
    return e;
  }
  if (s.size() == 2 && s[0] == '.' && s[1] >= 'a' && s[1] <= 'z') {
    e.kind = RcEl::Miss;
    e.miss = s[1] - 'a' + 1;
    return e;
  }
  if (s == "min" || s == "max") {
    e.kind = s == "min" ? RcEl::Min : RcEl::Max;
    return e;
  }
  static const std::regex kNum(R"(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)");
  if (!std::regex_match(s, kNum)) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(v)) return std::nullopt;
  e.num = v;
  return e;
}


// The keyword a word spells, if any: else (`*` too), missing (at least `mis`),
// nonmissing (at least `nonm`).
std::optional<std::string> recodeKeyword(const RcTok& t) {
  if (t.quoted) return std::nullopt;
  const std::string& s = t.text;
  if (s == "else" || s == "*") return std::string("else");
  if (s.size() >= 3 && std::string("missing").compare(0, s.size(), s) == 0)
    return std::string("missing");
  if (s.size() >= 4 && std::string("nonmissing").compare(0, s.size(), s) == 0)
    return std::string("nonmissing");
  return std::nullopt;
}


// ---- evaluation ------------------------------------------------------------

// float(): the nearest single, and system missing from 2^127 up, where Stata's
// float range ends (StataBE: float(2e38) is `.`, so an element or to-value that
// large acts as `.` at single precision; [D] Data types).
double rcSingle(double v) {
  if (std::isnan(v)) return v;
  if (std::fabs(v) < 0x1p127) {
    const double f = static_cast<double>(static_cast<float>(v));
    if (std::fabs(f) < 0x1p127) return f;
  }
  return cecon::stata_missing(cecon::kMissSys);
}


RcKey rcKeyOfValue(double v, bool single) {
  RcKey k;
  const double stored = single ? rcSingle(v) : v;
  if (std::isnan(stored)) {
    k.missing = true;
    k.kind = std::max(0, cecon::missing_kind(stored));
  } else {
    k.num = stored;
  }
  return k;
}


RcKey rcKeyOfEl(const RcEl& e, const RcBound& b) {
  switch (e.kind) {
    case RcEl::Miss: {
      RcKey k;
      k.missing = true;
      k.kind = e.miss;
      return k;
    }
    case RcEl::Min:
      return rcKeyOfValue(b.minV, b.single);
    case RcEl::Max:
      return rcKeyOfValue(b.maxV, b.single);
    case RcEl::Num:
      break;
  }
  return rcKeyOfValue(e.num, b.single);
}


int rcCompare(const RcKey& a, const RcKey& b) {
  if (a.missing != b.missing) return a.missing ? 1 : -1;
  if (a.missing) return a.kind < b.kind ? -1 : (a.kind > b.kind ? 1 : 0);
  return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
}


bool rcMatches(const RcElement& el, double v, const RcBound& b) {
  const RcKey x = rcKeyOfValue(v, b.single);
  if (!el.range) return rcCompare(x, rcKeyOfEl(el.lo, b)) == 0;
  return rcCompare(x, rcKeyOfEl(el.lo, b)) >= 0 && rcCompare(x, rcKeyOfEl(el.hi, b)) <= 0;
}


double rcToValue(const RcEl& e, const RcBound& b) {
  switch (e.kind) {
    case RcEl::Miss:
      return cecon::stata_missing(e.miss);
    case RcEl::Min:
      return b.minV;
    case RcEl::Max:
      return b.maxV;
    case RcEl::Num:
      break;
  }
  return e.num;
}


double rcApply(double v, const RcBound& b) {
  for (const RcRule& r : b.rs->cond)
    for (const RcElement& el : r.elems)
      if (rcMatches(el, v, b)) return rcToValue(r.to, b);
  if (b.rs->elseTo.has_value()) return rcToValue(*b.rs->elseTo, b);
  const bool miss = std::isnan(v);
  if (miss && b.rs->missTo.has_value()) return rcToValue(*b.rs->missTo, b);
  if (!miss && b.rs->nonmissTo.has_value()) return rcToValue(*b.rs->nonmissTo, b);
  return v;
}


// Whether a recode changed a value: at the source's precision (StataBE: a float
// holding .1 given the to-value .1 is no change, a long given a neighbour that
// rounds to the same float is one), missing kinds distinct.
bool rcDiffers(double before, double after, bool single) {
  const double a = single ? rcSingle(before) : before;
  const double b = single ? rcSingle(after) : after;
  const bool ma = std::isnan(a), mb = std::isnan(b);
  if (ma || mb) return ma != mb || cecon::missing_kind(a) != cecon::missing_kind(b);
  return a != b;
}


// ---- the dispatched expression ----------------------------------------------

std::string rcNumLiteral(double v) {
  const std::string s = eval::pythonFloatRepr(v);
  return v < 0 ? "(" + s + ")" : s;
}


std::string rcMissLiteral(int kind) {
  return kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + kind - 1);
}


std::string rcKeyLiteral(const RcKey& k) {
  return k.missing ? rcMissLiteral(k.kind) : rcNumLiteral(k.num);
}


// The condition one element tests, on source `x`. Numbers compare at the
// source's precision (`float(x)` against bounds already rounded to single for
// byte/int/float); missing codes compare exactly on `x` itself.
std::string rcElementCond(const RcElement& el, const std::string& x, const RcBound& b) {
  const std::string X = b.single ? "float(" + x + ")" : x;
  const RcKey lo = rcKeyOfEl(el.lo, b);
  if (!el.range) {
    if (lo.missing) return "(" + x + "==" + rcKeyLiteral(lo) + ")";
    return "(" + X + "==" + rcKeyLiteral(lo) + ")";
  }
  const RcKey hi = rcKeyOfEl(el.hi, b);
  if (!lo.missing && !hi.missing)
    return "(" + X + ">=" + rcKeyLiteral(lo) + "&" + X + "<=" + rcKeyLiteral(hi) + ")";
  if (lo.missing && hi.missing)
    return "(" + x + ">=" + rcKeyLiteral(lo) + "&" + x + "<=" + rcKeyLiteral(hi) + ")";
  if (lo.missing) return "(0)";  // a missing lower bound above a number: matches nothing
  return "((" + x + "<.&" + X + ">=" + rcKeyLiteral(lo) + ")|(" + x + ">=.&" + x +
         "<=" + rcKeyLiteral(hi) + "))";
}


std::string rcToLiteral(const RcEl& e, const RcBound& b) {
  const double v = rcToValue(e, b);
  if (std::isnan(v)) return rcMissLiteral(std::max(0, cecon::missing_kind(v)));
  return rcNumLiteral(v);
}


std::string rcExpression(const std::string& x, const RcBound& b) {
  std::string fallback;
  if (b.rs->elseTo.has_value()) {
    fallback = rcToLiteral(*b.rs->elseTo, b);
  } else if (b.rs->missTo.has_value() || b.rs->nonmissTo.has_value()) {
    fallback = "cond(" + x + ">=.," +
               (b.rs->missTo.has_value() ? rcToLiteral(*b.rs->missTo, b) : x) + "," +
               (b.rs->nonmissTo.has_value() ? rcToLiteral(*b.rs->nonmissTo, b) : x) + ")";
  } else {
    fallback = x;
  }
  std::string expr = fallback;
  for (auto it = b.rs->cond.rbegin(); it != b.rs->cond.rend(); ++it) {
    std::string c;
    for (const RcElement& el : it->elems) {
      if (!c.empty()) c += "|";
      c += rcElementCond(el, x, b);
    }
    expr = "cond(" + c + "," + rcToLiteral(it->to, b) + "," + expr + ")";
  }
  return expr;
}


// The storage type `: type` reports for a numeric source.
std::string rcSourceType(const SessionState& session, const std::string& v) {
  auto it = session.varMeta.find(v);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      storage::isNumericType(*it->second.stataType))
    return *it->second.stataType;
  if (session.implicitFloatVars.count(v) != 0) return "float";
  return "double";
}


// Whether a value stored at `created` precision survives a change to `target`
// unaltered ([D] recast without force; integer ranges per [D] Data types).
bool rcFitsType(double v, const std::string& target) {
  if (std::isnan(v)) return true;
  if (target == "double") return true;
  if (target == "float") return rcSingle(v) == v;
  if (v != std::floor(v)) return false;
  if (target == "byte") return v >= -127 && v <= 100;
  if (target == "int") return v >= -32767 && v <= 32740;
  if (target == "long") return v >= -2147483647.0 && v <= 2147483620.0;
  return false;
}


double rcStore(double v, const std::string& created) {
  return (created == "float" && !std::isnan(v)) ? rcSingle(v) : v;
}


// The variable label of a new variable: "RECODE of SRC (SRCLABEL)", at most
// 80 bytes, never cut inside a UTF-8 character.
std::string rcNewVarLabel(const SessionState& session, const std::string& src) {
  std::string text = "RECODE of " + src;
  auto it = session.varMeta.find(src);
  if (it != session.varMeta.end() && it->second.label.has_value() && !it->second.label->empty())
    text += " (" + *it->second.label + ")";
  if (text.size() > 80) {
    size_t cut = 80;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
  }
  return text;
}


std::string rcQuote(const std::string& s) {
  if (s.find('"') == std::string::npos) return "\"" + s + "\"";
  return "`\"" + s + "\"'";
}


// The first error block among a dispatch's returned blocks.
std::optional<ojson> rcFirstError(const ojson& blocks) {
  if (blocks.is_array()) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
  } else if (blocks.is_object() && blocks.value("type", std::string()) == "error") {
    return blocks;
  }
  return std::nullopt;
}
}
namespace piet::handlers {
using namespace data_mutation_detail;


ojson runRecode(const std::string& rawLine, SessionState& session,
                const std::optional<std::string>& ifExpr) {
  eval::ParsedCommand parsed;
  try {
    parsed = eval::tokenize(rawLine);
  } catch (const eval::TokenizerError& e) {
    return ojson(e.block);
  }
  eval::canonicalizeForCommand(parsed.opts, "recode");
  if (!session.hasData()) return eval::noDataLoaded();

  // ---- varlist and rule text (§ syntax 1 / syntax 2) ----
  const std::string args = trim(parsed.argText);
  if (!args.empty() && args[0] == '(') return rcErrorBlock("too few variables specified", 102);
  const size_t paren = args.find('(');
  const size_t eqPos = args.find('=');
  const bool parenthesised = paren != std::string::npos && (eqPos == std::string::npos || eqPos > paren);
  std::string varText, ruleText;
  if (parenthesised) {
    varText = args.substr(0, paren);
    ruleText = args.substr(paren);
  } else {
    const size_t sp = args.find_first_of(" \t");
    varText = sp == std::string::npos ? args : args.substr(0, sp);
    ruleText = sp == std::string::npos ? std::string() : args.substr(sp + 1);
  }
  ruleText = trim(ruleText);

  std::vector<std::string> sources;
  {
    std::istringstream ws(varText);
    std::string w;
    while (ws >> w) {
      if (w.find_first_of("*?~-") != std::string::npos) {
        auto vr = loops::expandVarlist(w, session);
        if (vr.isError) return vr.error;
        for (auto& n : vr.tokens) sources.push_back(n);
      } else {
        std::vector<std::string> one{w};
        if (auto err = resolveByKeyNames(session, one)) return *err;
        sources.push_back(one.front());
      }
    }
  }
  if (sources.empty()) return rcErrorBlock("varlist required", 100);
  if (!parenthesised && sources.size() > 1) return rcErrorBlock("too many variables specified", 103);

  const std::vector<VarInfo>* card = session.residentReadCard();
  auto dtypeOf = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      const VarInfo* vi = cardFind(*card, name);
      if (vi == nullptr) return std::nullopt;
      if (vi->dtype == "factor" || vi->dtype == "f64") return vi->dtype;
    }
    Datastore* d = session.datastore();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  for (const std::string& s : sources) {
    std::optional<std::string> dt = dtypeOf(s);
    if (!dt.has_value()) return rcErrorBlock("variable " + s + " not found", 111);
    if (*dt != "f64") return rcErrorBlock("recode only allows numeric variables", 108);
  }
  if (ruleText.empty()) return rcErrorBlock("rules expected", 198);

  // ---- options ----
  std::optional<std::string> genOpt, intoOpt, labelOpt, prefixOpt;
  bool copyrest = false, test = false;
  for (const auto& [k, v] : parsed.opts) {
    const bool flagOption = k == "copyrest" || k == "test";
    const bool nameOption = k == "generate" || k == "into" || k == "label" || k == "prefix";
    // StataBE: `gen` with no parentheses and `test(3)` are r(198); an empty
    // `gen()` is the same as not giving the option.
    if (nameOption && v.isBool) return rcErrorBlock("option " + k + " incorrectly specified", 198);
    if (flagOption && !v.isBool) return rcErrorBlock("option " + k + "() incorrectly specified", 198);
    const std::string text = v.isBool ? std::string() : trim(v.strValue);
    if (k == "copyrest") copyrest = v.boolValue;
    else if (k == "test") test = v.boolValue;
    else if (text.empty()) continue;
    else if (k == "generate") genOpt = text;
    else if (k == "into") intoOpt = text;
    else if (k == "label") labelOpt = text;
    else if (k == "prefix") prefixOpt = text;
  }
  std::vector<std::string> newNames;
  if (prefixOpt.has_value()) {
    if (genOpt.has_value() || intoOpt.has_value())
      return rcErrorBlock("option prefix() may not be combined with generate() or into()", 198);
    for (const std::string& s : sources) {
      const std::string nm = *prefixOpt + s;
      if (!strutil::isValidStataIdentifier(nm))
        return rcErrorBlock(nm + " invalid variable name", 198);
      if (dtypeOf(nm).has_value()) return rcErrorBlock("variable " + nm + " already exists", 110);
      newNames.push_back(nm);
    }
  } else if (genOpt.has_value() || intoOpt.has_value()) {
    if (genOpt.has_value() && intoOpt.has_value())
      return rcErrorBlock("options generate() and into() are synonyms", 198);
    std::istringstream ns(genOpt.has_value() ? *genOpt : *intoOpt);
    std::string nm;
    while (ns >> nm) {
      if (dtypeOf(nm).has_value()) return rcErrorBlock("variable " + nm + " already defined", 110);
      if (!strutil::isValidStataIdentifier(nm)) return rcErrorBlock(nm + " invalid name", 198);
      newNames.push_back(nm);
    }
    if (newNames.size() != sources.size())
      return rcErrorBlock("the number of new and transformed varnames should be the same", 198);
  }
  {
    std::set<std::string> seen;
    for (const std::string& nm : newNames)
      if (!seen.insert(nm).second)
        return rcErrorBlock("new variable " + nm + " mentioned more than once", 110);
  }
  const bool inPlace = newNames.empty();
  if (labelOpt.has_value()) {
    if (inPlace) return rcErrorBlock("option label() allowed only with generate()", 198);
    if (!strutil::isValidStataIdentifier(*labelOpt))
      return rcErrorBlock(*labelOpt + " invalid name", 7);
  }
  const RcLabelPolicy policy = inPlace ? RcLabelPolicy::InPlace
                               : (sources.size() > 1 && !labelOpt.has_value())
                                   ? RcLabelPolicy::MultiNoLabel
                                   : RcLabelPolicy::Allowed;

  // ---- rules ----
  RcRuleSet rules;
  {
    std::optional<std::vector<RcTok>> toks = recodeLex(ruleText);
    // A rule-grammar error leaves r() cleared: nothing is posted, so every
    // r() name reads as missing.
    auto clearR = [&]() {
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "recode", {});
    };
    if (!toks.has_value()) {
      clearR();
      return rcErrorBlock("unmatched quote in rule", 198);
    }
    RcParser parser(std::move(*toks), policy);
    try {
      if (parenthesised) parser.parseParenthesised(rules);
      else parser.parseBare(rules);
    } catch (const RcParseError& e) {
      clearR();
      return rcErrorBlock(e.msg, 198);
    }
  }

  // ---- the sample ----
  const std::optional<std::string> ifText =
      ifExpr.has_value() && !trim(*ifExpr).empty() ? ifExpr : parsed.ifExpr;
  const bool haveIf = ifText.has_value() && !trim(*ifText).empty();
  std::string rangeArg;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *parsed.inRange, "recode");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
  }
  // A qualified sample is marked once, before anything changes (a later
  // source's condition must not see an earlier source's recode).
  std::string marker;
  struct MarkerDrop {
    SessionState& s;
    std::string& name;
    ~MarkerDrop() {
      if (!name.empty()) dispatch("quietly drop " + name, s);
    }
  } markerDrop{session, marker};
  if (haveIf || !rangeArg.empty()) {
    for (int k = 0;; ++k) {
      std::string cand = "__recode_touse" + std::to_string(k);
      if (!dtypeOf(cand).has_value()) {
        marker = cand;
        break;
      }
    }
    std::string line = "quietly generate byte " + marker + " = 1";
    if (haveIf) line += " if (" + *ifText + ")";
    if (!rangeArg.empty()) line += " in " + rangeArg;
    ojson blocks = dispatch(line, session);
    if (auto err = rcFirstError(blocks)) {
      // Nothing was created; the marker name must not be dropped.
      if (!dtypeOf(marker).has_value()) marker.clear();
      return *err;
    }
    card = session.residentReadCard();
  }
  const std::string inSample = marker.empty() ? std::string() : "(" + marker + "<.)";
  std::optional<std::vector<bool>> sampleMask;  // read on demand
  auto sampleRows = [&]() -> const std::vector<bool>* {
    if (marker.empty()) return nullptr;  // every observation
    if (!sampleMask.has_value()) {
      std::optional<std::vector<double>> m = headColumnValues(session, marker);
      if (!m.has_value()) return nullptr;
      std::vector<bool> mask(m->size());
      for (size_t i = 0; i < m->size(); ++i) mask[i] = !std::isnan((*m)[i]);
      sampleMask = std::move(mask);
    }
    return &*sampleMask;
  };

  const std::string created = storage::resolveCreateType(session, "");
  double lastChanges = 0;
  bool overlapNote = false, unusedNote = false;
  // Every source is recoded from its values as they were when the command
  // started: a variable named twice is counted twice from those values and
  // rewritten once (StataBE: `recode w1 w1 (3=4)` reports the same change
  // count twice).
  std::map<std::string, std::vector<double>> startValues;
  std::set<std::string> rewritten;

  for (size_t si = 0; si < sources.size(); ++si) {
    const std::string& x = sources[si];
    const std::string srcType = rcSourceType(session, x);
    RcBound bound;
    bound.rs = &rules;
    bound.single = srcType == "byte" || srcType == "int" || srcType == "float";

    // Whether the new variable can take the source's type without reading
    // the data: every to-value fits it, and a pass-through value is the
    // source's own (so it fits unless float storage can move a long).
    std::string target;
    bool fitKnown = true, fits = true;
    if (!inPlace) {
      target = srcType;
      if (target != created && target != "double") {
        if (target == "long" && created == "float") {
          fitKnown = false;
        } else {
          auto toFits = [&](const RcEl& e) {
            return e.kind != RcEl::Num || rcFitsType(rcStore(e.num, created), target);
          };
          bool all = true;
          for (const RcRule& r : rules.cond) all = all && toFits(r.to);
          for (const auto* k : {&rules.elseTo, &rules.missTo, &rules.nonmissTo})
            if (k->has_value()) all = all && toFits(**k);
          if (!all) fitKnown = false;
        }
      }
    }

    // r(N) is the last source's count. In place: the changes. With a new
    // variable: the changes when its type is already the source's, else the
    // values that kept it from taking that type (0 when it did). StataBE
    // differential, oracle/cr_recode_fuzz.
    const bool countChanges =
        inPlace || (si + 1 == sources.size() && target == created);
    const bool needData = countChanges || test || rules.usesMinMax || !fitKnown;
    std::vector<double> values;
    const std::vector<bool>* mask = nullptr;
    bool transform = true;
    if (needData) {
      auto known = startValues.find(x);
      if (known != startValues.end()) {
        values = known->second;
      } else {
        std::optional<std::vector<double>> got = headColumnValues(session, x);
        if (!got.has_value())
          return engineErrorBlock("recode: could not read variable " + x);
        values = std::move(*got);
        if (inPlace) startValues[x] = values;
      }
      mask = sampleRows();
      if (mask == nullptr && !marker.empty())
        return engineErrorBlock("recode: could not read the if/in sample");
      if (rules.usesMinMax) {
        bool any = false;
        for (size_t r = 0; r < values.size(); ++r) {
          if (mask != nullptr && !(*mask)[r]) continue;
          const double v = values[r];
          if (std::isnan(v)) continue;
          if (!any) {
            bound.minV = bound.maxV = v;
            any = true;
          } else {
            bound.minV = std::min(bound.minV, v);
            bound.maxV = std::max(bound.maxV, v);
          }
        }
        // [D] recode: with no value to take min/max from, the source is not
        // transformed at all.
        transform = any;
      }
    }

    double changes = 0;
    if (needData && transform) {
      size_t nElems = 0;
      for (const RcRule& r : rules.cond) nElems += r.elems.size();
      std::vector<char> used(nElems, 0);
      for (size_t r = 0; r < values.size(); ++r) {
        if (mask != nullptr && !(*mask)[r]) continue;
        const double v = values[r];
        if (rcDiffers(v, rcApply(v, bound), bound.single)) changes += 1;
        if (test) {
          size_t idx = 0, hits = 0;
          for (const RcRule& rule : rules.cond)
            for (const RcElement& el : rule.elems) {
              if (rcMatches(el, v, bound)) {
                used[idx] = 1;
                ++hits;
              }
              ++idx;
            }
          if (hits > 1) overlapNote = true;
        }
      }
      if (test)
        for (char u : used)
          if (!u) unusedNote = true;
    }

    if (inPlace) {
      lastChanges = changes;
      if (!transform || !rewritten.insert(x).second) continue;
      std::string line = "quietly replace " + x + " = " + rcExpression(x, bound);
      if (!inSample.empty()) line += " if " + inSample;
      ojson blocks = dispatch(line, session);
      if (auto err = rcFirstError(blocks)) return *err;
      card = session.residentReadCard();
      continue;
    }

    // ---- a new variable ----
    const std::string& nv = newNames[si];
    std::string rhs;
    if (!transform) {
      rhs = x;  // nothing to take min/max from: a copy of the source everywhere
    } else if (inSample.empty()) {
      rhs = rcExpression(x, bound);
    } else {
      rhs = "cond(" + inSample + "," + rcExpression(x, bound) + "," + (copyrest ? x : ".") + ")";
    }
    ojson blocks = dispatch("quietly generate " + nv + " = " + rhs, session);
    if (auto err = rcFirstError(blocks)) return *err;
    card = session.residentReadCard();

    double misfits = 0;
    if (!fitKnown) {
      for (size_t r = 0; r < values.size(); ++r) {
        const bool in = mask == nullptr || (*mask)[r];
        double v;
        if (!transform) v = values[r];
        else if (in) v = rcApply(values[r], bound);
        else if (copyrest) v = values[r];
        else continue;
        if (!rcFitsType(rcStore(v, created), target)) misfits += 1;
      }
      fits = misfits == 0;
    }
    lastChanges = target == created ? changes : misfits;
    if (target != created && fits) {
      VarMeta& vm = session.varMeta[nv];
      vm.stataType = target;
      vm.format = storage::stataDefaultFormat(created);
      session.implicitFloatVars.erase(nv);
    }
    session.varMeta[nv].label = rcNewVarLabel(session, x);
  }

  // ---- value labels from the rules ----
  if (!inPlace && !rules.labels.empty()) {
    const std::string set = labelOpt.has_value() ? *labelOpt : newNames.front();
    std::map<std::string, std::string> latest;  // later rule wins
    std::vector<std::string> order;
    for (const auto& [code, text] : rules.labels) {
      if (latest.find(code) == latest.end()) order.push_back(code);
      latest[code] = text;
    }
    std::string line = "quietly label define " + set;
    for (const std::string& code : order) line += " " + code + " " + rcQuote(latest[code]);
    line += ", modify";
    ojson blocks = dispatch(line, session);
    if (auto err = rcFirstError(blocks)) return *err;
    for (const std::string& nv : newNames) {
      ojson vb = dispatch("quietly label values " + nv + " " + set, session);
      if (auto err = rcFirstError(vb)) return *err;
    }
  }

  // Drop the sample marker before the result card is taken.
  if (!marker.empty()) {
    ojson db = dispatch("quietly drop " + marker, session);
    marker.clear();
    if (auto err = rcFirstError(db)) return *err;
  }

  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", lastChanges);
  eval::declareRPosted(session.eval, "recode", {"N"});

  ojson result = inPlace ? mutationResult(session, "recode") : mutationResult(session, "recode", newNames);
  if (!test) return result;
  ojson out = ojson::array();
  out.push_back(result);
  auto note = [](const std::string& text) {
    ojson b;
    b["type"] = "display";
    b["class"] = "note";
    b["text"] = text;
    return b;
  };
  if (overlapNote) out.push_back(note("(transformation rules \"overlapped\")"));
  if (unusedNote) out.push_back(note("(one or more transformation rules did not match any values)"));
  return out;
}
}
