// AER-SCOPE1 bundle I (UTIL, 2026-09-04) -- the twelve data-tidying verbs the
// ninety-paper corpus reaches for between `use` and the first regression:
//
//   unique      distinct values of a varlist, or a per-group count column
//   gunique     gtools' spelling of the same command (ONE arm, one handler)
//   distinct    the per-variable total/distinct table
//   missings    report | dropvars | dropobs | tag(), each routed to a primitive
//   mdesc       the missing-value table, with r(miss)/r(total)/r(percent)
//   labmask     a value label synthesised from a second variable's text
//   renvarlab   a rename loop: prefix()/suffix()/upper/lower
//   splitvallabels  a value label's text, chopped for a graph's relabel()
//   marksample  the program-internal `touse` marker
//   filelist    the filesystem, as a dataset
//   carryforward  last-non-missing forward fill, reset per by-group
//   rangestat   a closed rolling window over a key variable
//   tsspell     spell/sequence/end columns over a tsset series
//
// NOTHING HERE COMPUTES A STATISTIC A SECOND TIME. That is the whole design:
// the per-variable missing counts come off the session's resident card (with
// `Datastore::nMissing` as the fallback the card's own contract demands),
// `missings tag` is `runEgen`'s rowmiss, `missings dropvars`/`dropobs` are
// `runDrop`/`runDropIf`, `labmask` is `runLabelDefine` + `runLabelValues`,
// `renvarlab` is a loop over `runRename`, `marksample` is `runTempvar` plus a
// dispatched `generate`, and `tsspell`'s cond() is a dispatched `generate`
// rather than a private expression evaluator. A second implementation of any
// of those is the two-layer divergence class this campaign keeps finding.
//
// NO ENGINE CHANGE, NO NEW COBRA VERB. Reads go through `fetchColumns` (with
// the materialized head as the fallback) or through `applyIfAndInFilter` when a
// qualifier is present; writes go through `cobra setcol` + `session.load` +
// `mutationResult`, the pair every data verb in this codebase has used since
// handlers.cpp:787-841.
//
// FIVE COMMANDS FROM THE PLAN'S SEVENTEEN ARE DELIBERATELY ABSENT, and the
// registry says so too. `dropmiss` and `renvars` have no SSC package under
// their own name and did not arrive with a sibling, so authorship could not be
// confirmed and the campaign's GPL-attribution rule gives them no registry row.
// `strdist`/`matchit`/`reclink` are record linkage, out of scope; they get no
// refusal ARM either, because an unregistered name already answers r(199),
// which is exactly what the refusal would have said. Adding a refusal arm for
// a name with no row would be a second, weaker copy of that answer.
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at
// least once in this campaign; the first has now been caught four times in six
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborts the whole selftest binary. The bundle-I arms in
//     dispatcher.cpp route through `pushEnvResult`, which flattens an array and
//     drops nulls, so an array IS legal -- but ONLY an array whose every
//     element is a real block object carrying a "type" key. Every `return`
//     below is one such object, or an ojson::array() of such objects (possibly
//     empty, which is how the silent verbs say "nothing to print").
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`/`unsupported_option`;
//     a refusal built with the default `engine_error` code silently becomes
//     rc 1. So: a USER SYNTAX error carries "syntax_error", a real Stata option
//     this build does not implement goes through the reject net or
//     `unsupportedOption`, a command-level refusal goes through
//     `eval::refusedCommand`, and an error with a known Stata rc that
//     coarseRc's message rules cannot recognise states it through the `reason`
//     channel (`utilErrorRc` below).
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "generate"` comparison see them -- is driven by the
//     vocabulary REGISTRY. A command absent from the registry gets its
//     abbreviations accepted by the net and then dropped unread. All thirteen
//     command tokens register at NAMESPACE scope below (a function-local static
//     constructs too late: after the first call that needed it), and every
//     handler's first act is the reject net. An option this build does NOT read
//     goes in the `unimplemented` list, never in `known`.
//  4. R-CLASS. `unique` (r(unique)/r(N)/r(sum)), `distinct` (r(ndistinct)/
//     r(N)), `mdesc` (r(miss)/r(total)/r(percent)), `splitvallabels`
//     (r(relabel)) and `filelist` (r(N)) all publish. The arms do not call
//     captureRFromBlock for these commands, so each handler fills
//     `session.eval.rValues`/`rStrings` itself.
//  5. ECHOED PATHS. `filelist` reports a directory. It echoes the string the
//     USER TYPED, never `resolveUserPath`'s output -- the resolved path is used
//     to walk the filesystem and nowhere else.
//
// THE BY-PREFIX IS NOT PARSED HERE. `bysort id: carryforward y, gen(f)` is
// handled in dispatcher.cpp (the "bundle I (UTIL)" branch beside egen's): it
// sorts and re-dispatches the line with `by()` injected as an ordinary option.
// `carryforward` and `tsspell` therefore see `by()` and never a prefix.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include "piet/bcal.h"  // AER scope-2 B2-3: business calendars
#include "piet/stata_format.h"  // piet::fmt::renderTd -- the describe table date column
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // marksample / tsspell cond(): a dispatched `generate`
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG -- Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is what the handler below actually READS; `unimplemented` names real
// Stata options of the same command that this build does not do. The split is
// the whole point: a name in NEITHER set reads as "unknown option", which is
// the wrong diagnosis for a real option, and a name wrongly placed in `known`
// is accepted and silently ignored, which is the defect the apparatus exists to
// prevent.
namespace {

// `unique` and `gunique` are ONE handler, so they share ONE vocabulary. Both
// gunique-only options are refused for both spellings -- `unique, detail`
// prints a per-variable breakdown and `gunique, replace` REPLACES the data in
// memory with the distinct rows, and either accepted-and-ignored would answer a
// different question at rc 0.
const piet::eval::OptVocab& kVocabUnique = piet::eval::registerOptVocab(
    {"unique", "gunique"}, {{"by", "gen", "generate"}, {"detail", "replace"}});

// `abbrev()` shortens the displayed names, `min()`/`max()` filter the rows this
// build prints in full. None is read; each changes what the table SAYS.
const piet::eval::OptVocab& kVocabDistinct = piet::eval::registerOptVocab(
    {"distinct"}, {{"joint", "missing"}, {"abbrev", "min", "max"}});

// `force` IS read: it is missings' own safety catch (see runMissings), not a
// no-op. `generate()` names the column `missings tag` creates.
const piet::eval::OptVocab& kVocabMissings = piet::eval::registerOptVocab(
    {"missings"},
    {{"generate", "force"}, {"minimum", "observations", "percent", "sort"}});

// `any`/`all`/`none` select WHICH variables mdesc reports; `abbreviate()`
// shortens the names. All four would change the table.
const piet::eval::OptVocab& kVocabMdesc = piet::eval::registerOptVocab(
    {"mdesc"}, {{}, {"abbreviate", "any", "all", "none"}});

// `decode` runs labmask in reverse (label -> a new string variable), which is a
// different command's output, not an option on this one's.
const piet::eval::OptVocab& kVocabLabmask = piet::eval::registerOptVocab(
    {"labmask"}, {{"values", "lblname"}, {"decode"}});

// This build's renvarlab is the NAME-transform half: prefix/suffix/case. The
// package's label-driven renaming (rename each variable to a token of its
// variable LABEL) is a different derivation of the new names and is refused
// rather than approximated.
const piet::eval::OptVocab& kVocabRenvarlab = piet::eval::registerOptVocab(
    {"renvarlab"}, {{"prefix", "suffix", "upper", "lower"},
                    {"label", "postfix", "subst", "presub", "postsub", "trim", "map", "symbol",
                     "display", "test"}});

// ---- AER scope 2, bundle B2 (2026-09-07) ----------------------------------
// strdist's own `syntax` line is
//   syntax anything [if] [in], [Generate(name) MAXdist(integer -1)]
// and both options are READ here. There is nothing to put in `unimplemented`:
// the package has exactly these two.
const piet::eval::OptVocab& kVocabStrdist = piet::eval::registerOptVocab(
    {"strdist"}, {{"generate", "maxdist"}, {}});

// `nobreak` suppresses the chopping this command exists to do; `recode()`
// renumbers the codes r(relabel) carries.
const piet::eval::OptVocab& kVocabSplitvallabels = piet::eval::registerOptVocab(
    {"splitvallabels"}, {{"length"}, {"nobreak", "recode"}});

// `strok` tells marksample to KEEP observations whose string variables are
// non-empty rather than marking them out, and `zeroweight` keeps zero-weight
// observations. Both change which rows the marker selects, so neither may be
// accepted-and-ignored -- the plan's scope table calls them "accept-ignore",
// which is precisely the shape defect class 3 forbids.
const piet::eval::OptVocab& kVocabMarksample = piet::eval::registerOptVocab(
    {"marksample"}, {{"novarlist"}, {"strok", "zeroweight"}});

// `save()`/`replace` write the file list OUT to a .dta; this build's filelist
// only builds it in memory.
const piet::eval::OptVocab& kVocabFilelist = piet::eval::registerOptVocab(
    {"filelist"}, {{"directory", "pattern", "norecursive"}, {"save", "replace"}});

// `by` arrives from the dispatcher's by-prefix branch as an ordinary option.
// `carryalong()` drags other variables along with each filled value,
// `dynamic_condition()` makes the fill conditional, `cfindic()` records which
// values were filled, `strict` errors instead of filling -- each changes what
// lands in the data.
const piet::eval::OptVocab& kVocabCarryforward = piet::eval::registerOptVocab(
    {"carryforward"}, {{"gen", "generate", "replace", "by"},
                       {"carryalong", "strict", "dynamic_condition", "cfindic"}});

// `excludeself` drops the row itself from its own window and `casewise` drops
// rows missing on ANY of the requested variables -- both change which
// observations enter every statistic.
const piet::eval::OptVocab& kVocabRangestat = piet::eval::registerOptVocab(
    {"rangestat"}, {{"interval", "by"}, {"excludeself", "casewise"}});

// `fcond()`/`pcond()` are tsspell's forward/previous-observation conditions:
// they change where a spell BEGINS, which is the one thing this command decides.
const piet::eval::OptVocab& kVocabTsspell = piet::eval::registerOptVocab(
    {"tsspell"}, {{"cond", "spell", "seq", "end", "by"}, {"fcond", "pcond"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ===========================================================================
// Errors
// ===========================================================================

ojson utilError(const std::string& message, const std::string& code = "engine_error") {
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

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

CobraError errorFromRaw(const CobraRunResult& r) {
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
std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return utilError(cobraErrorMessage(ce), ce.code);
}

// One CSV field, quoted only when it must be.
std::string csvField(const std::string& s, char delim) {
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
bool writePayloadCsv(const std::string& path, const std::vector<std::string>& cells, bool quoted) {
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

// A whole table on its way to `cobra convert` (filelist's only write shape).
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                  // parallel to `names`
  std::vector<std::vector<std::string>> rows;  // rows[r][c]; "" == missing
};

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
      for (size_t c = 0; c < t.names.size(); ++c) {
        if (c) f << ',';
        if (c < row.size()) f << csvField(row[c], ',');
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

// ===========================================================================
// Reading
// ===========================================================================

// THE SHARED READ SHAPE, in two flavours.
//
//  - No qualifier: ask the worker for a contract_A section-38 projection of
//    just the columns this command needs, and fall back to the session's own
//    materialized head whenever the projection is unavailable. A miss must cost
//    a fallback, never an answer -- column_fetch.h's own law.
//  - `if`/`in` present: `applyIfAndInFilter` writes a FILTERED scratch hbin and
//    this reads that. Row identity is lost by construction, which is exactly
//    why only the read-only verbs (unique's report form, distinct) accept a
//    qualifier; every verb that WRITES a column refuses one by name instead of
//    quietly producing a short column or a misaligned one.
class ReadScope {
 public:
  ReadScope(SessionState& session, const eval::ParsedCommand& parsed,
            const std::vector<std::string>& cols, const std::string& verb, bool honourQualifiers) {
    const bool wantFilter =
        honourQualifiers && ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                             (parsed.inRange.has_value() && !parsed.inRange->empty()));
    if (wantFilter) {
      if (!session.hasData()) {
        err_ = ojson(eval::noDataLoaded());
        return;
      }
      try {
        InRangeFilterResult f =
            applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, parsed.inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          // BUG-381: this filtered copy is READ and abandoned -- it never
          // becomes the head, so only the section-38 projection ledger can
          // reach it.
          session.noteProjectionScratch(f.dataPath);
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
        } else {
          ds_ = session.datastore();
        }
      } catch (const std::exception& e) {
        err_ = utilError(verb + ": could not apply the qualifier -- " + std::string(e.what()),
                         "data_io");
      }
      return;
    }
    try {
      if (!cols.empty()) {
        std::vector<const std::string*> exprs;
        exprs.reserve(cols.size());
        for (const auto& c : cols) exprs.push_back(&c);
        if (auto want = referencedColumns(session, exprs, "")) {
          std::string projPath;
          std::unique_ptr<Datastore> proj = fetchColumns(session, *want, session.workdir(),
                                                         &projPath);
          if (proj) {
            bool complete = true;
            for (const auto& c : cols) {
              if (!proj->has(c)) {
                complete = false;
                break;
              }
            }
            if (complete) owned_ = std::move(proj);
          }
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = utilError(verb + ": could not read the data -- " + std::string(e.what()), "data_io");
    }
  }

  Datastore* get() const { return ds_; }
  const ojson& error() const { return err_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

// A non-owning view of one column, type-erased just enough that the tuple/key
// code below never asks a second time what kind of column it is holding.
struct ColView {
  bool isFactor = false;
  const double* d = nullptr;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* levels = nullptr;
};

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

}  // namespace

// ===========================================================================
// unique / gunique
// ===========================================================================
//
// ONE arm, two spellings. `unique varlist` prints the two lines every do-file
// greps for and publishes r(unique)/r(N)/r(sum); `unique varlist, by(g)
// gen(new)` writes the per-group distinct count into EVERY observation of the
// group and prints nothing.
//
// MISSING IS A VALUE HERE, deliberately: `unique id` on a column with three
// missing ids counts one more distinct value, matching unique.ado (this is the
// exact point where `unique` and `distinct` disagree, and it is why they are
// two commands rather than one with a flag).
ojson runUnique(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string typed = lowerOf(parsed.cmd);
  const bool gtools = !typed.empty() && typed[0] == 'g';
  const std::string verb = gtools ? "gunique" : "unique";

  // The two options that are gunique's alone, named with the spelling the user
  // typed. The reject net below would catch both anyway (they are in the shared
  // `unimplemented` list), but the net cannot say WHICH command's option it is
  // refusing, and "gunique, replace" replacing the data in memory is a large
  // enough difference to be worth naming.
  if (hasFlag(parsed.opts, "replace")) {
    return ojson(eval::unsupportedOption(
        "replace",
        "`" + verb +
            ", replace` REPLACES the data in memory with one row per distinct combination. This "
            "build's " + verb +
            " only counts; use `duplicates drop` or `contract` to collapse the data."));
  }
  if (hasFlag(parsed.opts, "detail")) {
    return ojson(eval::unsupportedOption(
        "detail",
        "`" + verb +
            ", detail` prints a per-variable breakdown this build does not produce. Run `distinct "
            "varlist` for the per-variable table."));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabUnique.known,
                                            kVocabUnique.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(verb + ": syntax is `" + verb + " varlist [if] [in] [, by(varlist) gen(newvar)]`");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"gen", "generate"});

  if (!byVars.empty() && !gen.has_value()) {
    // unique.ado's by() form exists ONLY to fill a variable; without gen()
    // there is nothing to print that the no-by form does not already print.
    // Refusing by NAME (rather than inventing a per-group display table) keeps
    // the answer honest about what this build does.
    return ojson(eval::refusedCommand(verb, "unique_by_without_gen"));
  }
  if (gen.has_value() && byVars.empty())
    return utilSyntax(verb + ": gen() requires by() -- `" + verb +
                      " varlist, by(groupvars) gen(newvar)` writes the per-group count of "
                      "distinct " + joinWith(parsed.varlist, " ") + " combinations.");

  // The gen() form WRITES a column, so it must be as long as the frame and
  // aligned to it. `applyIfAndInFilter` returns a filtered FRAME, not a mask,
  // so honouring a qualifier here would need row identity this layer does not
  // have -- and the failure mode would be a plausible-looking, misaligned
  // column. Refused by name instead. The report form has no such problem and
  // honours both qualifiers.
  if (gen.has_value()) {
    ojson q = refuseQualifiers(
        parsed, "`" + verb +
            ", gen()` writes a column as long as the dataset, and this build cannot map a "
            "filtered sample back onto the original rows. Restrict first (`preserve` + `keep "
            "if ...`), then run " + verb + ".");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = parsed.varlist;
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, verb, !gen.has_value());
  if (scope.failed())
    return scope.error().is_null() ? utilError(verb + ": could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }

  const int64_t n = ds.nRows();
  std::vector<ColView> views;
  views.reserve(parsed.varlist.size());
  for (const auto& nm : parsed.varlist) views.push_back(viewOf(ds, nm));

  auto rowKey = [&](int64_t i) {
    std::vector<uint64_t> key;
    key.reserve(views.size());
    for (const auto& v : views) key.push_back(cellKey(v, i));
    return key;
  };

  if (byVars.empty()) {
    std::set<std::vector<uint64_t>> distinctKeys;
    for (int64_t i = 0; i < n; ++i) distinctKeys.insert(rowKey(i));
    const int64_t nUnique = static_cast<int64_t>(distinctKeys.size());
    session.eval.rValues.set("unique", static_cast<double>(nUnique));
    session.eval.rValues.set("N", static_cast<double>(n));
    // r(sum) is unique.ado's own third return: the total of the per-group
    // counts, which without by() is just the one count.
    session.eval.rValues.set("sum", static_cast<double>(nUnique));
    std::ostringstream oss;
    oss << "Number of unique values of " << joinWith(parsed.varlist, " ") << " is  " << nUnique
        << "\n";
    oss << "Number of records is  " << n << "\n";
    return displayBlock(oss.str());
  }

  // by() + gen(): the per-group count, written on the group's FIRST observation
  // and MISSING everywhere else.
  //
  // BUG-422: this used to fill every observation of the group, and the comment
  // here said unique.ado does the same. The P5 wave pin — which this comment
  // named as the thing that would settle it — was captured on StataBE 19.5 with
  // unique 1.2.4 on 2026-09-06 and settled it the other way: `summarize` over
  // the generated column reads N=5, sum=200 (five groups, one row each), not
  // N=4800, sum=192000. unique.ado does `by `by': gen `gen' = `count' if _n==1`.
  // Filling the whole group is the more USEFUL answer and it is not Stata's; a
  // do-file that then does `collapse (mean) u, by(region)` gets the same number
  // either way, and one that does `summarize u` does not.
  int64_t nGroups = 0;
  std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);
  std::vector<std::set<std::vector<uint64_t>>> perGroup(static_cast<size_t>(nGroups));
  std::set<std::vector<uint64_t>> overall;
  for (int64_t i = 0; i < n; ++i) {
    std::vector<uint64_t> k = rowKey(i);
    overall.insert(k);
    perGroup[static_cast<size_t>(gid[static_cast<size_t>(i)])].insert(std::move(k));
  }
  const std::string newvar = *gen;
  ojson guard = newVarGuard(session, verb, newvar);
  if (!guard.is_null()) return guard;

  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  int64_t total = 0;
  for (const auto& g : perGroup) total += static_cast<int64_t>(g.size());
  std::vector<bool> groupSeen(static_cast<size_t>(nGroups), false);
  for (int64_t i = 0; i < n; ++i) {
    const size_t g = static_cast<size_t>(gid[static_cast<size_t>(i)]);
    if (groupSeen[g]) {
      cells.emplace_back();  // Stata missing on every row but the group's first
      continue;
    }
    groupSeen[g] = true;
    cells.push_back(numCell(static_cast<double>(perGroup[g].size())));
  }
  std::string out;
  if (auto err = writeColumn(session, *session.currentHbin(), newvar, cells, "f64", true, &out))
    return *err;
  session.load(out);
  // r(unique) stays the DATASET's distinct count in both forms -- it answers
  // the same question the no-by form's line prints. r(sum) is what the by()
  // form adds: the total of the per-group counts, which equals r(unique) only
  // when no value is shared across groups.
  session.eval.rValues.set("unique", static_cast<double>(overall.size()));
  session.eval.rValues.set("N", static_cast<double>(n));
  session.eval.rValues.set("sum", static_cast<double>(total));
  return mutationResult(session, "gen", {newvar});
}

// ===========================================================================
// distinct
// ===========================================================================
//
// The table `distinct` prints, and the ONE place this bundle's two counting
// commands differ: distinct EXCLUDES missing unless `missing` is given, where
// `unique` always counts it as a value.
ojson runDistinct(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDistinct.known,
                                            kVocabDistinct.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const bool joint = hasFlag(parsed.opts, "joint");
  const bool withMissing = hasFlag(parsed.opts, "missing");

  ReadScope scope(session, parsed, parsed.varlist, "distinct", true);
  if (scope.failed())
    return scope.error().is_null() ? utilError("distinct: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "distinct", parsed.varlist, &vars)) return *err;

  const int64_t n = ds.nRows();
  std::vector<ColView> views;
  views.reserve(vars.size());
  for (const auto& nm : vars) views.push_back(viewOf(ds, nm));

  struct Row {
    std::string name;
    int64_t total = 0;
    int64_t distinct = 0;
  };
  std::vector<Row> rows;

  if (joint) {
    // One row: the distinct COMBINATIONS. Without `missing`, an observation
    // missing on ANY of the variables is out of the count entirely (casewise),
    // which is what makes "total" here smaller than any single variable's.
    std::set<std::vector<uint64_t>> keys;
    int64_t total = 0;
    for (int64_t i = 0; i < n; ++i) {
      bool anyMissing = false;
      for (const auto& v : views) {
        if (cellMissing(v, i)) {
          anyMissing = true;
          break;
        }
      }
      if (anyMissing && !withMissing) continue;
      std::vector<uint64_t> key;
      key.reserve(views.size());
      for (const auto& v : views) key.push_back(cellKey(v, i));
      keys.insert(std::move(key));
      ++total;
    }
    rows.push_back(Row{"joint", total, static_cast<int64_t>(keys.size())});
  } else {
    for (size_t c = 0; c < vars.size(); ++c) {
      std::set<uint64_t> keys;
      int64_t total = 0;
      for (int64_t i = 0; i < n; ++i) {
        if (cellMissing(views[c], i) && !withMissing) continue;
        keys.insert(cellKey(views[c], i));
        ++total;
      }
      rows.push_back(Row{vars[c], total, static_cast<int64_t>(keys.size())});
    }
  }

  // Layout: the name column is right-aligned at min(12, longest name), then
  // ` |`, then two %10.0g-shaped count fields. A name longer than the column
  // simply widens its own row rather than being truncated -- `abbrev()` is the
  // option that would shorten names and it is refused, so silently shortening
  // one here would be that refused option arriving through the back door.
  size_t nameW = 0;
  for (const auto& r : rows) nameW = std::max(nameW, r.name.size());
  nameW = std::min<size_t>(nameW, 12);
  if (nameW == 0) nameW = 1;

  std::ostringstream oss;
  oss << std::string(nameW, ' ') << " |" << rightAlign("Observations", 21) << "\n";
  oss << std::string(nameW, ' ') << " |" << rightAlign("total", 10) << rightAlign("distinct", 11)
      << "\n";
  oss << std::string(nameW + 1, '-') << "+" << std::string(21, '-') << "\n";
  for (const auto& r : rows) {
    oss << rightAlign(r.name, nameW) << " |" << rightAlign(piet::fmtStataDefaultG(
               static_cast<double>(r.total)), 10)
        << rightAlign(piet::fmtStataDefaultG(static_cast<double>(r.distinct)), 11) << "\n";
  }

  // distinct.ado returns the LAST row's numbers (the joint row when joint was
  // asked for), not a total across rows.
  if (!rows.empty()) {
    session.eval.rValues.set("ndistinct", static_cast<double>(rows.back().distinct));
    session.eval.rValues.set("N", static_cast<double>(rows.back().total));
  }
  return displayBlock(oss.str());
}

// ===========================================================================
// missings
// ===========================================================================
//
// FOUR SUB-VERBS, FOUR EXISTING PRIMITIVES. `report` reads the card; `dropvars`
// is runDrop; `dropobs` is runDropIf; `tag` is runEgen's rowmiss. The
// sub-verb is the first positional token, exactly as in missings.ado.
//
// WHY `report`'s OBSERVATION COUNT DOES NOT GO THROUGH rowmiss, which is what
// the plan specifies. `Frame::row_egen` calls `require_f64` on every column
// (src/data/frame.cpp), so rowmiss over a varlist containing a string variable
// THROWS -- and `missings report` with no varlist covers every variable, which
// on any real dataset includes strings. The two ways to use rowmiss here are
// (a) let the report fail on any dataset with a string variable, or (b) run it
// over the numeric subset and print a count that silently ignores missing
// string cells. Neither is acceptable, so the per-OBSERVATION predicate is
// computed here, over the same columns the per-VARIABLE table reports, while
// the per-variable COUNTS still come from the card and are never re-derived.
// `tag` still routes to runEgen: the user asked for that variable by name, and
// an honest engine error on a string varlist beats a second rowmiss.
ojson runMissings(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMissings.known,
                                            kVocabMissings.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(
        "missings: syntax is `missings report|dropvars|dropobs|tag [varlist] [, generate(newvar) "
        "force]`");

  const std::string sub = lowerOf(parsed.varlist[0]);
  const std::vector<std::string> asked(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (sub != "report" && sub != "dropvars" && sub != "dropobs" && sub != "tag") {
    return ojson(eval::unsupportedCommand("missings " + parsed.varlist[0], std::nullopt,
                                          std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // `report` and `dropvars` both answer WHOLE-COLUMN questions off the card's
  // counts, which describe every observation; a qualifier would have to change
  // the counts themselves, so it is refused by name rather than ignored.
  // `dropobs` and `tag` forward the qualifier into the primitive they route to,
  // which already implements it.
  if (sub == "report" || sub == "dropvars") {
    ojson q = refuseQualifiers(
        parsed, "`missings " + sub +
            "` answers from the dataset's per-variable missing counts, which describe every "
            "observation. Restrict first (`preserve` + `keep if ...`), then run it.");
    if (!q.is_null()) return q;
  }

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("missings: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());
  Datastore& ds = *dsp;

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "missings " + sub, asked, &vars)) return *err;
  const int64_t n = ds.nRows();
  const bool force = hasFlag(parsed.opts, "force");

  if (sub == "report") {
    std::ostringstream oss;
    oss << "Checking missings in " << (asked.empty() ? "all variables" : joinWith(vars, " "))
        << ":\n";
    size_t nameW = 0;
    for (const auto& nm : vars) nameW = std::max(nameW, nm.size());
    nameW = std::max<size_t>(nameW, 8);
    int64_t reported = 0;
    for (const auto& nm : vars) {
      const int64_t miss = missingCountOf(session, ds, nm);
      if (miss <= 0) continue;
      ++reported;
      oss << rightAlign(nm, nameW) << rightAlign(intText(miss), 12) << "\n";
    }
    if (reported == 0) oss << "  (no variables with missing values)\n";

    // The per-observation predicate: a row counts when ANY variable in scope is
    // missing there. See this function's header for why it is not rowmiss.
    std::vector<ColView> views;
    views.reserve(vars.size());
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    int64_t rowsWithMissing = 0;
    for (int64_t i = 0; i < n; ++i) {
      for (const auto& v : views) {
        if (cellMissing(v, i)) {
          ++rowsWithMissing;
          break;
        }
      }
    }
    oss << rowsWithMissing << " observations with missing values\n";
    return displayBlock(oss.str());
  }

  if (sub == "dropvars") {
    std::vector<std::string> doomed;
    for (const auto& nm : vars) {
      if (missingCountOf(session, ds, nm) == n && n > 0) doomed.push_back(nm);
    }
    if (doomed.empty()) return silent();
    if (doomed.size() == static_cast<size_t>(ds.nCols()) && !force) {
      // missings.ado's own safety catch, which is what `force` is FOR -- the
      // option is read here rather than accepted and ignored.
      return utilError(
          "missings dropvars: every variable in the dataset is missing in every observation; "
          "dropping them would leave nothing. Add `force` if that is what you want.",
          "bad_input");
    }
    return runDrop(doomed, session);
  }

  if (sub == "dropobs") {
    // The predicate goes to the ENGINE's expression evaluator via runDropIf,
    // and that evaluator has no missing() for a string column
    // (`require_f64`). Naming the offending variable is honest; building the
    // condition out of the numeric subset only would drop rows on a narrower
    // test than the user asked for.
    for (const auto& nm : vars) {
      if (ds.dtype(nm) == "factor")
        return utilError(
            "missings dropobs: `" + nm +
                "' is a string variable, and this build's expression evaluator has no missing() "
                "for string columns. Name the numeric variables explicitly.",
            "unimplemented");
    }
    std::string cond;
    for (const auto& nm : vars) {
      if (!cond.empty()) cond += " & ";
      cond += "missing(" + nm + ")";
    }
    // The user's own `if` is ANDed in, never dropped: `missings dropobs x if
    // year > 2000` must delete rows in 2001 and later only. runDropIf takes
    // `in` as its own argument and composes it the same way.
    const bool qualified = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                           (parsed.inRange.has_value() && !parsed.inRange->empty());
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty())
      cond = "(" + *parsed.ifExpr + ") & (" + cond + ")";
    // Would this drop every observation? Same catch as dropvars, same option.
    // Skipped when a qualifier is present: the count below is taken over the
    // whole frame, so it would answer a question the user did not ask.
    if (!force && !qualified) {
      std::vector<ColView> views;
      views.reserve(vars.size());
      for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
      int64_t doomedRows = 0;
      for (int64_t i = 0; i < n; ++i) {
        bool all = true;
        for (const auto& v : views) {
          if (!cellMissing(v, i)) {
            all = false;
            break;
          }
        }
        if (all) ++doomedRows;
      }
      if (n > 0 && doomedRows == n)
        return utilError(
            "missings dropobs: every observation is missing on all of " + joinWith(vars, " ") +
                "; dropping them would leave no data. Add `force` if that is what you want.",
            "bad_input");
    }
    return runDropIf(cond, session, parsed.inRange);
  }

  // tag
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"generate", "gen"});
  if (!gen.has_value())
    return utilSyntax("missings tag: requires generate(newvar) -- the tag has nowhere to go "
                      "otherwise.");
  ojson guard = newVarGuard(session, "missings tag", *gen);
  if (!guard.is_null()) return guard;

  // rowmiss ONLY when every variable in scope is numeric. `Frame::row_egen`
  // calls `require_f64`, so rowmiss over a varlist containing a string variable
  // THROWS -- and `missings tag` with no varlist covers every variable, which on
  // any real dataset includes strings. Measured: the registry pangram's
  // `missings tag, generate(...)` on kitchensink_panel (which has the factor
  // column `grp`) died with "column 'grp' is a factor, not f64".
  //
  // The fallback is NOT a second definition of missingness: it is the SAME
  // per-row predicate `report` computes a dozen lines above, over the same
  // ColViews, so the count `report` prints and the column `tag` writes can
  // never disagree. Restricting rowmiss to the numeric subset instead would
  // have tagged rows on a narrower test than the user asked for -- a silently
  // wrong column, which is the one outcome worth more code to avoid.
  bool allNumeric = true;
  for (const auto& nm : vars) {
    if (ds.dtype(nm) == "factor") {
      allNumeric = false;
      break;
    }
  }
  if (allNumeric) {
    return runEgen(*gen + " = rowmiss(" + joinWith(vars, " ") + ")", std::string(), parsed.ifExpr,
                   {}, session, parsed.inRange);
  }

  std::vector<ColView> tagViews;
  tagViews.reserve(vars.size());
  for (const auto& nm : vars) tagViews.push_back(viewOf(ds, nm));
  std::vector<std::string> tagCells;
  tagCells.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    double c = 0.0;
    for (const auto& v : tagViews) {
      if (cellMissing(v, i)) c += 1.0;
    }
    tagCells.push_back(numCell(c));
  }
  std::string tagOut;
  if (auto err = writeColumn(session, *session.currentHbin(), *gen, tagCells, "f64", true, &tagOut))
    return *err;
  session.load(tagOut);
  return mutationResult(session, "gen", {*gen});
}

// ===========================================================================
// mdesc
// ===========================================================================
//
// The same card counts `missings report` reads, in mdesc's own three-column
// layout. Nothing is scanned: every number below is either the card's or
// arithmetic on it.
ojson runMdesc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMdesc.known,
                                            kVocabMdesc.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("mdesc: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());
  Datastore& ds = *dsp;

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "mdesc", parsed.varlist, &vars)) return *err;
  const int64_t n = ds.nRows();

  size_t nameW = 0;
  for (const auto& nm : vars) nameW = std::max(nameW, nm.size());
  nameW = std::max<size_t>(nameW, 8);

  std::ostringstream oss;
  oss << rightAlign("Variable", nameW) << " |" << rightAlign("Missing", 12)
      << rightAlign("Total", 12) << rightAlign("Percent Missing", 18) << "\n";
  oss << std::string(nameW + 1, '-') << "+" << std::string(42, '-') << "\n";
  int64_t lastMiss = 0;
  double lastPct = 0.0;
  for (const auto& nm : vars) {
    const int64_t miss = missingCountOf(session, ds, nm);
    const double pct = (n > 0) ? (100.0 * static_cast<double>(miss) / static_cast<double>(n)) : 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", pct);
    oss << rightAlign(nm, nameW) << " |" << rightAlign(intText(miss), 12)
        << rightAlign(intText(n), 12) << rightAlign(std::string(buf), 18) << "\n";
    lastMiss = miss;
    lastPct = pct;
  }
  // mdesc.ado leaves r() describing the LAST variable it processed, the same
  // rule the plan states for distinct's r(ndistinct)/r(N). This is the one
  // number in this bundle that the P5 pin would flip if the oracle keeps a
  // whole-table total instead.
  session.eval.rValues.set("miss", static_cast<double>(lastMiss));
  session.eval.rValues.set("total", static_cast<double>(n));
  session.eval.rValues.set("percent", lastPct);
  return displayBlock(oss.str());
}

// ===========================================================================
// labmask
// ===========================================================================
//
// `labmask numvar, values(textvar)` gives numvar a value label whose text comes
// from textvar. It is a LABEL command, not a data command: the composed
// `label define` goes to runLabelDefine and the attach to runLabelValues, so
// the label plane has exactly one writer.
ojson runLabmask(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabmask.known,
                                            kVocabLabmask.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return utilSyntax("labmask: syntax is `labmask numvar, values(varname) [lblname(name)]`");
  const std::string numvar = parsed.varlist[0];
  const std::optional<std::string> valuesVar = optStr(parsed.opts, "values");
  if (!valuesVar.has_value() || valuesVar->empty())
    return utilSyntax("labmask: values(varname) is required -- it names the variable whose text "
                      "becomes the label.");
  const std::string lblname = optStr(parsed.opts, "lblname").value_or(numvar);
  if (!strutil::isValidStataIdentifier(lblname))
    return utilSyntax("labmask: `" + lblname + "' is not a usable label name -- " +
                      strutil::stataIdentifierRuleText());

  ReadScope scope(session, parsed, {numvar, *valuesVar}, "labmask", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("labmask: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  if (!ds.has(numvar)) return utilError("variable " + numvar + " not found");
  if (!ds.has(*valuesVar)) return utilError("variable " + *valuesVar + " not found");
  if (ds.dtype(numvar) == "factor")
    return utilError("labmask: `" + numvar +
                     "' is a string variable; a value label can only be attached to a numeric "
                     "variable.");

  const int64_t n = ds.nRows();
  const ColView num = viewOf(ds, numvar);
  const std::vector<std::string> text = columnAsStrings(ds, *valuesVar);

  // Two things must hold before a single label is defined: every code is an
  // integer, and every observation with the same code carries the same text.
  // Both are checked over the WHOLE column first -- a half-written label set
  // would be worse than no label at all.
  std::map<long long, std::string> mapping;
  for (int64_t i = 0; i < n; ++i) {
    if (cellMissing(num, i)) continue;
    const double v = num.d[i];
    if (v != std::floor(v))
      return utilError("labmask: `" + numvar + "' has the non-integer value " + numText(v) +
                       " in observation " + intText(i + 1) +
                       "; a value label can only label integer codes.");
    if (std::fabs(v) > 2147483647.0)
      return utilError("labmask: `" + numvar + "' has the value " + numText(v) +
                       " in observation " + intText(i + 1) +
                       ", which is outside the range a value label can code.");
    const long long code = static_cast<long long>(v);
    const std::string& t = text[static_cast<size_t>(i)];
    if (t.empty()) continue;  // no text to attach for this observation
    if (t.find('"') != std::string::npos)
      return utilError("labmask: the text for code " + std::to_string(code) + " (`" + t +
                       "') contains a double quote, which `label define` cannot carry in this "
                       "build. Remove the quote from `" + *valuesVar + "' first.");
    auto it = mapping.find(code);
    if (it == mapping.end()) {
      mapping.emplace(code, t);
    } else if (it->second != t) {
      return utilError("labmask: code " + std::to_string(code) + " of `" + numvar +
                       "' carries two different texts in `" + *valuesVar + "' (`" + it->second +
                       "' and `" + t + "'); labmask needs one text per code.");
    }
  }
  if (mapping.empty())
    return utilError("labmask: `" + *valuesVar + "' supplies no text for any value of `" + numvar +
                     "'; there is nothing to label.");

  std::string line = "label define " + lblname;
  for (const auto& [code, t] : mapping) line += " " + std::to_string(code) + " \"" + t + "\"";
  line += ", modify";
  ojson defined = runLabelDefine(line, session);
  if (const ojson* e = firstError(defined)) return *e;
  ojson attached = runLabelValues(numvar, lblname, session);
  if (const ojson* e = firstError(attached)) return *e;
  return silent();
}

// ===========================================================================
// renvarlab
// ===========================================================================
//
// A rename LOOP over runRename, with every target name validated before the
// first rename happens -- a collision on the third variable must not leave the
// first two renamed. The package's label-driven renaming (derive each new name
// from the variable's LABEL) is refused by the vocabulary rather than
// approximated: that is a different derivation of the names, not an option on
// this one.
ojson runRenvarlab(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRenvarlab.known,
                                            kVocabRenvarlab.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string prefix = optStr(parsed.opts, "prefix").value_or(std::string());
  const std::string suffix = optStr(parsed.opts, "suffix").value_or(std::string());
  const bool upper = hasFlag(parsed.opts, "upper");
  const bool lower = hasFlag(parsed.opts, "lower");
  if (upper && lower)
    return utilSyntax("renvarlab: `upper` and `lower` cannot both be given.");
  if (prefix.empty() && suffix.empty() && !upper && !lower)
    return utilSyntax(
        "renvarlab: nothing to do -- give at least one of prefix(), suffix(), upper, lower.");

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("renvarlab: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> vars;
  if (parsed.varlist.empty()) {
    vars = dsp->names();
  } else {
    // expandVarlist is the ONE place the wildcard/hyphen varlist grammar lives.
    loops::VarlistExpandResult ex = loops::expandVarlist(joinWith(parsed.varlist, " "), session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }
  if (vars.empty()) return utilError("renvarlab: no variables to rename");

  const std::set<std::string> renaming(vars.begin(), vars.end());
  std::set<std::string> taken;
  for (const auto& nm : dsp->names()) {
    if (renaming.count(nm) == 0) taken.insert(nm);
  }

  std::vector<std::pair<std::string, std::string>> plan;
  std::set<std::string> targets;
  for (const auto& nm : vars) {
    std::string out = prefix + nm + suffix;
    if (upper) out = upperAscii(out);
    if (lower) out = lowerOf(out);
    if (out == nm) continue;  // nothing to do for this one
    if (!strutil::isValidStataIdentifier(out))
      return utilSyntax("renvarlab: `" + nm + "' would become `" + out +
                        "', which is not a legal variable name -- " +
                        strutil::stataIdentifierRuleText());
    if (taken.count(out) != 0)
      return utilError("renvarlab: `" + nm + "' would become `" + out +
                           "', which already exists and is not itself being renamed.",
                       "bad_input");
    if (!targets.insert(out).second)
      return utilError("renvarlab: two variables would both become `" + out + "'.", "bad_input");
    plan.emplace_back(nm, out);
  }
  if (plan.empty()) return silent();

  std::vector<std::string> fromNames, toNames;
  for (const auto& [from, to] : plan) {
    ojson r = runRename(from, to, session);
    if (const ojson* e = firstError(r)) return *e;
    fromNames.push_back(from);
    toNames.push_back(to);
  }
  // BUG-452 (2026-09-07, AER scope 2 K1b): this used to return silent() and
  // SWALLOW every runRename's mutationResult, so a command that rewrites the
  // variable plane published nothing at all -- the consumer of the block stream
  // never learned the names changed. It went unseen because renvarlab's freeze
  // row is `no_fixture`: the capture gates can only watch what a command emits
  // ([[gates-watch-production-not-absence]]). Found when the identical swallow
  // in the new `tolower` made ITS freeze golden pin the preamble's varlist.
  // ONE block for the whole plan, with the complete new/dropped pair, rather
  // than one per rename.
  return mutationResult(session, "rename", toNames, fromNames);
}

// ===========================================================================
// strdist  (AER scope 2, bundle B2)
// ===========================================================================
//
//     strdist {var|"literal"} {var|"literal"} [if] [in] [, gen(name) maxdist(#)]
//
// Michael D Barker and Felix Poege's strdist.ado 1.2, whose own header names
// them. Levenshtein edit distance with unit insert/delete/substitute cost.
//
// BYTE-WISE, not codepoint-wise, because the package is: its Mata kernel calls
// `ascii()` on both strings and compares those numbers. That is not a detail to
// smooth over -- it is why `strdist "a" "A"` is 1 and not 0, measured, and why
// the package's own help file points Stata 14+ users at `ustrdist` instead.
//
// FIVE SHORTCUTS ARE THE PACKAGE'S, AND EACH CHANGES AN ANSWER:
//   a == b                            -> 0     (before any length test)
//   maxdist set, |len_a-len_b| > it   -> .     (missing, not the true distance)
//   either string empty               -> len_a + len_b
//   running row minimum > maxdist     -> .     (early stop)
//   final cell > maxdist              -> .
// `maxdist(#)` below 1 means NO limit, and the ado says so out loud when the
// argument is neither -1 nor >= 1; -1 is its own "unset" spelling.
//
// THE DEFAULT VARIABLE NAME IS THE LITERAL WORD `strdist`. `strdist a b` with no
// gen() creates a variable called `strdist` -- measured (`ds` afterwards reports
// `a b strdist`), not assumed. The two-literal form is the exception: with no
// gen() it DISPLAYS the number and returns r(d), creating nothing.
long long levenshteinBytes(const std::string& a, const std::string& b, long long maxdist,
                           bool* exceeded) {
  *exceeded = false;
  if (a == b) return 0;
  const long long la = static_cast<long long>(a.size());
  const long long lb = static_cast<long long>(b.size());
  const bool limited = maxdist >= 1;
  if (limited && std::llabs(la - lb) > maxdist) {
    *exceeded = true;
    return 0;
  }
  if (la == 0 || lb == 0) {
    // The package returns len_a+len_b here WITHOUT re-testing maxdist, and the
    // |la-lb| guard above has already refused the case that would exceed it.
    return la + lb;
  }
  std::vector<long long> v0(static_cast<size_t>(lb) + 1), v1(static_cast<size_t>(lb) + 1);
  for (long long j = 0; j <= lb; ++j) v0[static_cast<size_t>(j)] = j;
  for (long long i = 1; i <= la; ++i) {
    v1[0] = i;
    for (long long j = 1; j <= lb; ++j) {
      const long long cost = a[static_cast<size_t>(i - 1)] != b[static_cast<size_t>(j - 1)] ? 1 : 0;
      v1[static_cast<size_t>(j)] =
          std::min({v1[static_cast<size_t>(j - 1)] + 1, v0[static_cast<size_t>(j)] + 1,
                    v0[static_cast<size_t>(j - 1)] + cost});
    }
    v0.swap(v1);
    if (limited) {
      long long rowMin = v0[0];
      for (long long j = 1; j <= lb; ++j) rowMin = std::min(rowMin, v0[static_cast<size_t>(j)]);
      if (rowMin > maxdist) {
        *exceeded = true;
        return 0;
      }
    }
  }
  const long long d = v0[static_cast<size_t>(lb)];
  if (limited && d > maxdist) {
    *exceeded = true;
    return 0;
  }
  return d;
}

// A strdist argument is a LITERAL when the user quoted it, and a variable name
// otherwise. The tokenizer hands the quotes through, so they are the signal --
// the same test the ado makes with `gettoken ..., qed()`.
bool isQuotedLiteral(const std::string& tok, std::string* text) {
  if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') {
    *text = tok.substr(1, tok.size() - 2);
    return true;
  }
  return false;
}

// `confirm new variable` with STATA'S rc. newVarGuard above answers the same
// question but tags it `bad_input`, which coarseRc maps to rc 1; the package
// answers r(110), measured (`strdist p q, gen(p)` with p present).
std::optional<ojson> strdistNewVar(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name))
    return utilSyntax("strdist: invalid variable name `" + name + "' -- " +
                      strutil::stataIdentifierRuleText());
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(name))
    return utilErrorRc("variable " + name + " already defined", 110);
  return std::nullopt;
}

ojson runStrdist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabStrdist.known,
                                            kVocabStrdist.unimplemented))
    return ojson(*err);
  // ONE argument is legal and is not the same as two. The ado routes it to
  // `strdist12var VAR, m("")`, i.e. every value's distance from the EMPTY
  // string, which fastlev shortcuts to the string's own length. Measured:
  // `strdist p` over "kitten" is rc 0 and gives 6. Three arguments are r(103);
  // zero cannot reach here (the tokenizer needs a varlist).
  if (parsed.varlist.empty())
    return utilSyntax("strdist: syntax is `strdist {var|\"literal\"} [{var|\"literal\"}] "
                      "[, generate(name) maxdist(#)]'");
  if (parsed.varlist.size() > 2) return utilErrorRc("too many variables specified", 103);

  long long maxdist = -1;
  if (auto mv = optStr(parsed.opts, "maxdist")) {
    if (!parseStrictInt(*mv, &maxdist))
      return utilSyntax("strdist: maxdist() requires a whole number, got `" + *mv + "'");
  }
  // The ado's own normalisation, message included: anything below 1 that is not
  // its -1 "unset" spelling is announced and then treated as no limit.
  ojson blocks = ojson::array();
  if (maxdist < 1 && maxdist != -1) {
    blocks.push_back(displayBlock("Invalid argument maxdist (" + std::to_string(maxdist) +
                                 "). The Levenstein distance without limitation will be computed."));
    maxdist = 0;
  }
  if (maxdist < 1) maxdist = 0;  // 0 and -1 both mean "no limit" downstream

  std::string lit1, lit2;
  const bool isLit1 = isQuotedLiteral(parsed.varlist[0], &lit1);
  // A single argument behaves as `<arg> ""`: the ado leaves `second` empty and
  // the match string defaults to "".
  const bool oneArg = parsed.varlist.size() == 1;
  const bool isLit2 = !oneArg && isQuotedLiteral(parsed.varlist[1], &lit2);
  if (oneArg) lit2.clear();
  const std::string genName = optStr(parsed.opts, "generate").value_or(std::string());

  // ---- both literals: one number, displayed, r(d). --------------------
  if (isLit1 && (isLit2 || oneArg)) {
    bool exceeded = false;
    const long long d = levenshteinBytes(lit1, lit2, maxdist, &exceeded);
    if (genName.empty()) {
      blocks.push_back(displayBlock(exceeded ? std::string(".") : std::to_string(d)));
      session.eval.rValues.set("d", exceeded ? std::numeric_limits<double>::quiet_NaN()
                                             : static_cast<double>(d));
      return blocks;
    }
    if (!session.hasData()) return ojson(eval::noDataLoaded());
    if (auto err = strdistNewVar(session, genName)) return *err;
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    const std::string cell = exceeded ? std::string() : std::to_string(d);
    std::vector<std::string> cells(static_cast<size_t>(ds->nRows()), cell);
    std::string outPath;
    if (auto err = writeColumn(session, ds->path(), genName, cells, "f64", true, &outPath))
      return *err;
    session.load(outPath);
    session.eval.rValues.set("d", exceeded ? std::numeric_limits<double>::quiet_NaN()
                                           : static_cast<double>(d));
    session.eval.rStrings.set("strdist", genName);
    blocks.push_back(mutationResult(session, "gen", {genName}, {}));
    return blocks;
  }

  // ---- at least one variable: a column of distances --------------------
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const std::string target = genName.empty() ? std::string("strdist") : genName;
  if (auto err = strdistNewVar(session, target)) return *err;

  auto columnOf = [&](const std::string& nm) -> std::optional<std::vector<std::string>> {
    if (!ds->has(nm)) return std::nullopt;
    return columnAsStrings(*ds, nm);
  };
  std::vector<std::string> left, right;
  if (oneArg) {
    auto col = columnOf(parsed.varlist[0]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    left = *col;
    right.assign(left.size(), std::string());
  } else if (isLit1) {
    auto col = columnOf(parsed.varlist[1]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[1] + " not found", 111);
    right = *col;
    left.assign(right.size(), lit1);
  } else if (isLit2) {
    auto col = columnOf(parsed.varlist[0]);
    if (!col) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    left = *col;
    right.assign(left.size(), lit2);
  } else {
    auto c1 = columnOf(parsed.varlist[0]);
    if (!c1) return utilErrorRc("variable " + parsed.varlist[0] + " not found", 111);
    auto c2 = columnOf(parsed.varlist[1]);
    if (!c2) return utilErrorRc("variable " + parsed.varlist[1] + " not found", 111);
    left = *c1;
    right = *c2;
  }

  // `if`/`in` are NOT accepted, and the registry row says so rather than this
  // handler silently ignoring them. The package marks rows out with a `touse`
  // and writes only into it, so an excluded row keeps a missing -- a real
  // behaviour that needs a real mask, and `applyIfAndInFilter` hands back a
  // FILTERED FRAME rather than a mask, which would drop the excluded rows
  // instead of blanking them. Accepting the qualifier and answering the wrong
  // shape is worse than refusing it; the twelve-paper corpus uses neither.
  std::vector<std::string> cells(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    bool exceeded = false;
    const long long d = levenshteinBytes(left[i], right[i], maxdist, &exceeded);
    if (!exceeded) cells[i] = std::to_string(d);
  }
  std::string outPath;
  if (auto err = writeColumn(session, ds->path(), target, cells, "f64", true, &outPath))
    return *err;
  session.load(outPath);
  session.eval.rStrings.set("strdist", target);
  blocks.push_back(mutationResult(session, "gen", {target}, {}));
  return blocks;
}

// ===========================================================================
// splitvallabels
// ===========================================================================
//
// Chops a value label's texts at whitespace boundaries into pieces of at most
// length() characters and publishes them as r(relabel) -- the string a graph
// command's `relabel()` option takes verbatim. Read-only: it never touches the
// label plane it reads.
ojson runSplitvallabels(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitvallabels.known,
                                            kVocabSplitvallabels.unimplemented))
    return ojson(*err);
  if (parsed.varlist.size() != 1)
    return utilSyntax("splitvallabels: syntax is `splitvallabels varname [, length(#)]`");
  const std::string var = parsed.varlist[0];

  long long length = 12;  // splitvallabels.ado's own default
  if (auto lv = optStr(parsed.opts, "length")) {
    if (!parseStrictInt(*lv, &length) || length < 1)
      return utilSyntax("splitvallabels: length() requires a whole number of at least 1, got `" +
                        *lv + "'");
  }

  // The variable's value label, from the session's label plane. valueLabels is
  // the resolved per-variable map; when a variable was attached to a label set
  // that has since been (re)defined, varLblname + labelDefs is the same answer
  // from its two halves.
  std::map<long long, std::string> codes;
  auto vlIt = session.valueLabels.find(var);
  if (vlIt != session.valueLabels.end()) {
    for (const auto& [code, text] : vlIt->second) codes.emplace(static_cast<long long>(code), text);
  } else {
    auto lnIt = session.varLblname.find(var);
    if (lnIt != session.varLblname.end()) {
      auto defIt = session.labelDefs.find(lnIt->second);
      if (defIt != session.labelDefs.end()) {
        for (const auto& [codeStr, text] : defIt->second) {
          long long c = 0;
          if (parseStrictInt(codeStr, &c)) codes.emplace(c, text);
        }
      }
    }
  }
  if (codes.empty())
    return utilError("splitvallabels: `" + var +
                     "' has no value label attached, so there is nothing to split.");

  auto chop = [&](const std::string& text) {
    // Greedy packing at WHITESPACE boundaries: a word longer than length() is
    // its own piece rather than being cut mid-word (the option is a target, not
    // a hard truncation -- `nobreak`, which would suppress the chopping
    // entirely, is the refused option).
    std::vector<std::string> pieces;
    std::string cur;
    for (const auto& w : splitWs(text)) {
      if (cur.empty()) {
        cur = w;
      } else if (static_cast<long long>(cur.size() + 1 + w.size()) <= length) {
        cur += " " + w;
      } else {
        pieces.push_back(cur);
        cur = w;
      }
    }
    if (!cur.empty()) pieces.push_back(cur);
    if (pieces.empty()) pieces.emplace_back();
    return pieces;
  };

  std::string relabel;
  for (const auto& [code, text] : codes) {
    if (text.find('"') != std::string::npos)
      return utilError("splitvallabels: the label text for code " + std::to_string(code) +
                       " contains a double quote, which cannot travel inside r(relabel).");
    if (!relabel.empty()) relabel += " ";
    relabel += std::to_string(code);
    for (const auto& p : chop(text)) relabel += " \"" + p + "\"";
  }
  session.eval.rStrings.set("relabel", relabel);
  return silent();
}

// ===========================================================================
// marksample
// ===========================================================================
//
// PROGRAM-INTERNAL ONLY, and this is the one refusal in the bundle that exists
// to prevent a SILENT wrong answer rather than to admit a gap. `marksample`
// reads the local macro `varlist`, which is set by `syntax` -- and `syntax`
// only runs inside a program. At top level that local does not exist, the
// condition collapses to "1", and every observation gets marked: a `touse` that
// selects everything, which every later command then honours. Refusing when
// progCallDepth is 0 is the only honest answer.
ojson runMarksample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarksample.known,
                                            kVocabMarksample.unimplemented))
    return ojson(*err);
  if (parsed.varlist.size() != 1)
    return utilSyntax("marksample: syntax is `marksample name [, novarlist]`");
  if (session.progCallDepth == 0)
    return utilSyntax(
        "marksample: not allowed outside a program -- it reads the local macro `varlist`, which "
        "only `syntax` sets, and outside a program that local does not exist. Every observation "
        "would be marked.");
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string name = parsed.varlist[0];
  ojson tv = runTempvar({name}, session);
  if (const ojson* e = firstError(tv)) return *e;
  const std::string* minted = session.eval.locals.find(name);
  if (minted == nullptr || minted->empty())
    return utilError("marksample: could not mint a temporary variable name", "internal");

  std::string cond = "1";
  if (!hasFlag(parsed.opts, "novarlist")) {
    const std::string* vl = session.eval.locals.find("varlist");
    const std::vector<std::string> vars = vl ? splitWs(*vl) : std::vector<std::string>();
    if (!vars.empty()) {
      cond.clear();
      for (const auto& v : vars) {
        if (!cond.empty()) cond += " & ";
        cond += "!missing(" + v + ")";
      }
    }
  }
  // DOCUMENTED DEVIATION: real marksample also folds the `if`/`in` the
  // enclosing program was called with into the marker. That binding comes from
  // `syntax`, which this build does not implement, so there is nothing to fold;
  // the marker reflects the varlist only.
  ojson blocks = dispatch("quietly generate byte " + *minted + " = (" + cond + ")", session);
  if (const ojson* e = firstError(blocks)) return *e;
  return silent();
}

// ===========================================================================
// filelist
// ===========================================================================
//
// The filesystem, as a dataset: one row per file, with dirname/filename/fsize.
// REPLACES what is in memory, which is why `filelist` is in the dispatcher's
// kNoDataCommands list and runs with nothing loaded.
//
// THE ECHOED PATH IS THE ONE THE USER TYPED (defect class 5). `resolveUserPath`
// is used to WALK the tree and nowhere else; every dirname cell and the count
// line are built from the typed string, so a do-file that later matches on
// `dirname == "data"` still matches.
// `globMatch` moved to piet/strutil.h so esttab/estout's coefficient selection
// can use the SAME matcher (BUG-496). Body unchanged; `using` keeps the call
// sites below spelled as they were.
using piet::strutil::globMatch;

ojson runFilelist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFilelist.known,
                                            kVocabFilelist.unimplemented))
    return ojson(*err);
  if (!parsed.varlist.empty())
    return utilSyntax("filelist: syntax is `filelist [, directory(dir) pattern(glob) "
                      "norecursive]` -- it takes no varlist.");

  const std::string typedDir = optStr(parsed.opts, "directory").value_or(std::string("."));
  const std::string pattern = optStr(parsed.opts, "pattern").value_or(std::string("*"));
  const bool recursive = !hasFlag(parsed.opts, "norecursive");
  const std::string resolved = resolveUserPath(session, typedDir);

  std::error_code ec;
  if (!std::filesystem::is_directory(cecon::to_native_path(resolved), ec) || ec)
    return utilErrorRc("filelist: `" + typedDir + "' is not a directory.", 601);

  struct FileRow {
    std::string dirname;
    std::string filename;
    double fsize = 0.0;
  };
  std::vector<FileRow> found;

  // The dirname a row reports = the directory the USER named, plus the
  // subdirectory the file was found under, so a `directory("data")` scan
  // reports `data` and `data/raw` -- never the absolute path resolveUserPath
  // produced.
  auto record = [&](const std::filesystem::path& full) {
    std::error_code fec;
    if (!std::filesystem::is_regular_file(full, fec) || fec) return;
    const std::string name = full.filename().string();
    if (!globMatch(pattern, name)) return;
    std::error_code rec;
    std::filesystem::path rel =
        std::filesystem::relative(full.parent_path(), cecon::to_native_path(resolved), rec);
    std::string dirname = typedDir;
    if (!rec && !rel.empty() && rel.string() != ".") dirname = typedDir + "/" + rel.generic_string();
    std::error_code sec;
    const auto sz = std::filesystem::file_size(full, sec);
    found.push_back(FileRow{dirname, name,
                            sec ? std::numeric_limits<double>::quiet_NaN()
                                : static_cast<double>(sz)});
  };

  try {
    if (recursive) {
      for (std::filesystem::recursive_directory_iterator
               it(cecon::to_native_path(resolved), std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           it != end; it.increment(ec)) {
        if (ec) break;
        record(it->path());
      }
    } else {
      for (std::filesystem::directory_iterator
               it(cecon::to_native_path(resolved), std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           it != end; it.increment(ec)) {
        if (ec) break;
        record(it->path());
      }
    }
  } catch (const std::exception& e) {
    return utilError(std::string("filelist: could not read `") + typedDir + "' -- " + e.what(),
                     "data_io");
  }

  std::sort(found.begin(), found.end(), [](const FileRow& a, const FileRow& b) {
    if (a.dirname != b.dirname) return a.dirname < b.dirname;
    return a.filename < b.filename;
  });

  if (found.empty()) {
    // No rows means no dataset: `cobra convert` on a header-only CSV would
    // produce a zero-observation frame that every later command then treats as
    // real data. r(2000) ("no observations") says what happened instead.
    session.eval.rValues.set("N", 0.0);
    return utilErrorRc("filelist: no files matching `" + pattern + "' found in `" + typedDir + "'.",
                       2000);
  }

  CsvTable t;
  t.names = {"dirname", "filename", "fsize"};
  t.isFactor = {1, 1, 0};
  t.rows.reserve(found.size());
  for (const auto& f : found)
    t.rows.push_back({f.dirname, f.filename, numCell(f.fsize)});

  std::string hbin;
  if (auto err = writeAndConvert(session, t, "filelist", &hbin)) return *err;

  // Adopted the way `use` adopts: the resident chain is DROPPED (a load from a
  // freshly-built file never continues it) and the display metadata plane is
  // cleared, because this dataset declares no labels of its own.
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.invalidateDatastore();

  session.eval.rValues.set("N", static_cast<double>(found.size()));
  ojson out = ojson::array();
  out.push_back(displayBlock("Number of files found = " + intText(
                                 static_cast<int64_t>(found.size())) + "\n"));
  out.push_back(mutationResult(session, "use"));
  return out;
}

// ===========================================================================
// carryforward
// ===========================================================================
//
// Last-non-missing forward fill, reset at every by-group boundary.
//
// ROW ORDER IS THE ANSWER, not an implementation detail. The dispatcher's
// by-prefix branch already sorted the frame; this walks the frame in its
// CURRENT physical order, carrying each group's last non-missing value forward,
// and writes every result back at the row index it came from. A group whose
// rows are not contiguous (an explicit `by()` on unsorted data) still fills
// from the previous row OF THE SAME GROUP, which is the same answer sorting
// would have given and a defensible one when it was not.
ojson runCarryforward(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCarryforward.known,
                                            kVocabCarryforward.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(
        "carryforward: syntax is `[bysort g:] carryforward varlist, gen(newvarlist)|replace`");

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"gen", "generate"});
  if (replace == gen.has_value())
    return utilSyntax("carryforward: give exactly one of gen(newvarlist) and replace.");

  std::vector<std::string> newVars;
  if (gen.has_value()) {
    newVars = splitWs(*gen);
    if (newVars.size() != parsed.varlist.size())
      return utilSyntax("carryforward: gen() names " + intText(static_cast<int64_t>(newVars.size())) +
                        " variable(s) for " + intText(static_cast<int64_t>(parsed.varlist.size())) +
                        " input variable(s); the two lists must be the same length.");
  }

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);

  // A written column must be as long as the frame and aligned to it; a filtered
  // read cannot be mapped back. Refused by name (registry allowIf is false for
  // this row, so this is the belt to the tokenizer's suspenders).
  {
    ojson q = refuseQualifiers(
        parsed, "`carryforward` writes a column as long as the dataset, and this build cannot map a "
        "filtered sample back onto the original rows. Restrict first (`preserve` + `keep if "
        "...`), then run carryforward.");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = parsed.varlist;
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, "carryforward", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("carryforward: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  for (const auto& nm : newVars) {
    ojson guard = newVarGuard(session, "carryforward", nm);
    if (!guard.is_null()) return guard;
  }

  const int64_t n = ds.nRows();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

  std::string cur = *session.currentHbin();
  std::vector<std::string> rewritten;
  for (size_t k = 0; k < parsed.varlist.size(); ++k) {
    const std::string& src = parsed.varlist[k];
    const std::string dtype = (ds.dtype(src) == "factor") ? "factor" : "f64";
    std::vector<std::string> cells = columnAsStrings(ds, src);
    // "" is missing in a setcol payload, which is exactly the state that gets
    // filled -- and a group's FIRST observation, having nothing before it,
    // keeps its missing rather than borrowing from another group.
    std::vector<std::string> last(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
    std::vector<char> haveLast(static_cast<size_t>(std::max<int64_t>(nGroups, 1)), 0);
    for (int64_t i = 0; i < n; ++i) {
      const size_t g = static_cast<size_t>(gid[static_cast<size_t>(i)]);
      std::string& cell = cells[static_cast<size_t>(i)];
      if (!cell.empty()) {
        last[g] = cell;
        haveLast[g] = 1;
      } else if (haveLast[g]) {
        cell = last[g];
      }
    }
    const std::string target = gen.has_value() ? newVars[k] : src;
    std::string out;
    if (auto err = writeColumn(session, cur, target, cells, dtype, gen.has_value(), &out))
      return *err;
    cur = out;
    if (!gen.has_value()) rewritten.push_back(target);
  }

  session.load(cur);
  if (gen.has_value()) return mutationResult(session, "gen", newVars);
  return mutationResult(session, "gen", {}, {}, rewritten);
}

// ===========================================================================
// rangestat
// ===========================================================================
//
// `rangestat (stat) var [(stat) var ...], interval(key lo hi) [by(varlist)]`.
//
// THE WINDOW IS CLOSED AND CONTAINS THE ROW ITSELF: for a row whose key is k,
// every observation of the same by-group whose key lies in [k+lo, k+hi] enters
// the statistic, including that row. `.` for a bound means unbounded on that
// side. `sd` divides by n-1. `count` counts NON-MISSING values. A window with
// no observations at all gives missing on every statistic -- never 0, not even
// for count; a window that HAS observations but none with a non-missing value
// gives count 0 and missing for the rest, which is the same distinction Stata's
// own count/mean pair makes.
//
// RESULTS GO BACK IN ORIGINAL ROW ORDER. The computation sorts by key within
// group because a two-pointer window needs sorted keys; every value is then
// written at the row index it came from. Sorting to compute and forgetting to
// restore is a silent wrong answer that looks perfectly plausible, so the
// sorted array below never holds anything but an index INTO the original rows.
namespace {

enum class RangeStat { Mean, Sum, Count, Min, Max, Sd };

struct RangeSpec {
  RangeStat stat = RangeStat::Mean;
  std::string statName;
  std::string src;
  std::string target;
};

bool parseRangeStatName(const std::string& s, RangeStat* out) {
  if (s == "mean") *out = RangeStat::Mean;
  else if (s == "sum") *out = RangeStat::Sum;
  else if (s == "count") *out = RangeStat::Count;
  else if (s == "min") *out = RangeStat::Min;
  else if (s == "max") *out = RangeStat::Max;
  else if (s == "sd") *out = RangeStat::Sd;
  else return false;
  return true;
}

}  // namespace

ojson runRangestat(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRangestat.known,
                                            kVocabRangestat.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // The registry row allows if/in, and rangestat WRITES columns -- see
  // carryforward for why a filtered read cannot be mapped back onto the rows.
  {
    ojson q = refuseQualifiers(
        parsed, "`rangestat` writes columns as long as the dataset, and a restricted sample would also "
        "change which observations enter each window. Restrict first (`preserve` + `keep if "
        "...`), then run rangestat.");
    if (!q.is_null()) return q;
  }

  // The stat specs are read off the varlist: the tokenizer splits the pre-comma
  // text on whitespace, so `(mean) y (sum) z` arrives as four tokens and
  // `(mean)y` as one. Rejoining and scanning is the same grammar either way.
  const std::string head = joinWith(parsed.varlist, " ");
  std::vector<RangeSpec> specs;
  {
    size_t p = 0;
    while (p < head.size()) {
      while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      if (p >= head.size()) break;
      if (head[p] != '(')
        return utilSyntax("rangestat: expected `(statname)' at `" + head.substr(p) +
                          "' -- syntax is `rangestat (stat) var [(stat) var ...], interval(key lo "
                          "hi) [by(varlist)]`");
      const size_t close = head.find(')', p);
      if (close == std::string::npos)
        return utilSyntax("rangestat: unclosed `(' in `" + head + "'");
      const std::string statName = lowerOf(trimOf(head.substr(p + 1, close - p - 1)));
      p = close + 1;
      while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      size_t q = p;
      while (q < head.size() && !std::isspace(static_cast<unsigned char>(head[q])) && head[q] != '(')
        ++q;
      if (q == p)
        return utilSyntax("rangestat: `(" + statName + ")' names no variable.");
      const std::string term = head.substr(p, q - p);
      p = q;

      RangeSpec spec;
      if (!parseRangeStatName(statName, &spec.stat)) {
        return ojson(eval::unsupportedOption(
            "(" + statName + ")",
            "`rangestat` in this build computes mean, sum, count, min, max and sd. `" + statName +
                "` is a real rangestat statistic this build does not implement; answering it with "
                "one of the six would be a different number under the requested name."));
      }
      spec.statName = statName;
      const size_t eq = term.find('=');
      if (eq == std::string::npos) {
        spec.src = term;
        spec.target = term + "_" + statName;
      } else {
        spec.target = trimOf(term.substr(0, eq));
        spec.src = trimOf(term.substr(eq + 1));
      }
      if (spec.src.empty() || spec.target.empty())
        return utilSyntax("rangestat: could not read `" + term + "' as [newname=]varname");
      specs.push_back(spec);
    }
  }
  if (specs.empty())
    return utilSyntax(
        "rangestat: syntax is `rangestat (stat) var [(stat) var ...], interval(key lo hi) "
        "[by(varlist)]`");

  const std::optional<std::string> intervalOpt = optStr(parsed.opts, "interval");
  if (!intervalOpt.has_value())
    return utilSyntax("rangestat: interval(key lo hi) is required -- without it there is no "
                      "window.");
  const std::vector<std::string> iv = splitWs(*intervalOpt);
  if (iv.size() != 3)
    return utilSyntax("rangestat: interval() takes exactly three terms -- a key variable, a low "
                      "offset and a high offset (`.` for unbounded), got `" + *intervalOpt + "'");
  const std::string keyVar = iv[0];
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
  if (iv[1] != ".") {
    if (!parseStrictDouble(iv[1], &lo))
      return utilSyntax("rangestat: interval()'s low offset `" + iv[1] +
                        "' is not a number (use `.` for unbounded).");
  }
  if (iv[2] != ".") {
    if (!parseStrictDouble(iv[2], &hi))
      return utilSyntax("rangestat: interval()'s high offset `" + iv[2] +
                        "' is not a number (use `.` for unbounded).");
  }
  if (lo > hi)
    return utilSyntax("rangestat: interval()'s low offset (" + iv[1] +
                      ") is above its high offset (" + iv[2] + "); the window would be empty.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);

  std::vector<std::string> readCols = {keyVar};
  for (const auto& s : specs) readCols.push_back(s.src);
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, "rangestat", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("rangestat: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  if (ds.dtype(keyVar) == "factor")
    return utilError("rangestat: the interval() key `" + keyVar +
                     "' is a string variable; the window is an arithmetic range and needs a "
                     "numeric key.");
  for (const auto& s : specs) {
    if (ds.dtype(s.src) == "factor")
      return utilError("rangestat: `" + s.src +
                       "' is a string variable; " + s.statName + " needs a numeric variable.");
  }
  std::set<std::string> targetsSeen;
  for (const auto& s : specs) {
    ojson guard = newVarGuard(session, "rangestat", s.target);
    if (!guard.is_null()) return guard;
    if (!targetsSeen.insert(s.target).second)
      return utilError("rangestat: two results would both be called `" + s.target +
                           "'; name one of them with `newname=var`.",
                       "bad_input");
  }

  const int64_t n = ds.nRows();
  const double* key = ds.col(keyVar).f64();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

  // Row indices per group, sorted by key ascending. Rows with a MISSING key
  // have no window at all and never enter one, so they are left out here and
  // their results stay missing.
  std::vector<std::vector<int64_t>> order(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(key[i])) continue;
    order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
  }
  for (auto& g : order) {
    std::stable_sort(g.begin(), g.end(),
                     [&](int64_t a, int64_t b) { return key[a] < key[b]; });
  }

  const bool wantExtrema =
      std::any_of(specs.begin(), specs.end(), [](const RangeSpec& s) {
        return s.stat == RangeStat::Min || s.stat == RangeStat::Max;
      });

  std::vector<std::vector<double>> results(
      specs.size(),
      std::vector<double>(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN()));

  for (const auto& g : order) {
    if (g.empty()) continue;
    const size_t m = g.size();
    // Per statistic source: prefix count/sum/sumsq over the sorted order, so a
    // window's totals are one subtraction rather than a walk. Only NON-MISSING
    // values contribute -- which is exactly what makes `count` the count of
    // non-missing.
    std::vector<std::vector<double>> pSum(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<double>> pSumSq(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<int64_t>> pCnt(specs.size(), std::vector<int64_t>(m + 1, 0));
    std::vector<const double*> srcPtr;
    srcPtr.reserve(specs.size());
    for (const auto& s : specs) srcPtr.push_back(ds.col(s.src).f64());
    for (size_t s = 0; s < specs.size(); ++s) {
      for (size_t j = 0; j < m; ++j) {
        const double v = srcPtr[s][g[j]];
        const bool ok = !std::isnan(v);
        pSum[s][j + 1] = pSum[s][j] + (ok ? v : 0.0);
        pSumSq[s][j + 1] = pSumSq[s][j] + (ok ? v * v : 0.0);
        pCnt[s][j + 1] = pCnt[s][j] + (ok ? 1 : 0);
      }
    }
    // The multiset window for min/max, driven by the SAME two pointers. Both
    // are monotone as the centre row's key increases, so each element is
    // inserted once and erased once.
    std::vector<std::multiset<double>> win(wantExtrema ? specs.size() : 0);
    size_t winL = 0, winR = 0;

    size_t L = 0, R = 0;  // [L, R) is the window, as indices into g
    for (size_t c = 0; c < m; ++c) {
      const double k = key[g[c]];
      const double lowBound = k + lo;   // -inf when lo is `.`
      const double highBound = k + hi;  // +inf when hi is `.`
      while (L < m && key[g[L]] < lowBound) ++L;
      while (R < m && key[g[R]] <= highBound) ++R;
      if (R < L) R = L;  // an empty window still has to be well formed
      if (wantExtrema) {
        while (winR < R) {
          for (size_t s = 0; s < specs.size(); ++s) {
            const double v = srcPtr[s][g[winR]];
            if (!std::isnan(v)) win[s].insert(v);
          }
          ++winR;
        }
        while (winL < L) {
          for (size_t s = 0; s < specs.size(); ++s) {
            const double v = srcPtr[s][g[winL]];
            if (!std::isnan(v)) {
              auto it = win[s].find(v);
              if (it != win[s].end()) win[s].erase(it);
            }
          }
          ++winL;
        }
      }
      const int64_t obsInWindow = static_cast<int64_t>(R) - static_cast<int64_t>(L);
      if (obsInWindow <= 0) continue;  // no observations at all -> every result stays missing
      for (size_t s = 0; s < specs.size(); ++s) {
        const int64_t cnt = pCnt[s][R] - pCnt[s][L];
        const double sum = pSum[s][R] - pSum[s][L];
        double& out = results[s][static_cast<size_t>(g[c])];
        switch (specs[s].stat) {
          case RangeStat::Count:
            out = static_cast<double>(cnt);
            break;
          case RangeStat::Sum:
            if (cnt > 0) out = sum;
            break;
          case RangeStat::Mean:
            if (cnt > 0) out = sum / static_cast<double>(cnt);
            break;
          case RangeStat::Sd: {
            if (cnt >= 2) {
              const double ss = (pSumSq[s][R] - pSumSq[s][L]) - sum * sum / static_cast<double>(cnt);
              out = std::sqrt(std::max(ss, 0.0) / static_cast<double>(cnt - 1));
            }
            break;
          }
          case RangeStat::Min:
            if (!win[s].empty()) out = *win[s].begin();
            break;
          case RangeStat::Max:
            if (!win[s].empty()) out = *win[s].rbegin();
            break;
        }
      }
    }
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> newVars;
  for (size_t s = 0; s < specs.size(); ++s) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) cells.push_back(numCell(results[s][static_cast<size_t>(i)]));
    std::string out;
    if (auto err = writeColumn(session, cur, specs[s].target, cells, "f64", true, &out)) return *err;
    cur = out;
    newVars.push_back(specs[s].target);
  }
  session.load(cur);
  return mutationResult(session, "gen", newVars);
}

// ===========================================================================
// tsspell
// ===========================================================================
//
// Spells over a tsset series: `_spell` numbers them within panel, `_seq`
// numbers the observations within a spell, `_end` flags each spell's last
// observation. Two forms:
//
//   tsspell varname       a spell is a run of EQUAL values of varname
//   tsspell, cond(expr)   a spell is a run of observations where expr is true
//
// In either form a TIME GAP ends the spell: the previous observation must be
// exactly one `delta` earlier (tsset's own step, defaulting to 1), or the run
// restarts. The cond() form leaves non-spell observations at 0/0/0, which is
// tsspell.ado's own convention and is why `_spell` is 1-based.
//
// LIKE rangestat, THE ANSWERS GO BACK IN ORIGINAL ROW ORDER: the panel/time
// ordering below is an index permutation and never a rewrite of the frame.
ojson runTsspell(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTsspell.known,
                                            kVocabTsspell.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!session.timevar.has_value() || session.timevar->empty())
    return utilError(
        "tsspell: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows which "
        "variable orders the spells.",
        "unsupported_option");

  const std::optional<std::string> cond = optStr(parsed.opts, "cond");
  if (parsed.varlist.size() > 1)
    return utilSyntax("tsspell: syntax is `tsspell [varname] [, cond(expression) spell(name) "
                      "seq(name) end(name)]`");
  if (parsed.varlist.empty() && !cond.has_value())
    return utilSyntax("tsspell: give either a varname or cond(expression) -- without one there is "
                      "nothing to define a spell.");
  if (!parsed.varlist.empty() && cond.has_value())
    return utilSyntax("tsspell: give either a varname or cond(expression), not both.");

  {
    ojson q = refuseQualifiers(
        parsed, "`tsspell` writes columns as long as the dataset and walks the series in time order; a "
        "restricted sample would silently redefine where a spell breaks. Restrict first "
        "(`preserve` + `keep if ...`), then run tsspell.");
    if (!q.is_null()) return q;
  }

  const std::string spellVar = optStr(parsed.opts, "spell").value_or(std::string("_spell"));
  const std::string seqVar = optStr(parsed.opts, "seq").value_or(std::string("_seq"));
  const std::string endVar = optStr(parsed.opts, "end").value_or(std::string("_end"));
  for (const auto& nm : {spellVar, seqVar, endVar}) {
    ojson guard = newVarGuard(session, "tsspell", nm);
    if (!guard.is_null()) return guard;
  }
  if (spellVar == seqVar || spellVar == endVar || seqVar == endVar)
    return utilSyntax("tsspell: spell(), seq() and end() must name three different variables.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  if (byVars.empty() && session.panelvar.has_value() && !session.panelvar->empty())
    byVars.push_back(*session.panelvar);

  // The cond() form needs a row-wise truth value of an arbitrary expression.
  // PIET has exactly one expression evaluator and it lives behind `generate`;
  // this materializes the indicator through it and drops it afterwards, so
  // there is no second evaluator to drift.
  std::string indicator;
  if (cond.has_value()) {
    if (auto err = materializeIndicator(session, "tsspell", *cond, &indicator)) return *err;
  }
  // The indicator column is scaffolding, never data. It has to be gone BEFORE
  // `mutationResult` reads the frame, or the varlist this command reports would
  // name a temporary the user never asked for and cannot see -- so the drop is
  // a callable that the success path fires at exactly that point and the
  // failure paths fire on the way out. Idempotent: whichever runs first wins.
  bool indicatorLive = !indicator.empty();
  ojson indicatorDropError;
  auto dropIndicator = [&]() {
    if (!indicatorLive) return;
    indicatorLive = false;
    ojson dropped = runDrop({indicator}, session);
    if (const ojson* e = firstError(dropped)) indicatorDropError = *e;
  };

  ojson result;
  {
    std::vector<std::string> readCols = {*session.timevar};
    if (!parsed.varlist.empty()) readCols.push_back(parsed.varlist[0]);
    if (!indicator.empty()) readCols.push_back(indicator);
    for (const auto& b : byVars) readCols.push_back(b);

    ReadScope scope(session, parsed, readCols, "tsspell", false);
    if (scope.failed()) {
      result = scope.error().is_null() ? utilError("tsspell: could not read the data", "data_io")
                                       : scope.error();
    } else {
      Datastore& ds = *scope.get();
      std::string missingName;
      for (const auto& nm : readCols) {
        if (!ds.has(nm)) missingName = nm;
      }
      if (!missingName.empty()) {
        result = utilError("variable " + missingName + " not found");
      } else if (ds.dtype(*session.timevar) == "factor") {
        result = utilError("tsspell: the time variable `" + *session.timevar +
                           "' is a string variable; tsspell needs a numeric time index.");
      } else {
        const int64_t n = ds.nRows();
        const double* tv = ds.col(*session.timevar).f64();
        const double delta = session.delta.value_or(1.0);
        int64_t nGroups = 0;
        const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

        const bool byValue = !parsed.varlist.empty();
        ColView valueView;
        const double* ind = nullptr;
        if (byValue)
          valueView = viewOf(ds, parsed.varlist[0]);
        else
          ind = ds.col(indicator).f64();

        std::vector<std::vector<int64_t>> order(
            static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
        for (int64_t i = 0; i < n; ++i)
          order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
        for (auto& g : order) {
          std::stable_sort(g.begin(), g.end(),
                           [&](int64_t a, int64_t b) { return tv[a] < tv[b]; });
        }

        std::vector<double> spell(static_cast<size_t>(n), 0.0);
        std::vector<double> seq(static_cast<size_t>(n), 0.0);
        std::vector<double> endf(static_cast<size_t>(n), 0.0);

        for (const auto& g : order) {
          int64_t spellNo = 0;
          int64_t seqNo = 0;
          int64_t prevRow = -1;
          bool inSpell = false;
          for (size_t j = 0; j < g.size(); ++j) {
            const int64_t r = g[j];
            const bool active = byValue ? true : (!std::isnan(ind[r]) && ind[r] != 0.0);
            if (!active) {
              inSpell = false;
              prevRow = r;
              continue;
            }
            bool newSpell = !inSpell;
            if (!newSpell && prevRow >= 0) {
              // A gap in TIME breaks the run whatever the values do.
              const double dt = tv[r] - tv[prevRow];
              if (std::isnan(dt) || dt != delta) newSpell = true;
            }
            if (!newSpell && byValue && prevRow >= 0) {
              if (cellKey(valueView, r) != cellKey(valueView, prevRow)) newSpell = true;
            }
            if (newSpell) {
              ++spellNo;
              seqNo = 1;
            } else {
              ++seqNo;
            }
            spell[static_cast<size_t>(r)] = static_cast<double>(spellNo);
            seq[static_cast<size_t>(r)] = static_cast<double>(seqNo);
            inSpell = true;
            prevRow = r;
          }
          // `_end` marks the LAST observation of each spell, which is known
          // only once the following observation (or the group's end) is seen.
          for (size_t j = 0; j < g.size(); ++j) {
            const int64_t r = g[j];
            if (spell[static_cast<size_t>(r)] == 0.0) continue;
            bool last = true;
            if (j + 1 < g.size()) {
              const int64_t nx = g[j + 1];
              last = spell[static_cast<size_t>(nx)] != spell[static_cast<size_t>(r)];
            }
            endf[static_cast<size_t>(r)] = last ? 1.0 : 0.0;
          }
        }

        auto cellsOf = [&](const std::vector<double>& v) {
          std::vector<std::string> out;
          out.reserve(static_cast<size_t>(n));
          for (int64_t i = 0; i < n; ++i) out.push_back(numCell(v[static_cast<size_t>(i)]));
          return out;
        };

        std::string cur = *session.currentHbin();
        bool failed = false;
        for (const auto& [nm, vals] :
             std::vector<std::pair<std::string, const std::vector<double>*>>{
                 {spellVar, &spell}, {seqVar, &seq}, {endVar, &endf}}) {
          std::string out;
          if (auto err = writeColumn(session, cur, nm, cellsOf(*vals), "f64", true, &out)) {
            result = *err;
            failed = true;
            break;
          }
          cur = out;
        }
        if (!failed) {
          session.load(cur);
          dropIndicator();
          result = mutationResult(session, "gen", {spellVar, seqVar, endVar});
        }
      }
    }
  }

  // Every path that did not reach the success branch above still owes the drop.
  // A drop failure is reported only when nothing worse already has.
  dropIndicator();
  if (!indicatorDropError.is_null() && !isErrorBlock(result)) return indicatorDropError;
  return result;
}

// ===========================================================================
// bcal -- Stata business calendars ([D] bcal)
// ===========================================================================
//
// AER scope-2 B2-3, the blocker of paper 066. The rules live in
// piet/bcal.{h,cpp}, whose header carries the authority note (the create
// branch's primary text is /Applications/StataNow/ado/base/b/bcal.ado, not the
// manual). This handler is the WIRING: option parse, the read of the from()
// column, the file write, the cache refresh, the describe table and r().
//
// NAMED DIVERGENCE (D2/D3/D4): Stata searches BASE;SITE;.;PERSONAL;PLUS for a
// .stbcal and `bcal dir` walks the same path. PIET models no sysdir, so the
// search directory is the session's cwd alone: `bofd("simple", ...)` -- the one
// calendar StataBE ships, in BASE -- is missing here unless the file is in the
// working directory, `bcal dir` lists the cwd, and `personal` is refused by
// name rather than writing somewhere PIET cannot then find.

namespace {

const eval::OptVocab& kVocabBcal = eval::registerOptVocab(
    {"bcal"}, {{"from", "generate", "personal", "excludemissing", "replace", "purpose",
                "dateformat", "range", "centerdate", "maxgap", "rc0"},
               {},
               // bcal.ado's own `syntax` line: Generate() and CENTERdate().
               // The 2-character prefix floor cannot produce `g(...)`.
               {{"g", "generate"}, {"gen", "generate"}}});

// %9.0fc -- the describe table's own width-9 comma-grouped integer field.
std::string fmt9c(long long v) {
  std::string digits = std::to_string(v < 0 ? -v : v);
  std::string grouped;
  int c = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (c != 0 && c % 3 == 0) grouped += ',';
    grouped += *it;
    ++c;
  }
  std::reverse(grouped.begin(), grouped.end());
  if (v < 0) grouped = "-" + grouped;
  if (grouped.size() < 9) grouped = std::string(9 - grouped.size(), ' ') + grouped;
  return grouped;
}

std::string fmtW(const std::string& body, size_t w) {
  if (body.size() >= w) return body;
  return std::string(w - body.size(), ' ') + body;
}

std::string fmtF(double v, int width, int dec) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%*.*f", width, dec, v);
  return std::string(buf);
}

// `di ... _col(37) "text"`: pad the line out to column 37 (1-based) first.
std::string atCol37(const std::string& line, const std::string& tail) {
  std::string out = line;
  while (out.size() < 36) out += ' ';
  return out + tail;
}

// Stata's %td rendering, for the describe table's date column.
std::string td9(std::int64_t d) {
  auto r = piet::fmt::renderTd(static_cast<double>(d));
  return r ? *r : std::to_string(d);
}

// The one place `bcal describe`'s numbers are derived, shared by describe and
// by create's own trailing describe (bcal.ado:290-310 and :666-690 compute
// them identically).
struct DescribeNumbers {
  long long ddays = 0, bdays = 0, omitted = 0, included = 0;
  double omitYear = 0.0, inclYear = 0.0;
};

DescribeNumbers describeNumbers(const bcal::BusinessCalendar& c) {
  DescribeNumbers d;
  d.ddays = static_cast<long long>(c.hi - c.lo + 1);
  d.bdays = static_cast<long long>(c.maxTb - c.minTb + 1);
  d.omitted = d.ddays - d.bdays;
  d.included = d.bdays;
  d.omitYear = (static_cast<double>(d.omitted) / static_cast<double>(d.ddays)) * 365.25;
  d.inclYear = (static_cast<double>(d.included) / static_cast<double>(d.ddays)) * 365.25;
  return d;
}

// `%11.1f` through a local, which is how bcal.ado rounds r(omitted_year).
double round1(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return std::atof(buf);
}

std::string describeTable(const bcal::BusinessCalendar& c) {
  const DescribeNumbers d = describeNumbers(c);
  const std::string bfmt = "%tb" + c.name;
  std::ostringstream o;
  o << "\n";
  o << "  Business calendar " << c.name << " (format " << bfmt << "):\n";
  o << "\n";
  o << "    Purpose: " << c.purpose << "\n";
  o << "\n";
  o << "      Range: " << td9(c.lo) << "  " << td9(c.hi) << "\n";
  o << atCol37("             " + fmtW(std::to_string(c.lo), 9) + "  " +
                   fmtW(std::to_string(c.hi), 9),
               "in %td units") << "\n";
  o << atCol37("             " + fmtW(std::to_string(c.minTb), 9) + "  " +
                   fmtW(std::to_string(c.maxTb), 9),
               "in " + bfmt + " units") << "\n";
  o << "\n";
  o << "     Center: " << td9(c.center) << "\n";
  o << atCol37("             " + fmtW(std::to_string(c.center), 9), "in %td units") << "\n";
  o << atCol37("             " + fmtW("0", 9), "in " + bfmt + " units") << "\n";
  o << "\n";
  o << atCol37("    Omitted: " + fmt9c(d.omitted), "days") << "\n";
  o << atCol37("             " + fmtF(d.omitYear, 11, 1), "approx. days/year") << "\n";
  o << "\n";
  o << atCol37("   Included: " + fmt9c(d.included), "days") << "\n";
  o << atCol37("             " + fmtF(d.inclYear, 11, 1), "approx. days/year") << "\n";
  return o.str();
}

void setDescribeR(SessionState& session, const bcal::BusinessCalendar& c, bool withCounts) {
  // r(fn) is a PRESENTATION string, not the path we opened. Stata posts the
  // adopath-relative name it found -- `./a_wk.stbcal` for a calendar in the
  // cwd (measured 2026-09-09: D_desc_r_fn / D_dir_fn = `./a_wk.stbcal`, while
  // `bcal create`'s own r(fn) is the bare `a_wk.stbcal`, D_create_r_fn).
  // `c.fn` is the ABSOLUTE path this build opened, and posting it leaked a
  // machine path into a user-visible return: a do-file that echoes r(fn)
  // printed `/private/tmp/.../a_wk.stbcal` (review 2026-09-09, non-blocking 3).
  // The cwd is the only directory PIET searches, so `./NAME.stbcal` is the
  // right answer for every calendar this build can resolve.
  session.eval.rStrings.set("fn", "./" + c.name + ".stbcal");
  session.eval.rStrings.set("purpose", c.purpose);
  session.eval.rStrings.set("name", c.name);
  session.eval.rValues.set("max_date_tb", static_cast<double>(c.maxTb));
  session.eval.rValues.set("min_date_tb", static_cast<double>(c.minTb));
  session.eval.rValues.set("ctr_date_td", static_cast<double>(c.center));
  session.eval.rValues.set("max_date_td", static_cast<double>(c.hi));
  session.eval.rValues.set("min_date_td", static_cast<double>(c.lo));
  if (!withCounts) return;
  const DescribeNumbers d = describeNumbers(c);
  session.eval.rValues.set("omitted", static_cast<double>(d.omitted));
  session.eval.rValues.set("included", static_cast<double>(d.included));
  session.eval.rValues.set("omitted_year", round1(d.omitYear));
  session.eval.rValues.set("included_year", round1(d.inclYear));
}

// The session's cwd, which is the ONLY directory PIET looks in. The
// derivation is bcal::searchDirFor and NOT a second copy of it: dispatcher.cpp
// sets the same search directory at depth 0, and if the two disagreed about
// what an unset session.cwd means, one of them would RESOLVE `NAME.stbcal`
// against the absolute cwd while the other resolved it against "" -- a real
// file found by one caller and missed by the other.
// (Until the 2026-09-09 review this string was also part of the CACHE key,
// which is what made the disagreement a stale-answer bug as well; the key is
// the name alone now, so this is a resolution question only.)
std::string bcalCwd(SessionState& session) {
  return bcal::searchDirFor(session.cwd.value_or(std::string()));
}

// Read `NAME.stbcal` from the cwd (through the cache), for describe/load.
std::shared_ptr<const bcal::BusinessCalendar> bcalFind(SessionState& session,
                                                       const std::string& nameIn, bool forceReload,
                                                       bcal::StbcalError* err) {
  bcal::setSearchDir(bcalCwd(session));
  std::string name = bcal::calNameFromFormat(nameIn);
  if (name.empty()) name = nameIn;
  // The calendar-NAME rule (bcal.h). It has to be HERE and not only in
  // bcal::lookup, because this function falls through to loadFile() -- which
  // would happily read `elevenchars.stbcal` off the disk and answer where
  // Stata refuses (r(198), measured).
  if (!bcal::isValidCalName(name)) {
    if (err != nullptr) {
      err->rc = 198;
      // The name AS TYPED, which is what Stata echoes: `bcal describe
      // %tbelevenchars` says "%tbelevenchars is an invalid calendar name or
      // %fmt", not the stripped "elevenchars" (measured, s1.log R11).
      err->message = nameIn + " is an invalid calendar name or %fmt";
    }
    return nullptr;
  }
  if (!forceReload) {
    if (auto hit = bcal::lookup(name)) return hit;
  }
  const std::string path = resolveUserPath(session, name + ".stbcal");
  return bcal::loadFile(name, path, err);
}

}  // namespace

ojson runBcal(const eval::ParsedCommand& parsed, SessionState& session) {
  // NOTE: no reject net runs here. kVocabBcal is the UNION of five
  // sub-commands' option lists (it has to be -- it is what the dispatcher's
  // canonicalizeForCommand abbreviates against), and rejecting against the
  // union BEFORE the sub-command is known is how every sub-command came to
  // swallow every other sub-command's options as a silent no-op. The net now
  // runs per sub-command, below, against the ado's own `syntax` line.
  if (parsed.varlist.empty())
    return utilSyntax("bcal: syntax is `bcal create|describe|load|dir|check ...'");
  const std::string sub = lowerOf(parsed.varlist[0]);
  // bcal.ado's own dispatcher: check/describe accept any prefix, dir/load/
  // create are spelled in full.
  auto isPrefixOf = [&](const char* full) {
    const std::string f = full;
    return !sub.empty() && sub.size() <= f.size() && f.compare(0, sub.size(), sub) == 0;
  };
  const bool isCheck = isPrefixOf("check");
  const bool isDescribe = !isCheck && isPrefixOf("describe");
  const bool isDir = sub == "dir";
  const bool isLoad = sub == "load";
  const bool isCreate = sub == "create";
  if (!isCheck && !isDescribe && !isDir && !isLoad && !isCreate)
    return utilErrorRc(sub + " invalid bcal subcommand", 198);
  if (!isCreate && (parsed.ifExpr.has_value() || parsed.inRange.has_value()))
    return utilErrorRc("bcal " + sub + ": if/in qualifiers are not allowed", 101);

  // --- the option net, NARROWED to the resolved sub-command ----------------
  // Each set below is that sub-command's own `syntax` line in bcal.ado, and
  // each refusal code is measured (StataBE 19.5, this machine):
  //   bcal_describe :91 and bcal_load :69 are a BARE `syntax` -- ANY option is
  //     "options not allowed", r(101), and it beats the calendar-name error
  //     (`bcal describe elevenchars, maxgap(5)` is 101, not 198);
  //   bcal_dir :50    `syntax [, TESTMODE]`      -- `bcal dir, rc0`      r(198);
  //   bcal_check :38  `syntax [varlist] [, RC0]` -- `bcal check bd, maxgap(4)`
  //     r(198), while `bcal check bd, rc0` and even `, rc` are r(0);
  //   bcal_create :237-248 has no rc0 -- `bcal create c1, from(bd) replace
  //     rc0` r(198).
  // Before this narrowing all seven of those lines answered rc 0 here.
  if (isDescribe || isLoad) {
    if (!parsed.opts.empty()) return utilErrorRc("options not allowed", 101);
  } else {
    static const std::unordered_set<std::string> kNoneKnown;
    // `testmode` is bcal_dir's undocumented test hook (it re-points the paths
    // bcal_dir prints). Stata accepts it (r(0), measured); PIET has no such
    // mode, so it is refused BY NAME as known-but-absent rather than accepted
    // as a silent no-op.
    static const std::unordered_set<std::string> kDirUnimpl = {"testmode"};
    static const std::unordered_set<std::string> kCheckKnown = {"rc0"};
    static const std::unordered_set<std::string> kCreateKnown = {
        "from",    "generate",   "personal", "excludemissing", "replace",
        "purpose", "dateformat", "range",    "centerdate",     "maxgap"};
    const std::unordered_set<std::string>& known =
        isCheck ? kCheckKnown : (isCreate ? kCreateKnown : kNoneKnown);
    const std::unordered_set<std::string>& unimpl =
        isDir ? kDirUnimpl : kVocabBcal.unimplemented;
    if (auto e = eval::rejectUnknownOptions(parsed.opts, known, unimpl)) return ojson(*e);
    if (isCreate) {
      std::set<std::string> seen;
      for (const auto& [key, value] : parsed.opts) {
        if (!seen.insert(key).second)
          return utilErrorRc("bcal create: duplicate option " + key, 198);
        const bool flag = key == "replace" || key == "personal";
        if (value.isBool != flag)
          return utilErrorRc("bcal create: invalid syntax for option " + key, 198);
        if (!flag && key != "purpose" && trimOf(value.strValue).empty())
          return utilErrorRc("bcal create: option " + key + " requires an argument", 198);
      }
    }
  }

  bcal::setSearchDir(bcalCwd(session));

  // --- describe / load -----------------------------------------------------
  if (isDescribe || isLoad) {
    if (parsed.varlist.size() < 2)
      return utilErrorRc("bcal " + std::string(isLoad ? "load" : "describe") +
                             ": nothing was found where a calendar name or %tbfmt was expected",
                         198);
    const std::string asked = parsed.varlist[1];
    bcal::StbcalError err;
    auto cal = bcalFind(session, asked, /*forceReload=*/isLoad, &err);
    if (!cal) {
      // The calendar-NAME rule (bcal.h), in Stata's own words. It is a
      // DIFFERENT refusal from "no such file": `bcal load nosuchcal` is
      // r(601), `bcal load elevenchars` is r(198), both measured.
      if (err.rc == 198) return utilErrorRc(err.message, 198);
      if (err.rc == 601)
        return utilErrorRc("bcal " + std::string(isLoad ? "load" : "describe") + ": calendar " +
                               asked + " not found (" + bcal::calNameFromFormat(asked) +
                               ".stbcal is not in the working directory)",
                           601);
      return utilErrorRc("bcal: " + asked + ".stbcal is not a valid calendar file -- " +
                             err.message,
                         err.rc);
    }
    if (isLoad) {
      setDescribeR(session, *cal, /*withCounts=*/false);
      return displayBlock("\n(calendar loaded successfully)\n");
    }
    setDescribeR(session, *cal, /*withCounts=*/true);
    return displayBlock(describeTable(*cal));
  }

  // --- dir -----------------------------------------------------------------
  if (isDir) {
    std::string pattern = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string("*");
    if (!pattern.empty() && pattern[0] == '%')
      return utilErrorRc("bcal dir: % is not allowed -- name the calendar (nyse), not its format "
                         "(%tbnyse)",
                         198);
    auto globMatch = [](const std::string& pat, const std::string& s) {
      // The two wildcards bcal dir's own pattern grammar uses.
      std::function<bool(size_t, size_t)> m = [&](size_t pi, size_t si) -> bool {
        if (pi == pat.size()) return si == s.size();
        if (pat[pi] == '*') {
          for (size_t k = si; k <= s.size(); ++k)
            if (m(pi + 1, k)) return true;
          return false;
        }
        if (si == s.size()) return false;
        if (pat[pi] == '?' || pat[pi] == s[si]) return m(pi + 1, si + 1);
        return false;
      };
      return m(0, 0);
    };
    std::vector<std::pair<std::string, std::string>> found;  // name, path
    std::error_code ec;
    const std::string dir = bcalCwd(session);
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file()) continue;
      const std::string fn = it->path().filename().u8string();
      if (fn.size() <= 7 || fn.compare(fn.size() - 7, 7, ".stbcal") != 0) continue;
      const std::string nm = fn.substr(0, fn.size() - 7);
      if (!globMatch(pattern, nm)) continue;
      // Presentation form, exactly as in setDescribeR above: Stata's `bcal dir`
      // prints and posts `./NAME.stbcal` for a calendar in the cwd (measured,
      // cap2.log D_dir_fn) -- an absolute path is what it prints only for a
      // calendar found elsewhere on the adopath, which PIET has none of.
      found.emplace_back(nm, "./" + fn);
    }
    std::sort(found.begin(), found.end());
    std::ostringstream o;
    if (found.empty()) {
      o << "no calendar files matching " << pattern << " found\n";
    } else {
      o << "  " << found.size() << " calendar file" << (found.size() == 1 ? "" : "s")
        << " found:\n";
      for (const auto& [nm, path] : found) o << fmtW(nm, 20) << ":  " << path << "\n";
    }
    std::string names;
    for (const auto& [nm, path] : found) {
      session.eval.rStrings.set("fn_" + nm, path);
      if (!names.empty()) names += " ";
      names += nm;
    }
    session.eval.rStrings.set("calendars", names);
    return displayBlock(o.str());
  }

  // --- check ---------------------------------------------------------------
  if (isCheck) {
    const bool rc0 = hasFlag(parsed.opts, "rc0");
    std::vector<std::string> asked(parsed.varlist.begin() + 1, parsed.varlist.end());
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    std::vector<std::string> vars;
    if (!asked.empty()) {
      vars = asked;
    } else if (ds != nullptr) {
      vars = ds->names();
    }
    std::map<std::string, std::vector<std::string>> byCal;  // calname -> vars
    for (const auto& v : vars) {
      auto it = session.varMeta.find(v);
      if (it == session.varMeta.end() || !it->second.format.has_value()) continue;
      const std::string nm = bcal::calNameFromFormat(*it->second.format);
      if (nm.empty()) continue;
      byCal[nm].push_back(v);
    }
    if (byCal.empty()) return displayBlock("(no variables use %tb formats)\n");
    std::ostringstream o;
    std::string defined, undefined, errored;
    for (const auto& [nm, vlist] : byCal) {
      bcal::StbcalError err;
      auto cal = bcalFind(session, nm, /*forceReload=*/false, &err);
      o << "\n" << fmtW("%tb" + nm, 21) << ":  ";
      if (cal) {
        o << "defined, ";
        defined += (defined.empty() ? "" : " ") + nm;
      } else if (err.rc == 601 || err.rc == 198) {
        // 198 is the calendar-NAME rule. Stata cannot reach this line at all:
        // it refuses `format x %tbelevenchars` up front with r(120) "invalid
        // %format" (measured), so there is no Stata bucket to copy. "undefined"
        // is the honest one of the two Stata has -- there is no such calendar,
        // and "defined, has errors" would claim a file was read that was never
        // opened.
        o << "undefined, ";
        undefined += (undefined.empty() ? "" : " ") + nm;
      } else {
        o << "defined, has errors, ";
        errored += (errored.empty() ? "" : " ") + nm;
      }
      o << "used by " << (vlist.size() == 1 ? "variable" : "variables") << "\n";
      o << joinWith(vlist, " ") << "\n";
      session.eval.rStrings.set("varlist_" + nm, joinWith(vlist, " "));
    }
    session.eval.rStrings.set("defined", defined);
    session.eval.rStrings.set("undefined", undefined);
    session.eval.rStrings.set("error", errored);
    if (undefined.empty() && errored.empty()) return displayBlock(o.str());
    o << "\nundefined or defined-with-errors %tb formats\n";
    if (rc0) return displayBlock(o.str());
    return utilErrorRc(o.str(), 459);
  }

  // --- create --------------------------------------------------------------
  if (parsed.varlist.size() < 2)
    return utilErrorRc("bcal create: name of business calendar file expected", 198);
  if (parsed.varlist.size() > 2)
    return utilErrorRc("bcal create: only one calendar filename is allowed", 198);
  const std::string spec = parsed.varlist[1];

  // parse_pathname (bcal.ado:1183-1231): split path / calname / suffix.
  std::string path, calname = spec;
  const size_t slash = spec.find_last_of("/\\");
  if (slash != std::string::npos) {
    path = spec.substr(0, slash + 1);
    calname = spec.substr(slash + 1);
  }
  std::string suffix;
  const size_t dot = calname.find_last_of('.');
  if (dot != std::string::npos) {
    suffix = calname.substr(dot);
    calname = calname.substr(0, dot);
  }
  if (!path.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(resolveUserPath(session, path), ec))
      return utilErrorRc("path " + path + " does not exist", 601);
  }
  if (!strutil::isValidStataIdentifier(calname))
    return utilErrorRc(calname + " is not a valid calendar name", 198);
  if (calname.size() > 10)
    return utilErrorRc(calname + " too long; calendar names are limited to 10 characters", 198);
  if (!suffix.empty() && suffix != ".stbcal" && suffix != ".")
    return utilErrorRc("extension " + suffix + " in calendar file not allowed", 198);

  if (hasFlag(parsed.opts, "personal"))
    return ojson(eval::unsupportedOption(
        "personal",
        "`bcal create, personal' writes into Stata's PERSONAL ado directory. PIET models no "
        "sysdir, so a calendar written there could never be found again; write it into the "
        "working directory instead (drop the option)."));

  const std::optional<std::string> fromOpt = optStr(parsed.opts, "from");
  if (!fromOpt.has_value() || trimOf(*fromOpt).empty())
    return utilErrorRc("bcal create: option from() required", 198);
  const std::string fromVar = trimOf(*fromOpt);

  if (!session.hasData()) return utilErrorRc("no dataset in use", 3);

  bcal::InduceOpts iopts;
  iopts.calName = calname;
  if (auto df = optStr(parsed.opts, "dateformat")) {
    const std::string d = lowerOf(trimOf(*df));
    static const std::set<std::string> kOrders = {"ymd", "ydm", "myd", "mdy", "dym", "dmy"};
    if (kOrders.count(d) == 0)
      return utilErrorRc(d + " found where dateformat expected (ymd|ydm|myd|mdy|dym|dmy)", 198);
    iopts.dateformat = d;
  }
  if (auto pu = optStr(parsed.opts, "purpose")) {
    std::string p = *pu;
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
    if (p.size() > 63) return utilErrorRc("purpose() is limited to 63 characters", 198);
    iopts.purpose = p;
  }
  if (auto mg = optStr(parsed.opts, "maxgap")) {
    const std::string m = trimOf(*mg);
    char* endp = nullptr;
    const long v = std::strtol(m.c_str(), &endp, 10);
    if (endp == m.c_str() || *endp != '\0')
      return utilErrorRc("maxgap(): " + m + " found where integer expected", 198);
    if (v < 1) return utilErrorRc("maxgap() should be at least 1", 198);
    iopts.maxgap = v;
  }
  // Options and calendar files share one date grammar. Parse just the token;
  // building a calendar through year 9999 here would allocate millions of days.
  auto readDate = [&](const std::string& tok, std::int64_t* out) -> bool {
    return bcal::parseDailyDate(tok, iopts.dateformat, out);
  };
  if (auto rg = optStr(parsed.opts, "range")) {
    const std::vector<std::string> parts = splitWs(trimOf(*rg));
    if (parts.size() != 2) return utilErrorRc("two date arguments expected in range()", 198);
    if (parts[0] != ".") {
      if (!readDate(parts[0], &iopts.rangeLo))
        return utilErrorRc(parts[0] + " not in " + iopts.dateformat + " format", 198);
      iopts.haveRangeLo = true;
    }
    if (parts[1] != ".") {
      if (!readDate(parts[1], &iopts.rangeHi))
        return utilErrorRc(parts[1] + " not in " + iopts.dateformat + " format", 198);
      iopts.haveRangeHi = true;
    }
    if (iopts.haveRangeLo && iopts.haveRangeHi && iopts.rangeHi <= iopts.rangeLo)
      return utilErrorRc("second date should be after first date in range()", 198);
  }
  if (auto cd = optStr(parsed.opts, "centerdate")) {
    if (!readDate(trimOf(*cd), &iopts.center))
      return utilErrorRc(trimOf(*cd) + " not in " + iopts.dateformat + " format", 198);
    iopts.haveCenter = true;
  }

  // excludemissing(varlist [, any]): the variables are joined with `|` by
  // default and with `&` when `any` is given (bcal.ado:354-381 -- the option's
  // OWN sub-option, read out of the parenthesised text).
  std::vector<std::string> exclVars;
  bool exclAny = false;
  if (auto em = optStr(parsed.opts, "excludemissing")) {
    std::string body = trimOf(*em);
    const size_t comma = body.find(',');
    if (comma != std::string::npos) {
      const std::string tail = lowerOf(trimOf(body.substr(comma + 1)));
      if (!tail.empty() && std::string("any").compare(0, tail.size(), tail) == 0) exclAny = true;
      else if (!tail.empty())
        return utilErrorRc("excludemissing(): option " + tail + " not allowed", 198);
      body = trimOf(body.substr(0, comma));
    }
    exclVars = splitWs(body);
  }

  const std::optional<std::string> genVar = optStrAny(parsed.opts, {"generate", "gen"});
  const bool haveQualifier = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                             (parsed.inRange.has_value() && !parsed.inRange->empty());
  if (genVar.has_value() && haveQualifier) {
    // generate() writes a column as long as the dataset with MISSING on the
    // excluded rows; `applyIfAndInFilter` hands back a filtered FRAME, not a
    // mask, so this build cannot map those rows back. Refused by name rather
    // than writing a short or misaligned column.
    ojson q = refuseQualifiers(
        parsed, "`bcal create, generate()' writes a column as long as the dataset, and this "
                "build cannot map a filtered sample back onto the original rows. Run the create "
                "without generate() and then `gen long V = bofd(\"" + calname + "\", " + fromVar +
                    ")' yourself.");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = {fromVar};
  for (const auto& v : exclVars) readCols.push_back(v);
  ReadScope scope(session, parsed, readCols, "bcal create", /*honourQualifiers=*/true);
  if (scope.failed())
    return scope.error().is_null() ? utilError("bcal create: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  if (ds.col(fromVar).dtype == cecon::DType::Factor)
    return utilErrorRc(fromVar + " has non-numeric values", 198);

  const int64_t n = ds.nRows();
  const double* fromCol = ds.col(fromVar).f64();
  std::vector<ColView> exclViews;
  for (const auto& v : exclVars) exclViews.push_back(viewOf(ds, v));
  std::vector<double> values;
  std::vector<char> included(static_cast<size_t>(n), 0);
  values.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (!exclViews.empty()) {
      bool keep = exclAny;
      for (const auto& v : exclViews) {
        const bool present = !cellMissing(v, i);
        keep = exclAny ? (keep && present) : (keep || present);
      }
      if (!keep) continue;
    }
    included[static_cast<size_t>(i)] = 1;
    values.push_back(fromCol[i]);
  }
  if (values.empty()) return utilErrorRc("no observations found for calendar", 2000);

  {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    static const char* kMonth[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "* Created/replaced on %2d %s %d", tmv.tm_mday,
                  kMonth[tmv.tm_mon], tmv.tm_year + 1900);
    iopts.todayLine = buf;
  }

  bcal::InduceResult ind = induceFromDates(values, iopts);
  if (!ind.ok) return utilErrorRc(ind.message, ind.rc);

  const std::string filename = path + calname + ".stbcal";
  const std::string abs = resolveUserPath(session, filename);
  const bool existed = std::filesystem::exists(abs);
  if (existed && !hasFlag(parsed.opts, "replace"))
    return utilErrorRc("file " + filename + " already exists; use replace to overwrite", 602);

  // A self-parse BEFORE the write: bcal create runs `bcal describe` on the
  // file it just wrote and, when that fails, restores the previous file and
  // exits 197. Checking the bytes first reaches the same answer without ever
  // damaging the old file.
  bcal::ParseResult back = bcal::parseStbcal(ind.text, calname);
  if (!back.ok)
    return utilErrorRc("bcal describe " + calname + " failed -- " + back.err.message +
                           (existed ? "; " + filename + " left unchanged" : ""),
                       197);

  {
    std::ofstream f(cecon::to_native_path(abs), std::ios::binary);
    if (!f) return utilError("bcal create: cannot write " + abs, "data_io");
    f << ind.text;
    f.flush();
    if (!f) return utilError("bcal create: failed writing " + abs + " (disk full or I/O error)",
                             "data_io");
  }
  back.cal.fn = abs;
  auto cal = std::make_shared<const bcal::BusinessCalendar>(back.cal);
  // `bcal create ..., replace` REFRESHES the cache (measured: the paper's
  // second create on a narrower date set turns the outer rows missing at the
  // very next bofd, 38/80).
  bcal::install(calname, cal);

  std::ostringstream out;
  out << describeTable(*cal);
  out << "\n  Notes:\n\n";
  out << "    Business calendar file " << filename << " saved.\n";
  if (ind.leadNote)
    out << "\n    First date in range() occurs before first business date in dataset.\n";
  if (ind.trailNote)
    out << "\n    Second date in range() occurs after last business date in dataset.\n";
  if (ind.fractionalNote)
    out << "\n    " << fromVar << " contains noninteger dates; daily dates use their floor.\n";

  ojson blocks = ojson::array();
  if (genVar.has_value()) {
    const std::string newvar = trimOf(*genVar);
    ojson guard = newVarGuard(session, "bcal create", newvar);
    if (!guard.is_null()) {
      // The file is already written and cached, which is what Stata does too
      // (it generates AFTER the successful describe); the 110 is still a hard
      // stop and is reported as such.
      return utilErrorRc("bcal create: variable " + newvar + " already defined", 110);
    }
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      if (!included[static_cast<size_t>(i)]) {
        cells.emplace_back();
        continue;
      }
      cells.push_back(numCell(bcal::bofd(*cal, fromCol[i])));
    }
    std::string outPath;
    if (auto err = writeColumn(session, *session.currentHbin(), newvar, cells, "f64", true,
                               &outPath))
      return *err;
    session.load(outPath);
    session.varMeta[newvar].format = "%tb" + calname;
    session.varMeta[newvar].stataType = "long";
    session.implicitFloatVars.erase(newvar);
    session.userFormattedVars.insert(newvar);
    out << "\n    Variable " << newvar << " created; it contains business dates in %tb" << calname
        << " format.\n";
    blocks.push_back(displayBlock(out.str()));
    ojson mr = handlers::mutationResult(session, "gen", {newvar});
    if (mr.is_array())
      for (const auto& b : mr) blocks.push_back(b);
    else if (!mr.is_null())
      blocks.push_back(mr);
  } else {
    blocks.push_back(displayBlock(out.str()));
  }

  setDescribeR(session, *cal, /*withCounts=*/true);
  session.eval.rStrings.set("fn", filename);
  return blocks;
}

}  // namespace piet::handlers
