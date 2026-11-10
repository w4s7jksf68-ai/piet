// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) -- the community graph and geo
// verbs the ninety-paper corpus reaches for around its figures:
//
//   grc1leg2   `graph combine` with exactly one panel keeping its legend
//   geodist    the distance between two coordinate pairs, as a new column
//   geonear    each master row's k nearest rows in a `using` file
//   cdfplot    the empirical CDF, as the twoway line block already rendered
//   event_plot the stored event-study coefficients, in relative-time order
//
// (`grstyle`, the bundle's sixth registry row, is an accept-note handled
// entirely at its dispatcher arm and has no handler here.)
//
// NOTHING HERE ADDS A PLOT TYPE, A RENDERER, OR AN ENGINE VERB. That is the
// whole design. `cdfplot` emits `kind:"twoway"` with `kind:"line"` layers --
// byte-shape-identical to what layerLineOrConnected already produces, so the
// existing SVG builder and piet/frontend/js/graph.js draw it with no change.
// `grc1leg2` calls `runGraphCombine` and then sets `legend.off` on the panels
// that must lose their legend, a key graph.js:795 has honoured since BS-047.
// `event_plot` calls `runCoefplot` and reorders the coefficients it returns.
// The two geo verbs write their columns through `cobra setcol --new` +
// `session.load` + `mutationResult`, the same pair every data verb in this
// codebase uses (see datamgmt/data_generate.cpp). src/ (the engine), the
// frontend and graph/twoway.cpp are untouched.
//
// FIVE HANDLERS, NOT THE PLAN'S FOURTEEN COMMANDS. `grc1leg` -- the
// un-suffixed spelling -- is deliberately absent, here and in the registry:
// its package states no author (none in the ado, none in the help file), so
// the campaign's GPL-attribution rule gives it no row, and the plain spelling
// waits for its author while `grc1leg2` (which does claim one) ships. The
// eight names this bundle refuses -- cibar, binsreg, spmap, maptile, shp2dta,
// geoinpoly, heatplot, distplot -- are community names on the same footing,
// so they are unregistered for the same reason and already answer r(199),
// which is exactly what a named refusal arm would have said. There is no
// refusal arm here for any of them: a second, weaker copy of an answer the
// registry already gives is not honesty, it is duplication.
//
// THE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at least
// once in this campaign; the first has now been caught four times in seven
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads `["type"]`,
//     and that aborts the whole selftest binary. THIS BUNDLE HAS BOTH ARM
//     SHAPES, so the rule is per-handler, not per-file:
//       * `runGrc1leg`, `runCdfplot`, `runEventPlot` -- the arm hands the
//         result STRAIGHT to `registerGraphOutput` and then pushes it. Every
//         `return` in those three is ONE BLOCK OBJECT. Never an array.
//       * `runGeodist`, `runGeonear` -- the arm routes through
//         `pushEnvResult`, which flattens an array and drops nulls, so an
//         array of block objects is legal there and is how they carry a note
//         alongside their `mutationResult`.
//     The consequence for the three graph handlers is real and is why the
//     notes below live where they do: a graph arm CANNOT emit a separate
//     display block, so an "accepted but not applied" note is appended to the
//     block's own `note` string instead (see `appendBlockNote`).
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`/
//     `unsupported_option`; a refusal built with the DEFAULT `engine_error`
//     code silently becomes rc 1. So a user syntax error here always carries
//     "syntax_error" (`gxSyntax`), a real Stata option this build does not do
//     goes through the reject net's `unimplemented` half, and rc 1 is used
//     only where rc 1 is the honest answer (the geonear pair cap: the command
//     exists, the syntax is right, this build will not do the work).
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves, so it ACCEPTS an
//     abbreviation; `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "generate"` comparison see that abbreviation -- is
//     driven by the vocabulary REGISTRY. A command absent from the registry
//     therefore gets its abbreviations accepted by the net and then dropped
//     unread, and a name in NEITHER list is accepted-and-ignored outright.
//     All SIX command tokens register at NAMESPACE scope below (a
//     function-local static constructs too late: after the first call that
//     needed it), including `grstyle`, whose arm has no net -- the empty
//     registration is what gives `canonicalizeForCommand` an answer for the
//     token and keeps the inventory complete.
//     AN OPTION THIS BUILD DOES NOT READ GOES IN `unimplemented`, NEVER IN
//     `known`. Two consequences worth stating because they look like
//     exceptions and are not:
//       * The COSMETIC options (`grc1leg2`'s ring/position/span/symxsize/
//         symysize/labsize, `event_plot`'s plottype/ciplottype/graph_opt/
//         noautolegend) ARE read -- read to build a note that names them and
//         says they were accepted and not applied. The user is told; nothing
//         is silently dropped. That is the plan's design decision 1
//         ("known-ignored + note once") made literal.
//       * `xcommon`/`ycommon` are read and IMPLEMENTED (a common axis range
//         across the combined panels), rather than parked in the accepted-
//         and-noted set, because an axis scale is a claim about the numbers.
//  4. DELEGATE MESSAGES. `runGrc1leg` forwards to `runGraphCombine` and
//     `runEventPlot` to `runCoefplot`. An error from either delegate names
//     the DELEGATE ("graph combine: no graph named 'g1'"), which is a command
//     the user never typed. `renameDelegateError` rewrites the prefix so the
//     message names what the user actually ran.
//  5. ECHOED PATHS. `geonear` takes a `using` file. Every message about it
//     echoes THE SPELLING THE USER TYPED; `resolveUserPath`'s output is used
//     to open the file and nowhere else.
//
// ONE MORE RULE, FROM THE PLAN AND WORTH ITS OWN LINE: `cdfplot`'s ECDF is
// bundle F's `cumul` rank rule, TRANSCRIBED (see `ecdfRanks`). A second
// percentile/rank rule in one product is a silently wrong number waiting for
// a fixture that does not exist.
#include "piet/display_hardening.h"
#include "piet/handlers/graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle -- the ONE layer-style rule
#include "graph_extension_internal.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is what the handler below actually READS; `unimplemented` names real
// Stata options of the same command this build does not do, so the net can say
// "valid Stata option, not yet implemented" instead of the ambiguous "not
// allowed". A name in NEITHER set reads as an unknown option, which is the
// wrong diagnosis for a real one; a name wrongly placed in `known` is accepted
// and silently ignored, which is the defect the apparatus exists to prevent.
namespace {

// `grstyle` has no reject net at its arm (it accepts a note and returns), so
// this registration exists purely so `canonicalizeForCommand` has an answer for
// the token and the vocabulary inventory is complete -- the same reason
// datamgmt/dataset_shape_options.cpp registers `notes` and `compare` with empty sets.
const piet::eval::OptVocab& kVocabGrstyle =
    piet::eval::registerOptVocab({"grstyle"}, {{}, {}});


}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// `grstyle`'s registration is consumed by no net in this TU (its arm accepts
// and returns); naming it here keeps -Wunused-variable honest about that.
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {&kVocabGrstyle};

}  // namespace

namespace graph_extension_detail {

// ===========================================================================
// Errors and blocks
// ===========================================================================

ojson gxError(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is defect class 2.
ojson gxSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// Defect class 4. `runGraphCombine`/`runCoefplot` build their messages around
// their OWN command name; a user who typed `grc1leg2` must not be told what
// `graph combine` thinks. Rewrites the leading "<delegate>: " (or
// "<delegate>. ") prefix to the typed command, and prepends the typed name when
// the message does not start with the delegate's at all, so no error can escape
// this file naming a command the user did not run.
ojson renameDelegateError(const ojson& block, const std::string& typedCmd,
                          const std::string& delegate) {
  if (!isErrorBlock(block)) return block;
  std::string msg = block.value("message", std::string());
  const std::string pfx = delegate + ": ";
  if (msg.rfind(pfx, 0) == 0) {
    msg = typedCmd + ": " + msg.substr(pfx.size());
  } else if (msg.rfind(typedCmd + ":", 0) != 0) {
    msg = typedCmd + ": " + msg;
  }
  std::string code = block.value("code", std::string("engine_error"));
  ojson out = ojson(eval::engineError(msg, code));
  // The delegate may have carried an exact rc through the `reason` channel
  // (BUG-187); that outranks coarseRc's message rules and must survive the
  // rename, or a 198 silently becomes a 1.
  if (block.contains("reason") && !block["reason"].is_null()) out["reason"] = block["reason"];
  return out;
}

// THE ONE PLACE A GRAPH HANDLER CAN PUT A NOTE. The three graph arms push
// exactly one block (defect class 1), so a note cannot be a sibling display
// block the way `geodist`'s is. It is appended to the block's own `note` string
// -- a key the twoway and graph_combine renderers already draw. NOTE, HONESTLY:
// the `coefplot` renderer (svg.cpp's buildSvgCoefplot, and graph.js's
// coefplot branch) does NOT draw `note` today, so `event_plot`'s note reaches
// the block JSON and not the drawn figure. Making it visible means touching the
// renderer, which is outside this bundle's manifest; it is reported rather than
// done.
void appendBlockNote(ojson& block, const std::string& text) {
  if (!block.is_object()) return;
  std::string cur;
  if (block.contains("note") && block["note"].is_string()) cur = block["note"].get<std::string>();
  block["note"] = cur.empty() ? text : (cur + "  " + text);
}

// ===========================================================================
// Small text / number helpers
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

// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which here would turn a mistyped variable name into a silently plausible
// latitude.
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

// The names among `keys` that are present at all, in the order listed -- the
// input to every "accepted and not applied" note in this file.
std::vector<std::string> presentOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  std::vector<std::string> out;
  for (const char* k : keys) {
    if (findOpt(opts, k) != nullptr) out.emplace_back(k);
  }
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

// ===========================================================================
// Column reading / writing
// ===========================================================================

std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) return out;  // callers reject factors first
  const double* d = c.f64();
  for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  return out;
}

// A column's values as DISPLAY strings ("" = missing).
std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name) {
  std::vector<std::string> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const int32_t code = codes[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size())
        out.emplace_back();
      else
        out.push_back(c.levels[static_cast<size_t>(code)]);
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out.push_back(numCell(d[i]));
  }
  return out;
}

// ===========================================================================
// saving(FILE[, replace]) -- one implementation, two graph handlers
// ===========================================================================
//
// `saving()` is in both vocabularies' `known` half, so it has to actually save.
// It routes through `runGraphSave`, which owns the `.gph` format (a JSON
// sidecar under `{"_magic":"PIETGPH1","block":...}`) -- writing those bytes a
// second time here would be a second format writer. `runGraphSave` reads
// `session.lastGraph`, so the caller installs the FINISHED block there first.
// Returns the error block on failure; `runGraphSave`'s own success note is
// discarded, because these arms push exactly one block and that block is the
// graph.
std::optional<ojson> honourSaving(SessionState& session, const std::string& verb,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const ojson& block) {
  auto raw = optStr(opts, "saving");
  if (!raw.has_value() || raw->empty()) return std::nullopt;
  // `saving("f.gph", replace)` -- split the filename from its suboptions.
  std::string spec = *raw;
  bool replace = false;
  const size_t comma = spec.find(',');
  if (comma != std::string::npos) {
    for (const auto& w : splitWs(spec.substr(comma + 1))) {
      if (lowerOf(w) == "replace") replace = true;
    }
    spec = trimOf(spec.substr(0, comma));
  }
  spec = trimOf(dequote(trimOf(spec)));
  if (spec.empty()) return gxSyntax(verb + ": saving() needs a file name.");

  session.lastGraph = block;
  std::vector<std::pair<std::string, eval::OptValue>> saveOpts;
  if (replace) saveOpts.emplace_back("replace", eval::OptValue::ofBool(true));
  ojson res = ojson(runGraphSave(std::nullopt, spec, saveOpts, session));
  if (isErrorBlock(res)) return renameDelegateError(res, verb, "graph save");
  return std::nullopt;
}

}  // namespace graph_extension_detail

}  // namespace piet::handlers
