// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "expression_support.h"
#include "prefix_support.h"
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/variables.h"

namespace piet::dispatch_detail {


StorageTypeInfo stripStorageType(const std::string& raw) {
  StorageTypeInfo info;
  info.expr = raw;
  std::string beforeEq = raw.substr(0, raw.find('='));
  size_t start = beforeEq.find_first_not_of(" \t");
  if (start == std::string::npos) return info;
  std::string trimmed = beforeEq.substr(start);
  // tokens: byte|int|long|float|double|str#|str#L|strL (case-insensitive),
  // followed by whitespace.
  size_t sp = trimmed.find_first_of(" \t");
  if (sp == std::string::npos) return info;
  std::string tok = lower(trimmed.substr(0, sp));
  bool isNumericType = (tok == "byte" || tok == "int" || tok == "long" || tok == "float" ||
                        tok == "double");
  bool isStrType = false;
  if (tok.rfind("str", 0) == 0) {
    // str, str#, str#L, strL — everything after "str" is digits and/or "l".
    std::string rest = tok.substr(3);
    bool ok = true;
    for (char c : rest) {
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'l') {
        ok = false;
        break;
      }
    }
    isStrType = ok;
  }
  if (isNumericType || isStrType) {
    size_t consumed = (start + sp);
    // consume the run of whitespace after the type token too
    size_t after = raw.find_first_not_of(" \t", consumed);
    info.expr = (after == std::string::npos) ? "" : raw.substr(after);
    info.stringTyped = isStrType;
    info.typeToken = tok;  // BUG-250: keep the declaration alive
  }
  return info;
}


// BUG-471 T2: an UNTYPED creation under `set type double` is a double, and its
// label says so (w2e_float_default.log settype_double_gen :1252,
// egen_mean_settype_double :1196). Under the factory default float the record is
// BUG-295's implicit plane exactly as before, so the default path is unchanged.
StorageTypeInfo withSessionDefaultType(const SessionState& session, StorageTypeInfo info) {
  if (info.typeToken.empty() && session.defaultStorageType != "float")
    info.typeToken = session.defaultStorageType;
  return info;
}


// format-display-default-lifecycle: a creation arm renders its varlist card
// BEFORE the arm records the new variable's storage type, so the card's format
// for it was resolved with no type (%9.0g for a `generate double`). Re-resolve
// that one entry once the type is recorded, so the card states what describe
// and `: format` answer. A just-created column states no format of its own (the
// engine never sends one for a computed column, BUG-365).
void refreshCardFormat(const SessionState& session, ojson& blk, const std::string& varname) {
  if (varname.empty() || !blk.is_object() || !blk.contains("varlist_after") ||
      !blk["varlist_after"].is_array())
    return;
  for (auto& e : blk["varlist_after"]) {
    if (!e.is_object() || e.value("name", std::string()) != varname) continue;
    VarInfo v;
    v.name = varname;
    v.dtype = e.value("dtype", std::string());
    e["format"] = session.displayFormat(v);
  }
}


// BUG-250: record a declared storage subtype after a creation arm succeeded.
// `blk` is the block the arm is about to return — an "error" block means the
// column was not created and nothing may be recorded (a stale claim on a
// non-column is the BUG-190 defect family). No declaration or no name = no-op,
// so every undeclared `gen` stays byte-identical.
void recordDeclaredStataType(SessionState& session, const std::string& varname,
                             const StorageTypeInfo& info, ojson& blk) {
  if (varname.empty()) return;
  if (blk.is_object() && blk.value("type", std::string()) == "error") return;
  if (info.typeToken.empty() || info.typeToken == "str") {
    // FU-936: an untyped (or bare `str`) string generate has its str# recorded
    // by the row writer (strmutate writeNew, from the longest value); keep it
    // and re-resolve the card's format from it.
    auto it = session.varMeta.find(varname);
    if (it != session.varMeta.end() && it->second.stataType.has_value() &&
        it->second.stataType->compare(0, 3, "str") == 0) {
      session.implicitFloatVars.erase(varname);
      refreshCardFormat(session, blk, varname);
      return;
    }
  }
  if (info.typeToken.empty()) {
    // BUG-295: no declaration means Stata's own default, `float` — record it on
    // the implicit plane so the .dta export stops handing this column to the
    // writer's narrowest-lossless scan (which stored `byte` for `gen z = _n`,
    // and made the SAME column answer float before a save and byte after).
    // Not written to varMeta.stataType: `describe`/`: type` already answer
    // float here via BUG-250's fallback, and a declaration this layer never
    // saw must not start looking like one.
    session.implicitFloatVars.insert(varname);
    return;
  }
  session.implicitFloatVars.erase(varname);  // an explicit declaration wins
  // The token is lowercased by the strip; Stata spells the long-string type
  // `strL` (`: type` answers strL, and the .dta reader labels it so).
  session.varMeta[varname].stataType = info.typeToken == "strl" ? std::string("strL")
                                                                  : info.typeToken;
  refreshCardFormat(session, blk, varname);
}


// BUG-1123: generate's "(N missing values generated)", "(1 missing value
// generated)" for one and nothing for none (StataBE 19.5, numeric and string
// alike: an empty string counts). The count is the new column's n_missing on the
// data_mutation block the arm just pushed; a display block, so `quietly` drops it
// as Stata does. It used to reach the console only as that field.
// Stata counts the expression's missing values, before the storage type turns the
// ones it cannot hold into missing too: `gen byte b = 300` and `gen float f =
// 1e40` print nothing, `gen int i = 1e6 if _n>1` on 5 rows "(1 missing value
// generated)" (StataBE 19.5). Those stores are the engine's `store_overflow`,
// which runGen/runBysortGen leave on the block (translate::carryStoreOverflow),
// and the string-function path's own count (strmutate writeNew's storeAs);
// they are taken off the count, and the field goes no further.
void appendGenMissingNote(ojson& blocks, const std::string& varname) {
  if (varname.empty() || blocks.empty()) return;
  ojson& blk = blocks.back();
  if (!blk.is_object() || blk.value("type", std::string()) != "data_mutation") return;
  long long overflow = 0;
  if (auto it = blk.find("store_overflow"); it != blk.end()) {
    if (it->is_number_integer()) overflow = it->get<long long>();
    blk.erase(it);
  }
  if (!blk.contains("varlist_after") || !blk["varlist_after"].is_array()) return;
  for (const auto& e : blk["varlist_after"]) {
    if (!e.is_object() || e.value("name", std::string()) != varname) continue;
    const auto nm = e.find("n_missing");
    if (nm == e.end() || !nm->is_number_integer()) return;
    const long long n = nm->get<long long>() - overflow;
    if (n <= 0) return;
    std::string digits = std::to_string(n), count;  // "(1,500 missing values generated)"
    for (size_t i = 0; i < digits.size(); ++i) {
      if (i > 0 && (digits.size() - i) % 3 == 0) count += ',';
      count += digits[i];
    }
    ojson note;
    note["type"] = "display";
    note["class"] = "note";
    note["text"] = "(" + count + " missing value" + (n == 1 ? "" : "s") + " generated)";
    blocks.push_back(std::move(note));
    return;
  }
}


// format-display-default-lifecycle: `compress [varlist]`. It was an
// accept-and-ignore no-op, so a compressed variable kept answering the type
// (and the default format) it had before. METADATA only, like recast: the
// frame stores every number as f64 and every string as a factor, so the
// demotion is the declared storage type ([D] compress: double to long, int or
// byte; float, long to int or byte; int to byte; str# to the longest value's
// width). A numeric keeps its format and a string's default follows its new
// width (prepareStorageChange): w2af_format_lifecycle.log af_compress
// `byte|%9.0g|int|%10.0g|byte|%12.0g|str2|%9s`, and the messages at
// :763-767. A string column with no recorded width is left as it is: there
// is no "was" type to report. BUG-1102 (FU-1043): a strL becomes the str# of
// its longest value, at least str1, when every value fits a str# (2045 bytes),
// none holds a binary 0, and the str# takes less memory than the strL would
// once its duplicates were stored once ([D] compress: it "will do nothing if
// it cannot save memory by changing a strL"). Stata counts a strL observation
// as 8 bytes when empty, 81 bytes plus the text when it holds its own copy,
// and 32 bytes when it shares an earlier copy. StataBE 19.5, measured
// 2026-11-03: "ab" in 2 observations saves 111, in 3 saves 141; 100
// observations of string(_n) save 7,992 as str3, but with one 2,000-byte
// value they stay strL ("(0 bytes saved)"), as does one `generate` of a
// 50-character text over 100 observations; 10 observations alternating two
// 50-character texts become str50 (810 saved), alternating 55-character ones
// stay; a tie stays. Whether a value shares a copy depends on how it was
// written: one command shares a value equal to the last non-empty one it
// wrote, so the reported saving takes a value equal to the previous non-empty
// one as shared. That misses copies written by separate commands (Stata
// counts them in full) and copies a sort moved apart (Stata still shares
// them); the decision itself does not depend on it. A strL that stays is not
// coalesced (Stata's `s is strL now coalesced`). Its line follows the str#
// lines (`s3 was str20 now str3` before `sl was strL now str2`; w2_geo_strl
// .log:231 `variable t was strL now str5`). The messages are command
// output ("display" blocks, as destringNoteBlock): `quietly`/`capture`
// suppress them and `noisily`/`capture noisily` print them (log:762-767). An
// "echo" block is the command echo, which those prefixes treat the other way.
std::vector<ojson> runCompressMeta(const eval::ParsedCommand& parsed, SessionState& session) {
  std::vector<ojson> out;
  auto echo = [&out](const std::string& text) {
    ojson b;
    b["type"] = "display";
    b["text"] = text;
    out.push_back(std::move(b));
  };
  auto commas = [](int64_t v) {
    std::string d = std::to_string(v), r;
    for (size_t i = 0; i < d.size(); ++i) {
      if (i > 0 && (d.size() - i) % 3 == 0) r += ',';
      r += d[i];
    }
    return r;
  };
  if (!session.hasData()) {
    echo("  (0 bytes saved)");
    return out;
  }
  std::vector<std::string> vars;
  {
    std::string spec;
    for (const std::string& tok : parsed.varlist) spec += (spec.empty() ? "" : " ") + tok;
    auto vr = loops::expandVarlist(spec.empty() ? std::string("_all") : spec, session);
    if (vr.isError) {
      out.push_back(vr.error);
      return out;
    }
    vars = vr.tokens;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) {
    out.push_back(ojson(eval::noDataLoaded()));
    return out;
  }
  const int64_t n = ds->nRows();
  auto bytesOf = [](const std::string& t) -> int64_t {
    if (t == "byte") return 1;
    if (t == "int") return 2;
    if (t == "long" || t == "float") return 4;
    if (t == "double") return 8;
    if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
        std::all_of(t.begin() + 3, t.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
      return std::stoll(t.substr(3));
    return 0;
  };
  struct Change {
    std::string name, from, to;
    int64_t saved = 0;
  };
  // Stata reports floats, then ints, longs and doubles, then strings, each in
  // varlist order (af_compress :763-766: float, long, double, str#), then strLs.
  static const std::vector<std::string> kOrder = {"float", "int",  "long",
                                                  "double", "str", "strL"};
  std::map<std::string, std::vector<Change>> byFrom;
  for (const std::string& v : vars) {
    if (!ds->has(v)) continue;
    auto mit = session.varMeta.find(v);
    std::string cur = (mit != session.varMeta.end() && mit->second.stataType.has_value())
                          ? *mit->second.stataType
                          : std::string();
    const bool isStr = ds->dtype(v) == "factor";
    const bool isStrL = isStr && cur == "strL";
    std::string to;
    int64_t strLBytes = 0;
    if (isStr) {
      if (!isStrL && (bytesOf(cur) == 0 || cur.compare(0, 3, "str") != 0)) continue;
      const cecon::Column& c = ds->col(v);
      const int32_t* codes = c.codes();
      size_t longest = 0;
      bool binary = false;
      int64_t coalescedBytes = 0;
      int32_t lastCode = -1;
      std::vector<char> seen(isStrL ? c.levels.size() : 0, 0);
      for (int64_t i = 0; i < n; ++i) {
        const int32_t k = codes[i];
        size_t len = 0;
        if (k >= 0 && static_cast<size_t>(k) < c.levels.size()) {
          const std::string& text = c.levels[static_cast<size_t>(k)];
          len = text.size();
          binary = binary || text.find('\0') != std::string::npos;
        }
        longest = std::max(longest, len);
        if (!isStrL) continue;
        const int64_t copy = 81 + static_cast<int64_t>(len);
        if (len == 0) {
          strLBytes += 8;
          coalescedBytes += 8;
          continue;
        }
        strLBytes += k == lastCode ? 32 : copy;
        coalescedBytes += seen[static_cast<size_t>(k)] != 0 ? 32 : copy;
        seen[static_cast<size_t>(k)] = 1;
        lastCode = k;
      }
      const std::string t = "str" + std::to_string(std::max<size_t>(1, longest));
      if (isStrL ? (longest <= 2045 && !binary && bytesOf(t) * n < coalescedBytes)
                 : bytesOf(t) < bytesOf(cur))
        to = t;
    } else {
      if (cur.empty()) cur = "float";  // BUG-250/BUG-295: `: type` answers float
      if (cur == "byte" || bytesOf(cur) == 0) continue;
      const double* d = ds->col(v).f64();
      bool integral = true;
      double lo = 0.0, hi = 0.0;
      bool any = false;
      for (int64_t i = 0; i < n && integral; ++i) {
        const double x = d[i];
        if (std::isnan(x)) continue;
        if (!std::isfinite(x) || std::trunc(x) != x) {
          integral = false;
          break;
        }
        if (!any || x < lo) lo = x;
        if (!any || x > hi) hi = x;
        any = true;
      }
      if (!integral) continue;
      std::string t;
      if (lo >= -127.0 && hi <= 100.0) t = "byte";
      else if (lo >= -32767.0 && hi <= 32740.0) t = "int";
      else if (lo >= -2147483647.0 && hi <= 2147483620.0) t = "long";
      if (!t.empty() && bytesOf(t) < bytesOf(cur)) to = t;
    }
    if (to.empty()) continue;
    const int64_t fromBytes = isStrL ? strLBytes : bytesOf(cur) * n;
    byFrom[isStrL ? cur : isStr ? std::string("str") : cur].push_back(
        {v, cur, to, fromBytes - bytesOf(to) * n});
  }
  int64_t saved = 0;
  for (const std::string& from : kOrder) {
    auto it = byFrom.find(from);
    if (it == byFrom.end()) continue;
    for (const Change& c : it->second) {
      VarInfo vi;
      vi.name = c.name;
      vi.dtype = from == "str" || from == "strL" ? "factor" : "f64";
      session.prepareStorageChange(vi);
      session.varMeta[c.name].stataType = c.to;
      session.implicitFloatVars.erase(c.name);
      saved += c.saved;
      echo("  variable " + c.name + " was " + c.from + " now " + c.to);
    }
  }
  echo("  (" + commas(saved) + " bytes saved)");
  return out;
}


// BUG-373: collapse reports r(N) = the number of observations it PROCESSED, the
// row count BEFORE it ran (live StataBE: 336 on a 336-row fixture, one row left).
// contract and fillin post nothing: their 336 in that capture was the previous
// command's r(N) (BUG-1451's discriminating probe: 28 from `summarize` survives
// them on 30 rows).

int64_t rowsBeforeForRN(SessionState& session) {
  if (std::optional<int64_t> r = session.residentRowsIfCurrent()) return *r;
  Datastore* ds = session.datastore();
  return ds != nullptr ? ds->nRows() : -1;
}


// Is `varname` a variable of the loaded data? nullopt = no datastore to ask.
std::optional<bool> targetVarExists(SessionState& session, const std::string& varname) {
  // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT card is
  // authoritative for name presence either way (language/confirm_command.cpp::runConfirm's
  // idiom). nullptr means "answer from the datastore", never "assume absent".
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) {
      if (vi.name == varname) return true;
    }
    return false;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return std::nullopt;
  return ds->has(varname);
}


// BUG-112(1) PIET-half: Stata's `generate` REFUSES a name that already exists
// (r(110) "variable X already defined"); `replace` is the verb that overwrites.
// PIET honoured that on exactly ONE of its four gen sinks — strmutate::
// runStringGen guards (datamgmt/string_generate_replace.cpp) — while the three NUMERIC sinks
// (handlers::runGen · egen::runBysortGen via the standalone `_n`/`_N` + `in`
// route · the same handler via the `bysort v:` prefixed route) all shipped
// straight to `cobra eval --gen`, whose Frame::generate is create-or-replace.
// Live-measured on this tree before the fix: `gen x = 2` over an existing x==1
// moved summarize's mean 1 -> 2 with NO diagnostic, and likewise `gen z = 77`
// over `gen z = _n` (3 -> 77) and `bysort x: gen y = 99` (3 -> 99).
//
// The guard lives HERE, at the VERB, not in the handlers: runGen/runBysortGen
// are also the internal engine of ~10 deliberately create-or-replace callers
// (reshape/egen/encode/bygen-tag/merge-cardinality/mkspline — and `replace`
// itself, which builds the identical `--gen` call), so a handler-level guard
// would reject all of them; selftest's direct piet::handlers::runGen() legs
// bypass dispatch() and stay untouched for the same reason. Verb-level also
// keeps the fix engine-INDEPENDENT — it takes no dependency on the cobra-half's
// `--gen-strict` flag (BUG-112(1) engine-half), so the Art-7 archive-pin arm
// behaves identically to a fresh build instead of failing on an unknown flag.
//
// Message spelling is strmutate's VERBATIM: errors.cpp::coarseRc keys r(110)
// off the "already defined" substring, so a `capture`-wrapped gen sets the same
// _rc Stata does, and the two sinks cannot drift apart in wording.
//
// BUG-1120: a reserved name is refused at the same point, for all four sinks
// alike, by generate's own rule (strutil::generateNameRefusal, with _se and the
// r(table) row names): `generate _cons = 1` is "_cons invalid varname" r(198),
// `generate _skip = 1` "_skip not allowed" r(101), `generate in = 1` "'=' invalid
// observation number" r(198), `generate double float = 1` "too few variables
// specified" r(102) (StataBE 19.5, typed or not, with `if`/`in` or not). They used
// to be created. `typeWord` is the storage type stripStorageType took off the
// line, as typed: when nothing stands between it and `=` (`generate byte = 2`),
// Stata read it as the type of a variable never named, r(102) -- this used to
// create a variable with an empty name. str, strl and str0 are names to Stata
// that the strip takes for types; those are left as they were.
std::optional<ojson> genTargetNameError(SessionState& session, const std::string& varname,
                                        const std::string& typeWord ) {
  std::optional<strutil::NameRefusal> refusal =
      strutil::generateNameRefusal(varname.empty() ? typeWord : varname, /*resultRows=*/true);
  if (refusal.has_value()) {
    return ojson(eval::errorBlock("engine_error", refusal->message, std::nullopt, std::nullopt,
                                  std::nullopt, "r(" + std::to_string(refusal->rc) + ")"));
  }
  if (varname.empty() || !session.hasData()) return std::nullopt;
  if (!targetVarExists(session, varname).value_or(false)) return std::nullopt;
  return ojson(eval::engineError("variable " + varname + " already defined"));
}


// BUG-1142 (FU-971): `replace` changes an EXISTING variable and never creates
// one, and Stata reads the target before anything else (StataBE 19.5, measured
// 2026-10-29): a storage type is r(101) "double not allowed"; a name that is no
// variable is r(111) "variable qq not found", with or without an `=exp`; a
// wildcard matching several is r(103) "too many variables specified"; an
// existing name with no `=exp` is r(100) "=exp required". Both sinks build the
// same create-or-replace `--gen` call, so `replace qq = 7` used to make qq at
// rc 0. The target may be an abbreviation or a one-match wildcard ([U] 11.2):
// StataBE rp.do `replace edu_category = 0 if edu_category==.` replaces
// edu_category_BL (007's Alt_main_results_alternate_controls.do), so
// `info.expr` is rewritten onto the full name. Other target shapes (`x[2]`,
// `x:lbl`, `x y`) are left to the sinks' own refusals.
std::optional<ojson> replaceTargetError(SessionState& session, StorageTypeInfo& info) {
  if (!info.typeToken.empty()) {
    return ojson(eval::errorBlock("syntax_error", info.typeToken + " not allowed", std::nullopt,
                                  std::nullopt, std::nullopt, "r(101)"));
  }
  if (!session.hasData()) return std::nullopt;
  const size_t eq = info.expr.find('=');
  const std::string target =
      trimWs(eq == std::string::npos ? info.expr : info.expr.substr(0, eq));
  if (target.empty() || !cecon::ident::isIdentStartByte(target.front()) ||
      target.find_first_of(" []():") != std::string::npos)
    return std::nullopt;
  std::vector<std::string> names = {target};
  if (auto err = handlers::resolveByKeyNames(session, names)) return err;
  if (names.size() > 1) {
    return ojson(eval::errorBlock("syntax_error", "too many variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(103)"));
  }
  if (names.empty() || !targetVarExists(session, names.front()).value_or(true)) {
    return ojson(eval::errorBlock("engine_error", "variable " + target + " not found",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  }
  if (eq == std::string::npos) {
    return ojson(eval::errorBlock("syntax_error", "=exp required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  }
  if (names.front() != target)
    info.expr = names.front() + " = " + trimWs(info.expr.substr(eq + 1));
  return std::nullopt;
}


// BUG-1144: replace's one option is a flag, given once. `replace f = 2.5,
// nopromote(1)` is r(198) "option nopromote() incorrectly specified" on StataBE
// 19.5 and f stays int, and `replace f = 1, nopromote nopromote` is r(198)
// "option nopromote not allowed" (measured 2026-10-29); kVocabReplace's net
// accepts the key whatever it carries and however often, and a valued key never
// set the flag, so f was promoted to float.
std::optional<ojson> replaceNoPromote(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool& noPromote) {
  bool seen = false;
  for (const auto& [k, v] : opts) {
    if (k != "nopromote") continue;
    if (!v.isBool) {
      return ojson(eval::errorBlock("syntax_error", "option nopromote() incorrectly specified",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    if (seen) {
      return ojson(eval::errorBlock("syntax_error", "option nopromote not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, "r(198)"));
    }
    seen = true;
    if (v.boolValue) noPromote = true;
  }
  return std::nullopt;
}


// `[type] NEWVAR = expr` (post-stripStorageType text) -> NEWVAR. First `=`
// wins, matching both the gen block's own inline split and datamgmt/data_generate.cpp::
// splitAssign. No `=` at all -> empty, i.e. NO exists-guard: that form is a
// syntax error the downstream handler already owns, and Stata likewise reports
// the syntax problem rather than the name one.
std::string genTargetName(const std::string& exprAfterType) {
  size_t eq = exprAfterType.find('=');
  if (eq == std::string::npos) return std::string();
  return trimWs(exprAfterType.substr(0, eq));
}


// The storage-type word stripStorageType took off `raw`, as the user typed it
// ("" when it took none): the strip folds case, and Stata's types do not.
std::string strippedTypeWord(const std::string& raw, const StorageTypeInfo& info) {
  if (info.typeToken.empty()) return std::string();
  std::istringstream iss(raw);
  std::string word;
  iss >> word;
  return word;
}


// `generate if = 1`, `clonevar if = x`: the tokenizer takes `if = 1` for the
// condition, and so does Stata, which then refuses the condition's first word,
// "=1 invalid name" r(198) (StataBE 19.5). Only a plain name or number is answered
// here; `generate if = (x+1)` is Stata's r(133) "unknown function =()" and keeps
// the engine's own refusal.
std::optional<ojson> conditionAsNameError(const std::optional<std::string>& ifExpr) {
  if (!ifExpr.has_value()) return std::nullopt;
  const std::string cond = trimWs(*ifExpr);
  if (cond.empty() || cond[0] != '=') return std::nullopt;
  std::istringstream iss(cond.substr(1));
  std::string word;
  iss >> word;
  if (word.empty()) return std::nullopt;
  for (char c : word)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return std::nullopt;
  return ojson(eval::errorBlock("syntax_error", "=" + word + " invalid name", std::nullopt,
                                std::nullopt, std::nullopt, "r(198)"));
}


StringInRangeFold foldStringInRange(const std::optional<std::string>& ifExpr,
                                    const std::optional<std::string>& inRange) {
  StringInRangeFold res;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveRange = inRange.has_value() && !inRange->empty();
  if (!haveRange) {
    res.ok = true;
    res.combinedIf = haveIf ? *ifExpr : std::string();
    return res;
  }
  auto parseBound = [](const std::string& raw) -> std::optional<std::string> {
    std::string t = trimWs(raw);
    std::string tl = lower(t);
    if (tl == "f") return std::string("1");
    if (tl == "l") return std::string("_N");
    if (t.empty()) return std::nullopt;
    if (t[0] == '-') return std::nullopt;  // negative — caller checks this case separately
    for (char c : t) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    return t;
  };
  std::string t = trimWs(*inRange);
  size_t slash = t.find('/');
  std::string p1 = slash == std::string::npos ? t : t.substr(0, slash);
  std::string p2 = slash == std::string::npos ? t : t.substr(slash + 1);
  if ((!p1.empty() && p1[0] == '-') || (!p2.empty() && p2[0] == '-')) {
    res.negative = true;
    return res;
  }
  auto a = parseBound(p1);
  auto b = parseBound(p2);
  if (!a.has_value() || !b.has_value()) {
    return res;  // ok=false, negative=false -> generic "unsupported form"
  }
  std::string rangePred = "_n>=" + *a + " & _n<=" + *b;
  res.combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  res.ok = true;
  return res;
}


// Turns a failed foldStringInRange() into the same honest-reject shape
// egen.cpp's runBysortGen already uses for the identical grammar, so a
// string-typed `gen`/`replace ... in ...` gets byte-identical wording to
// its numeric sibling.
ojson stringInRangeError(const StringInRangeFold& fold, const std::string& inRangeRaw) {
  if (fold.negative) {
    return ojson(eval::unsupportedOption(
        "in " + inRangeRaw,
        "Negative (from-the-end) `in` bounds are not supported yet. Use a positive 1-based "
        "row range, e.g. `in 1/10`, `in f/10`, `in 5/l`, `in f/l`."));
  }
  return ojson(eval::unsupportedOption(
      "in " + inRangeRaw,
      "This `in` row-range form is not supported for gen/replace. Supported forms: INT, "
      "INT/INT, f/INT, INT/l, f/l (f = first row, l = last row)."));
}


// A variable label keeps its first 80 characters, counted as UTF-8 characters
// (`label variable`, lb_var_long log:190; tabulate's generate() labels alike,
// StataBE 19.5: `s==` and 77 a's, or 77 é's in 157 bytes).
std::string variableLabelText(const std::string& text) {
  size_t chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (chars == 80) return text.substr(0, i);
    ++chars;
  }
  return text;
}


// BUG-456: gen/replace's own expression, resolved BEFORE strmutate::needsStringEval
// decides the route. `_b[]`/`_se[]`/e()/scalar() go to literals (model-gated
// for the coefficient half, BUG-432), then the constant string compares fold
// (`(e(cmd)=="regress")` -> `(1)`). `rhs` is re-derived so the detour test that
// follows sees the resolved text. Returns the error block for r(111)/r(109).
std::optional<ojson> preResolveGenExpr(std::string& expr, std::string& rhs,
                                              SessionState& session) {
  // BUG-1420: a c(NAME) that is no c() key is r(133) "c(NAME) undefined" here as
  // on display/local/scalar (StataBE 19.5: generate, replace and a string
  // generate alike); cobra read it as a call of an unknown function c(), rc 1.
  // The served names are substituted first, here rather than after the string
  // detour, which otherwise got c(os) raw: `generate str12 o = c(os)`,
  // `generate L = strlen(c(os))` and `replace s = c(os) in 1` failed at obs 1
  // where StataBE gives MacOSX, 6 and MacOSX.
  if (expr.find("c(") != std::string::npos) {
    expr = substituteCRefs(expr, session);
    if (auto undef = eval::undefinedCRef(expr))
      return ojson(eval::errorBlock("engine_error", *undef, std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(133)")));
  }
  {  // An impossible date literal is Stata's r(198) (w2y_fn_datetime fd_tq_bad,
     // fd_tm_bad, fd_td_bad: `generate double g = tq(2010q5)`). The engine refuses
     // it too, but as an internal "expr: ... invalid" error that reached the user
     // as rc 1; the string-path evaluator raised it at obs 1 with no code at all.
    // The text as Stata reads it (core/expr_blanks.h), so the message names the
    // literal as Stata prints it: tc(01jan201025:00:00) invalid. Brackets that
    // do not pair come first: `generate g = tq(2010q5) + (1` is r(132).
    if (const std::string u = cecon::bracket_imbalance(expr); !u.empty())
      return ojson(eval::errorBlock("engine_error", u, std::nullopt, std::nullopt, std::nullopt,
                                    std::string("r(132)")));
    std::string bad;
    cecon::for_each_datetime_literal(
        cecon::squeeze_bracketed_blanks(expr),
        [&](const std::string& name, const std::string& raw, size_t, size_t) {
          cecon::LiteralScale ls;
          cecon::literal_scale_for(name, ls);
          if (!std::isnan(cecon::stata_datetime_literal(raw, ls))) return true;
          bad = name + "(" + raw + ")";
          return false;
        });
    if (!bad.empty())
      return ojson(eval::errorBlock("engine_error", bad + " invalid", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
  }
  {  // e(sample) T2: the per-row mask, before the resolver's obs-1 arm can see it
    std::optional<ojson> esErr;
    expr = rewriteEsampleToken(expr, session, &esErr);
    if (esErr.has_value()) return esErr;
  }
  {  // BUG-1359: `_rc` on the right-hand side, before either route sees it
    const size_t eq = expr.find('=');
    if (eq != std::string::npos)
      expr = expr.substr(0, eq + 1) + substituteRcValue(expr.substr(eq + 1), session);
  }
  {  // BUG-1330: c() values too: the string route runs before the cobra route's
     // own substituteCRefs, so `gen str20 s = c(os) + "!"` failed at obs 1. A
     // c() nothing answers is r(133), not cobra's `function c() is not supported`.
    const size_t eq = expr.find('=');
    if (eq != std::string::npos) {
      const std::string sub = substituteCRefs(expr.substr(eq + 1), session);
      std::string cRc;
      if (auto cErr = unservedCRefError(sub, &cRc))
        return ojson(eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt,
                                      std::nullopt, "r(" + cRc + ")"));
      expr = expr.substr(0, eq + 1) + sub;
    }
  }
  // eb-order T-P3: no model gate, as in substituteRowExprRefs above -- the
  // resolver refuses a null coefficient source with r(111) itself.
  const bool wantsEst = expr.find("_b[") != std::string::npos ||
                        expr.find("_se[") != std::string::npos ||
                        expr.find("scalar(") != std::string::npos || eval::hasBareEParen(expr);
  if (wantsEst) {
    std::string bErr;
    expr = eval::resolveEstScalarRefs(expr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
      const std::string code = eval::splitErrCode(&bErr);
      return ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  // BUG-456 follow-up (review finding 3, 2026-09-09): gen's TYPE decision is
  // made on a COPY with the stored scalars substituted, because the real
  // substitution happens further down (after the strmutate detour has already
  // claimed anything holding a string literal) and gen must not be re-routed by
  // a scalar. Measured: `scalar one = 1` then `gen z = (e(cmd)==one)` was rc 1
  // "generate: expression evaluation failed at obs 1" with NO r() code, where
  // the identical `count if e(cmd)==one` reported r(109) -- the same claim,
  // two codes. Only the ERROR comes from this copy; `expr` is untouched.
  {
    std::string probe = substituteStoredScalars(expr, session);
    std::string badCall, probeMismatch;
    if (eval::stringFnNumericArg(probe, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
    if (!foldRowConstStringCompares(probe, &probeMismatch)) {
      return ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + probeMismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  {  // BUG-456 follow-up: the string-slot type test, ungated (see the row plane).
    std::string badCall;
    if (eval::stringFnNumericArg(expr, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  std::string mismatch;
  if (!foldRowConstStringCompares(expr, &mismatch)) {
    return ojson(eval::errorBlock("engine_error",
                                  "type mismatch: `" + mismatch + "` compares a string with a number",
                                  std::nullopt, std::nullopt, std::nullopt, "r(109)"));
  }
  const size_t eq = expr.find('=');
  rhs = (eq == std::string::npos) ? std::string() : trimWs(expr.substr(eq + 1));
  return std::nullopt;
}
}  // namespace piet::dispatch_detail

namespace piet {
using namespace dispatch_detail;
namespace handlers {

std::optional<ojson> resolveByKeyNames(SessionState& session, std::vector<std::string>& names) {
  if (names.empty() || !session.hasData()) return std::nullopt;
  bool patterned = false;
  for (const auto& t : names)
    if (t.find_first_of("*?-") != std::string::npos || lower(t) == "_all") patterned = true;
  std::vector<std::string> all;
  if (const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : *card) all.push_back(v.name);
  } else if (Datastore* ds = session.datastore()) {
    all = ds->names();
  }
  if (patterned) {
    loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(names), session);
    if (ex.isError) {
      // Review of e19291ad (REFUSAL rc): expandVarlist's range branch
      // (`zz-yy` with an endpoint that is no variable) returns a block with no
      // rc, which `capture` reads as 1. Stata answers r(111) "variable zz not
      // found". A block that already carries its rc passes through unchanged.
      const std::string reason = ex.error.value("reason", std::string());
      if (reason.compare(0, 2, "r(") == 0) return ex.error;
      std::string missing;
      for (const auto& t : names) {
        const size_t dash = t.find('-');
        if (dash == std::string::npos || t.find_first_of("*?") != std::string::npos) continue;
        for (const std::string& end : {t.substr(0, dash), t.substr(dash + 1)}) {
          if (missing.empty() && std::find(all.begin(), all.end(), end) == all.end()) missing = end;
        }
      }
      if (missing.empty()) missing = joinSpace(names);
      return ojson(eval::errorBlock("engine_error", "variable " + missing + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
    names = ex.tokens;
  }
  for (auto& t : names) {
    if (std::find(all.begin(), all.end(), t) != all.end()) continue;
    std::vector<std::string> hits;
    for (const auto& v : all)
      if (v.size() > t.size() && v.compare(0, t.size(), t) == 0) hits.push_back(v);
    if (hits.size() == 1) {
      t = hits.front();
    } else if (hits.size() > 1) {
      return ojson(eval::errorBlock("engine_error", t + " ambiguous abbreviation", std::nullopt,
                                    std::nullopt, std::nullopt, "r(111)"));
    }
  }
  return std::nullopt;
}

}  // namespace handlers
}  // namespace piet
