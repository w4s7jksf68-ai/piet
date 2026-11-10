// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/gtools_aliases.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "gtools_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace gtools_detail;

namespace {

bool parseIntStrict(const std::string& s, long long* out) {
  if (s.empty()) return false;
  const char* begin = s.c_str();
  char* end = nullptr;
  const long long v = std::strtoll(begin, &end, 10);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

// gtools' _pctile with no options returns the median, i.e. the nquantiles(2)
// split, matching Stata's own `_pctile`. Same number as above, different
// command, so it is named separately rather than shared by coincidence.
constexpr long long kPctileDefaultNq = 2;

// ---------------------------------------------------------------------------
// Percentile rule, and the scratch/setcol sequence the pctile form writes with.
// ---------------------------------------------------------------------------

// TRANSCRIBED FROM src/data/frame.cpp::stata_pctile, term for term.
//
// This is not a convenience copy. PIET and the engine both answer percentile
// questions -- egen median, summarize,detail and egen pctile go through the
// engine's copy, `gquantiles ..., pctile` goes through this one -- and if the
// two rules differ then the same dataset has two different medians depending on
// which command asked. The averaging branch is the whole rule: when N*p/100
// lands exactly on an integer rank, Stata averages that observation with the
// next one instead of taking either.
double stataPctile(const std::vector<double>& sorted, double p) {
  const int64_t N = static_cast<int64_t>(sorted.size());
  if (N == 0) return std::numeric_limits<double>::quiet_NaN();
  if (N == 1) return sorted[0];
  const double Pi = static_cast<double>(N) * p / 100.0;
  const double fl = std::floor(Pi);
  if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < N) {
    return (sorted[static_cast<size_t>(static_cast<int64_t>(Pi) - 1)] +
            sorted[static_cast<size_t>(static_cast<int64_t>(Pi))]) /
           2.0;
  }
  int64_t idx = static_cast<int64_t>(fl) + 1;
  if (idx > N) idx = N;
  if (idx < 1) idx = 1;
  return sorted[static_cast<size_t>(idx - 1)];
}

// mintScratchPath always names a ".hbin"; the setcol payload is a CSV.
// Registering the minted .hbin name and then writing a .csv beside it would
// leave the file that actually exists unregistered, which is the BUG-389 orphan
// class the mint registry exists to close (sweepOrphanScratch only removes
// paths it knows). Same helper, same reason, as winsor.cpp.
std::string mintCsvPath(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  return csv;
}

// The headerless single-column payload `cobra setcol` reads
// (Frame::read_csv_column -- one raw value per row, row count == n_rows). An
// EMPTY line is how a missing value is spelled in that format; writing "nan" or
// "." instead would land a literal token in a numeric column.
bool writeValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// cobra reports a structured failure as {"error":{"code","message"}} on stderr.
// Surfacing the raw stderr blob instead would put JSON punctuation in front of
// the user; surfacing nothing would make a failed setcol indistinguishable from
// a successful one at the call site.
std::string cobraFailure(const CobraRunResult& r) {
  std::string text = r.stderr_text;
  if (!text.empty()) {
    try {
      const nlohmann::json parsed = nlohmann::json::parse(text);
      if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object() &&
          parsed["error"].contains("message")) {
        text = parsed["error"]["message"].get<std::string>();
      }
    } catch (const std::exception&) {
      // Not structured: the raw text below is the best available account.
    }
  }
  if (text.empty()) text = r.error;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  if (text.empty()) text = "cobra setcol failed with exit code " + std::to_string(r.exit_code);
  return text;
}

// r(r1), r(r2), ... are POSITIONAL, and PIET's r() store is never cleared
// between commands (dispatcher.cpp clears it only for `return clear`). So a
// `gquantiles x, _pctile nq(5)` leaves r(r3)/r(r4) behind, and a following
// nq(3) call that writes only r(r1)/r(r2) would let those two stale numbers be
// read as this call's answer. Stata REPLACES r() on every r-class command; the
// sweep below is how that is spelled here. Only the rN family is removed --
// r(N) and anything else are rewritten or left alone on their own terms.
void clearPositionalRValues(SessionState& session) {
  std::vector<std::string> doomed;
  for (const auto& kv : session.eval.rValues.items()) {
    const std::string& k = kv.first;
    if (k.size() < 2 || k[0] != 'r') continue;
    bool allDigits = true;
    for (size_t i = 1; i < k.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(k[i])) == 0) allDigits = false;
    }
    if (allDigits) doomed.push_back(k);
  }
  for (const auto& k : doomed) session.eval.rValues.erase(k);
}

// Publishing k scalars into an OrderedFloatDict costs O(k^2) -- its set() is a
// linear scan by design (eval/session.h). A typo'd nq(1000000) is therefore a
// hang wearing a computation's clothes, so the count is bounded and the bound
// is REPORTED rather than silently truncated.
constexpr size_t kMaxPercentileCount = 10000;

}  // namespace

// ---------------------------------------------------------------------------
// (3) gquantiles _pctile / pctile.
// ---------------------------------------------------------------------------

ojson runGQuantilesPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  const eval::OptVocab* vocab = eval::findOptVocab("gquantiles");
  if (vocab != nullptr) {
    // The refused keys were already answered upstream in translateGtoolsAlias,
    // so this net is here for the keys nobody named: without it an unknown
    // option is accepted and never read, which is the silent-ignore shape.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, vocab->known, vocab->unimplemented)) {
      return ojson(*err);
    }
  }

  const ModeWord mode = readModeWord(parsed);
  if (mode.mode != "_pctile" && mode.mode != "pctile") {
    // The dispatcher only routes here after the translation step set canonical
    // to "gquantiles", which happens for exactly these two modes.
    return gtErr("gquantiles: expected the `_pctile` or `pctile` form here", "syntax_error");
  }
  const bool wantVariable = (mode.mode == "pctile");

  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    // Same refusal, for the same reason, as runXtile's: Stata computes the
    // cutpoints from the RESTRICTED sample. Computing them over every row and
    // then reporting them would be the right shape carrying the wrong numbers.
    return ojson(eval::unsupportedCommand(
        "gquantiles with `if` (percentiles must come from the restricted sample; restrict first "
        "with `preserve` + `keep if ...`, then run gquantiles)",
        std::nullopt, std::nullopt, "refused:gquantiles_if",
        eval::UnsupportedCommandKind::kNotImplemented));
  }

  // `[newvar =] source`, with the mode word removed. The tokenizer splits the
  // pre-comma head on whitespace, so `new = x` arrives as three tokens and
  // `new=x` as one -- rejoining and splitting on '=' handles both.
  std::string joined;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    if (mode.fromVarlist && i == mode.varlistIndex) continue;
    if (!joined.empty()) joined += " ";
    joined += parsed.varlist[i];
  }
  std::string target;
  std::string source;
  const size_t eq = joined.find('=');
  if (eq == std::string::npos) {
    source = strutil::trim(joined);
  } else {
    target = strutil::trim(joined.substr(0, eq));
    source = strutil::trim(joined.substr(eq + 1));
  }
  if (source.empty()) {
    return gtErr("gquantiles: no source variable specified", "syntax_error");
  }
  if (wantVariable && target.empty()) {
    return gtErr("gquantiles ... , pctile requires a new variable: `gquantiles newvar = varname, "
                 "pctile nquantiles(#)`",
                 "syntax_error");
  }
  if (!wantVariable && !target.empty()) {
    // `_pctile` returns r() only. Accepting a target and never creating it
    // would report success for a variable that does not exist afterwards.
    return gtErr("gquantiles ... , _pctile does not create a variable (it returns r(r1), r(r2), "
                 "...); use `pctile` if you want one",
                 "syntax_error");
  }

  // Percentile list: percentiles() as typed, else the nquantiles() split.
  std::vector<double> pcts;
  std::string pctSpec;
  bool havePctSpec = false;
  bool haveNq = false;
  long long nq = kPctileDefaultNq;
  for (const auto& kv : parsed.opts) {
    if (kv.first == "percentiles") {
      if (kv.second.isBool) {
        return gtErr("gquantiles: percentiles() requires a list of numbers", "syntax_error");
      }
      pctSpec = kv.second.strValue;
      havePctSpec = true;
    } else if (kv.first == "nquantiles") {
      if (kv.second.isBool || !parseIntStrict(strutil::trim(kv.second.strValue), &nq)) {
        return gtErr("gquantiles: nquantiles() requires a whole number", "syntax_error");
      }
      haveNq = true;
    }
  }
  if (havePctSpec && haveNq) {
    // The two are ALTERNATIVE sources of the same list, and percentiles() wins
    // below. Letting it win silently would mean `nq(10)` produced a 2-way split
    // whenever a percentiles() sat beside it -- an ignored option that changes
    // the answer, which is the shape this bundle exists to refuse.
    return gtErr("gquantiles: percentiles() and nquantiles() are alternatives -- percentiles() "
                 "lists the cut percentages directly, nquantiles(#) asks for an even split into "
                 "# groups. Give one of them.");
  }
  if (havePctSpec) {
    // `percentiles(10, 50)` and `percentiles(10 50)` are both real spellings,
    // and the option value arrives with its interior comma intact because the
    // tokenizer's split is paren-aware.
    std::string normalised = pctSpec;
    std::replace(normalised.begin(), normalised.end(), ',', ' ');
    for (const auto& w : splitWords(normalised)) {
      double p = 0.0;
      if (!parseDoubleStrict(w, &p)) {
        return gtErr("gquantiles: percentiles(" + pctSpec + ") -- `" + w + "` is not a number",
                     "syntax_error");
      }
      if (!(p > 0.0 && p < 100.0)) {
        return gtErr("gquantiles: percentiles(" + pctSpec +
                         ") -- every percentile must satisfy 0 < p < 100");
      }
      pcts.push_back(p);
    }
    if (pcts.empty()) {
      return gtErr("gquantiles: percentiles() is empty", "syntax_error");
    }
  } else {
    if (nq < 2) {
      return gtErr("gquantiles: nquantiles() must be at least 2", "syntax_error");
    }
    if (static_cast<unsigned long long>(nq - 1) > kMaxPercentileCount) {
      return gtErr("gquantiles: nquantiles(" + std::to_string(nq) + ") asks for " +
                       std::to_string(nq - 1) + " percentiles; this build publishes at most " +
                       std::to_string(kMaxPercentileCount) + ".");
    }
    for (long long i = 1; i < nq; ++i) {
      pcts.push_back(100.0 * static_cast<double>(i) / static_cast<double>(nq));
    }
  }
  if (pcts.size() > kMaxPercentileCount) {
    return gtErr("gquantiles: " + std::to_string(pcts.size()) +
                     " percentiles requested; this build publishes at most " +
                     std::to_string(kMaxPercentileCount) + ".");
  }

  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  // contract_A section 38 projection: this handler reads ONE column, and on a
  // dirty virtual head opening the whole datastore would materialize the entire
  // frame to get at it. `target` rides in as an EXPRESSION rather than via
  // alsoInclude because referencedColumns only reports identifiers the shadow
  // card CONFIRMS -- so "the projection came back carrying target" is exactly
  // the statement "that variable already exists", which is the check this
  // handler owes before it writes. When the card cannot be trusted the
  // projection is nullopt and both questions fall back to the real datastore,
  // where they are direct. Same shape as runLevelsof and runWinsorCox.
  const std::string* targetExpr = target.empty() ? nullptr : &target;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  if (auto cols = referencedColumns(session, {targetExpr}, source)) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  if (mini && !mini->has(source)) mini.reset();  // subject absent -> not usable
  Datastore* ds = mini ? mini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  if (!ds->has(source)) {
    // "variable ... not found" is the wording eval::coarseRc maps to Stata's
    // r(111); rephrasing it would silently downgrade the rc to the generic 1.
    return gtErr("gquantiles: variable " + source + " not found");
  }
  if (ds->dtype(source) != "f64") {
    return gtErr("gquantiles: " + source +
                 " is a string variable; gquantiles needs a numeric variable");
  }
  if (wantVariable) {
    if (!strutil::isValidStataIdentifier(target)) {
      return gtErr("gquantiles: invalid variable name `" + target + "' -- " +
                       strutil::stataIdentifierRuleText(),
                   "syntax_error");
    }
    if (ds->has(target)) {
      // "already defined" is the wording coarseRc maps to r(110) (confirm new
      // variable). Same reason as above: the phrase is load-bearing.
      return gtErr("gquantiles: variable " + target + " already defined");
    }
  }

  const cecon::Column& column = ds->col(source);
  const double* data = column.f64();
  // n_values(), NOT nbytes/sizeof(double). `nbytes` is the payload width, and a
  // narrow (Narrow::F32) column carries FOUR bytes per observation while f64()
  // widens it into a cache of n_values() doubles -- so the byte division reads
  // half the row count and the guard below then rejects a perfectly good column
  // as "not full-length". io/columnar.h pairs f64() with n_values() for exactly
  // this reason; every other reader in the tree (winsor2's clampBoundaries) uses
  // the row count rather than deriving one from bytes.
  const int64_t count = column.n_values();
  if (data == nullptr || count != ds->nRows()) {
    return gtErr("gquantiles: could not read " + source + " as a full-length numeric column");
  }

  std::vector<double> sorted;
  sorted.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    if (!std::isnan(data[i])) sorted.push_back(data[i]);
  }
  if (sorted.empty()) {
    return gtErr("gquantiles: " + source + " has no non-missing observations");
  }
  std::sort(sorted.begin(), sorted.end());

  std::vector<double> values;
  values.reserve(pcts.size());
  for (double p : pcts) values.push_back(stataPctile(sorted, p));

  // Both forms are r-class, so both must REPLACE the store rather than add to
  // it. PIET never clears r() between commands (only `return clear` does), so
  // clearing on one arm only meant `gquantiles y, _pctile nq(5)` followed by
  // `gquantiles q = y, pctile nq(3)` left r(r4) readable and stale -- published
  // by the FIRST command, read as if it belonged to the second. Stata replaces
  // r() on every r-class command, and pctile is one.
  clearPositionalRValues(session);

  if (!wantVariable) {
    for (size_t i = 0; i < values.size(); ++i) {
      session.eval.rValues.set("r" + std::to_string(i + 1), values[i]);
    }
    // r(N) is the count the percentiles were computed FROM, i.e. the
    // non-missing observations, not the row count and not the number of
    // percentiles. gtools reports the same quantity.
    session.eval.rValues.set("N", static_cast<double>(sorted.size()));
    // Silent success. An EMPTY array, never a one-element one: the dispatcher
    // arm pushes whatever non-empty value comes back as a single block, and a
    // consumer reading "type" off an array aborts with nlohmann type_error.306.
    return ojson::array();
  }

  const int64_t nRows = ds->nRows();
  if (static_cast<int64_t>(values.size()) > nRows) {
    return gtErr("gquantiles: " + std::to_string(values.size()) +
                 " percentiles cannot be stored in a dataset of " + std::to_string(nRows) +
                 " observations");
  }
  std::vector<double> outCol(static_cast<size_t>(nRows),
                             std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < values.size(); ++i) outCol[i] = values[i];

  const std::string csvPath = mintCsvPath(session.workdir(), "gquantiles_pctile_vals");
  if (!writeValuesCsv(csvPath, outCol)) {
    return gtErr("gquantiles: could not write the percentile values");
  }

  // The new column is patched into the REAL head, not into the projection
  // above: the projection carries only the columns this handler fetched, so
  // writing to it would silently drop every other variable in the dataset.
  const std::string outPath = mintScratchPath(session.workdir(), "gquantiles_pctile");
  const CobraRunResult r =
      runCobra({"setcol", "--data", *session.currentHbin(), "--var", target, "--values", csvPath,
                "--dtype", "f64", "--new", "--out", outPath});
  if (!r.ok) return gtErr("gquantiles: " + cobraFailure(r));
  // BUG-124's torn-write guard, carried from the other stateless setcol sites:
  // the engine's writer creates `--out` before it can detect a write failure,
  // and a full scratch volume has twice reached PIET as a ZERO exit code.
  // session.load on a path that is not there does not fail -- it records the
  // head and hands the next Datastore open a missing or truncated mapping,
  // which is a crash rather than a diagnostic. This handler never opens a
  // ResidentEligible scope, so `--out` is always a real file here.
  if (!cecon::native_exists(outPath)) {
    return gtErr("gquantiles: failed writing output dataset (disk full or I/O error)");
  }

  session.load(outPath);
  // "gen", not "gquantiles": the mutation IS a pure add of one new column,
  // which is the branch seedNMissForPureAdd must take for the untouched columns
  // to carry their missing-counts forward.
  return mutationResult(session, "gen", {target});
}

}  // namespace piet::handlers
