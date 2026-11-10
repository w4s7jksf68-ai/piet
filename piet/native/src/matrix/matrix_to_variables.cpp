// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/results.h"
#include "piet/handlers/session_commands.h"

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
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
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

// `names()` is svmat's only option.
const eval::OptVocab& kSvmatVocab = eval::registerOptVocab({"svmat"}, {{"names"}, {}});

// getmata.ado's `syntax ... [, DOUBLE FORCE ID(string) REPLACE UPdate]`. All
// five are declared: update and id() are UNIMPLEMENTED (neither is in the
// corpus) and must be refused by name, not reported as unknown options and not
// accepted and ignored.
const eval::OptVocab& kGetmataVocab =
    eval::registerOptVocab({"getmata"}, {{"double", "force", "replace"}, {"update", "id"}});

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// mintScratchPath always names a ".hbin"; a setcol payload is a CSV. Registering
// the minted .hbin and then writing a .csv beside it would leave the file that
// actually exists unregistered, which is the BUG-389 orphan class the registry
// exists to close (sweepOrphanScratch only removes paths it knows). Same body as
// datamgmt/winsor.cpp's, and duplicated for the same reason that one is not
// shared: it is four lines behind a one-line intent, and a header for it would
// touch a merge surface this stint does not own.
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

// getmata stores from Mata's double vector, compresses exact integer values,
// then permits a float copy unless `double` was requested. The fixed capture
// w2c_ml_getmata.log:1058/1092 pins byte for the integer vector even with
// `double`, and float-rounded fractional copies by default. Existing variables
// retain at least their original type (getmata.ado combine_num_vartype).
std::string getmataStoredType(const std::vector<double>& values, bool useDouble,
                              const std::string& original) {
  bool integer = true, exactFloat = true, floatFits = true;
  double lo = 0.0, hi = 0.0;
  for (double value : values) {
    if (std::isnan(value)) continue;
    integer = integer && std::isfinite(value) && std::trunc(value) == value;
    lo = std::min(lo, value);
    hi = std::max(hi, value);
    const double f = static_cast<double>(static_cast<float>(value));
    exactFloat = exactFloat && f == value;
    floatFits = floatFits && std::isfinite(f) && std::fabs(f) <= 0x1.fffffep+126;
  }
  std::string compact;
  if (integer) compact = storage::promotedType("byte", false, lo, hi);
  else compact = exactFloat ? "float" : "double";
  if (original.empty()) {
    if (compact == "double" && !useDouble && floatFits) return "float";
    return compact;
  }
  if (original == "double" || compact == "double") return "double";
  if (original == compact) return original;
  if (original == "float") return compact == "long" ? "double" : "float";
  if (original == "long") return compact == "float" ? "double" : "long";
  if (original == "int" && compact == "byte") return "int";
  return compact;
}

// cobra reports a structured failure as {"error":{"code","message"}} on stderr.
// Surfacing the raw blob would put JSON punctuation in front of the user;
// surfacing nothing would make a failed setcol indistinguishable from a
// successful one at the call site.
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

}  // namespace

// ---------------------------------------------------------------------------
// svmat

namespace {

bool isNumericStorageType(const std::string& w) {
  return w == "double" || w == "float" || w == "long" || w == "int" || w == "byte";
}

bool isStringStorageType(const std::string& w) {
  if (w == "strl") return true;
  if (w.size() < 4 || w.compare(0, 3, "str") != 0) return false;
  return w.find_first_not_of("0123456789", 3) == std::string::npos;
}

}  // namespace

ojson runSvmat(const eval::ParsedCommand& parsed, SessionState& session) {
  // The reject net runs FIRST, before any matrix is resolved or any scratch file
  // is minted: a command that is going to be refused must cost nothing.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kSvmatVocab.known,
                                            kSvmatVocab.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string>& words = parsed.varlist;
  if (words.empty()) return aerError("svmat: no matrix specified");
  if (words.size() > 2) {
    return aerError("svmat: too many arguments; the syntax is `svmat [type] A [, names(...)]'",
                    "syntax_error");
  }

  if (words.size() == 2) {
    const std::string type = strutil::lower(words[0]);
    if (isStringStorageType(type)) {
      // A str# request is not a storage-type nicety here: it asks for a STRING
      // variable, and a matrix has no strings to put in one. Accepting it would
      // create a numeric column under a promise it does not keep.
      return aerError("svmat: type " + words[0] +
                          " is not allowed; a matrix holds numeric values only",
                      "syntax_error");
    }
    if (!isNumericStorageType(type)) {
      return aerError("svmat: `" + words[0] + "' is not a storage type", "syntax_error");
    }
    // ACCEPTED AND IGNORED, deliberately and visibly: PIET has exactly one
    // numeric storage type (f64), so double/float/long/int/byte all land in the
    // same column. Refusing them would stop do-files over a distinction this
    // engine does not have; honouring them is impossible. The values are exact
    // either way -- what is lost is only the on-disk width.
  }
  const std::string matName = words.back();

  const eval::EvalMatrix* mat = session.eval.findMatrix(matName);
  if (mat == nullptr) {
    // Stata answers r(111) for a matrix that does not exist, and coarseRc's
    // "variable"+"not found" rule does not cover the word "matrix".
    return aerErrorRc("svmat: matrix " + matName + " not found", 111);
  }
  const size_t nMatRows = mat->data.size();
  const size_t nMatCols = nMatRows > 0 ? mat->data[0].size() : 0;
  if (nMatRows == 0 || nMatCols == 0) {
    return aerError("svmat: matrix " + matName + " has no elements");
  }
  for (const auto& row : mat->data) {
    if (row.size() != nMatCols) {
      return aerError("svmat: matrix " + matName + " is ragged and cannot be read as columns");
    }
  }
  // COPY, not the pointer. findMatrix hands back a pointer INTO
  // session.eval.matrices, and every line below this one mutates the session
  // (runSetObs, session.load). Nothing on that path appends a matrix today, so
  // this guards a future one rather than a live bug -- but the failure it guards
  // is a dangling read that would write PLAUSIBLE numbers into the dataset,
  // which is the worst shape a defect can take here.
  const eval::EvalMatrix matrix = *mat;

  std::string namesArg;
  for (const auto& kv : parsed.opts) {
    if (kv.first != "names") continue;
    if (kv.second.isBool || kv.second.strValue.empty()) {
      return aerError("svmat: option names() requires an argument");
    }
    namesArg = strutil::trim(kv.second.strValue);
  }

  // eqcol is folded onto col rather than rejected: PIET's EvalMatrix carries no
  // equation names at all (eval/session.h has rownames/colnames and nothing
  // else), so "the column name with its equation stripped" IS the column name
  // here. Refusing it would stop a do-file over a distinction this build cannot
  // express; inventing an equation prefix would be a fabricated name.
  const std::string namesKey = strutil::lower(namesArg);
  const bool useColNames = (namesKey == "col" || namesKey == "eqcol" || namesKey == "matcol");
  const bool prefixWithMatName = (namesKey == "matcol");
  const bool useStub = !namesArg.empty() && !useColNames;
  const std::string stub = useStub ? namesArg : matName;

  std::vector<std::string> newVars;
  newVars.reserve(nMatCols);
  for (size_t j = 0; j < nMatCols; ++j) {
    std::string name;
    if (useColNames) {
      if (j >= matrix.colnames.size() || matrix.colnames[j].empty()) {
        return aerError("svmat: matrix " + matName + " has no column name for column " +
                        std::to_string(j + 1) + "; names(" + namesArg + ") cannot name it");
      }
      name = prefixWithMatName ? matName + matrix.colnames[j] : matrix.colnames[j];
    } else {
      name = stub + std::to_string(j + 1);
    }
    if (!strutil::isValidStataIdentifier(name)) {
      return aerError("svmat: `" + name + "' is not a valid variable name. " +
                          strutil::stataIdentifierRuleText(),
                      "syntax_error");
    }
    // Stata's reserved names ([U] 11.3) are well-formed identifiers that no
    // variable may take: `svmat b, names(col)` over e(b) meets `_cons` and
    // stops with "_cons invalid varname", r(198) (w2g_ebmat.log:1388-1393
    // e11_svmat). Reachable since e(b) carries Stata's `_cons` (eb-order T-P1)
    // where it used to carry the engine's `(intercept)`.
    {
      static const std::set<std::string> kReserved = {
          "_all", "_b",   "byte", "_coef", "_cons", "double", "float", "if",   "in",
          "int",  "long", "_n",   "_N",    "_pi",   "_pred",  "_rc",   "_skip", "strL",
          "using", "with"};
      static const std::regex reStrN(R"(^str[0-9]+$)");
      if (kReserved.count(name) || std::regex_match(name, reStrN))
        return aerErrorRc(name + " invalid varname", 198);
    }
    if (std::find(newVars.begin(), newVars.end(), name) != newVars.end()) {
      // Two matrix columns sharing a colname would otherwise have the second
      // silently overwrite the first, losing a column with no diagnostic.
      return aerError("svmat: names(" + namesArg + ") would create variable " + name + " twice");
    }
    newVars.push_back(name);
  }

  // COLLISION CHECK BEFORE ANY WRITE. Each column is a separate cobra setcol, so
  // discovering the clash on column 3 would leave columns 1 and 2 already in the
  // dataset -- a half-written svmat, which is worse than a refused one because
  // the user has no way to tell how far it got.
  Datastore* ds = session.datastore();
  const int64_t curRows = ds ? ds->nRows() : 0;
  if (ds != nullptr) {
    for (const auto& name : newVars) {
      if (ds->has(name)) {
        // "already defined" is the wording coarseRc maps to r(110) (confirm new
        // variable); the phrase is load-bearing, not decoration.
        return aerError("svmat: variable " + name + " already defined");
      }
    }
  }

  // Stata's documented behaviour: svmat EXTENDS the dataset when the matrix has
  // more rows than there are observations, and with no dataset at all this is
  // how the dataset comes into existence (runSetObs' own no-currentHbin branch
  // builds an N-row, 0-variable frame).
  const int64_t nOut = std::max<int64_t>(curRows, static_cast<int64_t>(nMatRows));
  const bool extended = nOut > curRows;
  if (extended) {
    ojson setObs = runSetObs(std::to_string(nOut), session);
    if (isErrorBlock(setObs)) return setObs;
  }
  if (!session.currentHbin().has_value()) {
    return ojson(eval::noDataLoaded());
  }

  // Each column is its own setcol + session.load, so a failure INSIDE the loop
  // leaves the dataset half written and there is nothing to roll back to (the
  // committed columns and the `set obs` extension are both already the session
  // head). The collision net above is what makes this reachable only by I/O
  // failure -- but when it IS reached, the count is the difference between a
  // user who knows to drop the partial columns and re-run and one who reads a
  // bare "could not write" as "nothing happened" and re-runs into a collision
  // refusal that names a variable they never asked for.
  auto writeFailure = [&](size_t done, const std::string& what) {
    std::string msg = "svmat: " + what;
    if (done > 0) {
      msg += "; " + std::to_string(done) + " of " + std::to_string(nMatCols) +
             " columns were already written and are still in the dataset";
    } else if (extended) {
      msg += "; the dataset was already extended to " + std::to_string(nOut) + " observations";
    }
    return aerError(msg);
  };

  for (size_t j = 0; j < nMatCols; ++j) {
    std::vector<double> vals;
    vals.reserve(static_cast<size_t>(nOut));
    for (int64_t i = 0; i < nOut; ++i) {
      // Rows past the matrix are system-missing, not zero: the matrix says
      // nothing about them, and a zero would be a fabricated observation.
      vals.push_back(static_cast<size_t>(i) < nMatRows
                         ? matrix.data[static_cast<size_t>(i)][j]
                         : std::numeric_limits<double>::quiet_NaN());
    }

    const std::string csvPath = mintCsvPath(session.workdir(), "svmat_vals");
    if (!writeValuesCsv(csvPath, vals)) {
      return writeFailure(j, "could not write the values for " + newVars[j]);
    }
    const std::string outPath = mintScratchPath(session.workdir(), "svmat");
    const CobraRunResult r =
        runCobra({"setcol", "--data", *session.currentHbin(), "--var", newVars[j], "--values",
                  csvPath, "--dtype", "f64", "--new", "--out", outPath});
    if (!r.ok) return writeFailure(j, cobraFailure(r));
    // BUG-124's torn-write guard, carried from every other stateless setcol
    // site: the engine's writer creates `--out` before it can detect a write
    // failure, and a full scratch volume has twice reached PIET as a ZERO exit
    // code. session.load on a path that is not there does not fail -- it records
    // the head and hands the next Datastore open a missing or truncated
    // mapping, which is a crash rather than a diagnostic.
    if (!cecon::native_exists(outPath)) {
      return writeFailure(j, "failed writing output dataset (disk full or I/O error)");
    }
    session.load(outPath);
  }

  // ONE mutationResult for the whole command, not one per column: the card the
  // UI reads must describe the dataset the user ends up with, and k separate
  // "gen" blocks would each claim to be the finished state.
  return mutationResult(session, "gen", newVars);
}

// ---------------------------------------------------------------------------
// getmata
//
// Oracle: tests/eval/golden/aer_w2/w2c_ml_getmata.log family (8) (getmata_030_seq
// :1058, getmata_exists :1074, getmata_replace :1083, getmata_double :1092,
// getmata_rows_ne :1107, getmata_force_fewer :1116, getmata_force_more :1130,
// getmata_nodata :1141, getmata_notfound :1151, getmata_matrix_paren :1167,
// getmata_paren_badcount :1177) and family (9) getmata_087_shape :1225. Rules
// and their ORDER are getmata.ado's (ado/base/g/getmata.ado): per element, the
// matrix must exist (111), have a column (459), match the name count (102 too
// few / 103 too many) and not collide with an existing variable unless
// `replace` (110); then names must be unique across the getlist (198); then,
// unless `force`, every vector's rows must equal _N (459). `force` is implied
// when the session has no observations AND no variables (st_nobs()==0 &
// c("k")==0). Every check runs BEFORE the first write, as runSvmat's does.

namespace {

struct GetmataElement {
  std::vector<std::string> vars;
  std::string mat;
  eval::EvalMatrix value;  // a COPY: the writes below mutate the session
  std::vector<bool> exists;
};

// getmata.ado's tokeninit(" ", "=", "()"): whitespace separates, `=` is a
// token of its own, and a parenthesised group is ONE token.
std::vector<std::string> getlistTokens(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    const char ch = s[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    if (ch == '=') {
      out.emplace_back("=");
      ++i;
      continue;
    }
    if (ch == '(') {
      const size_t close = s.find(')', i);
      const size_t end = close == std::string::npos ? s.size() : close + 1;
      out.push_back(s.substr(i, end - i));
      i = end;
      continue;
    }
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '=' &&
           s[j] != '(')
      ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool isMataName(const std::string& n) { return cecon::ident::isValidName(n); }

}  // namespace

ojson runGetmata(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kGetmataVocab.known,
                                            kGetmataVocab.unimplemented)) {
    return ojson(*err);
  }
  bool force = false, replace = false;
  bool useDouble = session.defaultStorageType == "double";
  for (const auto& kv : parsed.opts) {
    if (kv.first == "force") force = true;
    if (kv.first == "replace") replace = true;
    if (kv.first == "double") useDouble = true;
  }

  std::string getlist;
  for (const auto& w : parsed.varlist) getlist += (getlist.empty() ? "" : " ") + w;
  if (strutil::trim(getlist).empty()) {
    return aerErrorRc("getmata: getlist required -- the syntax is `getmata name`, "
                      "`getmata varname=vecname` or `getmata (var1 var2 ...)=matname`",
                      100);
  }

  Datastore* ds = session.hasData() ? session.datastore() : nullptr;
  const int64_t curRows = ds ? ds->nRows() : 0;
  const size_t curVars = ds ? ds->names().size() : 0;
  if (curRows == 0 && curVars == 0) force = true;  // getmata: nothing in memory implies force

  // ---- parse and validate every element before touching anything ----------
  std::vector<GetmataElement> els;
  const std::vector<std::string> toks = getlistTokens(getlist);
  for (size_t t = 0; t < toks.size(); ++t) {
    GetmataElement el;
    bool expandStub = false;
    if (toks[t][0] == '(') {
      std::string inner = toks[t].substr(1);
      if (!inner.empty() && inner.back() == ')') inner.pop_back();
      std::istringstream iss(inner);
      std::string v;
      while (iss >> v) el.vars.push_back(v);
      if (el.vars.empty()) {
        return aerErrorRc("getmata: nothing found where evarnamelist expected", 198);
      }
      if (t + 1 >= toks.size() || toks[t + 1] != "=") {
        return aerErrorRc("getmata: " +
                              (t + 1 < toks.size() ? toks[t + 1] : std::string("nothing")) +
                              " found where equal sign (=) expected",
                          198);
      }
      if (t + 2 >= toks.size()) {
        return aerErrorRc("getmata: nothing found where matrix name expected", 198);
      }
      el.mat = toks[t + 2];
      t += 2;
      expandStub = el.vars.size() == 1 && el.vars[0].back() == '*';
    } else if (t + 1 < toks.size() && toks[t + 1] == "=") {
      if (t + 2 >= toks.size()) {
        return aerErrorRc("getmata: nothing found where matrix name expected", 198);
      }
      el.vars = {toks[t]};
      el.mat = toks[t + 2];
      t += 2;
    } else if (toks[t] == "=") {
      return aerErrorRc("getmata: = found where a variable name expected", 198);
    } else {
      el.vars = {toks[t]};
      el.mat = toks[t];
    }

    if (!isMataName(el.mat)) {
      return aerErrorRc("getmata: " + el.mat + " invalid vector or matrix name", 198);
    }
    // The Mata store ONLY (SPEC D3): a Stata matrix of the same name is a
    // different object and must not be found here.
    const eval::EvalMatrix* m = mata::findMataMatrix(session.eval, el.mat);
    if (m == nullptr) return aerErrorRc("vector or matrix " + el.mat + " not found", 111);
    el.value = *m;
    const size_t cols = mata::colsOf(el.value);
    if (cols == 0 && !expandStub) {
      return aerErrorRc("matrix " + el.mat + " has 0 columns", 459);
    }
    if (expandStub) {
      const std::string base = el.vars[0].substr(0, el.vars[0].size() - 1);
      el.vars.clear();
      for (size_t j = 1; j <= cols; ++j) el.vars.push_back(base + std::to_string(j));
    }
    if (el.vars.size() != cols) {
      const bool tooFew = el.vars.size() < cols;
      return aerErrorRc(std::string(tooFew ? "too few" : "too many") +
                            " variables specified for " + (cols == 1 ? "vector " : "matrix ") +
                            el.mat,
                        tooFew ? 102 : 103);
    }
    for (const auto& v : el.vars) {
      if (!strutil::isValidStataIdentifier(v)) {
        return aerErrorRc("getmata: " + v + " invalid variable name", 198);
      }
      const bool exists = ds != nullptr && ds->has(v);
      if (exists && !replace) {
        // Stata's own wording (getmata_exists, log :1070), with the rc stated
        // explicitly -- coarseRc keys r(110) off "already defined", not this.
        return aerErrorRc("variable " + v +
                              " already exists; either specify a different variable name or "
                              "specify getmata's replace option",
                          110);
      }
      if (exists && ds->dtype(v) == "factor") {
        return aerErrorRc("type mismatch: existing variable " + v + " is string and matrix " +
                              el.mat + " is real",
                          109);
      }
      el.exists.push_back(exists);
    }
    els.push_back(std::move(el));
  }

  {
    std::vector<std::string> all;
    for (const auto& el : els) all.insert(all.end(), el.vars.begin(), el.vars.end());
    std::vector<std::string> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i] == sorted[i - 1]) {
        return aerErrorRc("getmata: variable " + sorted[i] +
                              " multiply defined (it is named more than once in the getlist)",
                          198);
      }
    }
  }

  size_t minRows = std::numeric_limits<size_t>::max(), maxRows = 0;
  for (const auto& el : els) {
    minRows = std::min(minRows, el.value.data.size());
    maxRows = std::max(maxRows, el.value.data.size());
  }
  if (!force && (minRows != static_cast<size_t>(curRows) ||
                 maxRows != static_cast<size_t>(curRows))) {
    // getmata_rows_ne (log :1099-1107): r(459), and _N is left alone.
    const bool one = els.size() == 1;
    std::string msg = std::string(one ? "rows of matrix != _N; the matrix contains "
                                      : "rows of matrices != _N; the matrices contain ");
    msg += minRows == maxRows ? std::to_string(minRows) + (minRows == 1 ? " row" : " rows")
                              : "between " + std::to_string(minRows) + " and " +
                                    std::to_string(maxRows) + " rows";
    msg += " and the data have " + std::to_string(curRows) +
           (curRows == 1 ? " observation" : " observations") +
           ". Specify option force to align with the first observation and to extend the "
           "data if necessary.";
    return aerErrorRc(msg, 459);
  }

  // ---- write ---------------------------------------------------------------
  // `force` with more rows than _N EXTENDS the dataset (getmata_force_more,
  // getmata_nodata), through the same runSetObs path svmat uses; with fewer
  // rows the tail is missing, never 0 (getmata_force_fewer).
  const int64_t nOut = std::max<int64_t>(curRows, static_cast<int64_t>(maxRows));
  const bool extended = nOut > curRows || !session.hasData();
  if (extended) {
    ojson setObs = runSetObs(std::to_string(nOut), session);
    if (isErrorBlock(setObs)) return setObs;
  }
  if (!session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  size_t written = 0, total = 0;
  for (const auto& el : els) total += el.vars.size();
  auto writeFailure = [&](const std::string& what) {
    std::string msg = "getmata: " + what;
    if (written > 0) {
      msg += "; " + std::to_string(written) + " of " + std::to_string(total) +
             " variables were already written and are still in the dataset";
    } else if (extended) {
      msg += "; the dataset was already extended to " + std::to_string(nOut) + " observations";
    }
    return aerError(msg);
  };

  double kNew = 0, kExisting = 0;
  std::vector<std::string> touched;
  for (const auto& el : els) {
    const size_t rows = el.value.data.size();
    for (size_t j = 0; j < el.vars.size(); ++j) {
      std::vector<double> vals;
      vals.reserve(static_cast<size_t>(nOut));
      for (int64_t i = 0; i < nOut; ++i) {
        vals.push_back(static_cast<size_t>(i) < rows
                           ? el.value.data[static_cast<size_t>(i)][j]
                           : std::numeric_limits<double>::quiet_NaN());
      }
      const std::string original = el.exists[j]
                                       ? storage::resolveReplaceType(session, el.vars[j])
                                       : std::string();
      const std::string stored = rows == 0
                                     ? original.empty() ? (useDouble ? "double" : "float") : original
                                     : getmataStoredType(vals, useDouble, original);
      const std::string csvPath = mintCsvPath(session.workdir(), "getmata_vals");
      if (!writeValuesCsv(csvPath, vals)) {
        return writeFailure("could not write the values for " + el.vars[j]);
      }
      const std::string outPath = mintScratchPath(session.workdir(), "getmata");
      std::vector<std::string> args = {"setcol", "--data", *session.currentHbin(), "--var",
                                       el.vars[j], "--values", csvPath, "--dtype", "f64"};
      storage::appendStoreTypeArg(args, stored);
      // An existing variable is REPLACED in place (getmata.ado stores missing
      // over the whole column first, then the vector's rows -- which is what a
      // full-length column with a missing tail is).
      if (!el.exists[j]) args.push_back("--new");
      args.push_back("--out");
      args.push_back(outPath);
      const CobraRunResult r = runCobra(args);
      if (!r.ok) return writeFailure(cobraFailure(r));
      if (!cecon::native_exists(outPath)) {
        return writeFailure("failed writing output dataset (disk full or I/O error)");
      }
      session.load(outPath);
      storage::recordCreateType(session, el.vars[j], stored, stored);
      ++written;
      touched.push_back(el.vars[j]);
      if (el.exists[j]) kExisting += 1;
      else kNew += 1;
    }
  }

  // getmata is r-class: r() is replaced by exactly these two. eval::replaceRClass
  // is the one replacement shape: it leaves the reserved \x01 keys (s()) alone,
  // where clearing the whole rStrings dict destroyed them.
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("K_new", kNew);
  session.eval.rValues.set("K_existing", kExisting);
  return mutationResult(session, "gen", touched);
}

}  // namespace piet::handlers
