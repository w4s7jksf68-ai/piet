// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/dispatcher.h"
#include "piet/translate.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: the calendar search directory follows session.cwd

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/bundle_paths.h"
#include "piet/cclass.h"
#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/display_hardening.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-476: kStataMissingSentinel
#include "piet/eval/gettoken.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/program.h"
#include "piet/strmutate.h"
#include "piet/storage_type.h"  // BUG-471 T2: the store-type resolver
#include "piet/strutil.h"
#include "../dispatch_helpers.h"
#include "../dispatch_profile.h"  // WS-D1: per-zone dispatch attribution (stderr, opt-in)
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink (BUG-050 wide open)
#include "piet/delimited_io.h"  // describe, replace: the description staged as a dataset
#include "piet/mutation_result.h"
#include "data/missing.h"   // BUG-532: cecon::missing_kind, the `.a`-`.z` test on a by-key
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (preResolveGenExpr)
#include "piet/twoway_parse.h"
#include "piet/scratch_seq.h"
#include "piet/table_commands.h"
#include "piet/time_declarations.h"
#include "piet/settings.h"  // WS-B: settings::extcompatEnabled() (the extcompat gate below)
#include "piet/preflight.h"  // SPEC II-4: memory pre-flight before data-size-scaling verbs
#include "../registry.h"
#include "estimation_internal.h"  // e(sample) T2: runExpandIf and the copy verbs' mask helpers


namespace piet::dispatch_detail {
using ojson = nlohmann::ordered_json;
extern thread_local int tl_rcHiddenDepth;



// How a column's cells are printed, and the width they ask of the column.
struct TabstatCellStyle {
  std::string fmt;
  size_t width = 9;
};


// The display options the table reads, already validated.
struct TabstatLookOptions {
  bool ownFormats = false;
  std::string commonFormat;
  bool longStubAsked = false;
  bool varWidthGiven = false;
  bool separators = true;
  size_t labelWidth = 16;
  size_t varWidth = 12;
  std::string notes;
};


// The by() column: its width, its header word, and each group's text.
struct TabstatByColumn {
  size_t width = 0;
  std::string headerName;
  bool clipGroups = true;
  std::function<std::string(const ojson&)> groupText;
};


// mutate.py's storage-type prefix strip (`gen byte edu_hi = ...`) — item 3.
// Also detects string storage types (str#, strL) so the caller can route to
// unsupported_command instead of silently mis-generating a numeric column.
struct StorageTypeInfo {
  std::string expr;     // with the leading storage-type token removed
  bool stringTyped = false;
  // BUG-250: the DECLARED token itself (lowercased: "double", "byte", "str8",
  // ...; "" = no declaration). The strip used to be where the user's storage
  // subtype DIED — parsed, used only for the string-typed routing decision,
  // and discarded — so `describe` called every engine-created column "float"
  // and `: type` had nothing to answer. The creation arms below now record it
  // into VarMeta::stataType (the field the .dta import path already fills and
  // describe/`: type`/the save sidecar already read). It records the CLAIM,
  // not the storage: every numeric column is f64 in the datastore and stays
  // f64 — Stata's storage type is a declaration carried in metadata, and
  // whether storage should ever follow the claim is a different question,
  // named in BUG-250's ledger close.
  std::string typeToken;
};


// BUG-922: one more preserve level (SessionState::doFileDepth) for a nested
// do/run file or a `parallel` child while it runs, restored on every exit path.
// Without it a sub-file that preserves while its caller holds a copy was
// r(621) "already preserved" (Stata gives each do-file its own level).

class DoFileLevelGuard {
 public:
  explicit DoFileLevelGuard(SessionState& session) : session_(session) { ++session_.doFileDepth; }
  ~DoFileLevelGuard() { --session_.doFileDepth; }
  DoFileLevelGuard(const DoFileLevelGuard&) = delete;
  DoFileLevelGuard& operator=(const DoFileLevelGuard&) = delete;

 private:
  SessionState& session_;
};


// D1-②: the prefix table, sorted once instead of once per call.
//
// WHY. stripPrefix used to be handed a `std::set<std::string>` and, ON EVERY
// CALL, copy it into a vector (one heap allocation per spelling — 5 for the
// capture set, 11 for the quietly set) and sort that vector, then throw both
// away. It runs twice per dispatched line, so `local q 5` paid ~32 string
// allocations and two sorts to answer "does this line begin with `capture`?".
// The WS-D1 zone profile put the pair at 10.7% of the whole per-line budget,
// second only to the macro-definition regexes; the sampling profiler agreed
// (9.5%, of which 17% was the set->vector copy and 11% more the sort).
//
// The std::set stays the declared source of truth at each call site; the table
// is DERIVED from it in a function-local static, so it is built once and
// cannot drift from the set it came from. `first` is a first-byte bitmap that
// rejects a line which cannot begin with any spelling in the table before a
// single std::string is touched.
struct PrefixTable {
  std::vector<std::string> byLenDesc;  // longest-first (see the loop below)
  bool first[256] = {false};

  explicit PrefixTable(const std::set<std::string>& prefixes)
      : byLenDesc(prefixes.begin(), prefixes.end()) {
    std::sort(byLenDesc.begin(), byLenDesc.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& p : byLenDesc) {
      if (p.empty()) continue;
      unsigned char lo = static_cast<unsigned char>(p[0]);
      first[lo] = true;
      if (lo >= 'a' && lo <= 'z') first[lo - 'a' + 'A'] = true;
    }
  }
};


// ---------------------------------------------------------------------------
// M3b: bysort/by prefix — C++ port of bysort.py's _parse_bysort_prefix +
// run_bysort_cmd. Split out (not in the anonymous namespace above) only so
// it can stay grouped with its own small helpers; still translation-unit
// local.


struct BysortPrefix {
  bool isBysort = false;      // false = not a bysort/by line at all
  bool sortFirst = false;     // true=bysort (re-sort), false=by (no re-sort)
  std::vector<std::string> byVars;
  // BS-002: `bysort byvar (sortvar1 sortvar2):` — vars inside the
  // parenthetical are a SORT-ONLY tiebreak key (Stata: equivalent to `sort
  // byvar sortvar1 sortvar2` before grouping on byvar alone), never part of
  // the by-GROUP key. Empty when no parenthetical was present.
  std::vector<std::string> sortVars;
  std::string subCmd;
  std::string subRaw;
};


// BUG-532 (review of d82bdfa6, finding F): the by-keys and the parenthesised
// sort keys of a `by/bysort` prefix are a varlist, and Stata resolves them as
// one: a wildcard or range expands (loops::expandVarlist, the BUG-450 varlist
// expander every plain data verb uses), and a name that is not a variable but
// is the unique prefix of one stands for it (varname abbreviation; several
// candidates is r(111) "ambiguous abbreviation"). expandVarlist does not
// resolve abbreviations (a literal token passes through unchanged, and the
// plain `sort` verb answers `sort xv` r(111) on main too), so the prefix step
// is this function's own -- a design decision, no captured arm; [U] 11.2
// "Abbreviation rules". A token that matches nothing is left as typed: each
// branch below refuses it by its own rule (r(111) for keep/drop, the
// summarize driver's own check).
// FU-1088: shared through piet/handlers.h (binscatter's by()/controls()/
// absorb() resolve their names by this same rule), so it lives outside the
// anonymous namespace.

// BS-031(d)/(e) (BETA campaign C2, silent-if-drop sweep): the STRING gen/
// replace path (strmutate::runStringGen/runStringReplace, routed from the
// `generate`/`replace` arms below when needsStringEval() fires) only ever
// received `ifExpr` — never `parsed.inRange` — so `gen str8 sv = "x" in
// 1/10` silently set ALL 336 rows instead of just the 10 in-range ones
// (live-verified this stint: count if sv=="x" -> 10, count if sv=="" ->
// 326; `replace sv = "y" in 5/8` similarly: 4 changed, 332 unchanged). The
// numeric gen/replace path already folds `in a/b` into the same cond() the
// if-expr builds (BUG-077, egen.cpp's runBysortGen) — this is the
// IDENTICAL MVP grammar (bare INT, INT/INT, f/INT, INT/l, f/l; negative
// bounds rejected) reproduced here as a file-local helper so the STRING
// path (strmutate.cpp, a different TU with its own row-wise `_n`/`_N`
// evaluator — see strmutate.cpp's evalRows) can fold `in` into a combined
// RAW (un-rewritten) Stata-syntax ifExpr string BEFORE calling
// runStringGen/runStringReplace, which each internally rewrite_expr() the
// ifExpr they're given exactly like they already do for a real `if`. Not a
// new shared abstraction (matches this codebase's own established per-TU
// duplication convention for small helpers like this).
struct StringInRangeFold {
  bool ok = false;
  bool negative = false;
  // "" means neither `if` nor `in` was present (a genuinely absent cond).
  std::string combinedIf;
};


// Operand kinds: 0 = not a constant we fold, 1 = "literal", 2 = number/missing.
struct RowConstOperand {
  int kind = 0;
  size_t begin = 0;  // [begin, end) of the operand text incl. quotes
  size_t end = 0;
  std::string text;  // literal contents (kind 1) or the number token (kind 2)
};


// BUG-1015/BUG-1459: the version in force is SessionState::callerVersion. It
// is restored when the do-file or program that set it concludes.
struct CallerVersionScope {
  SessionState& s;
  double saved;
  explicit CallerVersionScope(SessionState& ss) : s(ss), saved(ss.callerVersion) {}
  ~CallerVersionScope() { s.callerVersion = saved; }
};


// BUG-1447: describe's r(k) and r(width) are the dataset's, whatever varlist was
// described (StataBE 19.5: `describe x1 c` on eight variables posts r(k) 8 and the
// full r(width) 36). r(width) is one observation's storage in bytes: byte 1, int 2,
// long and float 4, double and strL 8, str# its # (22 for float float byte double
// str5; 36 with an int, a long and a strL added). The types are the ones describe
// prints (the recorded storage type, else describe's default); a string whose width
// was never recorded ("str#") leaves the width unknown, and r(width) refused by name.
// Read off the resident card when it is current, as runDescribe reads it.
struct DescribeShape {
  size_t k = 0;
  std::optional<double> width;
};

struct RcHiddenScope {
  RcHiddenScope() { ++tl_rcHiddenDepth; }
  ~RcHiddenScope() { --tl_rcHiddenDepth; }
  RcHiddenScope(const RcHiddenScope&) = delete;
  RcHiddenScope& operator=(const RcHiddenScope&) = delete;
};


// W2 eststoprefix (BUG-520). `eststo`'s own options, read in ONE place so the
// prefix arm (Pre-0b) and the flat arm cannot drift: the prefix arm never
// reaches the tokenizer with its head, so a net that lived only in the flat arm
// would leave `eststo m1, bogus: regress y x` accepted-and-ignored.
struct EststoStoreOpts {
  std::optional<std::string> title;
  std::string autoPrefix = "est";  // estout's default; w2_eststo_prefix.log A2/A11
};


// BUG-524 (W2 labelglued; oracle w2_cmdtriage.log section B): Stata's `label`
// subcommands abbreviate down to a documented minimum each, and BELOW that
// minimum the line is r(198) "invalid syntax" -- not an unknown subcommand.
// Measured (each full spelling is its abbreviation's control):
//   label define / label def / la de        rc 0    (label_define_full, label_def, la_de)
//   label d                                 r(198)  (label_d_too_short)
//   label values / label val / label value  rc 0    (label_values_full, label_val, label_value)
//   label va                                r(198)  (label_va_too_short; f keeps yn3)
// The remaining floors are the syntax block of `help label`
// (ado/base/l/label.sthlp: `{opt da:ta}` `{opt var:iable}` `{opt di:r}`
// `{opt l:ist}` `{opt lang:uage}`; copy, drop, rename and save carry no colon
// and do not abbreviate) -- the published definition, not an oracle arm, and
// said so here. The command word's own floor (`la`) is the registry's row.
// PIET compared the token to the full spelling only, so about 1,070 corpus
// statements (`label val` 519, `label def` 453, `label de` 79 ...) were
// r(199) hard stops.
struct LabelSubFloor {
  const char* full;
  size_t minLen;
};

enum class LabelSubMatch { kResolved, kTooShort, kUnknown };

struct QualifierColumnGuard {
    SessionState& session;
    std::vector<std::string> columns;
    ~QualifierColumnGuard() {
      for (auto it = columns.rbegin(); it != columns.rend(); ++it) {
        try {
          strmutate::removeConditionMask(session, *it);
        } catch (...) {
        }
      }
    }
  };
}  // namespace piet::dispatch_detail
