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
#include "piet/display_hardening.h"
#include "piet/inrange_filter.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "engine/scratch_columns.h"
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

namespace {

// gtools picks between sorting and selecting from a cost estimate and returns the ratio of the
// two as r(method_ratio): with n the rows read and `out` the quantiles asked for, sorting costs
// log(n) + 1 operations plus a pass for each extra output (xtile: 2 log(n) + 3 against out + 2;
// percentiles only: log(n) + 1 against out).
double gtoolsMethodRatio(double rows, double out, bool forXtile) {
  const double sortCost = forXtile ? 2.0 * std::log(rows) + 3.0 : std::log(rows) + 1.0;
  const double selectCost = forXtile ? out + 2.0 : out;
  return sortCost / selectCost;
}

void putRMatrixColumn(SessionState& session, const std::string& name,
                      const std::vector<double>& values) {
  eval::EvalMatrix m;
  for (size_t i = 0; i < values.size(); ++i) {
    m.data.push_back({values[i]});
    m.rownames.push_back("r" + std::to_string(i + 1));
  }
  m.colnames.push_back("c1");
  m.gen = eval::nextStoreGeneration();
  for (auto it = session.eval.rMatrices.begin(); it != session.eval.rMatrices.end(); ++it) {
    if (it->first == name) {
      session.eval.rMatrices.erase(it);
      break;
    }
  }
  session.eval.rMatrices.emplace_back(name, std::move(m));
}

}  // namespace

// gquantiles ..., xtile leaves r(N) (non-missing observations), r(nquantiles) and r(nqused).
void gquantilesPublishXtile(const eval::ParsedCommand& parsed, SessionState& session,
                            const std::string& expText) {
  if (!session.hasData()) return;
  long long nq = 0;
  for (const auto& kv : parsed.opts) {
    if (kv.first == "cutpoints" || kv.first == "altdef") {
      if (kv.first == "cutpoints") return;
    }
    if (kv.first == "nquantiles" && !kv.second.isBool) parseIntStrict(strutil::trim(kv.second.strValue), &nq);
  }
  if (nq < 2) return;
  const std::string savedLeaf = session.leafCmdExpanded;
  std::string line = "quietly count if !missing(" + expText + ")";
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) line += " & (" + *parsed.ifExpr + ")";
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) line += " in " + *parsed.inRange;
  (void)piet::dispatch(line, session);
  session.leafCmdExpanded = savedLeaf;
  const double* nv = session.eval.rValues.find("N");
  if (nv == nullptr) return;
  const double n = *nv;
  session.eval.rValues.clear();
  double rows = static_cast<double>(session.datastore()->nRows());
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    const InRangeBounds bounds = parseInRangeBounds(*parsed.inRange, session.datastore()->nRows());
    if (bounds.ok) rows = static_cast<double>(bounds.b - bounds.a + 1);
  }
  session.eval.rValues.set("N", n);
  session.eval.rValues.set("nquantiles", static_cast<double>(nq));
  session.eval.rValues.set("nqused", static_cast<double>(nq - 1));
  session.eval.rValues.set("method_ratio",
                           gtoolsMethodRatio(rows, static_cast<double>(nq), /*forXtile=*/true));
  eval::declareRPosted(session.eval, "gquantiles", {"N", "nquantiles", "nqused", "method_ratio"});
}

// ---------------------------------------------------------------------------
// (4) gquantiles with gtools' own options: cutpoints() / cutoffs() /
// quantiles() / cutquantiles() as the source of the bin edges, by() with
// groupid(), altdef, binfreq, genp(), minmax, replace, method(), dedup,
// cutifin, strict and returnlimit(), and an `if` on every form.
// ---------------------------------------------------------------------------

namespace {

// gtools' cost estimate of sorting (m1) against selecting (m2), published as
// their ratio r(method_ratio). Nread is the rows read (an `in` range shortens
// it, an `if` does not), nout the number of cuts plus one. A request for
// quantiles costs one pass per output; a request for cutoffs costs a tenth of
// one. Creating the xtile variable adds a second sort; counting the bins one
// more pass. Measured against the bank: xtile nq(4) on 74 rows 1.934688,
// cutoffs(4000 6000 9000) 8.291522, `xtile nq(4) binfreq` with no variable
// 1.260813, _pctile p(25 75) 1.768022.
double gtoolsCostRatio(double nread, double nout, bool quantileRequest, bool generates,
                       bool countsBins) {
  const double l = std::log(nread);
  double m1 = l + 1.0;
  double m2 = quantileRequest ? nout : 1.0;
  if (generates) {
    m1 = 2.0 * l + 3.0;
    m2 = quantileRequest ? nout + 2.0 : 0.1 * nout + 1.0;
  } else if (countsBins) {
    m1 = l + 2.0;
    m2 = quantileRequest ? nout + 1.0 : 0.1 * nout + 1.0;
  }
  return m2 > 0.0 ? m1 / m2 : 0.0;
}

// The alternative definition (altdef): interpolate at rank (n + 1) p / 100.
double altdefPctile(const std::vector<double>& sorted, double p) {
  const double n = static_cast<double>(sorted.size());
  if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
  const double h = (n + 1.0) * p / 100.0;
  if (h <= 1.0) return sorted.front();
  if (h >= n) return sorted.back();
  const double i = std::floor(h);
  const size_t k = static_cast<size_t>(i);
  return sorted[k - 1] + (h - i) * (sorted[k] - sorted[k - 1]);
}

std::string smallestCategoryType(double largest) {
  if (largest <= 100.0) return "byte";
  if (largest <= 32740.0) return "int";
  return "long";
}

ojson rc198(const std::string& message) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(198)"));
}

ojson rcError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message,
                                std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson notImplemented(const std::string& what) {
  return ojson(eval::unsupportedCommand(what, std::nullopt, std::nullopt,
                                        "refused:gquantiles_extended",
                                        eval::UnsupportedCommandKind::kNotImplemented));
}

// Writes one column of the current head: appended when `isNew`, replaced in
// place otherwise (cobra setcol), stored as `storeType` when it is not empty.
std::optional<ojson> writeColumn(SessionState& session, const std::string& name,
                                 const std::vector<double>& values, bool isNew,
                                 const std::string& storeType) {
  const std::string csvPath = mintCsvPath(session.workdir(), "gquantiles_vals");
  if (!writeValuesCsv(csvPath, values)) return gtErr("gquantiles: could not write the values");
  const std::string outPath = mintScratchPath(session.workdir(), "gquantiles_col");
  std::vector<std::string> argv = {"setcol", "--data", *session.currentHbin(), "--var", name,
                                   "--values", csvPath, "--dtype", "f64"};
  if (isNew) argv.push_back("--new");
  if (!storeType.empty()) {
    argv.push_back("--store-type");
    argv.push_back(storeType);
  }
  argv.push_back("--out");
  argv.push_back(outPath);
  const CobraRunResult r = runCobra(argv);
  if (!r.ok) return gtErr("gquantiles: " + cobraFailure(r));
  if (!cecon::native_exists(outPath))
    return gtErr("gquantiles: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  if (!storeType.empty()) session.varMeta[name].stataType = storeType;
  return std::nullopt;
}

// A numlist of percentages or cutoffs as typed (blanks or commas between).
std::optional<ojson> readNumbers(const std::string& option, const std::string& text,
                                 std::vector<double>* out) {
  std::string normalised = text;
  std::replace(normalised.begin(), normalised.end(), ',', ' ');
  for (const auto& w : splitWords(normalised)) {
    double v = 0.0;
    if (!parseDoubleStrict(w, &v)) return rc198("option " + option + "() incorrectly specified");
    out->push_back(v);
  }
  if (out->empty()) return rc198("option " + option + "() incorrectly specified");
  return std::nullopt;
}

}  // namespace

bool gquantilesNeedsExtended(const eval::ParsedCommand& parsed) {
  static const std::unordered_set<std::string> kExtended = {
      "cutpoints", "cutoffs", "quantiles", "cutquantiles", "by",     "groupid", "altdef",
      "binfreq",   "genp",    "minmax",    "replace",      "method", "dedup",   "cutifin",
      "strict",    "returnlimit", "cutby", "xmissing"};
  for (const auto& kv : parsed.opts)
    if (kExtended.count(strutil::lower(kv.first)) != 0) return true;
  return false;
}

ojson runGQuantilesExtended(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const ModeWord mode = readModeWord(parsed);
  const bool isXtile = mode.mode == "xtile";
  const bool isPctile = mode.mode == "pctile";
  const bool isRPctile = mode.mode == "_pctile";
  if (!isXtile && !isPctile && !isRPctile)
    return gtErr("gquantiles: a mode is required -- write one of `xtile`, `_pctile` or `pctile`",
                 "syntax_error");
  if (parsed.weightType.has_value() && !parsed.weightType->empty())
    return notImplemented("gquantiles with weights together with this option combination");

  // `[newvar =] exp`.
  std::string joined;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    if (mode.fromVarlist && i == mode.varlistIndex) continue;
    if (!joined.empty()) joined += " ";
    joined += parsed.varlist[i];
  }
  std::string target, source;
  const size_t eq = joined.find('=');
  if (eq == std::string::npos) {
    source = strutil::trim(joined);
  } else {
    target = strutil::trim(joined.substr(0, eq));
    source = strutil::trim(joined.substr(eq + 1));
  }
  if (source.empty()) return rc198("Invalid syntax. Requried: newvarname = exp");
  if (isPctile && target.empty()) return rc198("pctile requires a new variable");
  if (isRPctile && !target.empty()) return rc198("_pctile does not create a variable");
  if (!target.empty() && !strutil::isValidStataIdentifier(target))
    return rc198(target + " invalid name");

  // Options.
  long long nq = 0;
  bool haveNq = false;
  std::string pctText, cutoffText, cutVar, cutQuantVar, genp, groupid;
  bool havePct = false, haveCutoffs = false;
  bool altdef = false, binfreq = false, minmax = false, replace = false, dedup = false;
  bool cutifin = false, strict = false;
  long long returnlimit = 1001;
  std::vector<std::string> byVars;
  for (const auto& kv : parsed.opts) {
    const std::string key = strutil::lower(kv.first);
    const eval::OptValue& v = kv.second;
    const std::string text = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (key == "xtile" || key == "pctile" || key == "_pctile") continue;
    if (key == "nquantiles") {
      if (v.isBool || !parseIntStrict(text, &nq)) return rc198("option nquantiles() incorrectly specified");
      if (nq == 0) nq = 2;
      haveNq = true;
    } else if (key == "percentiles" || key == "quantiles") {
      if (v.isBool) return rc198("option " + key + "() incorrectly specified");
      pctText = text;
      havePct = true;
    } else if (key == "cutoffs") {
      if (v.isBool) return rc198("option cutoffs() incorrectly specified");
      cutoffText = text;
      haveCutoffs = true;
    } else if (key == "cutpoints" || key == "cutquantiles") {
      if (v.isBool || text.empty() || text.find_first_of(" \t") != std::string::npos)
        return rc198("option " + key + "() incorrectly specified");
      bool ambiguous = false;
      const auto found = resolveVarName(text, session, &ambiguous);
      if (!found.has_value()) {
        // gtools reads the option as a variable name; a number or a name that
        // is not there is invalid syntax (bank 6d98fa25b86d0f39 `c(4000 ...)`).
        if (!strutil::isValidStataIdentifier(text)) return rc198("invalid syntax");
        return rcError("variable " + text + " not found", 111);
      }
      (key == "cutpoints" ? cutVar : cutQuantVar) = *found;
    } else if (key == "by") {
      if (v.isBool || text.empty()) return rc198("option by() incorrectly specified");
      for (const auto& w : splitWords(text)) {
        const auto found = resolveVarName(w, session);
        if (!found.has_value()) return rcError("variable " + w + " not found", 111);
        byVars.push_back(*found);
      }
    } else if (key == "groupid") {
      if (v.isBool || !strutil::isValidStataIdentifier(text))
        return rc198("option groupid() incorrectly specified");
      groupid = text;
    } else if (key == "genp") {
      if (v.isBool || !strutil::isValidStataIdentifier(text))
        return rc198("option genp() incorrectly specified");
      genp = text;
    } else if (key == "binfreq") {
      if (!v.isBool) return notImplemented("gquantiles, binfreq(newvar)");
      binfreq = true;
    } else if (key == "method") {
      long long m = 0;
      if (v.isBool || !parseIntStrict(text, &m) || m < 0 || m > 2)
        return rc198("option method() incorrectly specified");
      // method() picks the algorithm (sort or select); the answer is the same.
    } else if (key == "returnlimit") {
      double lim = 0.0;
      if (v.isBool || !parseDoubleStrict(text, &lim)) return rc198("option returnlimit() incorrectly specified");
      returnlimit = lim <= 0 || std::isnan(lim) ? 1001 : static_cast<long long>(lim);
    } else if (key == "altdef") {
      altdef = true;
    } else if (key == "minmax") {
      minmax = true;
    } else if (key == "replace") {
      replace = true;
    } else if (key == "dedup") {
      dedup = true;
    } else if (key == "cutifin") {
      cutifin = true;
    } else if (key == "strict") {
      strict = true;
    } else if (key == "cutby") {
      return notImplemented("gquantiles, cutby");
    } else if (key == "xmissing") {
      return notImplemented("gquantiles, xmissing");
    } else {
      return ojson(eval::errorBlock("unsupported_option", "option " + kv.first + " not allowed",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }
  const int sources = (haveNq ? 1 : 0) + (havePct ? 1 : 0) + (haveCutoffs ? 1 : 0) +
                      (cutVar.empty() ? 0 : 1) + (cutQuantVar.empty() ? 0 : 1);
  if (sources > 1)
    return rc198("specify only one of nquantiles(), percentiles(), cutpoints(), cutoffs() or "
                 "cutquantiles()");
  if (sources == 0) {
    nq = kPctileDefaultNq;
    haveNq = true;
  }
  if (haveNq && nq < 2) return rc198("nquantiles() must be at least 2");
  const bool byMode = !byVars.empty();
  if (byMode && isRPctile) return rc198("by() is not allowed with _pctile");
  if (byMode && minmax) return rc198("minmax is not allowed with by()");
  if (byMode && isPctile && !strict)
    return rc198("by() with pctile requires option strict");
  if (!groupid.empty() && !byMode) return notImplemented("gquantiles, groupid() without by()");
  if (byMode && !(haveNq || havePct))
    return notImplemented("gquantiles, by() with cutpoints(), cutoffs() or cutquantiles()");
  if (!genp.empty() && !isPctile) return notImplemented("gquantiles, genp() outside the pctile form");
  if (binfreq && (byMode || !(haveNq || havePct || haveCutoffs)))
    return notImplemented("gquantiles, binfreq with by(), cutpoints() or cutquantiles()");
  if (!isXtile && (haveCutoffs || !cutVar.empty()))
    return notImplemented("gquantiles, cutoffs()/cutpoints() outside the xtile form");
  if (!isXtile && !cutQuantVar.empty())
    return notImplemented("gquantiles, cutquantiles() outside the xtile form");

  Datastore* ds0 = session.datastore();
  if (ds0 == nullptr) return eval::noDataLoaded();
  const bool targetExists = !target.empty() && ds0->has(target);
  if (targetExists && !replace) return rcError("variable " + target + " already defined", 110);
  for (const std::string* extra : {&genp, &groupid})
    if (!extra->empty() && ds0->has(*extra) && !replace)
      return rcError("variable " + *extra + " already defined", 110);
  for (const auto& b : byVars)
    if (ds0->col(b).dtype == cecon::DType::Factor)
      return notImplemented("gquantiles, by() with a string variable");

  // The sample, the source and the by-values, all evaluated as generate would.
  std::string qualifier;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    qualifier += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    qualifier += " in " + *parsed.inRange;
  const int64_t nRows = ds0->nRows();
  std::vector<double> x, mark, cutValues;
  std::vector<std::vector<double>> byValues;
  {
    ScratchColumns scratch(session);
    std::string col;
    if (auto failed = scratch.add(source, "", &col)) return *failed;
    x = scratch.values(col);
    mark.assign(static_cast<size_t>(nRows), 1.0);
    if (!qualifier.empty()) {
      if (auto failed = scratch.add("1", qualifier, &col)) return *failed;
      mark = scratch.values(col);
    }
    const std::string& cv = cutVar.empty() ? cutQuantVar : cutVar;
    if (!cv.empty()) {
      if (auto failed = scratch.add(cv, "", &col)) return *failed;
      cutValues = scratch.values(col);
    }
    for (const auto& b : byVars) {
      if (auto failed = scratch.add(b, "", &col)) return *failed;
      byValues.push_back(scratch.values(col));
    }
  }
  if (session.datastore() == nullptr) return eval::noDataLoaded();
  // Every missing value (`.`, `.a` .. `.z`) is a NaN below; a by-value keeps
  // its kind, as the sentinel double, so `.a` and `.b` are separate groups.
  for (auto* column : {&x, &mark, &cutValues})
    for (double& v : *column)
      if (std::fabs(v) >= eval::kStataMissingSentinel) v = std::numeric_limits<double>::quiet_NaN();
  for (auto& bv : byValues)
    for (double& v : bv)
      if (std::isnan(v)) v = eval::kStataMissingSentinel;

  double nread = static_cast<double>(nRows);
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty()) {
    const InRangeBounds bounds = parseInRangeBounds(*parsed.inRange, nRows);
    if (bounds.ok) nread = static_cast<double>(bounds.b - bounds.a + 1);
  }
  std::vector<char> inSample(static_cast<size_t>(nRows), 0);
  long long markedRows = 0, nx = 0;
  for (int64_t i = 0; i < nRows; ++i) {
    if (std::isnan(mark[static_cast<size_t>(i)])) continue;
    ++markedRows;
    if (std::isnan(x[static_cast<size_t>(i)])) continue;
    inSample[static_cast<size_t>(i)] = 1;
    ++nx;
  }
  if (nx == 0) return rcError("no observations", 2000);

  auto pctOf = [&](const std::vector<double>& sorted, double p) {
    return altdef ? altdefPctile(sorted, p) : stataPctile(sorted, p);
  };

  // Percentages for the quantile requests.
  std::vector<double> pcts;
  if (haveNq)
    for (long long i = 1; i < nq; ++i) pcts.push_back(100.0 * static_cast<double>(i) / static_cast<double>(nq));
  if (havePct) {
    if (auto bad = readNumbers(mode.mode == "xtile" ? "quantiles" : "percentiles", pctText, &pcts)) return *bad;
  }
  std::vector<double> literalCuts;
  if (haveCutoffs)
    if (auto bad = readNumbers("cutoffs", cutoffText, &literalCuts)) return *bad;
  if (!cutVar.empty() || !cutQuantVar.empty()) {
    for (int64_t i = 0; i < nRows; ++i) {
      const double c = cutValues[static_cast<size_t>(i)];
      if (std::isnan(c)) continue;
      if (cutifin && std::isnan(mark[static_cast<size_t>(i)])) continue;
      (cutVar.empty() ? pcts : literalCuts).push_back(c);
    }
    std::vector<double>& got = cutVar.empty() ? pcts : literalCuts;
    std::sort(got.begin(), got.end());
    if (dedup) got.erase(std::unique(got.begin(), got.end()), got.end());
    if (got.empty()) return rc198("no non-missing values in " + (cutVar.empty() ? cutQuantVar : cutVar));
  }
  for (double p : pcts)
    if (!(p > 0.0 && p < 100.0)) return rc198("percentiles must be strictly between 0 and 100");
  if (pcts.size() > kMaxPercentileCount || literalCuts.size() > kMaxPercentileCount)
    return gtErr("gquantiles: more than " + std::to_string(kMaxPercentileCount) +
                 " quantiles requested");
  const bool quantileRequest = !pcts.empty();
  const size_t nCuts = quantileRequest ? pcts.size() : literalCuts.size();
  if (strict && haveNq && !byMode && nq > nx + 1)
    return rc198("nquantiles() must be less than or equal to the number of non-missing "
                 "observations plus one");

  // Groups (one when there is no by()).
  std::vector<std::vector<int64_t>> groups;
  std::vector<double> groupOfRow(static_cast<size_t>(nRows), std::numeric_limits<double>::quiet_NaN());
  {
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < nRows; ++i)
      if (!std::isnan(mark[static_cast<size_t>(i)])) rows.push_back(i);
    if (!byMode) {
      groups.push_back(rows);
    } else {
      // A missing by-value is a group of its own, sorted after every number
      // (bank 321824f7fe83df1f: by(foreign rep78) has J = 10, the eight
      // observed pairs and one missing-rep78 group per foreign).
      auto same = [](double a, double b) { return a == b; };
      auto before = [](double a, double b) { return a < b; };
      std::stable_sort(rows.begin(), rows.end(), [&](int64_t a, int64_t b) {
        for (const auto& bv : byValues) {
          const double va = bv[static_cast<size_t>(a)], vb = bv[static_cast<size_t>(b)];
          if (!same(va, vb)) return before(va, vb);
        }
        return false;
      });
      for (size_t k = 0; k < rows.size(); ++k) {
        bool fresh = groups.empty();
        if (!fresh)
          for (const auto& bv : byValues)
            if (!same(bv[static_cast<size_t>(rows[k])], bv[static_cast<size_t>(groups.back().front())])) fresh = true;
        if (fresh) groups.emplace_back();
        groups.back().push_back(rows[k]);
        groupOfRow[static_cast<size_t>(rows[k])] = static_cast<double>(groups.size());
      }
    }
  }

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> category(static_cast<size_t>(nRows), kNaN);
  std::vector<double> pctileCol(static_cast<size_t>(nRows), kNaN);
  std::vector<double> genpCol(static_cast<size_t>(nRows), kNaN);
  std::vector<double> usedCuts;  // the whole-sample cuts, in the order asked
  std::vector<double> binCounts(nCuts, 0.0);
  double sampleMin = std::numeric_limits<double>::infinity();
  double sampleMax = -std::numeric_limits<double>::infinity();
  double largest = 0.0;
  for (const auto& g : groups) {
    std::vector<double> sorted;
    for (int64_t r : g)
      if (inSample[static_cast<size_t>(r)]) sorted.push_back(x[static_cast<size_t>(r)]);
    if (sorted.empty()) continue;
    std::sort(sorted.begin(), sorted.end());
    sampleMin = std::min(sampleMin, sorted.front());
    sampleMax = std::max(sampleMax, sorted.back());
    const long long gn = static_cast<long long>(sorted.size());
    if (byMode && strict && haveNq && (isPctile ? nq > gn : nq > gn + 1)) continue;
    std::vector<double> cuts;
    if (quantileRequest)
      for (double p : pcts) cuts.push_back(pctOf(sorted, p));
    else
      cuts = literalCuts;
    if (!byMode) usedCuts = cuts;
    std::vector<double> edges = cuts;
    std::sort(edges.begin(), edges.end());
    if (isXtile) {
      for (int64_t r : g) {
        if (!inSample[static_cast<size_t>(r)]) continue;
        const double v = x[static_cast<size_t>(r)];
        const size_t below = static_cast<size_t>(std::lower_bound(edges.begin(), edges.end(), v) - edges.begin());
        const double cat = static_cast<double>(below + 1);
        category[static_cast<size_t>(r)] = cat;
        largest = std::max(largest, cat);
        if (below < nCuts) binCounts[below] += 1.0;
      }
    } else if (isPctile) {
      // The percentiles land in the group's first rows, in data order; without
      // by() they fill observations 1..nq-1 whatever the if/in sample's size.
      const size_t room = byMode ? g.size() : static_cast<size_t>(nRows);
      for (size_t k = 0; k < cuts.size() && k < room; ++k) {
        const size_t r = static_cast<size_t>(byMode ? g[k] : static_cast<int64_t>(k));
        pctileCol[r] = cuts[k];
        if (!genp.empty()) genpCol[r] = pcts[k];
      }
      if (!byMode && cuts.size() > static_cast<size_t>(nRows))
        return gtErr("gquantiles: " + std::to_string(cuts.size()) +
                     " percentiles cannot be stored in a dataset of " + std::to_string(nRows) +
                     " observations");
    }
  }

  // Variables.
  if (isXtile && !target.empty()) {
    if (auto failed = writeColumn(session, target, category, !targetExists, smallestCategoryType(largest)))
      return *failed;
  }
  if (isPctile) {
    if (auto failed = writeColumn(session, target, pctileCol, !targetExists, "")) return *failed;
    if (!genp.empty()) {
      const bool exists = session.datastore()->has(genp);
      if (auto failed = writeColumn(session, genp, genpCol, !exists, "")) return *failed;
    }
  }
  if (!groupid.empty()) {
    const bool exists = session.datastore()->has(groupid);
    if (auto failed = writeColumn(session, groupid, groupOfRow, !exists,
                                  smallestCategoryType(static_cast<double>(groups.size()))))
      return *failed;
  }

  // r().
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  auto setR = [&](const std::string& name, double v) {
    session.eval.rValues.set(name, v);
    posted.push_back(name);
  };
  if (byMode) {
    double minJ = std::numeric_limits<double>::infinity(), maxJ = 0.0;
    for (const auto& g : groups) {
      minJ = std::min(minJ, static_cast<double>(g.size()));
      maxJ = std::max(maxJ, static_cast<double>(g.size()));
    }
    setR("N", static_cast<double>(markedRows));
    setR("Nx", static_cast<double>(nx));
    setR("J", static_cast<double>(groups.size()));
    setR("minJ", minJ);
    setR("maxJ", maxJ);
  } else {
    setR("N", static_cast<double>(nx));
    setR("nqused", static_cast<double>(nCuts));
    setR("method_ratio",
         gtoolsCostRatio(nread, static_cast<double>(nCuts + 1), quantileRequest,
                         isXtile && !target.empty(), binfreq));
    if (minmax) {
      setR("min", sampleMin);
      setR("max", sampleMax);
    }
    if (haveNq) setR("nquantiles", static_cast<double>(nq));
    if (havePct) {
      setR("nquantiles_used", static_cast<double>(nCuts));
      session.eval.rStrings.set("quantiles", pctText);
      posted.push_back("quantiles");
      putRMatrixColumn(session, "quantiles_used", usedCuts);
      posted.push_back("quantiles_used");
    }
    if (haveCutoffs) {
      setR("ncutoffs_used", static_cast<double>(nCuts));
      session.eval.rStrings.set("cutoffs", cutoffText);
      posted.push_back("cutoffs");
      putRMatrixColumn(session, "cutoffs_used", usedCuts);
      posted.push_back("cutoffs_used");
    }
    if (!cutVar.empty()) setR("ncutpoints", static_cast<double>(nCuts));
    if (!cutQuantVar.empty()) setR("nquantpoints", static_cast<double>(nCuts));
    if (isRPctile && haveNq) {
      putRMatrixColumn(session, "quantiles_used", usedCuts);
      posted.push_back("quantiles_used");
    }
    if (isRPctile) {
      const size_t shown = std::min(usedCuts.size(), static_cast<size_t>(returnlimit));
      for (size_t i = 0; i < shown; ++i) setR("r" + std::to_string(i + 1), usedCuts[i]);
    }
    if (binfreq) {
      const std::string name = haveCutoffs ? "cutoffs_binfreq" : "quantiles_binfreq";
      putRMatrixColumn(session, name, binCounts);
      posted.push_back(name);
    }
  }
  eval::declareRPosted(session.eval, "gquantiles", posted);

  std::vector<std::string> made, rewritten;
  for (const std::string* v : {&target, &genp, &groupid}) {
    if (v->empty() || (v == &target && isRPctile) || (v == &target && isXtile && target.empty())) continue;
    (v == &target && targetExists ? rewritten : made).push_back(*v);
  }
  if (made.empty() && rewritten.empty()) return ojson::array();
  return mutationResult(session, "gen", made, {}, rewritten);
}

ojson runGQuantilesPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (gquantilesNeedsExtended(parsed) || readModeWord(parsed).mode == "xtile" ||
      (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()))
    return runGQuantilesExtended(parsed, session);
  if (parsed.weightType.has_value() && !parsed.weightType->empty())
    return notImplemented("gquantiles with weights in the _pctile and pctile forms");
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
      if (nq == 0) nq = 2;  // nquantiles(0) is "not given": two groups, as for xtile
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
  double rowsRead = 0.0;
  const int64_t count = column.n_values();
  if (data == nullptr || count != ds->nRows()) {
    return gtErr("gquantiles: could not read " + source + " as a full-length numeric column");
  }

  // `in a/b`: only those rows are read, and they are the rows gtools reports having read.
  int64_t firstRow = 1, lastRow = count;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    const InRangeBounds bounds = parseInRangeBounds(*parsed.inRange, count);
    if (!bounds.ok) {
      InRangeFilterResult refused;
      refused.ok = false;
      refused.refusal = bounds.refusal;
      return inRangeRejectError(refused, *parsed.inRange, "gquantiles");
    }
    firstRow = bounds.a;
    lastRow = bounds.b;
  }
  rowsRead = static_cast<double>(lastRow - firstRow + 1);
  std::vector<double> sorted;
  sorted.reserve(static_cast<size_t>(count));
  for (int64_t i = firstRow - 1; i < lastRow; ++i) {
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

  // What gtools posts besides the percentiles themselves.
  {
    session.eval.rValues.set("N", static_cast<double>(sorted.size()));
    std::vector<std::string> posted = {"N", "method_ratio", "nqused"};
    const double rows = rowsRead;
    if (havePctSpec) {
      session.eval.rStrings.set("quantiles", strutil::trim(pctSpec));
      session.eval.rValues.set("nquantiles_used", static_cast<double>(values.size()));
      session.eval.rValues.set("nqused", static_cast<double>(values.size()));
      session.eval.rValues.set("method_ratio",
                               gtoolsMethodRatio(rows, static_cast<double>(values.size() + 1), false));
      putRMatrixColumn(session, "quantiles_used", values);
      posted.insert(posted.end(), {"quantiles", "nquantiles_used", "quantiles_used"});
    } else {
      session.eval.rValues.set("nquantiles", static_cast<double>(nq));
      session.eval.rValues.set("nqused", static_cast<double>(nq - 1));
      session.eval.rValues.set("method_ratio", gtoolsMethodRatio(rows, static_cast<double>(nq), false));
      posted.push_back("nquantiles");
      if (!wantVariable) {
        putRMatrixColumn(session, "quantiles_used", values);
        posted.push_back("quantiles_used");
      }
    }
    if (!wantVariable)
      for (size_t i = 0; i < values.size(); ++i) posted.push_back("r" + std::to_string(i + 1));
    eval::declareRPosted(session.eval, "gquantiles", posted);
  }
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
