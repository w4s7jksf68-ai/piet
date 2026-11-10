// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `egen NEWVAR = fn(...) [, by(g) missing p(#) ...]` — C++ port of
// piet/backend/piet/egen.py (option-suffix rebuilding) + mutate.py's
// run_egen (the cobra --egen / --rowegen dispatch). M3b (ROADMAP "M3b
// scope" item 4 — bysort'd egen via bysort.py's by()-injection is the
// M3a-RED-cell fix; this file is the underlying `egen` command itself,
// which the bysort injection path also calls through dispatch()).
//
// Row functions (rowmean/rowsd/rowtotal/rowmax/rowmin/rowmiss/rownonmiss)
// route to `cobra eval --rowegen NEWVAR=fn(a,b,c)` (comma-separated args,
// converted from the space-separated Stata varlist form). Every other
// function (the original 8 stat fns mean/sum/total/count/min/max/median/sd
// plus the cobra 0.13.0 extended set tag/group/seq/rank/pctile/std/iqr)
// routes to `cobra eval --egen NEWVAR=fn(expr)[,opts][, by(g1,g2)]`.
#include "piet/handlers/data_values.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum
#include "piet/translate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"  // isIdentByte: cut()'s argument
#include "data/missing.h"  // missing_kind: string() of a .a cell
#include "io/win_compat.h"  // to_native_path, native_exists, native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // cut's value label and the any* note are ordinary commands
#include "piet/display_hardening.h"  // exprUnboundNameRefusal: cut()'s =/exp
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // BUG-1001: expandVarlist, the one varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "egen_internal.h"

namespace {
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — egen's option vocabulary. Union of the
// base set and every per-function conditional key (see runEgen); registered so
// dispatcher.cpp's single resolution point can canonicalize an abbreviation
// BEFORE runEgen's own `k == "by"` / `k == "missing"` comparisons run.
const piet::eval::OptVocab& kVocabEgen = piet::eval::registerOptVocab(
    // The UNION across egen functions (BUG-233's rule): the dispatcher
    // canonicalizes an abbreviation against this set, and the per-function
    // check inside runEgen is what actually refuses a wrong pairing.
    // BUG-1001: the per-function sets of egenFnVocab() below join the union. egen
    // is in kOptMinComplete (generated/opt_minimums_bug264.inc), so the dispatcher
    // resolves no prefix against this union; runEgen resolves every key against the
    // function's OWN declared minimums.
    // values/punct/format/at/group/icodes/label: the options of the functions
    // runEgenAdoFunction computes.
    {"egen"}, {{"by", "missing", "p", "from", "f", "to", "t", "block", "b", "minmode",
                "maxmode", "nummode", "field", "track", "unique", "prop", "mean", "sd",
                "std", "strok", "values", "punct", "format", "at", "group", "icodes", "label"},
               {"autotype", "suffix", "decode", "maxlength"}});
}  // namespace

namespace piet::handlers {

using namespace egen_detail;

namespace {


// egen.py's _ROW_FUNS (mutate.py:187) — functions that route to --rowegen
// instead of --eigen. The Stata 8 names (rsum, rmax, ...) arrive renamed
// (rowAliasOf).
const std::set<std::string>& rowFuns() {
  static const std::set<std::string> s = {"rowmean", "rowsd",   "rowtotal",  "rowmax",
                                          "rowmin",  "rowmiss", "rownonmiss"};
  return s;
}

// The Stata 8 row-function names are aliases: each behaves as its Stata 9
// function on the same arguments, so the values, options and refusals are that
// function's, reported under the new name (`egen ... rowtotal() may not be
// combined with by` for rsum; BUG-1001, BUG-1112, StataBE 19.5). "" for any other
// name.
std::string rowAliasOf(const std::string& fn) {
  static const std::map<std::string, std::string> t = {
      {"rsum", "rowtotal"}, {"rmax", "rowmax"},   {"rmin", "rowmin"},    {"rmean", "rowmean"},
      {"rmiss", "rowmiss"}, {"robs", "rownonmiss"}, {"rsd", "rowsd"}};
  auto it = t.find(fn);
  return it == t.end() ? std::string() : it->second;
}

// Stata's egen functions that this build does not compute (they ship with Stata):
// named as unimplemented rather than "unknown", which would be false. Each row
// carries the options Stata accepts besides by(), which are read before the
// computation would start (one outside them is r(198) "option ... not allowed",
// StataBE 19.5 `egen q = mdev(k), bar`), and, for a function that refuses a
// by-list, the name that refusal reports (an old name reports its row*/any*
// successor: `egen q = robs(x k), by(k)` is "egen ... rownonmiss() may not be
// combined with by"; measured, StataBE oracle arms E_*_by).
struct UnimplementedEgenFn {
  std::vector<EgenOptSpec> opts;
  const char* noByName;  // nullptr: the function takes by()
};
const std::map<std::string, UnimplementedEgenFn>& unimplementedEgenFns() {
  static const std::map<std::string, UnimplementedEgenFn> t = {
      {"any", {{{"values", 1, true}}, "anyvalue"}},     // old name of anyvalue
      {"diff", {{}, "diff"}},
      {"ends",
       {{{"punct", 1, true}, {"head", 1, false}, {"last", 1, false}, {"tail", 1, false},
         {"trim", 2, false}},
        "ends"}},
      {"eqany", {{{"values", 1, true}}, "anymatch"}},   // old name of anymatch
      {"fill", {{}, "fill"}},
      {"kurt", {{}, nullptr}},
      {"ma", {{{"t", 1, true}, {"nomiss", 3, false}}, "ma"}},
      {"mad", {{}, nullptr}},
      {"mdev", {{}, nullptr}},
      {"mtr", {{}, "mtr"}},
      {"neqany", {{{"values", 1, true}}, "anycount"}},  // old name of anycount
      {"rfirst", {{}, "rowfirst"}},                     // old name of rowfirst
      {"rlast", {{}, "rowlast"}},                       // old name of rowlast
      {"rowfirst", {{}, "rowfirst"}},
      {"rowlast", {{}, "rowlast"}},
      {"rowmedian", {{}, "rowmedian"}},
      {"rowpctile", {{{"p", 1, true}}, "rowpctile"}},
      {"skew", {{}, nullptr}},
  };
  return t;
}

bool stataEgenFunctionUnimplemented(const std::string& fn) {
  return unimplementedEgenFns().count(fn) != 0;
}

// What a function's ado `syntax` binds first, before any option: a varlist or an
// =/exp. _gcount's `anything`, seq()'s empty argument, fill()'s numlist and mtr()'s
// own parser bind nothing there.
enum class EgenArgKind { kNone, kVarlist, kExp };
EgenArgKind egenArgKind(const std::string& fn) {
  static const std::set<std::string> varlist = {
      "group", "tag",   "concat", "anyvalue", "anycount", "anymatch", "mode",   "any",
      "diff",  "ends",  "eqany",  "neqany",   "rfirst",   "rlast",    "rmax",   "rmean",
      "rmin",  "rmiss", "robs",   "rowfirst", "rowlast",  "rowmedian", "rowpctile", "rsd"};
  static const std::set<std::string> exp = {"mean",   "total", "sum",  "sd",   "max",  "min",
                                            "median", "pctile", "iqr", "rank", "std",  "pc",
                                            "cut",    "mdev",  "kurt", "skew", "mad",  "ma"};
  if (rowFuns().count(fn) != 0 || varlist.count(fn) != 0) return EgenArgKind::kVarlist;
  if (exp.count(fn) != 0) return EgenArgKind::kExp;
  return EgenArgKind::kNone;
}

// egen's ado: a function with no _g<fn>.ado is r(133) (eg_ref_unknown); one
// whose ado ships but this build does not compute is named as unimplemented.
ojson unknownEgenFunctionError(const std::string& fn) {
  if (stataEgenFunctionUnimplemented(fn))
    return ojson(eval::errorBlock(
        "unsupported_command",
        "egen function " + fn + "() is a valid Stata function that this version "
        "of PIET does not yet implement.", fn));
  return egenRcError("unknown egen function " + fn + "()", 133);
}

// Option `full` of the function's ado, which this build does not compute: refused
// by name, never accepted and dropped.
ojson egenUnimplementedOption(const char* full) {
  return ojson(eval::engineError("Option `" + std::string(full) + "` is a valid Stata option that "
                                 "this version of PIET does not yet implement.",
                                 "unsupported_option"));
}

// egen.ado looks the function up (r(133) when no ado ships) and hands the argument
// to the function's ado, whose `syntax` binds it before any option, a by-list or
// the computation: a varlist's names ("variable V not found" r(111); no varlist
// there takes an operator, r(101)) or an =/exp's ("V not found" r(111)). StataBE
// 19.5: `egen q = group(x nosuch), by(k) bogus`, `egen q = mean(nosuch), bogus`
// -> r(111); `egen q = rowmean(L.x x)`, `egen q = group(i.k)` -> r(101). An =/exp
// with a time-series operator or a value-label literal is left to the evaluator.
std::optional<ojson> egenArgumentRefusal(const std::string& fn, const std::string& args,
                                         SessionState& session) {
  if (args.empty()) return std::nullopt;
  const EgenArgKind kind = egenArgKind(fn);
  if (kind == EgenArgKind::kVarlist) {
    // `egen q = rowmean(x, t)` -> "invalid syntax" r(198).
    if (args.find(',') != std::string::npos) return egenRcError("invalid syntax", 198);
    std::istringstream in(args);
    std::string tok;
    while (in >> tok)
      if (tok.find('.') != std::string::npos)
        return egenRcError("factor-variable and time-series operators not allowed", 101);
    loops::VarlistExpandResult vx = loops::expandVarlist(args, session);
    if (vx.isError) return vx.error;
    return loops::checkKnownVarlist(vx.tokens, session);
  }
  if (kind == EgenArgKind::kExp && !translate::exprHasTsToken(args) &&
      args.find("\":") == std::string::npos) {
    if (auto unbound = exprUnboundNameRefusal(args, session))  // "r(111): ..."
      return egenRcError(unbound->substr(unbound->find(": ") + 2), 111);
  }
  return std::nullopt;
}

// egen's ado `confirm new variable`: a name already in the dataset is r(110).
bool egenTargetExists(SessionState& session, const std::string& name) {
  if (name.empty() || !session.hasData()) return false;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card)
      if (vi.name == name) return true;
    return false;
  }
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// trim() wrapper above for why this stays a thin pass-through.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// egen.py's build_opts_suffix: opts dict (excluding `by`) -> cobra-syntax
// suffix, e.g. {"p":"90"} -> ",p(90)"; {"missing":true} -> ",missing".
// Preserves ParsedCommand::opts insertion order (parity with Python dict
// iteration order — both reflect tokenizer left-to-right option order).
std::string buildOptsSuffix(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                            const std::set<std::string>& exclude) {
  std::string suffix;
  bool any = false;
  for (const auto& [key, val] : opts) {
    if (exclude.count(key)) continue;
    if (val.isBool) {
      if (!val.boolValue) continue;  // Python: `val is True` only
      suffix += (any ? "," : ",") + key;
      any = true;
    } else if (!val.strValue.empty()) {
      suffix += "," + key + "(" + val.strValue + ")";
      any = true;
    }
  }
  return suffix;
}

// str# of a string column's longest value (str1 when every value is "", strL
// past str2045), the type `compress` and a widening replace leave.
std::string strTypeOfLongest(const Datastore& ds, const std::string& var) {
  const cecon::Column& c = ds.col(var);
  size_t longest = 0;
  for (int64_t i = 0; i < ds.nRows(); ++i) {
    const int32_t k = c.codes()[i];
    if (k >= 0) longest = std::max(longest, c.levels[static_cast<size_t>(k)].size());
  }
  if (longest > 2045) return "strL";
  return "str" + std::to_string(std::max<size_t>(1, longest));
}

}  // namespace

namespace egen_detail {


// BUG-042 (egen if-wiring): `cobra merge`'s success path prints a plain-text
// status line to stderr (not JSON), so `runCobraJson`'s "parse stdout as
// JSON" contract doesn't fit — this needs the raw `runCobra()` + a manual
// CobraError build from stderr, same as datamgmt_support.h's own
// `errorFromRaw` (file-local copy per this codebase's established
// one-copy-per-TU convention for exactly this helper).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

const std::map<std::string, EgenFnVocab>& egenFnVocab() {
  static const std::map<std::string, EgenFnVocab> t = {
      {"count", {{}, {}}},
      {"sum", {{{"missing", 1, false}}, {}}},                        // sum is total
      {"total", {{{"missing", 1, false}}, {}}},
      {"min", {{}, {{"missing", 4, false}}}},
      {"max", {{}, {{"missing", 4, false}}}},
      {"median", {{}, {{"autotype", 8, false}}}},
      {"iqr", {{}, {{"autotype", 8, false}}}},
      {"pctile", {{{"p", 1, true}}, {{"autotype", 8, false}}}},
      {"sd", {{}, {}}},
      {"std", {{{"mean", 1, true}, {"sd", 1, true}, {"std", 2, true}}, {}}},
      {"rank",
       {{{"field", 1, false}, {"track", 1, false}, {"unique", 1, false}},
        {{"suffix", 1, true}}}},
      {"pc", {{{"prop", 4, false}}, {}}},
      {"seq", {{{"block", 1, true}, {"from", 1, true}, {"to", 1, true}}, {}}},
      {"mode",
       {{{"missing", 4, false}, {"minmode", 3, false}, {"maxmode", 3, false},
         {"nummode", 1, true}},
        {}}},
      {"rowmax", {{}, {}}},        {"rowmin", {{}, {}}},
      {"rowmiss", {{}, {}}},       {"rowsd", {{}, {}}},
      {"rowmean", {{}, {}}},
      {"rownonmiss", {{{"strok", 1, false}}, {}}},
      {"rowtotal", {{{"missing", 1, false}}, {}}},
      // cut: at(numlist, ascending) | group(#), icodes, label (help egen; `a()` and
      // `i` are r(198), `ic`, `g()` and `l` run). runEgenAdoFunction computes it.
      {"cut",
       {{{"at", 2, true}, {"group", 1, true}, {"icodes", 2, false}, {"label", 1, false}}, {}}},
      // concat: punct(), decode, maxlength(), format() (help egen; `max()` is
      // r(198), `maxl()` runs); punct() and format() run in runEgenAdoFunction.
      {"concat",
       {{{"punct", 1, true}, {"format", 1, true}},
        {{"decode", 1, false}, {"maxlength", 4, true}}}},
      // values(): anyvalue keeps its text, anycount/anymatch read an integer numlist
      // (help egen); all three run in runEgenAdoFunction.
      {"anyvalue", {{{"values", 1, true}}, {}}},
      {"anycount", {{{"values", 1, true}}, {}}},
      {"anymatch", {{{"values", 1, true}}, {}}},
  };
  return t;
}

// The spec `key` names under Stata's rule: the full name, or a prefix of it at
// least minLen long. nullptr when none does.
const EgenOptSpec* matchEgenOpt(const std::string& key, const std::vector<EgenOptSpec>& specs) {
  for (const auto& sp : specs) {
    const std::string full(sp.full);
    if (key.size() >= sp.minLen && key.size() <= full.size() &&
        full.compare(0, key.size(), key) == 0)
      return &sp;
  }
  return nullptr;
}

// An error block carrying Stata's exact return code (coarseRc reads `reason`).
ojson egenRcError(const std::string& message, int rc, const std::string& code) {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// One function's option net (egenFnVocab): every key but by() is resolved under
// the ado's own minimums, and `canonical` (when given) receives it under its full
// name. An option the ado does not declare, or a valued one written bare (or the
// reverse), is Stata's r(198); one it declares and this build does not compute is
// held in `unimplemented` for the caller to refuse after the by-list.
std::optional<ojson> egenOptionNet(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const EgenFnVocab& vocab,
                                   std::vector<std::pair<std::string, eval::OptValue>>* canonical,
                                   std::optional<ojson>* unimplemented,
                                   bool missingInFull) {
  for (const auto& [k, v] : opts) {
    if (k == "by") continue;
    const EgenOptSpec* sp = matchEgenOpt(k, vocab.known);
    // `missingInFull`: gegen's own total()/sum() (BUG-1111, at the call).
    if (missingInFull && sp != nullptr && std::string(sp->full) == "missing" && k != "missing")
      sp = nullptr;
    if (sp == nullptr) {
      if (const EgenOptSpec* un = matchEgenOpt(k, vocab.unimplemented)) {
        if (!unimplemented->has_value()) *unimplemented = egenUnimplementedOption(un->full);
        continue;
      }
      return ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option"));
    }
    if (sp->valued == v.isBool)
      return ojson(eval::engineError("option `" + k + "` incorrectly specified",
                                     "unsupported_option"));
    if (canonical != nullptr) canonical->emplace_back(sp->full, v);
  }
  return std::nullopt;
}

}  // namespace egen_detail

ojson runEgen(const std::string& exprIn, const std::string& by,
              const std::optional<std::string>& ifExpr,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session, const std::optional<std::string>& inRange,
              const std::string& storeTypeIn, std::string* fixedTypeOut, bool gtools) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string expr = trim(exprIn);
  std::string outPath = nextPath(session.workdir(), "egen");

  auto eqPos = expr.find('=');
  std::string newvar = eqPos == std::string::npos ? "" : trim(expr.substr(0, eqPos));
  std::string rhs = eqPos == std::string::npos ? expr : trim(expr.substr(eqPos + 1));
  {
    static const std::regex reHead(R"(^(\w+)(\s*\(.*)$)");
    std::smatch hm;
    if (std::regex_match(rhs, hm, reHead)) {
      // egen function names are case-sensitive: `Rmax` looks for _gRmax.ado and
      // is r(199) in Stata, so the name is matched as typed.
      const std::string alias = rowAliasOf(hm.str(1));
      if (!alias.empty()) {
        rhs = alias + hm.str(2);
        expr = newvar.empty() ? rhs : newvar + " = " + rhs;
      }
    }
  }

  // BUG-1120: `egen _cons = mean(x)` is "_cons invalid varname" r(198), and so are
  // the storage types and if/in/using/with (`egen long = mean(x)`); a str# is
  // "str5 invalid variable name" (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return egenRcError(r->message, r->rc);
  // BUG-1000: egen's ado runs `confirm new variable` before anything else, so an
  // existing target is r(110) and is never overwritten (the engine's column store
  // is create-or-replace: `egen x = sum(xd)` replaced x at rc 0). Oracle
  // w2ap_egen_family eg_ref_exist (rc 110, x untouched).
  if (egenTargetExists(session, newvar))
    return egenRcError("variable " + newvar + " already defined", 110);

  {
    static const std::regex reFnArgs(R"(^(\w+)\s*\((.*)\)\s*$)");
    std::smatch fa;
    if (std::regex_match(rhs, fa, reFnArgs)) {
      const std::string fn = lower(fa.str(1));
      if (auto bad = egenArgumentRefusal(fn, trim(fa.str(2)), session)) return *bad;
      // anyvalue/anycount/anymatch, cut(), at()/group() and concat(), punct()/
      // format(): computed in runEgenAdoFunction (aer_w2 w2b_dataio
      // egen_anyvalue .. egen_cut_icodes_label, w2b_dataio.log:1030-1112).
      if (auto r = runEgenAdoFunction(fn, newvar, fa.str(2), opts, ifExpr, inRange, storeTypeIn,
                                      session)) {
        // The function recorded the result's type; that, not the declaration, is
        // what the dispatcher records (BUG-1108).
        const bool failed = r->is_object() && r->value("type", std::string()) == "error";
        auto vm = session.varMeta.find(newvar);
        if (!failed && fixedTypeOut != nullptr && vm != session.varMeta.end() &&
            vm->second.stataType.has_value())
          *fixedTypeOut = *vm->second.stataType;
        return *r;
      }
    }
  }

  // BUG-1001: the function and its own option vocabulary (egenFnVocab). Every key
  // is resolved against the function's declared minimums, and the forwarded suffix
  // carries the CANONICAL names, which are the words the engine reads. egen is in
  // the dispatcher's kOptMinComplete, so the keys arrive unresolved except for
  // the measured `missing` row; the resolution below is the only one.
  std::string fnName;
  {
    static const std::regex reFnName(R"(^(\w+)\s*\()");
    std::smatch fm;
    if (std::regex_search(rhs, fm, reFnName)) fnName = lower(fm.str(1));
  }
  const bool haveBy = !trim(by).empty();
  const bool isRowFn = rowFuns().count(fnName) != 0;
  std::vector<std::pair<std::string, eval::OptValue>> fnOpts;  // canonical, by() excluded
  // An option the ado takes and this build does not compute: refused after the
  // by-list below, which the ado's `syntax` lets it reach (BUG-1106: `egen v =
  // concat(x k), by(k) decode` is r(190) in StataBE 19.5).
  std::optional<ojson> unimplementedOpt;
  const auto vocabIt = egenFnVocab().find(fnName);
  const auto unimplementedIt = unimplementedEgenFns().find(fnName);
  if (vocabIt != egenFnVocab().end()) {
    // gtools' gegen computes total() and sum() itself and takes `missing` only
    // in full: `gegen q = total(a), m` / `, mi` is `option m not allowed`
    // r(198) with gtools installed, where egen's _gtotal reads m (BUG-1111).
    if (auto err = egenOptionNet(opts, vocabIt->second, &fnOpts, &unimplementedOpt,
                                 gtools && (fnName == "total" || fnName == "sum")))
      return *err;
  } else if (fnName == "mean" || fnName == "var" || fnName == "group" || fnName == "tag" ||
             fnName == "cut" || fnName == "nvals") {
    // Engine functions outside BUG-1001's rows keep the net they had: by() for every
    // one, plus `missing` for group (BUG-233).
    std::unordered_set<std::string> known = {"by"};
    std::vector<std::pair<std::string, eval::OptValue>> netOpts = opts;
    if (fnName == "group") {
      // egen group() `Missing`: m, mi, ... are missing (BUG-1111). The key
      // arrives as typed (the dispatcher leaves egen's keys to this function).
      // gtools' gegen takes `missing` only in full (`gegen q = group(a), m` is
      // `option m not allowed` r(198) with gtools installed).
      static const std::vector<EgenOptSpec> kMissing = {{"missing", 1, false}};
      for (auto& kv : netOpts) {
        if (kv.first == "missing" || matchEgenOpt(kv.first, kMissing) == nullptr) continue;
        if (gtools)
          if (auto err = eval::rejectUnknownOptions({kv}, {})) return *err;
        kv.first = "missing";
      }
      known.insert("missing");
      // `label` (egen group()): the groups' value label, built below from the
      // grouping variables' values (aer_w2 w2b_dataio egen_group_label `1
      // zero`, w2b_dataio.log:1076). It is PIET-side, never forwarded.
      known.insert("label");
    }
    // What egen group() and egen tag() also declare, and this build does not
    // compute, is refused after the by-list (BUG-1106: `egen g = tag(x), by(k)
    // missing`, `egen g = group(x), by(k) lname(foo)` are r(190) in StataBE 19.5).
    static const std::vector<EgenOptSpec> kGroupUnimplemented = {
        {"lname", 2, true}, {"truncate", 1, true}, {"autotype", 8, false},
        {"qlabel", 6, false}, {"nsort", 5, true}, {"debug", 5, false}};
    static const std::vector<EgenOptSpec> kTagUnimplemented = {{"missing", 1, false}};
    std::vector<std::pair<std::string, eval::OptValue>> netted;
    for (const auto& kv : netOpts) {
      const EgenOptSpec* un = nullptr;
      if (known.count(kv.first) == 0 && fnName == "group")
        un = matchEgenOpt(kv.first, kGroupUnimplemented);
      else if (known.count(kv.first) == 0 && fnName == "tag")
        un = matchEgenOpt(kv.first, kTagUnimplemented);
      if (un == nullptr) {
        netted.push_back(kv);
      } else if (!unimplementedOpt.has_value()) {
        unimplementedOpt = egenUnimplementedOption(un->full);
      }
    }
    if (auto err = eval::rejectUnknownOptions(netted, known)) return *err;
    for (const auto& kv : netted)
      if (kv.first != "by" && kv.first != "label") fnOpts.push_back(kv);
  } else if (unimplementedIt != unimplementedEgenFns().end()) {
    // Its ado ships, and its own `syntax` reads the options: one it does not
    // declare is r(198) (BUG-1110 follow-up; StataBE 19.5 `egen q = mdev(k), bar`,
    // `egen q = rowmedian(k x), by(x) bogus`).
    for (const auto& kv : opts)
      if (kv.first != "by" && matchEgenOpt(kv.first, unimplementedIt->second.opts) == nullptr)
        return ojson(eval::engineError("option `" + kv.first + "` not allowed",
                                       "unsupported_option"));
  } else {
    // A name no table above lists is not an egen function this build computes,
    // and an option on it must not ride along unread if the engine ever learns
    // the name. egen.ado looks the function up before its ado reads any option,
    // so the answer is the lookup's (BUG-1110: `egen q = nosuchfn(k), bar` is
    // r(133) "unknown egen function nosuchfn()" in StataBE 19.5, never the
    // option's r(198)).
    for (const auto& kv : opts)
      if (kv.first != "by") return unknownEgenFunctionError(fnName);
  }
  // _egennoby (every row function's ado, _ggroup, _gtag, _gconcat, _gcut and the
  // ados this build does not compute but noByName): a by-list, as an option or as
  // a by prefix the dispatcher turned into one, is r(190). Each ado reads its
  // `syntax` first, so the argument (egenArgumentRefusal) and an option it does
  // not take (the net above) answer before it. Oracle eg_ref_row_by; BUG-1106,
  // StataBE 19.5: `bysort k: egen g = tag(x)`, `egen g = group(x), by(k)`, `egen v
  // = concat(x k), by(k)`, `egen q = cut(x), by(k)` -> "egen ... tag() may not be
  // combined with by" r(190); `egen g = group(x), by(k) bogus` -> r(198); `egen g
  // = group(x nosuch), by(k) bogus` -> "variable nosuch not found" r(111).
  if (haveBy) {
    std::string noBy;
    if (isRowFn || fnName == "group" || fnName == "tag" || fnName == "concat" || fnName == "cut")
      noBy = fnName == "rsum" ? std::string("rowtotal") : fnName;  // _grsum -> _growtotal
    else if (unimplementedIt != unimplementedEgenFns().end() &&
             unimplementedIt->second.noByName != nullptr)
      noBy = unimplementedIt->second.noByName;
    if (!noBy.empty())
      return egenRcError("egen ... " + noBy + "() may not be combined with by", 190);
  }
  if (unimplementedOpt.has_value()) return *unimplementedOpt;
  if (unimplementedIt != unimplementedEgenFns().end()) return unknownEgenFunctionError(fnName);
  auto fnOpt = [&](const char* key) -> const eval::OptValue* {
    for (const auto& kv : fnOpts)
      if (kv.first == key) return &kv.second;
    return nullptr;
  };
  // opts_exclusive in egen rank() and egen mode(): r(198), naming the first two.
  auto exclusive = [&](std::initializer_list<const char*> keys) -> std::optional<ojson> {
    std::vector<std::string> given;
    for (const char* k : keys)
      if (fnOpt(k) != nullptr)
        given.push_back(std::string(k) + (std::string(k) == "nummode" ? "()" : ""));
    if (given.size() < 2) return std::nullopt;
    return egenRcError("only one of options " + given[0] + " or " + given[1] + " is allowed", 198);
  };
  if (fnName == "rank") {
    if (auto e = exclusive({"field", "track", "unique"})) return *e;  // oracle eg_ref_rank_two
  } else if (fnName == "mode") {
    if (auto e = exclusive({"minmode", "maxmode", "nummode"})) return *e;  // eg_ref_mode_two
    if (const eval::OptValue* nm = fnOpt("nummode")) {
      // Nummode(numlist max=1 int >= 1): anything else is r(125)/r(126).
      const std::string t = trim(nm->strValue);
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos ||
          std::strtol(t.c_str(), nullptr, 10) < 1)
        return egenRcError("nummode(): invalid numlist has elements outside of allowed range",
                           125);
    }
  } else if (fnName == "std") {
    // egen std(): sd() and its old spelling std() together are r(198); the
    // engine reads the one value as sd().
    if (fnOpt("sd") != nullptr && fnOpt("std") != nullptr)
      return egenRcError("only one of sd() or std() is allowed", 198);
    for (auto& kv : fnOpts)
      if (kv.first == "std") kv.first = "sd";
    if (const eval::OptValue* sv = fnOpt("sd")) {
      // Sd(numlist max=1 >0).
      char* end = nullptr;
      const std::string t = trim(sv->strValue);
      const double d = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(d > 0.0))
        return egenRcError("sd(): invalid numlist has elements outside of allowed range", 125);
    }
    if (const eval::OptValue* mv = fnOpt("mean")) {
      char* end = nullptr;
      const std::string t = trim(mv->strValue);
      (void)std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0')
        // Named as typed, as syntax does (StataBE 19.5: `std(x), m(abc)` is
        // `option m() incorrectly specified`).
        for (const auto& [k, v] : opts) {
          const EgenOptSpec* sp = k == "by" ? nullptr : matchEgenOpt(k, vocabIt->second.known);
          if (sp != nullptr && std::string(sp->full) == "mean")
            return egenRcError("option " + k + "() incorrectly specified", 198);
        }
    }
  } else if (fnName == "pctile") {
    // egen pctile(): P(real 50), and outside (0, 100) is r(198).
    if (const eval::OptValue* pv = fnOpt("p")) {
      char* end = nullptr;
      const std::string t = trim(pv->strValue);
      const double d = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0')
        return egenRcError("option p() incorrectly specified", 198);
      if (!(d > 0.0 && d < 100.0))
        return egenRcError("p(" + t + ") must be between 0 and 100", 198);
    }
  } else if (fnName == "cut") {
    // at() is a numlist: Stata reads `0 10 20`, `0,10,20` and `0(10)70` alike
    // (w2f_residuals.log cut_space/cut_comma/cut_rangelist/cut_range all
    // `0|10|30`). The engine reads a blank-separated list, so commas become
    // blanks and an INTEGER range a(step)b or a/b is expanded here; a
    // fractional range stays as typed for the engine's own refusal (Stata
    // builds those edges in float, which a double expansion would not match).
    for (auto& kv : fnOpts) {
      if (kv.first != "at" || kv.second.isBool) continue;
      std::string t = kv.second.strValue;
      std::replace(t.begin(), t.end(), ',', ' ');
      static const std::regex reStep(R"(^(-?\d+)\((-?\d+)\)(-?\d+)$)");
      static const std::regex reSlash(R"(^(-?\d+)/(-?\d+)$)");
      std::string out;
      std::istringstream in(t);
      std::string tok;
      while (in >> tok) {
        std::smatch m;
        long long a = 0, st = 0, b = 0;
        bool range = false;
        if (std::regex_match(tok, m, reStep)) {
          a = std::stoll(m.str(1));
          st = std::stoll(m.str(2));
          b = std::stoll(m.str(3));
          range = st != 0 && ((st > 0 && a <= b) || (st < 0 && a >= b));
        } else if (std::regex_match(tok, m, reSlash)) {
          a = std::stoll(m.str(1));
          b = std::stoll(m.str(2));
          st = a <= b ? 1 : -1;
          range = true;
        }
        if (range && (b - a) / st < 100000) {
          for (long long v = a; st > 0 ? v <= b : v >= b; v += st)
            out += (out.empty() ? "" : " ") + std::to_string(v);
        } else {
          out += (out.empty() ? "" : " ") + tok;
        }
      }
      kv.second.strValue = out;
    }
  } else if (fnName == "seq") {
    // from() and block() take a number that is a whole number within int's
    // range, and refuse anything else -- the first such option as typed --
    // before anything is computed (BUG-1114, StataBE 19.5:
    // 2.0, +2, 1e1 and -2 run; 1.5, abc, an empty from(), 1e10 and 0x1p1 are
    // `option from() incorrectly specified` r(198)).
    // The message names the option as typed (`seq(), f(1.5)` is `option f()
    // incorrectly specified`), so the typed keys are walked, not fnOpts' canonical
    // ones; both keep the typed order.
    static const std::regex reNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
    for (const auto& [k, v] : opts) {
      const EgenOptSpec* sp = k == "by" ? nullptr : matchEgenOpt(k, vocabIt->second.known);
      if (sp == nullptr || (std::string(sp->full) != "from" && std::string(sp->full) != "block"))
        continue;
      const std::string t = trim(v.strValue);
      const double d = std::regex_match(t, reNum) ? std::strtod(t.c_str(), nullptr) : std::nan("");
      if (!(d == std::trunc(d) && d >= -2147483648.0 && d <= 2147483647.0))
        return egenRcError("option " + k + "() incorrectly specified", 198);
    }
    // `block(0)` must be at least 1, r(498) (eg_ref_seq_block).
    if (const eval::OptValue* bv = fnOpt("block")) {
      const std::string t = trim(bv->strValue);
      if (std::strtod(t.c_str(), nullptr) < 1.0)
        return egenRcError("block(" + t + ") must be at least 1", 498);
    }
    // A given to() must be written as an integer, which reads the text: a
    // sign and digits (+3, 03 and -1 run), else `'2.5' found where integer
    // expected` r(7) (3.0, 1e1 and . alike), and a second word after one is
    // `'4' found where nothing expected` r(7).
    if (const eval::OptValue* tv = fnOpt("to")) {
      const std::string t = trim(tv->strValue);
      const size_t cut = t.find_first_of(" \t,");
      const std::string first = t.substr(0, cut);
      static const std::regex reInt(R"(^[+-]?\d+$)");
      if (!t.empty() && !std::regex_match(first, reInt))
        return egenRcError("'" + first + "' found where integer expected", 7);
      if (cut != std::string::npos) {
        const std::string rest = trim(t.substr(cut));
        const std::string next = rest[0] == ',' ? std::string(",")
                                                : rest.substr(0, rest.find_first_of(" \t,"));
        return egenRcError("'" + next + "' found where nothing expected", 7);
      }
    }
  }

  // BUG-471 T2: the result's storage type rides the ONE call that computes it
  // (never the if/in plumbing calls). tag()/seq()/mode() ignore the requested
  // type in Stata (DIAGNOSIS section B15-B17), so they get no flag at all. The
  // replace-built results (rowtotal/rowmax/rowmin, default rank, cut with a
  // fractional edge) widen an integer type in Stata; the engine refuses that
  // with bad_input, and the refusal is surfaced as an r(198) option refusal —
  // never retried without the flag, which would store a double under an
  // integer label.
  const std::string egenFn = storage::egenFunctionName(rhs);
  const std::string storeType = storage::egenIgnoresStoreType(egenFn) ? std::string() : storeTypeIn;
  auto egenStoreError = [&](const CobraError& ce) -> ojson {
    if (storage::isIntegerType(storeType) && ce.code == "bad_input" &&
        std::string(ce.what()).find("--store-type") != std::string::npos) {
      return ojson(eval::unsupportedOption(
          storeType, "egen " + egenFn + "() builds its result with replace, which widens a " +
                         storeType + " in Stata; PIET cannot widen it yet. Declare the result " +
                         "float or double instead."));
    }
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  };

  // BUG-042: `egen NEWVAR = fn(...) if COND` — Stata restricts BOTH the
  // computation (fn() sees only the matching rows: e.g. mean() is the
  // subset's mean, group()/tag() only consider the subset's distinct
  // values) AND the assignment (non-matching rows come back missing) to
  // the if-subset. Mechanical fix (row-filter infra reuse, mirroring
  // summarize/count's keepif prefilter): tag every row with a stable
  // index, filter a COPY to the if-subset, run the *exact same* egen op
  // there, then merge the freshly-computed column back onto the FULL
  // (untouched) dataset by that index — unmatched rows come back missing
  // for NEWVAR (cobra merge's standard unmatched-master semantics), while
  // every OTHER variable is untouched (unlike a whole-dataset keepif,
  // which would also drop rows from every other column). This is uniform
  // across every egen function family (row/stat/tag/group/seq) since it
  // never special-cases which fn was requested — it just re-runs whatever
  // cobra call was going to happen anyway, against the filtered subset.
  // No ifExpr/inRange: dataPath == the live hbin, zero extra cobra calls,
  // byte-identical to the pre-fix behavior (freeze-compatible).
  //
  // BS-090 (BETA Phase G): `in a/b` now composes with `if` via AND here
  // too — reusing the SAME kIdxVar this block already materializes for the
  // if-merge-back key (it is already the GLOBAL row index, `_n` over the
  // whole dataset), so unlike piet::handlers::applyIfAndInFilter's general
  // case, no SEPARATE idx materialization is needed.
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  // BUG-369 follow-up, E1 guard: with if/in, this handler computes fn() on a
  // row-FILTERED copy (the whole point of the BUG-042 fix above). But a TS
  // operator inside fn() must be resolved against the FULL tsset series before
  // any sample restriction (E1 — the same law cmds_ts.cpp/tsops enforces), and
  // on the filtered copy a boundary row's lag would silently come back missing
  // or skip across removed rows. A silently-wrong number is the one class this
  // codebase refuses to ship — so the combination is an honest reject with the
  // working recipe, until someone builds a pre-materialize step. `egen m =
  // mean(L.y)` WITHOUT if/in stays fully supported (whole-frame evaluation,
  // E1 holds by construction).
  if ((haveIf || (inRange.has_value() && !inRange->empty())) &&
      translate::exprHasTsToken(rhs)) {
    return engineErrorBlock(
        "egen with if/in cannot take a time-series operator inside the function yet: "
        "the operator must see the full tsset series BEFORE the sample restriction "
        "(materialize it first — `gen tmp = L.x`, then egen over tmp with your if/in)",
        "unimplemented");
  }
  bool haveIn = inRange.has_value() && !inRange->empty();
  // Stage 1b: the common (no if/in) egen is a clean single mutation whose --out
  // becomes the head and is never read back as a file — opt it into the
  // resident frame plane. The if/in path instead builds intermediate scratch
  // hbins and reads them back AS FILES out of resident order (the `Datastore
  // srcDs` below; the merge-back references the idx temp created several steps
  // earlier), so it stays on the default stateless path (real temp files, and
  // its `--data currentHbin` idx call already materializes any virtual head).
  std::optional<SessionState::ResidentEligible> egenResident;
  if (!(haveIf || haveIn)) egenResident.emplace(session);
  std::string egenDataPath = *session.currentHbin();
  std::string idxFullPath;
  static const char* kIdxVar = "__egen_ifidx__";
  // BUG-840: under by() -- the option, or a by-prefix, which dispatcher.cpp
  // turns into the option -- Stata evaluates the `if` inside each by-group: its
  // _n, _N and subscripts count within the group (w2m_bymask E01 :373 and E02
  // :383 `if _n > 1` give . 100 100 | . 80 80; E05 :411, D09 :712). A --keepif
  // over the whole frame counted them over the dataset (E01 gave . 100 100 | 90
  // 90 90 at rc 0). The selection is built by a --bygen over the by-groups in
  // their current row order (bysort has already sorted the rows in place), in
  // the same call that tags the merge-back index, and the filter keeps it. The
  // `!=0` keeps a missing condition selecting (BUG-622). Without by() nothing
  // changes.
  static const char* kSelVar = "__egen_ifsel__";
  std::vector<std::string> byList;
  {
    std::string tok;
    for (char ch : by + " ") {
      if (ch == ' ' || ch == ',' || ch == '\t') {
        if (!tok.empty()) byList.push_back(tok);
        tok.clear();
      } else {
        tok += ch;
      }
    }
  }
  const bool groupIf = haveIf && !byList.empty();
  if (haveIf || haveIn) {
    idxFullPath = nextPath(session.workdir(), "egen_ifidx");
    std::vector<std::string> idxArgs = {"eval", "--data", *session.currentHbin(), "--bygen",
                                        std::string(kIdxVar) + "=_n"};
    if (groupIf) {
      const std::string selArg = makeBygenExpr(
          std::string(kSelVar) + "=((" + *ifExpr + ")!=0)", byList, std::nullopt);
      idxArgs.push_back("--bygen");
      idxArgs.push_back(selArg);
      idxArgs = translate::injectTsContextForExpr(idxArgs, selArg, session);
    }
    idxArgs.push_back("--out");
    idxArgs.push_back(idxFullPath);
    try {
      runCobraJson(idxArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string combinedIf = groupIf ? std::string(kSelVar) : ifExpr.value_or("");
    if (haveIn) {
      Datastore srcDs(*session.currentHbin());
      InRangeBounds rr = parseInRangeBounds(*inRange, srcDs.nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "egen");
      }
      std::string rangePred = std::string(kIdxVar) + ">=" + std::to_string(rr.a) + " & " +
                              std::string(kIdxVar) + "<=" + std::to_string(rr.b);
      // BUG-622 (its egen.cpp sites): `!=0` on the `if` half, as
      // applyIfAndInFilter does (BUG-534). The group-relative selection is
      // already 0/1.
      if (groupIf) {
        combinedIf = std::string(kSelVar) + " & (" + rangePred + ")";
      } else {
        combinedIf = haveIf ? ("((" + *ifExpr + ")!=0) & (" + rangePred + ")") : rangePred;
      }
    }
    std::string filteredPath = nextPath(session.workdir(), "egen_iffilter");
    std::vector<std::string> filterArgs = translate::injectTsContextForExpr(
        {"eval", "--data", idxFullPath, "--keepif", combinedIf, "--out", filteredPath},
        combinedIf, session);  // BUG-369 follow-up: the if itself may carry L.
    try {
      runCobraJson(filterArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    egenDataPath = filteredPath;
  }

  // Row-function detection: `fn(a b c)` (space-separated Stata varlist) with
  // fn in _ROW_FUNS -> --rowegen NEWVAR=fn(a,b,c) (comma-separated).
  static const std::regex reRowFn(R"(^(\w+)\s*\(([^)]+)\)\s*$)");
  std::smatch m;
  std::string computedPath;
  if (std::regex_match(rhs, m, reRowFn) && rowFuns().count(lower(m.str(1)))) {
    std::string rowFnName = m.str(1);
    std::string argsStr = trim(m.str(2));
    // BUG-1001: the varlist expands its wildcards and ranges through the one
    // expander (`rowmax(w*)` reached the engine as a column named `w*`); a
    // pattern that matches nothing is r(111). Space-separated -> comma-separated
    // (mutate.py: ",".join(args_str.split())).
    loops::VarlistExpandResult rowVars = loops::expandVarlist(argsStr, session);
    if (rowVars.isError) return rowVars.error;
    // rowsd() takes at least two variables: one is r(102). Oracle eg_ref_rowsd_one.
    if (fnName == "rowsd" && rowVars.tokens.size() < 2)
      return egenRcError("too few variables specified", 102);
    std::string argsCsv;
    for (size_t t = 0; t < rowVars.tokens.size(); ++t) {
      if (t) argsCsv += ",";
      argsCsv += rowVars.tokens[t];
    }
    std::string rowegenExpr = newvar + "=" + rowFnName + "(" + argsCsv + ")";
    if (fnOpt("missing") != nullptr) rowegenExpr += ",missing";  // rowtotal only
    if (fnOpt("strok") != nullptr) rowegenExpr += ",strok";      // rownonmiss only
    std::vector<std::string> args = {"eval", "--data", egenDataPath, "--rowegen",
                                      rowegenExpr, "--out", outPath};
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      // A string variable in a row function (rownonmiss without strok included)
      // is Stata's r(109) type mismatch (StataBE oracle arms D_str_*).
      if (std::string(ce.what()).find("variable used where a number is required") !=
          std::string::npos)
        return egenRcError("type mismatch", 109);
      return egenStoreError(ce);
    }
    computedPath = outPath;
  } else {
    // Non-row function: --eigen path (cobra's --egen flag). BS-097: cobra's
    // own `--egen` CLI parsing (apps/cmds/cmds_data.cpp) never validates the
    // comma-suffix tail at all — it only ever substring-scans for the
    // specific keys it reads (by()/missing/p()) and drops everything else on
    // the floor. The per-function net at the top of runEgen (BUG-1001) is
    // therefore the ONLY place an unsupported egen option is caught, and it has
    // already run: `fnOpts` holds the canonical names it accepted.
    const std::string egenFnName = fnName;
    std::vector<std::pair<std::string, eval::OptValue>> fwdOpts = fnOpts;
    // Stata's seq(): an absent to() is _N of the WHOLE dataset. Under if/in this
    // call runs on the filtered copy, whose row count is smaller, so the full
    // count travels as an explicit to().
    if (egenFnName == "seq" && fnOpt("to") == nullptr && (haveIf || haveIn)) {
      eval::OptValue tv;
      tv.isBool = false;
      tv.strValue = std::to_string(Datastore(*session.currentHbin()).nRows());
      fwdOpts.emplace_back("to", tv);
    }
    // gtools' gegen computes total(), sum() and mean() in its own plugin, which
    // sums in data order with or without by() (StataBE 19.5 with gtools: gegen
    // total of 1e16, twenty-two 1s and -1e16 is 0 and of ten 0.1s
    // +1.fffffffffffffX-001, where egen gives 22 and 1). Its pc() and std() are
    // official egen's, and keep summarize's exact sum.
    if (gtools && (egenFnName == "total" || egenFnName == "sum" || egenFnName == "mean"))
      fwdOpts.emplace_back("dataorder", eval::OptValue::ofBool(true));
    // The suffix carries every option EXCEPT by() (appended separately below).
    std::string optsSuffix = buildOptsSuffix(fwdOpts, {"by"});
    std::string cobraExpr = expr + optsSuffix;

    if (!by.empty()) {
      // cobra eval --egen already parses by() out of the expr; append it.
      cobraExpr += ", by(" + by + ")";
    }

    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", egenDataPath, "--egen", cobraExpr, "--out", outPath},
        cobraExpr, session);  // BUG-369 follow-up: `egen m = mean(L.y)`
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    // BUG-471 T2 fix-up (F3): Stata's seq() result starts as a byte and widens as a
    // replace would (egen_seq_type), so its type follows the values it stored. The
    // computing call summarizes the new column after the egen (eval's
    // --summarize reads the final frame), so the min/max cost no extra engine
    // call and no dispatch(): r() and e() are untouched. Under if/in this frame
    // is the filtered copy, i.e. exactly the rows the replace touched.
    const bool seqSummary = fixedTypeOut != nullptr && egenFnName == "seq" && !newvar.empty();
    if (seqSummary) {
      args.push_back("--summarize");
      args.push_back(newvar);
    }
    ojson egenOut;
    try {
      egenOut = runCobraJson(args);
    } catch (const CobraError& ce) {
      const std::string what = ce.what();
      if (what.find("--egen unknown fn") != std::string::npos)
        return unknownEgenFunctionError(egenFnName);
      // A string where the function's `gen double x = (exp)` needs a number is
      // Stata's r(109) type mismatch (eg_ref_strarg, eg_ref_strarg_by).
      if (what.find("variable used where a number is required") != std::string::npos ||
          what.find("string literal used where a number is required") != std::string::npos)
        return egenRcError("type mismatch", 109);
      // egen mode() with nummode() checks exit 198 (eg_ref_mode_num).
      if (egenFnName == "mode" && what.find("nummode(") != std::string::npos)
        return egenRcError(what, 198);
      return egenStoreError(ce);
    }
    if (seqSummary) {
      const ojson summ = egenOut.value("summarize", ojson::object()).value(newvar, ojson::object());
      const auto num = [&](const char* k) {
        return summ.contains(k) && summ[k].is_number() ? summ[k].get<double>() : 0.0;
      };
      // No stored value (every row excluded): the `gen byte` stands.
      *fixedTypeOut = num("N") > 0 ? storage::promotedType("byte", false, num("min"), num("max"))
                                 : std::string("byte");
    }
    computedPath = outPath;
  }

  std::string finalPath = computedPath;
  if (haveIf || haveIn) {
    // Merge NEWVAR back onto the untouched full dataset by the stable idx
    // (--keepusing restricts the using-side to idx+NEWVAR only — merging
    // the full column set back in would immediately fail cobra's
    // both-sides-have-this-column conflict check). --nogenerate skips the
    // _merge indicator (this isn't a user-visible merge, just plumbing).
    std::string mergedPath = nextPath(session.workdir(), "egen_ifmerge");
    std::vector<std::string> mergeArgs = {
        "merge",       "--data", idxFullPath, "--using",     computedPath, "--type", "1:1",
        "--key",       kIdxVar,  "--keepusing", newvar,       "--nogenerate", "--out", mergedPath};
    CobraRunResult mcr = runCobra(mergeArgs);
    if (!mcr.ok) {
      CobraError ce = errorFromRaw(mcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string dropIdxPath = nextPath(session.workdir(), "egen_ifdropidx");
    std::vector<std::string> dropArgs = {"eval", "--data", mergedPath, "--drop", kIdxVar};
    if (groupIf) {
      dropArgs.push_back("--drop");
      dropArgs.push_back(kSelVar);
    }
    dropArgs.push_back("--out");
    dropArgs.push_back(dropIdxPath);
    try {
      runCobraJson(dropArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    finalPath = dropIdxPath;
  }

  session.load(finalPath);
  // BUG-1109: egen mode() ends with `qui compress g` for a string source
  // (:124-126), so the result is str# of its longest value, str1 when every value
  // is "", whatever the source's width or a declared type (strL too; StataBE
  // 19.5). The ado leaves before that only without `missing', when the sample
  // has no observation or the source is "" on every one of them
  // (CheckMissingVarlist): the result keeps the source's type, which the
  // dispatcher carries. The sample is the filtered copy under if/in.
  if (fnName == "mode" && fixedTypeOut != nullptr && !newvar.empty()) {
    static const std::regex reSrc(R"(^\w+\s*\(\s*([A-Za-z_]\w*)\s*\)$)");
    std::smatch sm;
    Datastore* ds = session.datastore();
    if (std::regex_match(rhs, sm, reSrc) && ds != nullptr && ds->has(newvar) &&
        ds->has(sm.str(1)) && ds->dtype(sm.str(1)) == "factor") {
      bool leftEarly = false;
      if (fnOpt("missing") == nullptr) {
        std::optional<Datastore> filtered;
        if (haveIf || haveIn) filtered.emplace(egenDataPath);
        const Datastore& sample = filtered.has_value() ? *filtered : *ds;
        const cecon::Column& sc = sample.col(sm.str(1));
        leftEarly = true;
        for (int64_t i = 0; i < sample.nRows() && leftEarly; ++i) {
          const int32_t k = sc.codes()[i];
          if (k >= 0 && !sc.levels[static_cast<size_t>(k)].empty()) leftEarly = false;
        }
      }
      if (!leftEarly) *fixedTypeOut = strTypeOfLongest(*ds, newvar);
    }
  }
  // concat() ignores egen's type too: the result is a string variable as wide as
  // the longest text it holds (StataBE 19.5: `egen
  // double c = concat(x s)` is str4; the dispatcher recorded double, or an
  // untyped float that made `: type` r(101)). This is the engine path, with no
  // punct() to trim off; runEgenAdoFunction records its own result's type.
  if (fnName == "concat" && fixedTypeOut != nullptr && !newvar.empty()) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(newvar) && ds->dtype(newvar) == "factor")
      *fixedTypeOut = strTypeOfLongest(*ds, newvar);
  }
  bool groupLabel = false;
  if (fnName == "group")
    for (const auto& kv : opts)
      if (kv.first == "label") groupLabel = true;
  if (groupLabel && !newvar.empty()) {
    // egen group() `label`: each group's label is its grouping variables'
    // values, value labels decoded, joined by a blank; the set is named
    // after the new variable.
    static const std::regex reArgs(R"(^\w+\s*\((.*)\)\s*$)");
    std::smatch am;
    std::vector<std::string> gvars;
    if (std::regex_match(rhs, am, reArgs)) {
      loops::VarlistExpandResult vx = loops::expandVarlist(trim(am.str(1)), session);
      if (!vx.isError) gvars = vx.tokens;
    }
    Datastore* gds = session.datastore();
    if (gds != nullptr && gds->has(newvar) && !gvars.empty()) {
      const cecon::Column& gc = gds->col(newvar);
      std::map<long long, std::string> labelOf;
      for (int64_t r = 0; r < gds->nRows(); ++r) {
        const double code = gc.f64()[r];
        if (std::isnan(code)) continue;
        const long long c = static_cast<long long>(code);
        if (labelOf.count(c)) continue;
        std::string text;
        for (const auto& gv : gvars) {
          if (!gds->has(gv)) continue;
          const cecon::Column& col = gds->col(gv);
          std::string piece;
          if (col.dtype == cecon::DType::Factor) {
            const int32_t k = col.codes()[r];
            piece = k >= 0 ? col.levels[static_cast<size_t>(k)] : std::string();
          } else {
            const double v = col.f64()[r];
            bool done = false;
            auto lv = session.varLblname.find(gv);
            if (lv != session.varLblname.end() && !std::isnan(v) && v == std::floor(v)) {
              auto ld = session.labelDefs.find(lv->second);
              if (ld != session.labelDefs.end()) {
                auto it = ld->second.find(std::to_string(static_cast<long long>(v)));
                if (it != ld->second.end()) { piece = it->second; done = true; }
              }
            }
            if (!done) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "%.9g", v);
              piece = std::isnan(v) ? std::string(".") : std::string(buf);
            }
          }
          text += (text.empty() ? "" : " ") + piece;
        }
        labelOf[c] = text;
      }
      for (const auto& [c, text] : labelOf) {
        std::string esc;
        for (char ch : text) esc += (ch == '"') ? '\'' : ch;
        (void)piet::dispatch("label define " + newvar + " " + std::to_string(c) + " \"" + esc +
                                 "\", add",
                             session);
      }
      (void)piet::dispatch("label values " + newvar + " " + newvar, session);
    }
  }
  return mutationResult(session, "egen",
                        newvar.empty() ? std::vector<std::string>{} : std::vector<std::string>{newvar});
}

}  // namespace piet::handlers
