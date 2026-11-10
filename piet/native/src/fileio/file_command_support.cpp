// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) -- the older text I/O verbs, plus
// `putexcel`, plus the three file commands that had no home:
//
//   insheet     a delimited file -> the dataset, with Stata's own naming rules
//   outsheet    the dataset -> a delimited file, TAB by default
//   infile      free-format tokens -> the dataset (records wrap on varlist size)
//   infix       fixed columns -> the dataset
//   outfile     the dataset -> a headerless, space-or-comma separated file
//   saveold     `save`, with an honest note about the format actually written
//   putexcel    a cell-addressed .xlsx writer, stateful across statements
//   filefilter  a whole-file search/replace, publishing r(occurrences)
//   copy        one file to another (a URL source is refused by name)
//
// WHY THESE ARE SHIMS AND NOT NEW READERS. `insheet` IS `import delimited`
// once the delimiter and the header question are settled, and `outsheet` IS
// `export delimited` with a different default separator; `saveold` IS `save`.
// A second delimited reader in this build would be a second thing to keep in
// step with the engine's own CSV path, and the first divergence between them
// would be invisible -- two commands answering the same question with
// different numbers. So the loaders here decide the OPTIONS and hand the bytes
// to the machinery that already exists (`cobra convert`, runExportDelimited,
// runSaveData). `infile`, `infix` and `outfile` are the exceptions: their
// formats are not delimited files at all, so they parse/emit here and stage a
// CSV for the SAME converter, which is what keeps the type inference identical
// to every other load in the product.
//
// NO NEW ENGINE VERB, and no `src/` edit: everything below composes
// `convert`/`rename` and the PIET-side writers.
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at
// least once in this campaign; the first has been caught three times in five
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborts the whole selftest binary. dispatcher.cpp's bundle-G arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal -- but ONLY an array whose every element is a real
//     block object carrying a "type" key. Every `return` below is one object
//     with "type", or an ojson::array() of such objects (possibly empty, which
//     is how `putexcel clear` and `copy` say "nothing to print").
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`; a refusal built
//     with the default `engine_error` code silently becomes rc 1. So:
//     command-level refusals carry `unsupported_command` + a `refused:<cause>`
//     reason (eval::refusedCommand, or the same shape by hand when the plan
//     pins the sentence), USER SYNTAX errors carry "syntax_error", and a known
//     Stata rc that coarseRc's message rules cannot recognise states itself
//     through the `reason` channel (`ioErrorRc` below). `copy` onto an
//     existing file without `replace` is r(602) and says so.
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "fullname"` comparison see them -- is driven by the
//     REGISTRY. A command absent from the vocabulary registry gets its
//     abbreviations accepted and then dropped unread. All nine names register
//     at NAMESPACE scope below (a function-local static constructs too late:
//     after the first call that needed it), and every handler's first act is
//     the reject net. An option this build does NOT read goes in the
//     `unimplemented` list, never in `known` -- bundle E shipped
//     `esttab, cells()` as known-and-unread and it had to be reverted.
//     `putexcel`'s FORMATTING options are the one deliberate exception the
//     plan specifies: accepted, ignored, and announced once per `set`.
//  4. ECHOED PATHS. Nothing here echoes a RESOLVED absolute path. Every
//     message and every echo names the spelling the user typed, because a
//     resolved path bakes the build machine's workdir into any golden that
//     captures it -- which has shipped once already. The shared delimited
//     writer names DelimWriteSpec::shown, and runSaveData names its own
//     spelling, so `saveold` passes its refusals through untouched.
//  5. R-CLASS. `filefilter` publishes r(occurrences) from the handler; its arm
//     has no table block to capture from.
//
// STATE THAT MUST NOT OUTLIVE WHAT IT DESCRIBES. `putexcel`'s open workbook
// lives in `SessionState::putexcel` (session_state.h). `clear all` drops the
// other session objects (estimates, the estadd bag, graphs, postfiles, file
// handles, frames) in `SessionState::clearAllSessionObjects`, and putexcel
// belongs in that list for the same reason -- but session_state.cpp is not in
// this bundle's write manifest, so the line is REPORTED, not added here.
// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is the plan's "corpus options (implemented)" column; `unimplemented`
// is its "refused options" column, WIDENED wherever an option the plan called
// "ignored" would actually change the result. The split is the whole point: a
// name in NEITHER set reads as "unknown option", which is the wrong diagnosis
// for a real Stata option this build simply does not do, and a name wrongly
// placed in `known` is accepted and silently ignored, which is the defect the
// apparatus exists to prevent.
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::missing_kind -- insheet's float columns carry `.` only
#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/delimited_io.h"  // insheet: the one native delimited reader
#include "piet/display_hardening.h"  // evalScalarExpr -- `putexcel A1 = (expr)`
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt -- outfile writes through display formats
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"  // infix `in`: parseInRangeBounds
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"  // piet::fmt::general -- outfile's %10.0g
#include "piet/strutil.h"
#include "xlsx.h"
#include "file_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace file_command_detail {

// ===========================================================================
// Errors and blocks
// ===========================================================================

ojson ioError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is bundle C's shipped defect.
ojson ioSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot be
// relied on to recognise. `reason` is the only channel carrying an exact rc
// (BUG-187) and it outranks the coarse mapping.
ojson ioErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// A refusal whose SENTENCE the plan pins, built in refusedCommand's own shape
// so `testRegistryRoutes` still tells it apart from the generic gap: code
// `unsupported_command` (rc 199), `token` == the command word, and a
// `refused:<cause>` reason rather than `piet_gap`.
ojson ioRefusal(const std::string& cmd, const std::string& cause, const std::string& message) {
  return ojson(eval::errorBlock("unsupported_command", message, cmd, std::nullopt, std::nullopt,
                                "refused:" + cause));
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

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// ===========================================================================
// Small text helpers
// ===========================================================================

std::string lowerOf(std::string s) { return strutil::lower(std::move(s)); }
std::string trimOf(const std::string& s) { return strutil::trim(s); }

std::string dequote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which is exactly how a mistyped field becomes a silently wrong number.
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

namespace {

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}

}  // namespace

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
// The pre-comma word list, QUOTE-AWARE
// ===========================================================================
//
// `parsed.varlist` cannot be used for a path: the tokenizer splits the
// pre-comma text on bare whitespace (eval/tokenizer.cpp's splitWs is an
// istringstream loop), so `using "my file.csv"` arrives as three tokens with
// stray quote characters. `scanOneToken` is the shared quote-aware scanner and
// it strips one level of quoting, which is exactly what a filename needs.

// Drop the leading command word (the first bare token; a command word is never
// quoted, so a plain whitespace scan is exact here).
std::string dropCommandWord(const std::string& in) {
  const std::string s = trimOf(in);
  size_t i = 0;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return trimOf(s.substr(i));
}

// Everything on `raw` before the first unquoted, unparenthesised comma, with
// the leading command word removed.
std::string preCommaArgs(const std::string& raw) {
  std::string s = raw;
  const long long comma = eval::findUnquotedComma(s);
  if (comma >= 0) s = s.substr(0, static_cast<size_t>(comma));
  return dropCommandWord(s);
}

std::vector<std::string> quoteAwareWords(const std::string& s) {
  std::vector<std::string> out;
  std::string rest = s;
  while (true) {
    auto [tok, remainder] = eval::scanOneToken(rest, true);
    if (tok.empty() && remainder.empty()) break;
    if (!tok.empty()) out.push_back(tok);
    if (remainder == rest) break;  // defensive: never spin on a token-less tail
    rest = remainder;
    if (rest.empty()) break;
  }
  return out;
}

// Split `[varlist] using FILE` (or a bare `FILE`) the way every `using`-taking
// command in this bundle needs it. Returns false when no filename is present.
bool splitUsing(const std::vector<std::string>& words, std::vector<std::string>* varlist,
                std::string* pathArg) {
  varlist->clear();
  pathArg->clear();
  bool sawUsing = false;
  for (const auto& w : words) {
    if (!sawUsing && lowerOf(w) == "using") {
      sawUsing = true;
      continue;
    }
    if (sawUsing)
      *pathArg = w;
    else
      varlist->push_back(w);
  }
  if (!sawUsing) {
    // `insheet "f.csv"` and `saveold f` -- the filename is the LAST bare word.
    if (varlist->empty()) return false;
    *pathArg = varlist->back();
    varlist->pop_back();
  }
  return !pathArg->empty();
}

// Remove an `if`/`in` qualifier's words from a command's pre-comma words:
// each run starts at an `if` or `in` word and ends before `using` (or at the
// end), so both `outsheet using f if g != 0` and `outfile s g if year == 2001
// using f` keep their varlist and file. The qualifier itself reaches the
// handler parsed (parsed.ifExpr / parsed.inRange).
std::vector<std::string> dropQualifierWords(const std::vector<std::string>& words) {
  std::vector<std::string> out;
  bool inQual = false;
  for (const auto& w : words) {
    const std::string lw = lowerOf(w);
    if (lw == "using") inQual = false;
    else if (lw == "if" || lw == "in") inQual = true;
    if (!inQual) out.push_back(w);
  }
  return out;
}

// ===========================================================================
// Files
// ===========================================================================

bool pathExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(cecon::to_native_path(p), ec) && !ec;
}

// Two paths naming the SAME file. Canonicalized so `./x` and `x` compare equal;
// a path that cannot be canonicalized falls back to a byte compare, which is
// conservative in the safe direction (it may miss an alias, never invent one).
bool sameFile(const std::string& a, const std::string& b) {
  if (a == b) return true;
  std::error_code ea, eb;
  const auto ca = std::filesystem::weakly_canonical(cecon::to_native_path(a), ea);
  const auto cb = std::filesystem::weakly_canonical(cecon::to_native_path(b), eb);
  if (ea || eb) return false;
  return ca == cb;
}

// "Is the destination also a source?" asked of the SESSION: writing a delimited
// file over the .hbin the session is reading from would truncate the dataset
// mid-command and leave `_N` describing bytes that no longer exist.
std::optional<ojson> refuseIfSessionHbin(SessionState& session, const std::string& verb,
                                         const std::string& resolved,
                                         const std::string& spelling) {
  if (!session.currentHbin().has_value()) return std::nullopt;
  if (!sameFile(resolved, *session.currentHbin())) return std::nullopt;
  return ioError(verb + ": " + spelling +
                 " is the file this session's own dataset is read from — writing over it would "
                 "destroy the data in memory. Choose another destination.");
}

// The shared "may I create this output?" check. NAMES THE SPELLING THE USER
// TYPED, never the resolved path (defect class 4), in Stata's own words and
// code: `file o.out already exists` r(602) for outsheet, outfile, filefilter and
// copy alike (StataBE 19.5, BUG-410).
std::optional<ojson> refuseExisting(const std::string& resolved, const std::string& spelling,
                                    bool replace) {
  if (replace || !pathExists(resolved)) return std::nullopt;
  return ioErrorRc("file " + spelling + " already exists", 602);
}

// A file named without a suffix is NAME<suffix>, in the path and in every
// message: .out for outsheet, .raw for outfile and for the data infile and
// infix read, .dct for a dictionary. StataBE 19.5 `infile y using bare` is
// `file bare.raw not found` r(601) even beside a file called bare, and
// `infile using d1` reads d1.dct. A dot before the last '/' names a directory.
void assumeSuffix(std::string* resolved, std::string* spelling, const char* suffix) {
  const size_t slash = resolved->find_last_of('/');
  const size_t dot = resolved->find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return;
  *resolved += suffix;
  *spelling += suffix;
}

// Write `bytes` to `resolved` through a SIBLING temp + rename(2), so a failure
// never truncates whatever was there and an in-place rewrite (filefilter over
// its own input, which is separately refused, or an output that happens to be a
// source of the same command) can never read torn bytes. nullopt = written.
std::optional<ojson> commitBytes(const std::string& verb, const std::string& resolved,
                                 const std::string& spelling, const std::string& bytes) {
  const std::string tmp = resolved + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary | std::ios::trunc);
    // A file Stata cannot create (its directory missing): `file nodir/o.raw
    // could not be opened` r(603), outfile and filefilter alike (BUG-410).
    if (!f) return ioErrorRc("file " + spelling + " could not be opened", 603);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError(verb + ": failed writing " + spelling + " (disk full or I/O error)",
                     "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    return ioError(verb + ": failed writing " + spelling + " — " + mvEc.message(), "data_io");
  }
  return std::nullopt;
}

// Read a whole file. nullopt on failure (the caller words its own error, so the
// message can name the spelling rather than the resolved path).
std::optional<std::string> readWholeFile(const std::string& resolved) {
  std::ifstream f(cecon::to_native_path(resolved), std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) return std::nullopt;
  return ss.str();
}

// ===========================================================================
// Delimiters and headers
// ===========================================================================

// `delimiter(D)`, spelled Stata's way. Returns false and fills `err` on a
// spelling this build cannot turn into one character.
bool readDelimiterOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, char* out,
                      std::string* err) {
  auto v = optStrAny(opts, {"delimiter", "delim"});
  if (!v.has_value()) return true;  // absent -> the caller's default
  const std::string raw = *v;
  const std::string low = lowerOf(raw);
  if (low == "tab" || raw == "\\t") {
    *out = '\t';
    return true;
  }
  if (low == "comma") {
    *out = ',';
    return true;
  }
  if (low == "semicolon") {
    *out = ';';
    return true;
  }
  if (raw.size() == 1) {
    *out = raw[0];
    return true;
  }
  *err = "delimiter(): expected a single character, `tab`, `comma` or `semicolon` (got '" + raw +
         "')";
  return false;
}

}  // namespace file_command_detail

}  // namespace piet::handlers
