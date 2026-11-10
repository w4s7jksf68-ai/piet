// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/session_commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // marksample / tsspell cond(): a dispatched `generate`
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/tokenizer.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

#include "utility_support.h"

namespace piet::handlers::utility_detail {
using ojson = nlohmann::ordered_json;



// ===========================================================================
// Errors
// ===========================================================================

ojson utilError(const std::string& message, const std::string& code ) {
  return ojson(eval::engineError(message, code));
}


// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is defect class 2.
ojson utilSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}


// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot
// recognise. `reason` is the only channel carrying an exact rc (BUG-187) and it
// outranks the coarse mapping.
ojson utilErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}


bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}


// An error block hiding inside a primitive's return value, which may be a bare
// object or an array of blocks (runDrop/runEgen/runLabelDefine all return one
// shape or the other depending on the branch taken).
const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result) {
      if (isErrorBlock(b)) return &b;
    }
  }
  return nullptr;
}


ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}


// "This command printed nothing" -- an EMPTY array, which `pushEnvResult`
// flattens into zero blocks. Never a bare object without a "type" key
// (defect class 1).
ojson silent() { return ojson::array(); }


// ===========================================================================
// Small text / number helpers
// ===========================================================================

std::string lowerOf(std::string s) { return strutil::lower(std::move(s)); }

std::string trimOf(const std::string& s) { return strutil::trim(s); }


std::string upperAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}


std::string dequote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}


std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}


std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}


std::string rightAlign(const std::string& s, size_t w) {
  if (s.size() >= w) return s;
  return std::string(w - s.size(), ' ') + s;
}


// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which is exactly how a mistyped option value becomes a silently wrong number.
bool parseStrictDouble(const std::string& sIn, double* out) {
  const std::string s = trimOf(sIn);
  if (s.empty()) return false;
  if (s.find('x') != std::string::npos || s.find('X') != std::string::npos) return false;
  try {
    size_t pos = 0;
    const double v = std::stod(s, &pos);
    if (pos != s.size()) return false;
    *out = v;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}


bool parseStrictInt(const std::string& s, long long* out) {
  double d = 0.0;
  if (!parseStrictDouble(s, &d)) return false;
  if (d != std::floor(d)) return false;
  *out = static_cast<long long>(d);
  return true;
}


// A count, or any integral quantity, as Stata prints it.
std::string intText(int64_t v) { return std::to_string(v); }


// A value's DISPLAY text: an integral double loses its decimal point, anything
// else gets the shortest round-trippable decimal.
std::string numText(double v) {
  if (std::isnan(v)) return ".";
  if (v == std::floor(v) && std::fabs(v) < 1e15)
    return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
}


const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}


bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  // A bare flag parses as isBool/true; a valued spelling still means "given".
  return v->isBool ? v->boolValue : true;
}


std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return trimOf(dequote(trimOf(v->strValue)));
}


std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (auto v = optStr(opts, k)) return v;
  }
  return std::nullopt;
}


// ===========================================================================
// Qualifier honesty
// ===========================================================================
//
// Five of the thirteen registry rows in this bundle carry allowIf=true, so the
// TOKENIZER lets `if`/`in` through for them and the handler must either honour
// the qualifier or refuse it BY NAME. Silently computing over every observation
// while the user wrote `if` is the "answers a different question" shape
// (Doctrine gate ①), and it is the reason every refusal below names the
// qualifier it is refusing and says what to do instead.
ojson refuseQualifiers(const eval::ParsedCommand& parsed, const std::string& why) {
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    return ojson(eval::unsupportedOption("if " + *parsed.ifExpr, why));
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    return ojson(eval::unsupportedOption("in " + *parsed.inRange, why));
  }
  return ojson();  // null => no objection
}


// ===========================================================================
// cobra plumbing
// ===========================================================================

static std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}


static CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}


// Run ONE cobra verb. nullopt = success; otherwise the error BLOCK to return.
// Never a bare parse of stdout_text and never a swallowed rc (IDIOMS).
static std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return utilError(cobraErrorMessage(ce), ce.code);
}


// One CSV field, quoted only when it must be.
static std::string csvField(const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') {
      needQuote = true;
      break;
    }
  }
  if (!needQuote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"')
      out += "\"\"";
    else
      out += c;
  }
  out += "\"";
  return out;
}


// A numeric cell: BLANK for missing (`Frame::read_csv_column` reads an empty
// cell as missing), else the shortest round-trippable decimal, so the value
// that comes back is bit-identical. Missing is `.`, never 0 -- in every cell
// this file writes.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}


// The one-column HEADERLESS payload `cobra setcol` reads: one raw value per
// row, row count == n_rows exactly. String cells are quoted even when they do
// not need it, so a value's own leading/trailing padding survives the trip
// (BUG-292); an EMPTY cell is written as a bare blank line, which is the only
// spelling read_csv_column accepts for a missing factor cell.
static bool writePayloadCsv(const std::string& path, const std::vector<std::string>& cells, bool quoted) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  for (const auto& c : cells) {
    if (c.empty()) {
      f << "\n";
      continue;
    }
    if (quoted) {
      f << '"';
      for (char ch : c) {
        if (ch == '"') f << '"';
        f << ch;
      }
      f << '"';
    } else {
      f << c;
    }
    f << "\n";
  }
  f.flush();
  return static_cast<bool>(f);
}


// Write ONE column into `basePath` -- `isNew` chooses between `setcol --new`
// (a fresh variable) and an in-place rewrite of an existing one. `cells` are
// the rendered values ("" == missing); `dtype` is "f64" or "factor".
std::optional<ojson> writeColumn(SessionState& session, const std::string& basePath,
                                 const std::string& var, const std::vector<std::string>& cells,
                                 const std::string& dtype, bool isNew, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), "utilvals") + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  if (!writePayloadCsv(csvPath, cells, dtype == "factor")) {
    cecon::native_unlink(csvPath);
    return utilError("could not write the column payload for `" + var +
                         "' (disk full or I/O error)",
                     "data_io");
  }
  const std::string out = nextPath(session.workdir(), "setcol");
  std::vector<std::string> args = {"setcol", "--data",  basePath, "--var", var,
                                   "--values", csvPath, "--dtype", dtype};
  if (isNew) args.push_back("--new");
  args.push_back("--out");
  args.push_back(out);
  if (auto err = runVerb(args)) {
    cecon::native_unlink(out);
    return err;
  }
  *outPath = out;
  return std::nullopt;
}


std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), prefix) + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return utilError(prefix + ": cannot stage " + csvPath, "data_io");
    for (size_t c = 0; c < t.names.size(); ++c) {
      if (c) f << ',';
      f << csvField(t.names[c], ',');
    }
    f << "\n";
    for (const auto& row : t.rows) {
      // BUG-1078: a row whose every cell is missing would be a BLANK line, and
      // cobra's CSV reader skips blank lines (BUG-697's pad rows, same reader),
      // so a one-variable `input` with a `""` or `.` row lost it and stopped on
      // the row-count assert. A quoted empty first field keeps the row; convert
      // reads it as missing for a factor and for an f64 column alike.
      bool allEmpty = true;
      for (size_t c = 0; c < t.names.size() && allEmpty; ++c) {
        if (c < row.size() && !row[c].empty()) allEmpty = false;
      }
      for (size_t c = 0; c < t.names.size(); ++c) {
        if (c) f << ',';
        if (c == 0 && allEmpty) {
          f << "\"\"";
        } else if (c < row.size()) {
          f << csvField(row[c], ',');
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      return utilError(prefix + ": failed staging " + csvPath + " (disk full or I/O error)",
                       "data_io");
    }
  }
  std::string factorList;
  for (size_t c = 0; c < t.names.size(); ++c) {
    if (!t.isFactor[c]) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += t.names[c];
  }
  const std::string hbin = nextPath(session.workdir(), prefix);
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  if (auto err = runVerb(args)) {
    cecon::native_unlink(hbin);
    return err;
  }
  *outPath = hbin;
  return std::nullopt;
}


ColView viewOf(const Datastore& ds, const std::string& name) {
  ColView v;
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    v.isFactor = true;
    v.codes = c.codes();
    v.levels = &c.levels;
  } else {
    v.d = c.f64();
  }
  return v;
}


bool cellMissing(const ColView& v, int64_t i) {
  if (v.isFactor) {
    const int32_t code = v.codes[i];
    return code < 0 || static_cast<size_t>(code) >= v.levels->size();
  }
  return std::isnan(v.d[i]);
}


// A cell's IDENTITY key, for grouping and distinct-counting. Missing collapses
// to ONE sentinel so every missing cell in a column compares equal to every
// other (that is what makes `unique` count missing as one value); a real value
// is bit-normalized first, because ±0.0 are the same Stata value and must not
// key apart (IDIOMS: "±0.0 normalize before hashing/keying").
uint64_t cellKey(const ColView& v, int64_t i) {
  if (cellMissing(v, i)) return 0xFFFFFFFFFFFFFFFFull;
  if (v.isFactor) return static_cast<uint64_t>(v.codes[i]);
  double x = v.d[i];
  if (x == 0.0) x = 0.0;  // -0.0 -> +0.0
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(x), "double is not 64 bits");
  std::memcpy(&bits, &x, sizeof(bits));
  return bits;
}


// A column's values as DISPLAY strings ("" = missing) -- the level text for a
// factor, the round-trip decimal for a number. This is the exact shape a
// `setcol` payload takes, which is why carryforward can propagate a value
// without ever caring which kind of column it came from.
std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name) {
  std::vector<std::string> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const ColView v = viewOf(ds, name);
  for (int64_t i = 0; i < n; ++i) {
    if (cellMissing(v, i)) {
      out.emplace_back();
    } else if (v.isFactor) {
      out.push_back((*v.levels)[static_cast<size_t>(v.codes[i])]);
    } else {
      out.push_back(numCell(v.d[i]));
    }
  }
  return out;
}


// A new variable name has to be legal AND free. Both checks, once, so every
// handler in this file refuses the same things for the same reasons.
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return utilSyntax(verb + ": invalid variable name `" + newvar + "' -- " +
                      strutil::stataIdentifierRuleText());
  // BUG-1120: nor is a reserved name one: `carryforward x, gen(_cons)', `tsspell
  // x, spell(_cons)', `rangestat (mean) _cons=x, ...', `missings tag, gen(_cons)'
  // and `bcal create ..., generate(_cons)' are each "_cons invalid varname" r(198)
  // (StataBE 19.5).
  if (strutil::isReservedVarName(newvar)) return utilErrorRc(newvar + " invalid varname", 198);
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(newvar))
    return utilError(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // null => no objection
}


// Expand a user varlist against the CURRENT dataset, defaulting to every
// variable when the user named none. Nothing fancier than name lookup: the
// wildcard/hyphen grammar belongs to expandVarlist, which the callers that need
// it (renvarlab) use directly.
std::optional<ojson> resolveVarlist(const Datastore& ds, const std::string& verb,
                                    const std::vector<std::string>& asked,
                                    std::vector<std::string>* out) {
  if (asked.empty()) {
    *out = ds.names();
    if (out->empty()) return utilError(verb + ": the dataset has no variables");
    return std::nullopt;
  }
  for (const auto& nm : asked) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
    out->push_back(nm);
  }
  return std::nullopt;
}


// The per-variable missing count, off the resident card when it is CURRENT and
// off the Datastore otherwise. The card is the cheap plane -- reading it does
// not mmap the frame -- and its own contract says a caller MUST fall back, so
// this is the fallback written once instead of at four call sites.
int64_t missingCountOf(SessionState& session, Datastore& ds, const std::string& name) {
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) {
      if (vi.name == name) return vi.nMissing;
    }
  }
  return ds.nMissing(name);
}


// ===========================================================================
// Group keys
// ===========================================================================

// The by-group each row belongs to, as a dense 0-based id. Missing is a group
// like any other value (Stata's `by` puts the missing group last; this build
// only needs the PARTITION, never the group's own sort position, because every
// consumer writes its answers back by row index).
std::vector<int64_t> groupIds(const Datastore& ds, const std::vector<std::string>& byVars,
                              int64_t* nGroupsOut) {
  const int64_t n = ds.nRows();
  std::vector<int64_t> gid(static_cast<size_t>(n), 0);
  if (byVars.empty()) {
    *nGroupsOut = (n > 0) ? 1 : 0;
    return gid;
  }
  std::vector<ColView> views;
  views.reserve(byVars.size());
  for (const auto& b : byVars) views.push_back(viewOf(ds, b));
  std::map<std::vector<uint64_t>, int64_t> seen;
  for (int64_t i = 0; i < n; ++i) {
    std::vector<uint64_t> key;
    key.reserve(views.size());
    for (const auto& v : views) key.push_back(cellKey(v, i));
    auto it = seen.find(key);
    if (it == seen.end()) {
      const int64_t next = static_cast<int64_t>(seen.size());
      seen.emplace(std::move(key), next);
      gid[static_cast<size_t>(i)] = next;
    } else {
      gid[static_cast<size_t>(i)] = it->second;
    }
  }
  *nGroupsOut = static_cast<int64_t>(seen.size());
  return gid;
}


// ===========================================================================
// A dispatched `generate`, for the two commands that need a row-wise TRUTH
// value of an arbitrary Stata expression (marksample's `touse`, tsspell's
// cond()). PIET has exactly one expression evaluator and it lives behind
// generate; a private one here would be the two-layer divergence class.
// ===========================================================================

// Mints a temporary variable name via runTempvar (which guarantees legality and
// freedom from collision), materializes `expr` into it with a quietly-dispatched
// `generate byte`, and hands back the name. The CALLER drops it.
std::optional<ojson> materializeIndicator(SessionState& session, const std::string& verb,
                                          const std::string& expr, std::string* nameOut) {
  const std::string local = "__aer_util_ind";
  ojson tv = runTempvar({local}, session);
  if (const ojson* e = firstError(tv)) return *e;
  const std::string* minted = session.eval.locals.find(local);
  if (minted == nullptr || minted->empty())
    return utilError(verb + ": could not mint a temporary variable name", "internal");
  const std::string name = *minted;
  ojson blocks = dispatch("quietly generate byte " + name + " = (" + expr + ")", session);
  if (const ojson* e = firstError(blocks)) return *e;
  *nameOut = name;
  return std::nullopt;
}
}
