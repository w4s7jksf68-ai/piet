// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/translate.h"

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "core/ident.h"  // gsort's varspec: bytes >= 0x80 are identifier bytes
#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/column_fetch.h"  // headColumnValues: one column of a resident head
#include "piet/dispatcher.h"  // clonevar/recode dispatch generate/replace lines
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/loops.h"         // expandVarlist (collapse's output metadata)
#include "piet/storage_type.h"  // resolveCreateType (collapse's c(type) outputs)
#include "piet/strutil.h"
#include "data/missing.h"
#include "fileio/xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
#include "datamgmt_support.h"
#include "estimation/estimation_sample_internal.h"  // e(sample) T2: the copy verbs' mask helpers



namespace piet::handlers {
nlohmann::ordered_json runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange);
nlohmann::ordered_json runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
nlohmann::ordered_json runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange);
nlohmann::ordered_json runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
nlohmann::ordered_json runDuplicatesExamples(const std::optional<std::vector<std::string>>& varlist,
                            SessionState& session, const std::optional<std::string>& ifExpr,
                            const std::optional<std::string>& inRange);
}
namespace piet::handlers::data_mutation_detail {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;


using ojson = nlohmann::ordered_json;


// R2: the shared helpers live in datamgmt_support.h (dv_detail), the common preamble
// that data_io_internal.h, data_mutation_internal.h and data_combine_internal.h include.
using namespace dv_detail;


// ---------------------------------------------------------------------------
// rename OLDVAR NEWVAR — C++ port of mutate.py's run_rename (two-arg form
// only — wildcard/multi-var syntax is OUT of scope, matching the oracle's
// own commands.py guard).
//
// rename is pure column-metadata relabeling — it must never touch a row's
// VALUE. L1/M3 (CSV-shim retirement): this calls cobra's native `rename`
// verb (`--pairs old=new`), which renames the header cell in place and
// leaves every other column — and the renamed column's own values and
// factor dictionary — byte-identical. Numeric and string/factor columns take
// the IDENTICAL path, no dtype branch.
//
// History (superseded, kept for the honesty rationale): this was first a
// `cobra eval --gen NEW=OLD --drop OLD` value-copy that only worked for f64
// columns and leaked a raw "[internal] expr: ..." engine exception when OLD
// was string/factor (BUG B, W3 stabilization); the fix routed it through a
// whole-frame CSV/`cobra convert` round-trip (writeColumnCsvRoundTrip, since
// deleted) because piet/native then had no way to rename a header without an
// hbin writer. The native verb supersedes both — no round-trip, no
// re-factorization.

// rename's two refusals with Stata's text and rc (w2ah_data_verbs.log
// ah_rename_exist / ah_ren_exist: rc 110, the target untouched; ah_rename_absent
// / ah_ren_absent: rc 111). The target message used to read "already exists",
// which coarseRc maps to rc 1.
ojson renameAbsent(const std::string& oldvar);

ojson renameTargetExists(const std::string& newvar);


// ---------------------------------------------------------------------------
// clonevar NEWVAR = OLDVAR — value copy + metadata clone (BUG-003, 1.6.0).
// C++ port of dispatcher.py's clonevar handler: gen NEWVAR=OLDVAR (action
// stays "gen", not "clonevar" — the oracle's own quirk) + clone
// label/format/stata_type/value-label attachment.
//
// Numeric clonevar uses the fast `cobra eval --gen NEW=OLD` path (also the
// one place `clonevar ... if` is supported, folded into a cond()); the
// string/factor path — which `cobra eval --gen`'s numeric-only RHS cannot
// serve (it once leaked "[internal] expr: string/factor variable used where
// a number is required" on `clonevar st2 = state`, BUG-037) — now calls
// cobra's native `clonevar` verb (`--pairs old=new`, L1/M3): the clone is
// appended at the end with the source's factor dictionary SHARED, OLDVAR
// survives untouched. (History: BUG-037 first routed the string/factor path
// through a whole-frame CSV round-trip, writeColumnCsvRoundTrip, since
// deleted — the native verb supersedes it.)
//
// BS-031(c) (BETA campaign C2, silent-if-drop sweep): `ifExpr` was
// previously accepted by the tokenizer but never threaded to this handler
// at all — `clonevar inc2 = income if year==1982` silently cloned ALL 336
// rows instead of only the 48 in-sample ones (oracle-verified live this
// stint: 48 non-missing, 288 missing — clonevar is gen-class, so an
// if-excluded row's target is MISSING, same as plain `gen`). The numeric
// fast path folds `if` into cobra's own `cond()` the exact same way
// data_generate.cpp::runGen already does for plain `gen ... if` — no filtering/
// merge-back needed since this is a per-row passthrough. The string/factor
// path (`cobra clonevar` is a whole-column copy with no per-row conditional)
// serves `if` through a dispatched string `generate`, clonevar.ado's own body.
//
// C4-L4B (FIX-C4L4B-RESIDUALS ①): the encode recipe, applied to the residual
// wrap class the C4-L4 capture named. clonevar's entry reads a NAME and a
// DTYPE and nothing else — no column data — so the resident card answers both
// (the D1 rule) and the datastore opens LAZILY; the engine call takes the head
// TOKEN (`*session.currentHbin()`) instead of `ds->path()` and is wrapped in
// ResidentEligible, which routes it through the frame plane. `cobra clonevar`
// rides the SAME §26 acquire_data/commit_frame bridge as `cmd_encode`
// (apps/cmds/cmds_data.cpp:365/:368 vs :693/:697 — read cross-lane, zero cobra
// change), and the numeric arm is plain `eval --gen`, the hot resident path
// itself. Escape (`PIET_NO_RESIDENT_READ` for the C4 family,
// `PIET_NO_RESIDENT_SETCOL` for the C2-specific arm) restores the pre-C4 entry
// and `ds->path()` VERBATIM, so the floor RED arm reproduces the old path with
// one env, exactly as at the destring/encode sites.

// clonevar.ado after its `gen': the variable label, the value label and the
// format follow the source, and so does the type claim -- through the one carry
// rule (storage::carryTypeClaim, FU-243): `gen `type'' declares the source's
// type, so the clone of an untyped generate is a float that `replace` keeps
// float (w2ah_data_verbs.log ah_fu243_clone, ah_fu243_bigint).
void copyCloneMeta(SessionState& session, const std::string& oldvar, const std::string& newvar);


// ---------------------------------------------------------------------------
// collapse (stat) varlist [(stat) varlist...] [, by(g1 g2)] [if COND] — C++
// port of dispatcher.py's _run_collapse. L1/M3 (CSV-shim retirement):
// cobra's native `collapse --by` now accepts factor by-vars directly — the
// output by-col stays factor with a fresh, self-consistent dictionary in
// first-seen output order (the same convention `convert --factor` produces,
// and what the old oracle's restore-levels dance rebuilt from scratch each
// collapse). Older cobra required all --by columns to be f64, so this
// handler used to pre-convert factor by-vars to f64 CODES via a CSV
// round-trip + egen-group() proxy and restore level text afterward; that
// whole pre/post dance is gone, by-vars pass straight through to `--by`.

// format-display-default-lifecycle: what collapse gives each result
// variable. Its format is the source variable's; its type follows the statistic: sum, rawsum
// and percent are double; count is long (double under a weight); max, min,
// first, last, firstnm and lastnm keep the source's type; every other statistic
// is double for a double or long source and c(type) otherwise.
// w2af_format_lifecycle.log af_collapse: `(mean) dt f92 v_b (sum) sm=v_b
// (count) cn=v_b (max) mx=v_l` -> double %td, double %9.2f, float %8.0g,
// double %8.0g, long %8.0g, long %12.0g. Before, a result kept whatever its
// name held (v_b stayed byte) and a new name took no type at all.
struct CollapseResultMeta {
  std::string target;
  std::string format;
  std::optional<std::string> type;  // nullopt = no claim (unknown source)
  std::string label;                // the result's variable label (see collapseResultLabel)
};


// FU-1013 / BUG-960: collapse's clist, parsed in this ONE place. Its grammar is
// collapse.ado's own note, `name[ ]=[ ]<varname>`: `s2 = src2`, `s2= src2`,
// `s2 =src2` and `s2=src2` are the same target (w2e_float_default.log
// collapse_sum_* / collapse_count_* / collapse_max_*: `(sum) s2 = src2 bsum = bb`,
// `(count) c = src`, `(max) mx = src bmx = bb` at rc 0). The handler used to
// re-join the stat-varlist token by token, so `s2 = src2` reached the engine's
// --stat as three names ("no column: s2", rc 1). Several targets per block and
// targets beside plain variables follow from the same walk. A plain token with
// a wildcard or a range (`modal*`, `a-b`) is expanded here, because the engine
// takes names only ("no column: x*", rc 1 before).
// Refusals (collapse): a duplicate result name is "name conflict" r(198)
// (:203-212), a by() variable as a result is r(198) (bynottar), no variable at
// all is r(198) (:115). `= old` with no target and `new =` with no source are
// UNPINNED by a capture: r(198) by the task's rule (collapse_17's own path would
// reach `syntax`, whose rc for these shapes is not captured).
struct CollapseItem {
  std::string stat;    // lowercased, without the parentheses ("mean" by default)
  std::string target;  // the result variable
  std::string source;  // the source variable as written
};


struct CollapseClist {
  std::string statCobra;            // the engine's --stat: `(stat) new=old name ...`
  std::vector<CollapseItem> items;  // one per result variable, in order
  std::optional<ojson> refusal;
};


// A statistic as each flavor's parser takes it, to the engine's name.
// collapse: the listed words, SEMean /
// SEBinomial / SEPoisson by abbreviation, p1..p99; else `(avg) invalid
// statistic` r(198) (cz_c_err_stat, cz_c_err_p100, cz_c_err_p0). gcollapse
// (ParseList and the quantile check): its own words, the
// short spellings sem/seb/sep/var/skew/kurt, p# below 100 but not p0; else
// `Invalid stat` r(110) (cz_g_err_stat, cz_g_err_p100). fcollapse (ftools)
// serves no SE statistic: r(110) (cz_f_semean_g, cz_f_err_stat).
// BUG-1098: every flavor reads the statistic as typed, so (MEAN), (Mean),
// (P50) and (SEM) are r(198) in collapse and r(110) in gcollapse and
// fcollapse (StataBE 19.5; this build lower-cased them and served them). And
// fcollapse checks a quantile only against `^p[0-9]+$` (fcollapse):
// (p0) is the minimum, (p100) and past it the maximum, (p050) the median, all
// at rc 0, where collapse refuses p0/p100 r(198) and gcollapse r(110).
std::optional<std::string> canonicalCollapseStat(const std::string& raw, const std::string& flavor,
                                                 std::string* rcOut);


CollapseClist parseCollapseClist(const std::string& statRaw, const std::vector<std::string>& byVars,
                                 SessionState& session, const std::string& flavor);


// The statistic as each flavor spells it in the result's variable label.
// collapse hands back `p 50` for (median) and
// `p #` for (p#), the full word for an SE abbreviation; the label is
// `(fcn) lab`: w2au_collapse_family.log cz_c_median_g_v2
// `[(p 50) xb]`, cz_c_p10_g_v2 `[(p 10) xb]`. gtools labels with the stat as
// its parser normalised it (gcollapse.ado ParseList, labelformat
// `(#stat#) #sourcelabel#` :1024): cz_g_median `[(median) byte source]`,
// cz_g_p50 `[(p50) byte source]`. ftools: sprintf("(%s) %s", stat, var)
// (fcollapse_main), the stat as typed once ParseList has taken a
// leading `raw` off it (fcollapse): (rawsum) labels `(sum) x`.
std::string collapseLabelStat(const std::string& stat, const std::string& flavor);


std::vector<CollapseResultMeta> collapseResultMeta(const std::vector<CollapseItem>& items,
                                                   const std::vector<std::string>& byVars,
                                                   const std::string& flavor, const std::string& wkind,
                                                   bool clabel, SessionState& session);


// fcollapse_main.mata compress_type, applied unless `nocompress`: the
// smallest storage type that holds a result. A byte stays; a float or double
// holding a non-integer stays; otherwise -127..100 is byte, -32767..32740 int,
// -2147483647..2147483620 long over the nonmissing values, and anything else
// (all missing included) keeps `type`. BUG-1100, StataBE 19.5: (sum) of
// integers summing to 3/7/5 is byte, to 40300 long, (count) byte, (max) of a
// float holding integers long; with nocompress double, double, long, float.
std::string fcollapseCompressedType(const double* x, int64_t n, const std::string& type);


// A result's claim after compress_type. A max/min/first/last of a source whose
// type nothing recorded has no claim; its values are compressed as a double's
// would be, and an integer type is claimed only when they all fit one.
void compressFcollapseResult(CollapseResultMeta& m, const double* x, int64_t n);


// Whether every value of x is kept by a store as `type`: what `recast` checks
// before it changes anything (it changes nothing when one value would change).
bool valuesFitType(const double* x, int64_t n, const std::string& type);


// fcollapse's `smart` test (fcollapse): the data's sort claim starts
// with the first by() variable's name, as text (BUG-1100, StataBE 19.5: data
// sorted by gg pass for by(g), and one observation sorted by g passes), or
// there are two observations or more and none has a first by() value below the
// one before it (a string by its bytes, a numeric missing above every number).
// Without by() fcollapse tests a constant of its own, which no sort claim
// names and which is sorted once there are two observations.
bool fcollapseSortedOnFirstBy(SessionState& session, const std::vector<std::string>& byVars);


// The comma that starts collapse's options: the first one outside parentheses and
// quotes (eval::findUnquotedComma, the tokenizer's own). `raw.find(',')` cut
// `collapse ... if strpos(s, "o") > 0, by(g)` inside strpos(), and the stat list
// became `... nx=x, "o") > 0` (rc 1 `no column: x,`; w2aj_qualifiers
// qf_if_collapse_strpos).
size_t collapseOptionComma(const std::string& raw);


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
std::optional<std::vector<double>> headColumnValues(SessionState& session, const std::string& col);


// The smallest integer type that holds every non-missing value (compress on a
// long), "" when a value is not an integer or leaves long's range.
std::string compressedIntegerType(const std::vector<double>& v);


// ---- rule tokens -----------------------------------------------------------

struct RcTok {
  std::string text;
  bool quoted = false;
  bool is(const char* s) const { return !quoted && text == s; }
};


// Blank-separated words; `(`, `)`, `/` and `=` stand alone; "..." and the
// compound `"..."' are one token each, without their outer quotes.
std::optional<std::vector<RcTok>> recodeLex(const std::string& s);


// ---- rules -----------------------------------------------------------------

// One value of a rule: a number, a missing code (kind 0 = `.`, 1..26 =
// `.a`..`.z`), or the source's min / max.
struct RcEl {
  enum Kind { Num, Miss, Min, Max } kind = Num;
  double num = 0;
  int miss = 0;
  std::string typed;
};


struct RcElement {
  RcEl lo;
  bool range = false;
  RcEl hi;
};


struct RcRule {
  std::vector<RcElement> elems;  // empty for a keyword rule
  RcEl to;
};


struct RcRuleSet {
  std::vector<RcRule> cond;
  std::optional<RcEl> elseTo, missTo, nonmissTo;
  // Rule labels in writing order: (code, text).
  std::vector<std::pair<std::string, std::string>> labels;
  bool usesMinMax = false;
};


enum class RcLabelPolicy { Allowed, InPlace, MultiNoLabel };


struct RcParseError {
  std::string msg;
};


std::optional<RcEl> recodeParseEl(const RcTok& t);


// The keyword a word spells, if any: else (`*` too), missing (at least `mis`),
// nonmissing (at least `nonm`).
std::optional<std::string> recodeKeyword(const RcTok& t);


class RcParser {
 public:
  RcParser(std::vector<RcTok> toks, RcLabelPolicy policy) : t_(std::move(toks)), policy_(policy) {}

  // Syntax 1: `(rule) (rule) ...`.
  void parseParenthesised(RcRuleSet& rs) {
    while (pos_ < t_.size()) {
      if (!t_[pos_].is("(")) throw RcParseError{"( expected, \"" + t_[pos_].text + "\" found"};
      ++pos_;
      parseOneRule(rs, /*paren=*/true);
    }
  }

  // Syntax 2: `elements = el elements = el ...` without parentheses.
  void parseBare(RcRuleSet& rs) {
    while (pos_ < t_.size()) parseOneRule(rs, /*paren=*/false);
  }

 private:
  const RcTok* peek() const { return pos_ < t_.size() ? &t_[pos_] : nullptr; }

  RcEl takeEl() {
    const RcTok* t = peek();
    if (t == nullptr) throw RcParseError{"unknown el  in rule"};
    std::optional<RcEl> e = recodeParseEl(*t);
    if (!e.has_value()) throw RcParseError{"unknown el " + t->text + " in rule"};
    ++pos_;
    return *e;
  }

  void noteMinMax(RcRuleSet& rs, const RcEl& e) {
    if (e.kind == RcEl::Min || e.kind == RcEl::Max) rs.usesMinMax = true;
  }

  void parseOneRule(RcRuleSet& rs, bool paren) {
    const RcTok* first = peek();
    if (first == nullptr) throw RcParseError{"unmatched open parenthesis"};
    RcRule rule;
    std::optional<std::string> keyword = recodeKeyword(*first);
    if (keyword.has_value()) {
      ++pos_;
      const RcTok* eq = peek();
      if (eq == nullptr || !eq->is("=")) throw RcParseError{"= expected"};
      ++pos_;
      if (*keyword == "else" ? rs.elseTo.has_value()
                             : (*keyword == "missing" ? rs.missTo.has_value()
                                                      : rs.nonmissTo.has_value()))
        throw RcParseError{"multiple specification of keyword " + *keyword};
      const bool isElse = *keyword == "else";
      if ((isElse && (rs.missTo.has_value() || rs.nonmissTo.has_value())) ||
          (!isElse && rs.elseTo.has_value()))
        throw RcParseError{"keywords else/* and missing/nonmissing may not be combined"};
      sawKeyword_ = true;
      rule.to = takeEl();
    } else {
      if (sawKeyword_) throw RcParseError{"unknown keyword " + first->text};
      for (;;) {
        const RcTok* t = peek();
        if (t == nullptr) {
          if (paren) throw RcParseError{"unmatched open parenthesis"};
          throw RcParseError{"invalid syntax"};
        }
        if (t->is("=")) {
          if (rule.elems.empty())
            throw RcParseError{"transformation rule without \"condition\" part"};
          ++pos_;
          break;
        }
        RcElement el;
        el.lo = takeEl();
        const RcTok* slash = peek();
        if (slash != nullptr && slash->is("/")) {
          ++pos_;
          el.range = true;
          el.hi = takeEl();
        }
        rule.elems.push_back(el);
      }
      rule.to = takeEl();
    }
    for (const RcElement& el : rule.elems) {
      noteMinMax(rs, el.lo);
      if (el.range) noteMinMax(rs, el.hi);
    }
    noteMinMax(rs, rule.to);

    if (paren) {
      const RcTok* t = peek();
      if (t == nullptr) throw RcParseError{"unmatched open parenthesis"};
      if (!t->is(")")) {
        // A label: one token, then the closing parenthesis.
        const RcTok label = *t;
        const RcTok* after = pos_ + 1 < t_.size() ? &t_[pos_ + 1] : nullptr;
        if (label.text.empty() || after == nullptr || !after->is(")"))
          throw RcParseError{") expected, \"" + label.text + "\" found"};
        pos_ += 2;
        checkLabel(rule.to);
        rs.labels.emplace_back(labelCode(rule.to), label.text);
      } else {
        ++pos_;
      }
    }
    if (keyword.has_value()) {
      if (*keyword == "else") rs.elseTo = rule.to;
      else if (*keyword == "missing") rs.missTo = rule.to;
      else rs.nonmissTo = rule.to;
    } else {
      rs.cond.push_back(std::move(rule));
    }
  }

  void checkLabel(const RcEl& to) {
    if (policy_ == RcLabelPolicy::InPlace)
      throw RcParseError{"Rules defining value labels not allowed when overwriting a variable"};
    if (policy_ == RcLabelPolicy::MultiNoLabel)
      throw RcParseError{
          "Rules defining value labels not allowed with labeled recode of multiple vars, option "
          "label() required"};
    if (to.kind == RcEl::Min || to.kind == RcEl::Max || (to.kind == RcEl::Miss && to.miss == 0))
      throw RcParseError{"Value label not allowed for missing value code ."};
    if (to.kind == RcEl::Num && to.num != std::floor(to.num))
      throw RcParseError{"Value label not allowed for noninteger " + to.typed};
  }

  static std::string labelCode(const RcEl& to) {
    if (to.kind == RcEl::Miss) return std::string(".") + static_cast<char>('a' + to.miss - 1);
    return eval::pythonIntStrFromDouble(to.num);
  }

  std::vector<RcTok> t_;
  RcLabelPolicy policy_;
  size_t pos_ = 0;
  bool sawKeyword_ = false;
};


// ---- evaluation ------------------------------------------------------------

// float(): the nearest single, and system missing from 2^127 up, where Stata's
// float range ends (StataBE: float(2e38) is `.`, so an element or to-value that
// large acts as `.` at single precision; [D] Data types).
double rcSingle(double v);


// The rule set bound to one source: min/max resolved, and whether values are
// compared at single precision (byte/int/float sources).
struct RcBound {
  const RcRuleSet* rs = nullptr;
  bool single = false;
  double minV = 0, maxV = 0;
};


// An element bound as a comparable value: numbers first, then `.` < `.a` <
// ... < `.z`.
struct RcKey {
  bool missing = false;
  double num = 0;
  int kind = 0;
};


RcKey rcKeyOfValue(double v, bool single);


RcKey rcKeyOfEl(const RcEl& e, const RcBound& b);


int rcCompare(const RcKey& a, const RcKey& b);


bool rcMatches(const RcElement& el, double v, const RcBound& b);


double rcToValue(const RcEl& e, const RcBound& b);


double rcApply(double v, const RcBound& b);


// Whether a recode changed a value: at the source's precision (StataBE: a float
// holding .1 given the to-value .1 is no change, a long given a neighbour that
// rounds to the same float is one), missing kinds distinct.
bool rcDiffers(double before, double after, bool single);


// ---- the dispatched expression ----------------------------------------------

std::string rcNumLiteral(double v);


std::string rcMissLiteral(int kind);


std::string rcKeyLiteral(const RcKey& k);


// The condition one element tests, on source `x`. Numbers compare at the
// source's precision (`float(x)` against bounds already rounded to single for
// byte/int/float); missing codes compare exactly on `x` itself.
std::string rcElementCond(const RcElement& el, const std::string& x, const RcBound& b);


std::string rcToLiteral(const RcEl& e, const RcBound& b);


std::string rcExpression(const std::string& x, const RcBound& b);


// The storage type `: type` reports for a numeric source.
std::string rcSourceType(const SessionState& session, const std::string& v);


// Whether a value stored at `created` precision survives a change to `target`
// unaltered ([D] recast without force; integer ranges per [D] Data types).
bool rcFitsType(double v, const std::string& target);


double rcStore(double v, const std::string& created);


// The variable label of a new variable: "RECODE of SRC (SRCLABEL)", at most
// 80 bytes, never cut inside a UTF-8 character.
std::string rcNewVarLabel(const SessionState& session, const std::string& src);


std::string rcQuote(const std::string& s);


// The first error block among a dispatch's returned blocks.
std::optional<ojson> rcFirstError(const ojson& blocks);


// ---------------------------------------------------------------------------
// duplicates report|drop|tag|list [VARLIST] — C++ port of duplicates.py.
// report/drop/tag route to cobra's native `duplicates` verb; `list` (no
// cobra native equivalent) is built from `duplicates tag` + a keepif filter
// on the tag column (group_size-1 > 0 <=> duplicate).



// _resolve_varlist: varlist or all names; validates each var exists
// ("variable `{v}' not found" — backtick-quote, matching Python's f-string).
std::optional<std::string> resolveVarlist(Datastore* ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          std::vector<std::string>& outCols);


// GA sweep 2026-08-22. The card-answered twin. Everything the Datastore version
// asks of the frame is METADATA — the column names, and whether a name exists —
// which the resident shadow card holds exactly, so a caller does not have to
// materialize the whole virtual head to ask it. Same idiom and the same safety
// rule as joinby's INSPECT-2608 fix and runSort's varlist check: the card answers
// only when it provably describes the current head, and a nullptr card means the
// caller falls back to the byte-identical Datastore path above.
//
// Measured cause (census47h, caller-attributed materialization log): of the 962
// head flushes triggered by a `datastore()` call in one 47-task run,
// runDuplicatesDrop was 320 — and its entry call existed only to resolve a
// varlist. Its TAIL flush was already removed in the 08-20 phase-2 work; this is
// the entry half the same census named.
std::optional<std::string> resolveVarlistFromCard(const std::vector<VarInfo>* card,
                                                  const std::optional<std::vector<std::string>>& varlist,
                                                  std::vector<std::string>& outCols);


// duplicates.ado is rclass: every subcommand returns r(N), the observations it
// considered (`qui count if `touse''), report adds r(unique_value) and drop
// r(N_drop); none of them is posted when there is nothing to consider, which
// is `error 2000'. w2ah_data_verbs.log ah_dup_report_all (N=8, unique_value=7),
// ah_dup_list, ah_dup_tag, ah_dup_drop_all (N=8, N_drop=1), ah_dup_drop_none
// (N_drop=0). Main posted none of them, so every read was a named r(111).
// duplicates.ado's tag: `gen long g = Ngroup - 1 if touse' then `compress g' --
// the smallest integer type of the counts, with long's %12.0g kept
// (w2ah_data_verbs.log ah_dup_tag: byte %12.0g; main left no claim, so `: type'
// read float and the format %9.0g).
void recordDuplicatesTagType(SessionState& session, const std::string& genvar);


std::optional<ojson> postDuplicatesR(SessionState& session, int64_t n,
                                     std::initializer_list<std::pair<const char*, double>> more);




// The display columns `s` fills: one per character, two for the East Asian wide
// ranges (StataBE 19.5 lists "héllo" in five columns and "日本" in four).
size_t displayCols(const std::string& s);


// list's header abbreviation of a variable name to `n` columns: the leading
// characters that fit, `~`, then the last character (`household_id` -> `househ~d`,
// `ééééééééé` -> `éééééé~é`, StataBE 19.5). A name that fits is kept whole.
std::string abbrevName(const std::string& name, size_t n);
}
