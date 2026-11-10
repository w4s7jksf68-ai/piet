// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) -- the seventeen shape/sample verbs
// that change what the rows and columns of the dataset ARE:
//
//   split       one string column -> k string columns
//   xpose       the whole frame, transposed
//   recast      a declared Stata storage type (METADATA only)
//   insobs      blank observations, at the end or at a position
//   stack       k columns -> one column of k*n rows plus `_stack`
//   separate    one column -> one column per level of a by() variable
//   mvencode    missing -> a numeric code
//   sample      a random subset, by percent or by count
//   splitsample a group id assigning every row to one of nsplit() groups
//   drawnorm    multivariate normal draws (small dense Cholesky)
//   input       a do-file DATA block -> a dataset (or appended rows)
//   notes       the Stata `char` note table, on _dta and on variables
//   compare     two variables, side by side (display only)
//   cumul       the empirical cdf of a variable
//   bsample     a bootstrap resample of the data in memory
//   pctile      percentiles into a new variable
//   _pctile     the same percentiles into r(r1)..r(rN)
//
// WHY ONE TU, AND WHY NO NEW ENGINE VERB. Every one of these is a COMPOSITION
// of verbs cobra already has -- `setcol`, `convert`, `append`, `merge`, `eval`.
// Adding a `split` or an `xpose` verb to the engine would make this a
// two-binary change (engine + PIET), which is a different lane and a different
// release; the composition costs a CSV round trip per command and buys the
// whole group at once. Reading is `fetchColumns` with the materialized head as
// the fallback, writing is a cobra verb + `session.load` + `mutationResult` --
// the pair data_generate.cpp's runGen established and every data verb since has used.
//
// RANDOMNESS IS PHILOX, KEYED PER ROW (design decision 7). `sample`,
// `splitsample`, `bsample` and `drawnorm` never touch a global generator: each
// row's key is a pure function of (seed, row), so `set seed 7` followed by the
// same command twice gives the identical rows, and the answer does not depend
// on the order the rows were visited in. The seed is the command's own
// `seed()`/`rseed()` if it has one, else `session.seed`, else 123456789.
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at
// least once in this campaign; the first has been caught three times in four
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborted the whole selftest binary. dispatcher.cpp's bundle-F arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal -- but ONLY an array whose every element is a real
//     block object carrying a "type" key. Every `return` below is one object
//     with "type", or an ojson::array() of such objects (possibly empty, which
//     is how `recast` and `_pctile` say "nothing to print").
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`; a refusal built
//     with the default `engine_error` code silently becomes rc 1. So:
//     command-level refusals go through `eval::refusedCommand(cmd, cause)`,
//     feature refusals through `eval::unsupportedCommand(..., kNotImplemented)`,
//     USER SYNTAX errors carry "syntax_error", and an error with a known Stata
//     rc that coarseRc's message rules cannot recognise states it through the
//     `reason` channel (`rsErrorRc` below). Bundle C had to repair six sites
//     that had defaulted to rc 1.
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "generate"` comparison see them -- is driven by the
//     REGISTRY. A command absent from the vocabulary registry gets its
//     abbreviations accepted by the net and then dropped unread. All seventeen
//     command names register at NAMESPACE scope below (a function-local static
//     constructs too late: after the first call that needed it), and every
//     handler's first act is the reject net. An option this build does NOT read
//     goes in the `unimplemented` list, never in `known` -- bundle E shipped
//     `esttab, cells()` as known-and-unread and it had to be reverted.
//  4. ECHOED PATHS. Nothing here echoes a RESOLVED absolute path; there is no
//     `using` file in this bundle at all, so the class is structurally absent.
//  5. R-CLASS. `_pctile`'s entire purpose is r(r1)..r(rN), and its arm
//     deliberately does not call `captureRFromBlock` (there is no table block
//     to capture FROM), so the handler fills `session.eval.rValues` itself.
//     `split` fills r(nvars)/r(varlist) the same way.
//
// TWO FILES THIS LANE DOES NOT TOUCH, AND WHY THE COPY BELOW IS DELIBERATE.
// `estimation/bootstrap_prefix.cpp` owns `writeIndexCsv` in an ANONYMOUS namespace,
// so it is unreachable from here by construction, and the `kRowIdx` in
// `estimation/resampling_prefix_internal.h` is resample's own column name; `bsample`'s
// three-step index/merge shape is the same one `bootstrap` uses, so the twelve
// lines are copied rather than hoisted. Hoisting them would mean editing that
// file, which the plan puts out of scope for this bundle -- the shared-helper
// obligation (COMMS 4.9, third copy) is FILED here, not discharged: when a
// third caller appears, these two and resample's originals become one helper.
// `session_state.h` is likewise untouched: `ScratchPin` and
// `noteProjectionScratch` are used exactly as they already exist.
#pragma once
#include "vendor/nlohmann/json.hpp"

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace dataset_shape_detail {

ojson rsError(const std::string& message, const std::string& code = "engine_error");
ojson rsSyntax(const std::string& message);
ojson rsErrorRc(const std::string& message, int rc);
bool isErrorBlock(const ojson& b);
ojson displayBlock(const std::string& text);
ojson silent();

std::string nextPath(const std::string& workdir, const std::string& prefix);
std::optional<ojson> runVerb(const std::vector<std::string>& args);

// ===========================================================================
// Reading
// ===========================================================================

// The shared READ shape: ask the worker for a contract_A section-38 projection
// of just the columns this command needs, and fall back to the session's own
// materialized head whenever the projection is unavailable (no live worker,
// head not resident-dirty, request refused). A miss must cost a fallback, never
// an answer -- column_fetch.h's own law. The projection file is registered with
// the session's projection ledger by fetchColumns itself.
class ReadFrame {
 public:
  ReadFrame(SessionState& session, const std::vector<std::string>& cols) {
    try {
      if (!cols.empty()) {
        std::vector<const std::string*> exprs;
        exprs.reserve(cols.size());
        for (const auto& c : cols) exprs.push_back(&c);
        if (auto want = referencedColumns(session, exprs, "")) {
          std::string projPath;
          owned_ = fetchColumns(session, *want, session.workdir(), &projPath);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      ds_ = nullptr;
      why_ = e.what();
    }
  }
  Datastore* get() const { return ds_; }
  const std::string& why() const { return why_; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  std::string why_;
};

std::string numCell(double v);

// A whole table on its way to `cobra convert`: a header row, then the cells.
// Factor columns are named explicitly rather than left to type inference,
// because a column whose visible rows happen to be all-numeric (or all-blank,
// which is every padded row an `insobs, before()` writes) would otherwise come
// back f64 and collide with the master's dtype at the append.
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                     // parallel to `names`
  std::vector<std::vector<std::string>> rows;     // rows[r][c]; "" == missing
};

std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath);
std::optional<ojson> appendColumn(SessionState& session, const std::string& basePath,
                                  const std::string& var, const std::vector<std::string>& cells,
                                  const std::string& dtype, std::string* outPath);

std::string lowerOf(std::string s);
std::string trimOf(const std::string& s);
std::vector<std::string> splitWs(const std::string& s);
std::string joinWith(const std::vector<std::string>& v, const std::string& sep);
bool parseStrictDouble(const std::string& sIn, double* out);
bool parseStrictInt(const std::string& s, long long* out);

bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key);
std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);
std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys);

uint64_t drawSeed(SessionState& session, const std::optional<std::string>& optSeed);
uint32_t rowKey32(uint64_t seed, int64_t j);

std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name);
std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name);
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar);
std::optional<ojson> assertRowCount(SessionState& session, const std::string& verb,
                                    int64_t expected);
std::optional<ojson> refuseQualifiers(const eval::ParsedCommand& parsed, const std::string& verb);

}  // namespace dataset_shape_detail

}  // namespace piet::handlers
