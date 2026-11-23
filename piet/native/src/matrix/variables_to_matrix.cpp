// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/results.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/stata_format.h"  // string(): a numeric rownames()/roweq() value is %9.0g
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "data/missing.h"  // cecon::is_missing
#include "piet/handlers/variables.h"  // resolveByKeyNames
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "matrix_command_errors.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace matrix_command_detail;

namespace {

// ---------------------------------------------------------------------------
// Option vocabularies -- NAMESPACE SCOPE, not function-local statics.
//
// The registry is consulted by the DISPATCHER (canonicalizeForCommand, before
// the handler runs). A function-local static would only be constructed the
// first time the handler is entered, i.e. AFTER the one call that needed it, so
// the very first `svmat A, name(col)` in a session would canonicalize against
// an empty registry and land in the handler still spelled "name". Namespace
// scope registers at static-init time, which is what the dispatcher's ordering
// requires. registerOptVocab is documented safe there (function-local static
// registry, no cross-TU init order).

// `mat` is listed alongside `matrix` because [P] mkmat documents both
// spellings; a 1-char-longer prefix like `matr` still resolves (unique among
// the two), while `ma` is ambiguous and rejects -- which is Stata's answer too.
// roweq()/nchar() are declared UNIMPLEMENTED rather than omitted: omitting them
// makes them "unknown option" (a wrong diagnosis), and accepting them would
// silently ignore an equation-name or name-truncation request.
const eval::OptVocab& kMkmatVocab = eval::registerOptVocab(
    {"mkmat"},
    {{"matrix", "mat", "rownames", "nomissing", "obs", "rowprefix", "roweq", "nchar"}, {}});

// Replace-or-append into session.eval.matrices. matrix_expression_internal.h's
// storeMatrixResult does the same three lines, but this copy is file-local
// (anonymous namespace) -- appending a duplicate instead would leave
// findMatrix() answering the OLD entry forever, since it returns the first
// match. A scalar of the same name is destroyed: scalars and matrices share one
// namespace ([P] matrix define, "Namespace").
void storeMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) {
      p.second = std::move(mat);
      return;
    }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

}  // namespace

// ---------------------------------------------------------------------------
// mkmat

// The if/in filter that keeps each row's position in the working frame
// (engine/inrange_filter.cpp; declared at its callers, as in estimation/linear_estimation.cpp).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar);

namespace {

// A row name derived from a variable's VALUE. Stata's mkmat rownames() names
// each row after that observation's value, so an integer must not print as
// "1.000000" -- an integer-valued double is emitted with no decimal point at
// all. PIET stores a genuine STRING variable as a factor column, so a factor's
// level string IS the value here; there is no value-label lookup, because
// rownames() takes the value and a value-labelled numeric's value is its number.
std::string rowNameFromValue(const Datastore& ds, const std::string& var, int64_t row) {
  const cecon::Column& c = ds.col(var);
  if (ds.dtype(var) == "factor") {
    const int32_t code = c.codes()[row];
    if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) return ".";
    return c.levels[static_cast<size_t>(code)];
  }
  const double v = c.f64()[row];
  if (std::isnan(v)) return ".";
  const double rounded = std::round(v);
  if (v == rounded && std::fabs(rounded) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(rounded));
    return std::string(buf);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return std::string(buf);
}

}  // namespace

ojson runMkmat(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kMkmatVocab.known,
                                            kMkmatVocab.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) return aerError("mkmat: no variables specified");
  if (!session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string spec;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    if (i) spec += " ";
    spec += parsed.varlist[i];
  }
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) return ojson(ex.error);
  const std::vector<std::string> vars = ex.tokens;
  if (vars.empty()) return aerError("mkmat: no variables specified");

  std::string matName;
  std::string rowVar;
  std::string rowPrefix = "r";
  bool nomissing = false;
  bool useObsNumbers = false;
  std::string roweqVar;
  int nchar = 32;
  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "matrix" || k == "mat") {
      if (v.isBool || v.strValue.empty()) return aerError("mkmat: option matrix() requires a name");
      matName = strutil::trim(v.strValue);
    } else if (k == "rownames") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("mkmat: option rownames() requires a variable name");
      }
      rowVar = strutil::trim(v.strValue);
    } else if (k == "rowprefix") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("mkmat: option rowprefix() requires a string");
      }
      rowPrefix = strutil::trim(v.strValue);
    } else if (k == "nomissing") {
      nomissing = true;
    } else if (k == "obs") {
      useObsNumbers = true;
    } else if (k == "roweq") {
      if (v.isBool || v.strValue.empty()) return aerErrorRc("option roweq() incorrectly specified", 198);
      roweqVar = strutil::trim(v.strValue);
    } else if (k == "nchar") {
      // numlist integer max=1 >=1 <=32
      const std::string t = strutil::trim(v.strValue);
      try {
        size_t pos = 0;
        const double d = std::stod(t, &pos);
        if (pos != t.size()) throw std::invalid_argument("n");
        if (d != std::floor(d)) return aerErrorRc("nchar() invalid -- invalid numlist has noninteger elements", 126);
        if (d < 1 || d > 32)
          return aerErrorRc("nchar() invalid -- invalid numlist has elements outside of allowed range", 125);
        nchar = static_cast<int>(d);
      } catch (const std::exception&) {
        return aerErrorRc("nchar() invalid -- invalid number", 121);
      }
    }
  }
  // StataBE 19.5: `mkmat x y, matrix(M) obs rownames(v)` is r(198), with or
  // without if/in -- a row takes one name.
  if (useObsNumbers && !rowVar.empty())
    return aerErrorRc("options obs and rownames() are exclusive", 198);
  if (!matName.empty() && !strutil::isValidStataIdentifier(matName)) {
    return aerError("mkmat: `" + matName + "' is not a valid matrix name. " +
                        strutil::stataIdentifierRuleText(),
                    "syntax_error");
  }

  const std::string basePath = *session.currentHbin();
  InRangeFilterResult fr;
  // `obs` names a row after the observation it came from, so a filtered copy
  // keeps each row's position in the data (indexVar names that column).
  std::string indexVar;
  try {
    fr = useObsNumbers ? applyIfAndInFilterKeepingIndex(session, basePath, parsed.ifExpr,
                                                        parsed.inRange, &indexVar)
                       : applyIfAndInFilter(session, basePath, parsed.ifExpr, parsed.inRange);
  } catch (const CobraError& ce) {
    return aerError("mkmat: " + cobraErrorMessage(ce), ce.code);
  }
  if (!fr.ok) {
    return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "mkmat");
  }

  // applyIfAndInFilter hands back basePath itself when neither qualifier is
  // present, and a fresh scratch .hbin otherwise. Reading through
  // session.datastore() in the first case reuses the already-open mapping; the
  // second needs its own, because the session head is NOT the filtered file and
  // mkmat must never move it (this is a read-only command).
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = nullptr;
  if (fr.dataPath == basePath) {
    ds = session.datastore();
  } else {
    try {
      filtered = std::make_unique<Datastore>(fr.dataPath);
    } catch (const std::exception& e) {
      return aerError(std::string("mkmat: could not read the filtered data: ") + e.what());
    }
    ds = filtered.get();
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  for (const auto& v : vars) {
    if (!ds->has(v)) {
      // coarseRc maps "variable"+"not found" to r(111); the wording is
      // load-bearing.
      return aerError("mkmat: variable " + v + " not found");
    }
    if (ds->dtype(v) != "f64") {
      // Stata refuses these too: a matrix holds numbers, and PIET's `factor`
      // dtype is how a genuine string variable is stored. Stata answers r(109).
      return aerErrorRc("mkmat: " + v + " is a string variable; mkmat needs numeric variables",
                        109);
    }
  }
  if (!rowVar.empty() && !ds->has(rowVar)) {
    return aerError("mkmat: variable " + rowVar + " not found");
  }
  if (!roweqVar.empty()) {
    std::vector<std::string> one = {roweqVar};
    if (auto bad = resolveByKeyNames(session, one)) return *bad;
    roweqVar = one[0];
    if (!ds->has(roweqVar)) return aerError("mkmat: variable " + roweqVar + " not found");
  }
  std::vector<std::string> rowEqs;

  const int64_t nRows = ds->nRows();
  std::vector<const double*> cols;
  cols.reserve(vars.size());
  for (const auto& v : vars) {
    const cecon::Column& c = ds->col(v);
    const double* p = c.f64();
    if (p == nullptr || c.n_values() != nRows) {
      return aerError("mkmat: could not read " + v + " as a full-length numeric column");
    }
    cols.push_back(p);
  }
  const double* obsIndex = nullptr;
  if (!indexVar.empty()) {
    if (!ds->has(indexVar) || ds->col(indexVar).f64() == nullptr)
      return aerError("mkmat: could not read the observation numbers of the filtered data");
    obsIndex = ds->col(indexVar).f64();
  }

  // The sample: LISTWISE, not per-cell -- `nomissing` drops the OBSERVATION
  // when any selected variable is missing there, which is what keeps the k
  // columns of the result aligned to one another. Without the option a missing
  // value is carried into the matrix as missing rather than silently dropped.
  std::vector<int64_t> sample;
  for (int64_t i = 0; i < nRows; ++i) {
    bool anyMissing = false;
    for (const double* p : cols) anyMissing = anyMissing || std::isnan(p[i]);
    if (nomissing && anyMissing) continue;
    sample.push_back(i);
  }
  // A rownames() or roweq() variable is checked over the whole sample, rownames()
  // first, each r(198): a string one must have no empty value; a numeric one no
  // missing value, then integer values only, then none below zero (vs-stata
  // bank d1ca14d19d129ebd for the missing-value refusal). A numeric value
  // becomes its string(), i.e. %9.0g: 123456792 is 1.23e+08.
  const auto checkStripeVar = [&](const std::string& optn,
                                  const std::string& var) -> std::optional<ojson> {
    if (var.empty()) return std::nullopt;
    if (ds->dtype(var) == "factor") {
      for (int64_t i : sample)
        if (rowNameFromValue(*ds, var, i).empty())
          return aerErrorRc(optn + "() invalid; " + var + " contains empty strings", 198);
      return std::nullopt;
    }
    const double* v = ds->col(var).f64();
    if (v == nullptr) return aerErrorRc(optn + "() invalid; " + var + " contains missing values", 198);
    for (int64_t i : sample)
      if (cecon::is_missing(v[i]))
        return aerErrorRc(optn + "() invalid; " + var + " contains missing values", 198);
    for (int64_t i : sample)
      if (v[i] != std::round(v[i]))
        return aerErrorRc(optn + "() invalid; " + var + " is not integer valued", 198);
    for (int64_t i : sample)
      if (v[i] < 0) return aerErrorRc(optn + "() invalid; " + var + " is not strictly positive", 198);
    return std::nullopt;
  };
  if (auto e = checkStripeVar("rownames", rowVar)) return *e;
  if (auto e = checkStripeVar("roweq", roweqVar)) return *e;
  const auto stripeText = [&](const std::string& var, int64_t i) -> std::string {
    if (ds->dtype(var) == "factor") return rowNameFromValue(*ds, var, i);
    const double v = ds->col(var).f64()[i];
    return fmt::general(v == 0.0 ? 0.0 : v, 9, 0);
  };

  std::vector<std::vector<double>> rows;
  std::vector<std::string> rowNames;
  for (int64_t i : sample) {
    std::vector<double> row;
    row.reserve(cols.size());
    for (const double* p : cols) row.push_back(p[i]);
    rows.push_back(std::move(row));
    // The row NUMBER a default name is built from is the position in the
    // matrix: `r3` is its third row. `obs` names the row after its observation
    // instead, prefix kept (BUG-403; StataBE 19.5, 2026-11-03 oracle: rows from
    // observations 1 and 3 are r1 r3, `if y > 3` gives r4 r5 r6, and
    // rowprefix(p) p3 p4 ...).
    const size_t k = rows.size();
    if (!roweqVar.empty()) rowEqs.push_back(stripeText(roweqVar, i));
    if (!rowVar.empty()) {
      rowNames.push_back(stripeText(rowVar, i));
    } else if (useObsNumbers) {
      const int64_t obs =
          obsIndex != nullptr ? static_cast<int64_t>(std::llround(obsIndex[i])) : i + 1;
      rowNames.push_back(rowPrefix + std::to_string(obs));
    } else {
      rowNames.push_back(rowPrefix + std::to_string(k));
    }
  }

  // mkmat counts its sample (`count if touse`) before anything else can fail
  // and is no r-class program itself, so that count's r() is what it leaves:
  // r(N), the rows it takes (vs-stata bank 8f34184c9b65d0aa `mkmat price mpg in
  // 1/5` r(N) 5, b21dd3f21df74b9f r(N) 74).
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", static_cast<double>(rows.size()));
  eval::declareRPosted(session.eval, "mkmat", {"N"});

  if (rows.empty()) {
    // Stata's r(2000). coarseRc has no message rule for this phrase, so the rc
    // is stated explicitly.
    return aerErrorRc("mkmat: no observations", 2000);
  }

  // A row name has its blanks and periods made underscores and is cut to
  // nchar() characters (default 32); so is a string roweq() equation, while a
  // numeric one is its string() unchanged. Each row is then eq:name (StataBE
  // 19.5, vs-stata bank 365324ea, 532ce902, 696ef3d2).
  const auto cleanUp = [nchar](std::string t) {
    for (char& ch : t)
      if (ch == ' ' || ch == '.') ch = '_';
    size_t cut = 0, cps = 0;
    while (cut < t.size() && cps < static_cast<size_t>(nchar)) {
      const unsigned char b = static_cast<unsigned char>(t[cut]);
      cut += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
      ++cps;
    }
    return t.substr(0, std::min(cut, t.size()));
  };
  for (auto& nm : rowNames) nm = cleanUp(nm);
  if (!roweqVar.empty()) {
    const bool strEq = ds->dtype(roweqVar) == "factor";
    for (size_t r = 0; r < rowNames.size() && r < rowEqs.size(); ++r)
      rowNames[r] = (strEq ? cleanUp(rowEqs[r]) : rowEqs[r]) + ":" + rowNames[r];
  }

  if (!matName.empty()) {
    eval::EvalMatrix m;
    m.data = std::move(rows);
    m.rownames = rowNames;
    m.colnames = vars;
    storeMatrix(session, matName, std::move(m));
  } else {
    // No matrix(): one N x 1 matrix per variable, each named after the variable
    // it came from -- [P] mkmat's documented default.
    for (size_t j = 0; j < vars.size(); ++j) {
      eval::EvalMatrix m;
      m.data.reserve(rows.size());
      for (const auto& row : rows) m.data.push_back({row[j]});
      m.rownames = rowNames;
      m.colnames = {vars[j]};
      storeMatrix(session, vars[j], std::move(m));
    }
  }

  // EMPTY array = silent success, which is what Stata's mkmat is. A non-empty
  // array here would be defect class 1 (see this file's header): the dispatcher
  // arm pushes this value as ONE block without flattening it.
  return ojson::array();
}

}  // namespace piet::handlers
