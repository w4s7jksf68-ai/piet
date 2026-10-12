// AER-SCOPE1 bundle E (TABLES, 2026-09-04) -- the `regress -> estadd -> eststo
// -> estout|esttab using f.tex` pipeline, plus the other community table
// commands that leave a FILE behind.
//
//   estadd      attach a local/scalar/matrix to the ACTIVE e()
//   estpost     a pseudo-estimate whose e(mean)/e(sd)/... are 1xk matrices
//   estout      the stored estimates as TeX / fixed-width / delimited
//   outreg2     the same, plus a .txt ledger that `append` reads back
//   texsave     the DATASET as a LaTeX table
//   texdoc/tex  an open .tex handle plus the line verb that writes into it
//   frmttable   a session MATRIX as a LaTeX table (TeX only; RTF refused)
//   regsave     the estimates as a DATASET
//   parmest     the same builder, parmest's column vocabulary
//   mat2txt     a session matrix as tab-separated text
//   erepost     overwrite the active e(b) from a matrix
//
// WHY ONE TU. Every one of these renders THE SAME THING -- a table of stored
// estimates -- and the plan's design decision 5 is binding: ONE `Table` model
// and THREE renderers (`renderTex` / `renderFixed` / `renderDelim`), shared by
// estout, esttab-using, outreg2, frmttable and texsave. The rejected
// alternative was a bespoke `\begin{tabular}` builder per command: five string
// assemblers means five goldens that drift apart one bug-fix at a time, and
// this codebase has paid for that shape before (the two e()-scalar tables, the
// four mutationResult copies).
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each shipped at least
// once during this campaign; the first has been caught three times in four
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborted the whole selftest binary. dispatcher.cpp's bundle-E arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal here -- but ONLY an array whose every element is a
//     real block object carrying a "type" key. Every `return` below is either
//     one object with "type", or an ojson::array() of such objects.
//  2. RC. `eval::coarseRc` answers 199 for `unsupported_command` or a `r(NNN)`
//     reason and 198 for `syntax_error`; a refusal built with the default
//     `engine_error` code silently becomes rc 1. So: command-level refusals go
//     through `eval::refusedCommand(cmd, cause)`, sub-verb refusals through
//     `eval::unsupportedCommand(..., kNotImplemented)`, USER SYNTAX errors
//     carry code "syntax_error" (rc 198), and anything with a known Stata rc
//     that coarseRc's message rules do not recognise states it through the
//     `reason` channel (bundle C had to repair six sites that defaulted to 1).
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes the
//     handler's own `k == "replace"` compare see them -- is driven by the
//     REGISTRY. An unregistered command gets its abbreviations accepted by the
//     net and then dropped unread: `outreg2 ..., repl` would run without
//     `replace` and say nothing. All twelve command names register at NAMESPACE
//     scope below (function-local statics construct too late -- after the first
//     call that needed them), and every handler's first act is the reject net.
//  4. ALIASING. A rewrite must move BOTH the routing name and the reported
//     name. No aliases in this bundle, so this class is structurally absent;
//     the one place `parsed` is reinterpreted (estadd/estpost/texdoc sub-verbs)
//     never rewrites `parsed.cmd`.
//  5. R-CLASS FORWARDING. r() is filled at the dispatcher CALL SITE, not in the
//     handler. None of these twelve verbs publishes r() in Stata (estpost and
//     estadd write e(), which this build carries in the estimates snapshot),
//     so no arm here needs `captureRFromBlock`.
//
// ------------------------------------------------------------------------
// THE SEAMS WITH THE FILES THIS LANE DOES NOT OWN.
//
//  (a) `runEststo` (eststo_esttab.cpp) attaches `snapshot["escalars"]` and
//      `snapshot["added"]` at store time, discarding a stale bag against the
//      SHARED fingerprint. That fingerprint is `SessionState::estAddFingerprint`
//      -- one function, written once, WRITTEN here by `estadd` and READ there by
//      `eststo`. Two private copies differing by one field would make the bag
//      never match, and the symptom would be an `estadd`ed row quietly missing
//      from the table at rc 0, so this file must never grow its own.
//      `materializeModel` below still has a read-time reconstruction path. It is
//      a BACKSTOP, not the mechanism: a snapshot that carries the fields wins
//      outright, and the fallback only fires for an estimate stored by some
//      other route (a snapshot built before this bundle, or one assembled from
//      LastModel's cached result rather than from lastEstBlock).
//
//  (b) `runEsttab` falls back to the ACTIVE estimate when no models are named
//      and the store is empty, which is what makes `estpost summarize x y` +
//      a bare `esttab` work. Two consequences land in THIS file:
//      `runEstpost` posts to `session.lastEstBlock` and never calls
//      `session.eststo(...)` (Stata's estpost stores nothing, and minting an
//      `m1` would change what a later `eststo` numbers itself); and
//      `writeEsttabFile` resolves the active estimate directly when the name it
//      is handed is not in the store, instead of reporting "model not found"
//      for a table that just rendered on screen.
#include "piet/handlers.h"

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
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::evalScalarExpr
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // piet::eval::eScalarKey
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// The `known` sets are the plan's command scope table verbatim; the
// `unimplemented` sets are the plan's "refusal options (named)" column. The
// split matters: an option in NEITHER set reads as "unknown option" (which is
// the wrong diagnosis for a real Stata option this build simply does not do),
// and an option wrongly placed in `known` is accepted and silently ignored,
// which is the defect this whole apparatus exists to prevent.
namespace {

const piet::eval::OptVocab& kVocabEstadd = piet::eval::registerOptVocab(
    {"estadd"}, {{"replace", "prefix", "quietly"}, {}});

const piet::eval::OptVocab& kVocabEstpost = piet::eval::registerOptVocab(
    {"estpost"},
    {{"statistics", "stat", "stats", "columns", "by", "listwise", "nototal", "quietly", "detail"},
     // `elabels` asks for by-group VALUE LABELS in the posted matrix names.
     // Declared unimplemented rather than accepted: accepting it would name the
     // rows by their numeric codes while the user asked for labels, and the
     // difference is invisible in the resulting table.
     {"casewise", "missing", "elabels", "escal"}});

const piet::eval::OptVocab& kVocabEstout = piet::eval::registerOptVocab(
    {"estout"},
    {{"style", "replace", "append", "cells", "b", "se", "t", "p", "fmt", "stats", "mlabels",
      "collabels", "keep", "drop", "order", "star", "starlevels", "varlabels", "labels", "label",
      "title", "note",
      // B4-W5: the discriminating pair the oracle itself identified. `estout
      // nonumbers` alone produced a file byte-identical to the plain arm
      // (milfpins_oracle_t2.do's own header says so), because estout's DEFAULT
      // header is the model NAME -- so the arm that separates them is
      // `numbers` vs `nonumbers`, e_numbers.txt vs e_numbers_off.txt. Before
      // this both spellings were unknown options and answered rc 198.
      "numbers", "nonumbers",
      // `transform()` -- 12 of the 18 corpus uses are on this command. `eform` is
      // NOT added with it: it is measurably `transform(exp(@) exp(@))` PLUS its
      // own removal of `_cons` (w1b_transform.log u2 has no intercept row where
      // u1 does), it has zero corpus uses on either writer, and accepting it as
      // an alias would drop that second half silently.
      "transform",
      // W1a-4: the two geometry knobs of `style(tex)`. Both are MINIMA -- a
      // longer label or number overflows unpadded (w1b_estout_tex2.log s25,
      // w1b_estout_tex.log rc) -- and `nolabel` is here because it is what
      // decides the stub's default between 20 and 12, winning over `label` when
      // both are given (w1b_estout_opts.log q3).
      "varwidth", "modelwidth", "nolabel",
      // W1a-4, all measured in w1b_estout_opts{,2}.log. The four seam hooks are
      // ADDITIVE here, unlike esttab's, where they replace a block (BUG-500);
      // `begin()` prefixes every row; `legend` prints the ladder DESCENDING and
      // only when the cells carry stars; `extracols()` inserts empty columns
      // after the columns it names; `rename()` is a row-label map whose misses
      // are no-ops (y b).
      "prehead", "posthead", "prefoot", "postfoot", "begin", "legend", "extracols", "rename"},
     {"unstack", "eqlabels", "indicate", "refcat", "transpose", "wide", "substitute", "delimiter",
      "end", "eform"}});
// `esttab`'s option vocabulary is NOT registered here. It has exactly one owner
// (`eststo_esttab.cpp`'s kVocabEsttab), because a command whose option set is
// declared in two translation units is a command with two answers about what it
// accepts. `cells` and `p` are listed there as UNIMPLEMENTED: `runEsttab` reads
// nostar/b/se/t/star and neither of those two, so accepting them would be the
// accept-and-ignore class this campaign exists to prevent (bundle A shipped it
// once as `ivreg ..., ro`). `estout` DOES implement cells() -- that handler is
// in this file and reads the spec -- so the honest statement is per command,
// not per option name.

const piet::eval::OptVocab& kVocabOutreg2 = piet::eval::registerOptVocab(
    {"outreg2"},
    {{"replace", "append", "ctitle", "addtext", "addstat", "label", "tex", "excel", "nocons",
      "keep", "dec", "bdec", "sdec", "nor2", "noobs", "adjr2", "noaster",
      // B4-W5: the four content options, each pinned to a captured arm in
      // tests/eval/golden/aer_b4/tables/ (o_tstat, o_tstat_tdec4, o_auto4,
      // o_stats). `noauto` comes with them because it is the switch outreg2's
      // own source uses to turn the adaptive number format off.
      "tstat", "tdec", "auto", "noauto", "stats",
      // `adec(#)` is the added-statistic decimal count -- 106 corpus uses over
      // three papers. `afmt()` is deliberately NOT here: it is the ado's only
      // escape from the separator bug, it has ZERO corpus uses, and its non-`f`
      // suffixes are unmeasured, so it stays an unknown option rather than an
      // accepted one this build would have to guess at.
      "adec",
      // `text` asks for the plain-text ledger, which this build ALWAYS writes
      // (the `.txt` file is its append source of truth), so it is accepted and
      // is a no-op rather than a rejection. Every one of the five captured
      // outreg2 arms spells it.
      "text"},
     {"word", "sortvar", "alpha", "e", "sideway", "paren", "bracket", "drop", "rdec",
      "onecol", "seeout", "symbol", "title"}});

const piet::eval::OptVocab& kVocabTexsave = piet::eval::registerOptVocab(
    {"texsave"},
    {{"replace", "title", "headerlines", "frag", "hlines", "align", "nofix", "footnote",
      "varlabels"},
     {"size", "width", "marker", "location", "bold", "italics", "underline", "geometry",
      "landscape", "nonames", "autonumber", "rowsep", "sw"}});

// `force` is texdoc's "overwrite even if the file is open" flag; `cmdstrip` and
// friends belong to `texdoc do`/`texdoc stlog`, which this build does not run.
const piet::eval::OptVocab& kVocabTexdoc = piet::eval::registerOptVocab(
    {"texdoc"}, {{"replace", "append", "force"}, {"cmdstrip", "gropts", "nolog", "nooutput"}});

// `tex` (texdoc's line verb) takes no options at all: everything after the
// verb is literal TeX. Registering an EMPTY vocabulary is not a no-op -- it is
// what makes any `tex ..., foo` reach the net as an unknown key rather than
// being silently swallowed.
const piet::eval::OptVocab& kVocabTexLine =
    piet::eval::registerOptVocab({"tex"}, {{}, {}});

const piet::eval::OptVocab& kVocabFrmttable = piet::eval::registerOptVocab(
    {"frmttable"},
    {{"statmat", "sdec", "tex", "replace", "rtitles", "ctitles", "title", "note", "using",
      "fragment",
      // W1a-5h. `nocenter` drops the `\begin{center}` wrapper and nothing else
      // (w1b_frmttable_base.log h7 against h1); `sfmt()` names the numeric
      // format whose `fc` spelling is ALREADY the default this writer uses, so
      // `sfmt(fc,...)` is byte-identical to leaving it out (cells2 k7 against
      // k6) -- accepted as a measured no-op for that spelling, and any other
      // suffix is refused below rather than silently treated as `fc`.
      "nocenter", "sfmt"},
     // `replay`/`replay1`/`store` are frmttable's stored-table accumulator
     // (frmttable.ado:96-184 keeps the built table in a Mata external
     // `_FrmtT<name>`): `store()` names it, `append`/`merge` accumulate into it,
     // and `replay` writes it out. Measured in w1b_frmttable_replay.log -- append
     // STACKS rows (g2) and merge adds COLUMNS (g4) -- and named unimplemented
     // rather than unknown, because the model is understood and absent, not
     // unrecognised. `multicol()` is the tex span option paper 004 pairs with
     // replay (gb).
     {"merge", "append", "varlabels", "substat", "annotate", "asymbol", "coljust", "hlines",
      "doubles", "replay", "store", "multicol", "addrows"}});

const piet::eval::OptVocab& kVocabRegsave = piet::eval::registerOptVocab(
    {"regsave"},
    {{"replace", "append", "ci", "se", "pval", "tstat", "addlabel", "level", "cmdline"},
     {"table", "detail", "format", "asterisk", "covar", "autoid", "saveold"}});

const piet::eval::OptVocab& kVocabParmest = piet::eval::registerOptVocab(
    {"parmest"},
    {{"saving", "norestore", "list", "omit", "label", "idnum", "idstr", "level", "replace"},
     {"format", "escal", "eform", "flist", "elist", "stars", "rename", "bmat", "vmat"}});

const piet::eval::OptVocab& kVocabMat2txt = piet::eval::registerOptVocab(
    {"mat2txt"}, {{"matrix", "saving", "replace", "append", "title", "format", "note"}, {}});

const piet::eval::OptVocab& kVocabErepost = piet::eval::registerOptVocab(
    {"erepost"}, {{"rename"}, {"esample", "noesample", "cmd", "properties"}});

// `tabout` is refused at the ARM (dispatcher.cpp), so it never reaches a
// handler here. It still gets a vocabulary entry so that
// `canonicalizeForCommand` has something to answer for the token and the
// registry inventory guard sees a complete bundle. Everything is
// `unimplemented`: the whole grammar is what is missing, not a set of options.
const piet::eval::OptVocab& kVocabTabout =
    piet::eval::registerOptVocab({"tabout"}, {{}, {"using", "replace", "append", "style", "cells",
                                                  "format", "clab", "sum", "svy"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// The registrations above are consumed by the reject nets below; naming them
// here keeps `-Wunused-variable` honest about the two that no handler in this
// TU reads (tabout is refused at the arm, and `tex` has an empty vocabulary).
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {
    &kVocabTabout, &kVocabTexLine};

// ===========================================================================
// Errors
// ===========================================================================

ojson tblError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. `syntax_error` is what coarseRc maps to rc 198; the
// default `engine_error` maps to rc 1, and bundle C had to repair six sites
// that had made exactly that substitution.
ojson tblSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson tblErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson echoBlock(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// ===========================================================================
// Option access
// ===========================================================================

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
  // A bare flag parses as isBool/true; `tex(frag)` parses as a STRING value but
  // still means "tex was given". Both forms answer yes -- reading only the bool
  // form is how `outreg2 ..., tex(frag)` would have quietly written plain text.
  return v->isBool ? v->boolValue : true;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return v->strValue;
}

// ===========================================================================
// Small text helpers
// ===========================================================================

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

// Split at TOP-LEVEL whitespace only: `b(star fmt(3)) se(par)` is two items,
// not five. Quotes count as depth too, so `label("Std. err.")` survives.
std::vector<std::string> splitTopLevel(const std::string& s, char sep = ' ') {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQuote = false;
  for (char c : s) {
    if (c == '"') inQuote = !inQuote;
    if (!inQuote) {
      if (c == '(' || c == '[' || c == '{') ++depth;
      if (c == ')' || c == ']' || c == '}') --depth;
    }
    const bool isSep = (sep == ' ') ? (c == ' ' || c == '\t') : (c == sep);
    if (isSep && depth == 0 && !inQuote) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
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

std::string rpad(const std::string& s, size_t w) {
  return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

std::string lpad(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// ===========================================================================
// Numbers
// ===========================================================================

// A Stata missing is `.`, never 0 -- in EVERY cell this file renders. A zero
// where a missing belongs reads as an estimated null effect, which is a
// different scientific claim from "not estimated".
constexpr const char* kMissingCell = ".";

// `fmt(3)` means `%9.3f`; `fmt(%9.2f)` is passed through. `fmt(a3)` is Stata's
// "adaptive" width and is REFUSED by the callers rather than silently rendered
// as a fixed 3 decimals -- refusing is the plan's decision 4.
std::string normalizeFmt(const std::string& raw, bool* okOut) {
  *okOut = true;
  const std::string f = strutil::trim(dequote(raw));
  if (f.empty()) {
    *okOut = false;
    return "%9.3f";
  }
  if (f[0] == '%') return f;
  if (f.find_first_not_of("0123456789") == std::string::npos) {
    return "%9." + f + "f";
  }
  *okOut = false;
  return "%9.3f";
}

// The `%[-][w].[p][fge]` subset the corpus exercises. Anything else falls back
// to three decimals, which is Stata's own table default.
// Defined below, next to the other number shapes; declared here because the
// `a#` branch of fmtNum needs it and fmtNum comes first in this file.
std::string sigDigits(double v, int d, int minDec, bool comma);

std::string fmtNum(double v, const std::string& fmt) {
  if (std::isnan(v)) return kMissingCell;
  // W1a-5b: `a#` is esttab's own ADAPTIVE format -- at least # significant
  // digits, which is what a footer row carries when nothing overrides it.
  // Measured: `esttab, scalars(mymean)` renders 6165.257 as `6165.3`
  // (w1b_sfmt_rule.log q5), which %9.3f cannot produce. The spelling is
  // accepted here so the footer can ask for it by name instead of the caller
  // reaching for a second formatter.
  if (!fmt.empty() && fmt[0] == 'a') {
    int d = 3;
    if (fmt.size() > 1) {
      try {
        size_t used = 0;
        const int n = std::stoi(fmt.substr(1), &used);
        if (used + 1 == fmt.size() && n >= 0 && n <= 12) d = n;
      } catch (const std::exception&) {
        d = 3;
      }
    }
    return sigDigits(v, d, /*minDec=*/1, /*comma=*/false);
  }
  int prec = 3;
  char kind = 'f';
  size_t i = 0;
  if (i < fmt.size() && fmt[i] == '%') ++i;
  while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '0' || fmt[i] == ' ')) ++i;
  while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) ++i;
  if (i < fmt.size() && fmt[i] == '.') {
    ++i;
    std::string digits;
    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) digits += fmt[i++];
    if (!digits.empty()) prec = std::stoi(digits);
  }
  if (i < fmt.size() && (fmt[i] == 'f' || fmt[i] == 'g' || fmt[i] == 'e')) kind = fmt[i];
  // `%9.0g` / `%10.0g` is Stata's DEFAULT general format, not "one significant
  // digit": C's `%.0g` is treated as `%.1g` and would render 1.234 as "1".
  // `fmtStataDefaultG` is the measured nine-character rule display already uses,
  // so mat2txt's default format and `display` agree on the same number.
  if (kind == 'g' && prec == 0) return piet::fmtStataDefaultG(v);
  char buf[128];
  if (kind == 'f') {
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  } else if (kind == 'e') {
    std::snprintf(buf, sizeof(buf), "%.*e", prec, v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
  }
  return std::string(buf);
}

// --- estout's `a#` adaptive format -----------------------------------------
// estout.ado:3877-3917 `SignificantDigits` ("idea stolen from outreg2.ado",
// says its own comment -- which is why outreg2's `auto(#)` shares this body).
// `minDec` is the one place the two callers differ: esttab floors at ONE
// decimal (1946.069 -> 1946.1) and outreg2 floors at ZERO (-> 1,946). That
// single integer is the whole difference between t_plain.txt and o_plain.txt
// on the same coefficient, so it is a parameter rather than two copies.
// --- outreg2 `adec(#)`: three regimes of one width-12 format ---------------
// outreg2.ado:10245 `_addstat_parse` renders each added statistic as
//   string(value, "%12.<adec><afmt>")            (afmt defaults to `fc`)
// and splices the result back into the COMMA-DELIMITED addstat() string that
// outreg2 then re-parses. A thousands separator therefore comes back as a FIELD
// separator, which is why the corpus's own line -- `addstat(Mean dep var,
// `mdp') adec(2)`, 106 uses across three papers -- is r(198) in this Stata
// whenever that mean reaches four integer digits (w1b_adec_rule5.log,
// W1BAD5REC|corpus_macro|198|, beside W1BAD5REC|corpus_rmean_ok|0| on a mean of
// 21.3). The refusal belongs to the caller; this builds the string and counts
// the separators that survived.
//
// The `%12.` WIDTH is what makes this three regimes rather than two, and each
// one is measured (w1b_adec_rule{1,3,4,5}.log):
//   plain "%.<adec>f" over 12 chars -> e-notation, min(adec,5) decimals, and
//                                      adec(0) also five; no separator, rc 0
//   separated spelling within 12    -> separators kept -> outreg2 FAILS
//   separated spelling over 12      -> separators dropped, rc 0
// 6165.257 flips between adec(6) (`6,165.257000`, 12 chars, r(198)) and adec(7)
// (`6,165.2570000`, 13 chars, rc 0, rendering 6165.2570000). Testing |value| >=
// 1000 instead would get BOTH ends wrong: 999.96 at adec(1) IS `1,000.0` and
// refuses (W1BADREC|v999_96_adec1|198|), and 9999999 at adec(3) does not.
struct AdecCell {
  std::string text;
  int seps = 0;
};

AdecCell outreg2AdecCell(double v, int adec) {
  AdecCell out;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", adec, v);
  const std::string plain(buf);
  if (plain.size() > 12) {
    // Stata's width overflow. The mantissa carries adec decimals up to five, and
    // adec(0) carries five as well: 1e12 is `1.0e+12` at adec(1), `1.000e+12` at
    // adec(3), `1.00000e+12` at adec(0), adec(5), adec(6) and adec(11)
    // (w1b_adec_rule4.log u1/u2/u4/u5/u6 and w1b_adec_rule3.log t6).
    const int edec = (adec >= 1 && adec <= 5) ? adec : 5;
    std::snprintf(buf, sizeof(buf), "%.*e", edec, v);
    out.text = buf;
    return out;
  }
  std::string sep = plain;
  const size_t dot = sep.find('.');
  const size_t start = (!sep.empty() && sep[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? sep.size() : dot;
  int inserted = 0;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    sep.insert(at, ",");
    ++inserted;
  }
  if (inserted > 0 && sep.size() <= 12) {
    out.text = sep;
    out.seps = inserted;
    return out;
  }
  out.text = plain;  // the separated spelling did not fit, so the ado drops it
  return out;
}

std::string sigDigits(double v, int d, int minDec, bool comma) {
  if (std::isnan(v)) return kMissingCell;
  int dec = 0;
  if (v == std::trunc(v)) {
    dec = 0;  // "integer: print no decimal places"
  } else if (std::fabs(v) < 1.0) {
    // `right` = zeros after the decimal point. Stata's int() truncates toward
    // zero, which is what a C cast does, so -1.0128 -> -1 on both sides.
    const double frac = std::fabs(v - std::trunc(v));
    const int right = -static_cast<int>(std::log10(frac));
    dec = std::max(1, d + right);
    if (dec > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.*e", std::min(9, d - 1), v);
      return std::string(e);
    }
  } else {
    const int left = static_cast<int>(std::log10(std::fabs(v)) + 1);
    if (left > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.0e", v);
      return std::string(e);
    }
    dec = std::max(minDec, d - left + 1);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string s(buf);
  if (!comma) return s;
  // %fc: thousands separators on the integer part only.
  const size_t dot = s.find('.');
  size_t start = (s[0] == '-') ? 1 : 0;
  size_t end = (dot == std::string::npos) ? s.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    s.insert(at, ",");
  }
  return s;
}

std::string fmtJson(const ojson& v, const std::string& fmt) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  return fmtNum(v.get<double>(), fmt);
}

// An observation count renders as an integer (Stata's `%9.0g` on an integral
// value), never as "30.000".
std::string fmtCount(const ojson& v) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  const double d = v.get<double>();
  if (std::isnan(d)) return kMissingCell;
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    return std::to_string(static_cast<long long>(d));
  }
  return fmtNum(d, "%9.3f");
}

struct StarLevel {
  const char* mark;
  double cutoff;
};

// estout/esttab's default ladder and outreg2's are DIFFERENT, and this is not
// cosmetic: the same p = 0.03 gets one star from estout and two from outreg2.
// The plan's Stata-semantics section fixes both explicitly for that reason.
const std::vector<StarLevel>& estoutStars() {
  static const std::vector<StarLevel> k = {{"***", 0.001}, {"**", 0.01}, {"*", 0.05}};
  return k;
}
const std::vector<StarLevel>& outreg2Stars() {
  static const std::vector<StarLevel> k = {{"***", 0.01}, {"**", 0.05}, {"*", 0.10}};
  return k;
}

std::string starsFor(const ojson& p, const std::vector<StarLevel>& ladder) {
  if (!p.is_number()) return "";
  const double pv = p.get<double>();
  if (std::isnan(pv)) return "";
  for (const auto& lv : ladder) {
    if (pv < lv.cutoff) return lv.mark;
  }
  return "";
}

std::string starNote(const std::vector<StarLevel>& ladder) {
  std::string s;
  for (size_t i = 0; i < ladder.size(); ++i) {
    if (i) s += ", ";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s p<%g", ladder[i].mark, ladder[i].cutoff);
    s += buf;
  }
  return s;
}

// `starlevels(* 0.10 ** 0.05 *** 0.01)` -- pairs of mark/threshold. A malformed
// list keeps the default rather than half-applying, which is the same rule
// eststo_esttab.cpp's parseStarOpt already follows for `star()`.
std::vector<StarLevel> parseStarLevels(const std::string& spec,
                                       const std::vector<StarLevel>& fallback,
                                       std::vector<std::string>* markStore) {
  const std::vector<std::string> toks = splitWs(dequote(spec));
  if (toks.size() < 2 || toks.size() % 2 != 0) return fallback;
  std::vector<StarLevel> out;
  markStore->clear();
  markStore->reserve(toks.size() / 2);
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    try {
      size_t pos = 0;
      const double th = std::stod(toks[i + 1], &pos);
      if (pos != toks[i + 1].size()) return fallback;
      markStore->push_back(toks[i]);
      out.push_back(StarLevel{nullptr, th});
    } catch (...) {
      return fallback;
    }
  }
  for (size_t i = 0; i < out.size(); ++i) out[i].mark = (*markStore)[i].c_str();
  std::sort(out.begin(), out.end(),
            [](const StarLevel& a, const StarLevel& b) { return a.cutoff < b.cutoff; });
  return out;
}

// ===========================================================================
// THE TABLE MODEL AND ITS THREE RENDERERS (plan design decision 5)
// ===========================================================================

struct TRow {
  enum Kind { kCoef, kSe, kStat, kText, kRule };
  Kind kind = kCoef;
  std::string label;
  std::vector<std::string> cells;  // one per column; "" renders as blank
};

struct Table {
  std::string title;
  std::vector<std::string> colNums;   // "(1)", "(2)", ... ; empty = suppressed
  std::vector<std::string> colHeads;  // mtitles / ctitles / depvars
  // The head of the STUB column (the leftmost, label-bearing one). Blank for an
  // estimates table, whose stub holds coefficient names and needs no head;
  // texsave's leftmost column is a data column and its variable name would
  // otherwise be dropped on the floor.
  std::string stubHead;
  // BUG-486: the CELL-NAME row. estout writes one under the model heads naming
  // which statistic each column carries (`b`, or `b/se` for a stacked pair) --
  // `tests/eval/golden/aer_w1/w1b_tables/e2.txt` row 2, and e_plain.txt row 2
  // from the 09-09 capture. `collabels(none)` is exactly the option that
  // removes it (w1b_tables/e7.tex, which carries the model names and no cell
  // row). Empty for every other writer, whose renderers therefore do not
  // change shape.
  std::vector<std::string> colLabels;
  std::vector<TRow> body;
  std::vector<TRow> foot;
  std::vector<std::string> notes;
  std::string align;  // texsave's align(); empty = derived

  size_t ncols() const {
    size_t n = colHeads.size();
    n = std::max(n, colNums.size());
    n = std::max(n, colLabels.size());
    for (const auto& r : body) n = std::max(n, r.cells.size());
    for (const auto& r : foot) n = std::max(n, r.cells.size());
    return n;
  }
};

// LaTeX's ten specials. `nofix` (texsave) turns this off; everything else in
// this file escapes unconditionally, because an unescaped `_` in a variable
// name is the single most common way a generated .tex file fails to compile.
std::string texEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '_': case '&': case '%': case '$': case '#': case '{': case '}':
        out += '\\';
        out += c;
        break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '\\': out += "\\textbackslash{}"; break;
      default: out += c;
    }
  }
  return out;
}

struct TexStyle {
  bool booktabs = false;   // \toprule/\midrule/\bottomrule instead of \hline
  bool fragment = false;   // rows only -- no tabular environment at all
  bool doubleTop = false;  // estout style(tex): \hline\hline above the header
  bool escape = true;
  bool standalone = false;  // wrap in \documentclass ... \begin{document}
  // The column specification is NOT cosmetic here: the plan's Stata-semantics
  // section fixes a different spelling per command (estout writes
  // `l*{k}{c}`, outreg2 writes `l c c`), and these strings are what the frozen
  // .tex goldens compare against.
  enum Align { kPlain, kStar, kSpaced };
  Align align = kPlain;
  // W1a-5c: the LETTER inside `l*{k}{c}`. esttab's `alignment()`/`align()`
  // picks it -- `alignment(S)` writes `l*{2}{S}` (w1b_esttab_resid.log y12),
  // and `alignment(c)` is byte-identical to the control because `c` is already
  // the default (y11). Anything else is the caller's own LaTeX column type
  // (siunitx's `S`, `D{.}{.}{-1}` from dcolumn) and is written through
  // unchanged rather than validated against a list this build cannot know.
  std::string alignLetter = "c";
};

std::string renderTex(const Table& t, const TexStyle& st) {
  const size_t k = t.ncols();
  auto esc = [&](const std::string& s) { return st.escape ? texEscape(s) : s; };
  auto rowOut = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = esc(label);
    for (size_t c = 0; c < k; ++c) {
      line += " & ";
      if (c < cells.size()) line += esc(cells[c]);
    }
    line += " \\\\";
    return line;
  };

  std::ostringstream o;
  const std::string top = st.booktabs ? "\\toprule" : (st.doubleTop ? "\\hline\\hline" : "\\hline");
  const std::string mid = st.booktabs ? "\\midrule" : "\\hline";
  const std::string bot = st.booktabs ? "\\bottomrule" : "\\hline";

  if (!st.fragment) {
    if (st.standalone) {
      o << "\\documentclass{article}\n\\begin{document}\n";
    }
    if (!t.title.empty()) o << "\\caption{" << esc(t.title) << "}\n";
    std::string align = t.align;  // texsave's align() wins outright
    if (align.empty()) {
      if (st.align == TexStyle::kStar) {
        align = "l*{" + std::to_string(k) + "}{" + st.alignLetter + "}";
      } else if (st.align == TexStyle::kSpaced) {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += " c";
      } else {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += "c";
      }
    }
    o << "\\begin{tabular}{" << align << "}\n";
    o << top << "\n";
  }

  if (!t.colNums.empty()) {
    o << rowOut("", t.colNums) << "\n";
  }
  if (!t.colHeads.empty() || !t.stubHead.empty()) {
    o << rowOut(t.stubHead, t.colHeads) << "\n";
  }
  // The cell-name row travels with the table, so it appears in every style.
  // Its PLACEMENT in tex is not separately captured -- w1b_tables/e7.tex is a
  // `collabels(none)` arm and carries no such row -- but a table model that
  // holds a row the tex writer silently drops is the worse of the two.
  if (!t.colLabels.empty()) {
    o << rowOut("", t.colLabels) << "\n";
  }
  if (!st.fragment && (!t.colNums.empty() || !t.colHeads.empty() || !t.stubHead.empty())) {
    o << mid << "\n";
  }

  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) {
      o << mid << "\n";
      continue;
    }
    o << rowOut(r.label, r.cells) << "\n";
  }

  if (!t.foot.empty()) {
    if (!st.fragment) o << mid << "\n";
    for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) {
        o << mid << "\n";
        continue;
      }
      o << rowOut(r.label, r.cells) << "\n";
    }
  }

  if (!st.fragment) {
    o << bot << "\n";
    o << "\\end{tabular}\n";
    for (const auto& n : t.notes) {
      // \multicolumn would need the column count inside the tabular; outside it
      // a note is ordinary text, which is what both estout and outreg2 emit.
      o << "\\begin{tablenotes}\n\\item " << esc(n) << "\n\\end{tablenotes}\n";
    }
    if (st.standalone) o << "\\end{document}\n";
  }
  return o.str();
}

// estout's four SEAM hooks and its row prefix. On esttab these four REPLACE a
// block of the table (BUG-500); on estout there is no block to replace -- a tab
// table has no rules -- and they are purely ADDITIVE lines at the seam
// (tests/eval/golden/aer_w1/w1b_estout_opts.log qb..qe). `prefoot` emits even
// when there are no stats rows, at the position the footer would have started
// (w1b_estout_opts2.log y9), and `begin(X)` prefixes EVERY row with no separator
// (qf: `BEGIN\te1`, `BEGINmpg\t-49.51222`).
struct EstoutHooks {
  std::optional<std::string> prehead, posthead, prefoot, postfoot;
  std::string begin;
};

std::string renderFixed(const Table& t, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  std::vector<size_t> width(k + 1, 0);
  auto measure = [&](const std::string& label, const std::vector<std::string>& cells) {
    width[0] = std::max(width[0], label.size());
    for (size_t c = 0; c < k; ++c) {
      if (c < cells.size()) width[c + 1] = std::max(width[c + 1], cells[c].size());
    }
  };
  measure("", t.colNums);
  measure("", t.colHeads);
  measure("", t.colLabels);
  for (const auto& r : t.body) measure(r.label, r.cells);
  for (const auto& r : t.foot) measure(r.label, r.cells);
  for (size_t c = 1; c <= k; ++c) width[c] = std::max<size_t>(width[c], 8);
  width[0] = std::max<size_t>(width[0], 12);

  size_t total = width[0];
  for (size_t c = 1; c <= k; ++c) total += 1 + width[c];
  const std::string rule(total, '-');

  std::ostringstream o;
  if (!t.title.empty()) o << t.title << "\n";
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = rpad(label, width[0]);
    for (size_t c = 0; c < k; ++c) {
      line += " " + lpad(c < cells.size() ? cells[c] : std::string(), width[c + 1]);
    }
    // Trailing blanks carry no information and would make every golden
    // sensitive to a column that happens to be empty.
    while (!line.empty() && line.back() == ' ') line.pop_back();
    o << hk.begin << line << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  o << rule << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  o << rule << "\n";
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (!t.foot.empty()) {
    o << rule << "\n";
    if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
      emit(r.label, r.cells);
    }
  }
  o << rule << "\n";
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

// A delimited cell. Comma output quotes anything containing the delimiter or a
// quote; tab output does not, because outreg2's own `.txt` ledger is read back
// by this build's `append` parser and an added quote would not round-trip.
std::string delimCell(const std::string& s, char delim) {
  if (delim != ',') return s;
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// --- estout's style(tex) --------------------------------------------------
// BUG-486's SIXTH axis, and the largest single block left in this bundle: 148 of
// the corpus's 219 estout lines carry tex, over 11 papers. This writer handed
// them to `renderTex` -- the esttab emitter -- so all 148 got
// `\begin{tabular}{l*{2}{c}}`, `\hline\hline` and ` & e1 & e2 \\`, where the ado
// writes a bare FRAGMENT of fixed-width right-aligned cells and no rules at all:
//
//     `            &          t1&          t2\\`
//     `mpg         &   -49.51222&   -238.8943\\`
//
// (tests/eval/golden/aer_w1/w1b_estout_tex.log r1, and w1b_tables/e7.tex from the
// 2026-09-14 capture, which shows the same thing and was never diffed -- the
// "captured but not compared" shape BUG-486 already records about itself.)
//
// Three widths are the whole implementable content, and each is measured:
//   stub  = varwidth() if given, else 20 when `label` is in effect and 12 when it
//           is not. It is the LABEL FLAG and not the content: a two-character
//           label still gets 20 and a 25-character one overflows unpadded
//           (w1b_estout_tex2.log s12..s25 against the no-label control).
//   cell  = modelwidth() if given, else 12, right-aligned, also a MINIMUM (a
//           13-character number overflows: w1b_estout_tex.log rc).
//   star  = 3, LEFT-aligned after the cell, so `1.746559** ` keeps its trailing
//           space where `-238.8943***` has none (r4). Padding value+star together
//           to 15 would get the first of those wrong and the second right.
std::string renderEstoutTex(const Table& t, size_t stubW, size_t cellW,
                            size_t starW, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  auto padR = [](const std::string& v, size_t w) {
    return v.size() >= w ? v : v + std::string(w - v.size(), ' ');
  };
  auto padL = [](const std::string& v, size_t w) {
    return v.size() >= w ? v : std::string(w - v.size(), ' ') + v;
  };
  // The star mark is concatenated into the cell text upstream (`cellFor` appends
  // it), and the two halves pad differently -- so it is split back off here. A
  // trailing run of `*`/`+` is the mark: no number this writer formats ends in
  // either, and `(omitted)` does not.
  auto splitStar = [](const std::string& c, std::string* star) {
    size_t e = c.size();
    while (e > 0 && (c[e - 1] == '*' || c[e - 1] == '+')) --e;
    *star = c.substr(e);
    return c.substr(0, e);
  };
  auto line = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string out = padR(label, stubW);
    for (size_t c = 0; c < k; ++c) {
      std::string star;
      const std::string v = c < cells.size() ? splitStar(cells[c], &star) : std::string();
      out += "&" + padL(v, cellW) + padR(star, starW);
    }
    return out + "\\\\";
  };

  std::ostringstream o;
  auto row = [&](const std::string& label, const std::vector<std::string>& cells) {
    o << hk.begin << line(label, cells) << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  // `numbers` puts the (1)/(2) row ABOVE the model names (r a), which is the
  // opposite of esttab's order.
  if (!t.colNums.empty()) row(std::string(), t.colNums);
  if (!t.colHeads.empty()) row(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) row(std::string(), t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) row(r.label, r.cells);
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) row(r.label, r.cells);
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

std::string renderDelim(const Table& t, char delim, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  std::ostringstream o;
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = delimCell(label, delim);
    for (size_t c = 0; c < k; ++c) {
      line += delim;
      line += delimCell(c < cells.size() ? cells[c] : std::string(), delim);
    }
    o << hk.begin << line << "\n";
  };
  if (!t.title.empty()) o << delimCell(t.title, delim) << "\n";
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << delimCell(n, delim) << "\n";
  return o.str();
}

// ===========================================================================
// File writing
// ===========================================================================

// SIBLING TEMP + rename(2), never a write into the destination. Copied in shape
// from `export delimited`'s writer (handlers_data_verbs3.cpp:1245-1340): a
// failure never leaves a truncated file where a readable one used to be, and a
// destination that is ALSO a source (outreg2's `append` ledger) is safe because
// the caller has already read it fully into memory before calling this.
//
// `userPath` is what the user TYPED and is the only spelling that ever reaches
// the output; `resolved` is what gets opened. Bundle D echoed the resolved
// absolute path and baked a build machine's workdir into a golden.
ojson writeTextFileAtomic(const std::string& cmd, const std::string& resolved,
                          const std::string& userPath, const std::string& bytes) {
  const std::string tmp = resolved + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary);
    if (!f) return tblError(cmd + ": cannot write " + userPath, "data_io");
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath + " (disk full?)", "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    return tblError(cmd + ": failed writing " + userPath, "data_io");
  }
  return ojson();  // null == success; callers test .is_null()
}

std::string readWholeFile(const std::string& path, bool* okOut) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) {
    *okOut = false;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *okOut = true;
  return ss.str();
}

// The existence/replace/append gate every file-writing verb here shares.
// Returns a non-null error block when the write must not happen.
ojson guardOverwrite(const std::string& cmd, const std::string& resolved,
                     const std::string& userPath, bool replace, bool append) {
  if (cecon::native_exists(resolved) && !replace && !append) {
    // coarseRc maps "file ... already exists" to rc 602, which is Stata's own
    // code for this, so the wording is load-bearing rather than decorative.
    return tblError(cmd + ": file " + userPath +
                    " already exists (add `replace` to overwrite)");
  }
  return ojson();
}

// `using f` with no extension gets one. Stata's table commands each have their
// own default and getting it wrong writes a file the user cannot find.
std::string ensureExt(const std::string& path, const std::string& ext) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return path;
  return path + ext;
}

std::string lowerExt(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return strutil::lower(path.substr(dot));
}

// `using FILE` split out of a varlist. Returns the raw (still user-spelled)
// path, and erases `using` and everything after it from `words`.
std::optional<std::string> takeUsing(std::vector<std::string>& words) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (strutil::lower(words[i]) == "using") {
      std::optional<std::string> path;
      if (i + 1 < words.size()) path = dequote(words[i + 1]);
      words.erase(words.begin() + static_cast<long>(i), words.end());
      return path;
    }
  }
  return std::nullopt;
}

// ===========================================================================
// Stored estimates: the bag, the e() scalars, and the staleness rule
// ===========================================================================

ojson emptyBag() {
  ojson b;
  b["scalars"] = ojson::object();
  b["locals"] = ojson::object();
  b["matrices"] = ojson::object();
  return b;
}

// Freeze the e() scalars a stored estimate should carry. `eScalarKey()` is the
// ONE table of Stata-spelling -> cobra-key (eval/macros.h); it was exposed at
// namespace scope for this call precisely so a third copy would not appear.
ojson buildEScalars(const ojson& est) {
  ojson out = ojson::object();
  if (!est.is_object()) return out;
  for (const auto& [stataName, engineKey] : eval::eScalarKey()) {
    if (out.contains(stataName)) continue;
    if (!est.contains(engineKey)) continue;
    const ojson& v = est.at(engineKey);
    if (!v.is_number()) continue;
    const double d = v.get<double>();
    if (std::isnan(d)) continue;
    out[stataName] = d;
  }
  // `r2` is the one name whose snapshot spelling can differ from the block's:
  // makeSnapshot prefers r2_within when present, and a table asking for `r2`
  // must get the number the snapshot itself reports.
  return out;
}

// A stored estimate plus everything a renderer needs from it.
struct Model {
  std::string name;
  ojson snap;       // the eststo snapshot
  ojson added;      // {scalars, locals, matrices}
  ojson escalars;   // Stata-spelled e() scalars
};

// The snapshot's own `escalars`/`added` -- what `runEststo` attaches -- WIN.
// The fallback below is a backstop for an estimate that reached the store by
// some other route (a snapshot from before this bundle, or one built from
// LastModel's cached result), and it fires only when the shared fingerprint
// agrees. A mismatch discards rather than bleeds: attaching another model's
// additions is exactly the failure the fingerprint exists to stop.
Model materializeModel(SessionState& session, const std::string& name, const ojson& snap) {
  Model m;
  m.name = name;
  m.snap = snap;
  m.added = emptyBag();
  m.escalars = ojson::object();

  if (snap.contains("added") && snap["added"].is_object()) {
    m.added = snap["added"];
  } else if (!session.estAddedFp.empty() &&
             session.estAddedFp == SessionState::estAddFingerprint(snap)) {
    m.added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!m.added.contains(k) || !m.added[k].is_object()) m.added[k] = ojson::object();
  }

  if (snap.contains("escalars") && snap["escalars"].is_object()) {
    m.escalars = snap["escalars"];
  } else if (session.lastEstBlock.has_value() &&
             SessionState::estAddFingerprint(*session.lastEstBlock) ==
                 SessionState::estAddFingerprint(snap)) {
    m.escalars = buildEScalars(*session.lastEstBlock);
  }
  // N and r2 live on the snapshot itself, so they are available even when the
  // live block has moved on. They never OVERWRITE a frozen escalars entry.
  if (!m.escalars.contains("N") && snap.contains("n_obs") && snap["n_obs"].is_number()) {
    m.escalars["N"] = snap["n_obs"].get<double>();
  }
  if (!m.escalars.contains("r2") && snap.contains("r2") && snap["r2"].is_number()) {
    m.escalars["r2"] = snap["r2"].get<double>();
  }
  return m;
}

// Resolve a namelist (possibly empty -> every stored estimate) into models.
// On failure `errOut` is a real error block and the return is empty.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut);

std::vector<Model> collectModels(SessionState& session, const std::string& cmd,
                                 const std::vector<std::string>& namesIn, ojson* errOut) {
  // W2 esttabnames (BUG-552): a written namelist goes through the ONE
  // est_expand port (SessionState::expandEstimatesNamelist): `A_*`, `A_?`,
  // `*`, `_all`, `.`, in STORE order; a token matching nothing is r(111)
  // "estimation result NAME not found" -- w2_smalls.log section G. The old
  // exact-name lookup answered `esttab A_*` with "model(s) not found" at rc 1.
  std::vector<std::string> names;
  if (!namesIn.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(namesIn);
    if (ex.rc != 0) {
      *errOut = tblErrorRc(ex.message, ex.rc);
      return {};
    }
    names = std::move(ex.names);
  } else {
    names = session.estimatesNames();
  }
  if (names.empty()) {
    *errOut = tblError(cmd + ": no stored estimates. Run a regression, then `eststo m1` first.");
    return {};
  }
  std::vector<Model> out;
  for (const auto& n : names) {
    if (n == ".") {
      // The active results, carrying no store name: read straight from e().
      Model m;
      if (!activeModel(session, cmd, &m, errOut)) return {};
      m.name = ".";
      out.push_back(std::move(m));
      continue;
    }
    const ojson* snap = session.estimatesGet(n);
    if (snap == nullptr) {
      // Unreachable after the expansion (it returns stored names or "."); kept
      // so a future caller can never index a null snapshot.
      *errOut = tblErrorRc("estimation result " + n + " not found", 111);
      return {};
    }
    out.push_back(materializeModel(session, n, *snap));
  }
  return out;
}

// The coefficient rows of one model, keyed by name.
const ojson* coefRow(const ojson& snap, const std::string& name) {
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return nullptr;
  for (const auto& r : snap["coeftable"]) {
    if (r.value("name", std::string()) == name) return &r;
  }
  return nullptr;
}

// Union of coefficient names in first-appearance order across models, then
// keep()/drop()/order() applied in Stata's own precedence: `order()` promotes,
// `keep()` restricts, `drop()` removes.
// The ONE coefficient-selection filter. There were two: `coefNames` for
// estout/outreg2 and a second copy inside `writeEsttabFile` for esttab, and the
// glob fix below reached only the first one -- `keep(*2*)` selected `x2` on
// estout and emptied the table on esttab in the same build. A command whose row
// selection is implemented twice is a command with two answers about what `*`
// means, which is the shadow-evaluator class this codebase keeps paying for.
void applyCoefSelection(std::vector<std::string>* names,
                        const std::optional<std::string>& keepOpt,
                        const std::optional<std::string>& dropOpt,
                        const std::optional<std::string>& orderOpt, std::string* errOut) {
  std::vector<std::string>& sel = *names;
  // An `eq:name` token matches the equation-qualified spelling as well as the
  // bare one, because this build's coeftable carries no equation for a
  // single-equation model and `keep(*:*eigh*)` still keeps `weight` there
  // (w1b_keepglob.log k7). So a token with a colon is tried against BOTH the
  // name and `:name`, which is what makes the corpus's `keep(3:0.MI*...)` reach
  // rows this engine names without an equation.
  auto tokenHits = [](const std::string& tok, const std::string& name) {
    if (piet::strutil::globMatch(tok, name)) return true;
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) return false;
    return piet::strutil::globMatch(tok.substr(colon + 1), name);
  };
  if (keepOpt.has_value()) {
    const std::vector<std::string> keep = splitWs(dequote(*keepOpt));
    for (const auto& k : keep) {
      bool any = false;
      for (const auto& n : sel) {
        if (tokenHits(k, n)) {
          any = true;
          break;
        }
      }
      if (!any && errOut != nullptr && errOut->empty()) {
        *errOut = "coefficient " + k + " not found";
      }
    }
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      for (const auto& k : keep) {
        if (tokenHits(k, n)) {
          filtered.push_back(n);
          break;
        }
      }
    }
    sel = filtered;
  }
  if (dropOpt.has_value()) {
    const std::vector<std::string> drop = splitWs(dequote(*dropOpt));
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      bool hit = false;
      for (const auto& d : drop) {
        if (tokenHits(d, n)) {
          hit = true;
          break;
        }
      }
      if (!hit) filtered.push_back(n);
    }
    sel = filtered;
  }
  if (orderOpt.has_value()) {
    const std::vector<std::string> order = splitWs(dequote(*orderOpt));
    std::vector<std::string> promoted;
    for (const auto& o : order) {
      for (const auto& n : sel) {
        if (!tokenHits(o, n)) continue;
        if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
      }
    }
    for (const auto& n : sel) {
      if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
    }
    sel = promoted;
  }
}

// BUG-496: every one of `keep()`/`drop()`/`order()`'s tokens is a PATTERN, not a
// name. `keep(*romer*)` is how all four of paper 015's transform lines select
// their coefficients and `keep(3:0.MI*#c.romer*)` how another does, and exact
// matching answered them with an EMPTY body at rc 0. `keep(mp?)` is `mpg` too
// (w1b_keepglob.log ka), so it is Stata's `match()`, not a prefix rule. `errOut`
// carries the other half: a keep() token that matches nothing is r(111)
// "coefficient zz* not found" (kg_glob_none), not a table with no rows.
std::vector<std::string> coefNames(const std::vector<Model>& models,
                                   const std::optional<std::string>& keepOpt,
                                   const std::optional<std::string>& dropOpt,
                                   const std::optional<std::string>& orderOpt,
                                   std::string* errOut = nullptr) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const auto& m : models) {
    if (!m.snap.contains("coeftable") || !m.snap["coeftable"].is_array()) continue;
    for (const auto& r : m.snap["coeftable"]) {
      const std::string n = r.value("name", std::string());
      if (seen.insert(n).second) names.push_back(n);
    }
  }
  applyCoefSelection(&names, keepOpt, dropOpt, orderOpt, errOut);
  return names;
}

// `varlabels(x "Income" y "Age")` / `coeflabels(...)` -- pairs. Plus `label`,
// which asks for the DATASET's variable labels instead.
std::map<std::string, std::string> parseLabelPairs(const std::string& spec) {
  std::map<std::string, std::string> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec));
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out[toks[i]] = dequote(toks[i + 1]);
  }
  return out;
}

std::string labelFor(const std::string& name, const std::map<std::string, std::string>& explicitMap,
                     bool useVarLabels, SessionState& session) {
  auto it = explicitMap.find(name);
  if (it != explicitMap.end()) return it->second;
  if (useVarLabels) {
    const std::string lbl = session.eval.varLabels.get(name);
    if (!lbl.empty()) return lbl;
  }
  return name;
}

// BUG-486 removed this file's last caller of a `statLabel` auto-label map --
// estout labels a footer row with the key AS TYPED (`r2`, not `R-squared`;
// w1b_estout_defaults.log g3), and a pretty label comes from
// `stats(..., labels(...))`, which is the user's text and needs no map. The map
// is deleted rather than parked: a dead table of guesses is how a later reader
// concludes the guesses were measured. esttab's footer is built by its own
// writer and its labels were byte-verified by the 2026-09-09 capture.

// One footer statistic for one model: e() scalars first, then estadd's scalars,
// then estadd's locals (a TEXT row -- "Controls Yes"). Absent = blank cell, not
// zero: a missing statistic is not a statistic equal to nothing.
std::string statCell(const Model& m, const std::string& key, const std::string& fmt,
                     bool* isTextOut) {
  *isTextOut = false;
  // `ar2` is esttab's spelling of e(r2_a); both reach the same frozen scalar.
  std::vector<std::string> tryKeys{key};
  if (key == "ar2") tryKeys.push_back("r2_a");
  if (key == "r2_a") tryKeys.push_back("ar2");
  // W1a-5d: the pseudo-R2 arrives under TWO names depending on the estimator,
  // measured on this engine: `poisson` publishes `r2_p` (.0126126) while
  // `logit` and `probit` publish `r2_pseudo` (.0321488 / .0345996) and each
  // leaves the other missing; `nbreg` publishes neither. So a reader that
  // knows only one key answers `.` for two of the three families esttab's
  // `pr2` is actually used on -- and both live corpus uses are probit.
  if (key == "pr2" || key == "r2_p" || key == "r2_pseudo") {
    for (const char* alt : {"r2_p", "r2_pseudo"}) {
      if (std::find(tryKeys.begin(), tryKeys.end(), alt) == tryKeys.end()) tryKeys.push_back(alt);
    }
  }
  for (const auto& k : tryKeys) {
    if (m.escalars.contains(k) && m.escalars[k].is_number()) {
      // BUG-486: `N` is formatted by the CALLER's format like every other
      // statistic. estout renders `74` under %9.0g and `74.000` under the
      // fmt(3) its cells left running (w1b_estout_defaults.log g3 vs g4), so a
      // hard `fmtCount` here made one of those two impossible. esttab's writer
      // (the other caller) keeps its measured behaviour by passing `%9.0f` for
      // this key explicitly, which renders an integral count identically.
      return fmtNum(m.escalars[k].get<double>(), fmt);
    }
  }
  if (m.added["scalars"].contains(key)) {
    const ojson& v = m.added["scalars"][key];
    if (v.is_number()) return fmtNum(v.get<double>(), fmt);
  }
  if (m.added["locals"].contains(key)) {
    const ojson& v = m.added["locals"][key];
    *isTextOut = true;
    return v.is_string() ? v.get<std::string>() : std::string();
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// `transform()`. Declared in piet/handlers.h; see there for the grammar and for
// why it is shared rather than per-writer.
// ---------------------------------------------------------------------------
namespace {

// Stata's `gettoken` with no `bind` splits on WHITESPACE only, so `exp(@)` and
// `@/(-_b[weight])` are each one token and `pattern(1 1 0 0)` is FOUR. That last
// one is not a detail: it is why paper 083's `transform(@/M 1/M pattern(1 1 0 0)
// @*100 100 pattern(0 0 1 1))` -- no comma, so pattern() cannot be a suboption --
// reads `pattern(1` as a coefficient pattern and silently transforms nothing in
// Stata (w1b_transform.log tr_083_verb, rc 0 and byte-identical to the control).
std::vector<std::string> trTokens(const std::string& s) { return splitWs(s); }

// Split at the FIRST top-level comma: `transform(<list> [, pattern(...)])`.
void trSplitSubopts(const std::string& raw, std::string* list, std::string* subs) {
  int depth = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') --depth;
    else if (c == ',' && depth == 0) {
      *list = raw.substr(0, i);
      *subs = raw.substr(i + 1);
      return;
    }
  }
  *list = raw;
  subs->clear();
}

// estout.ado:3486 `MatchCoef`. A token with no colon matches the NAME; one with
// a colon matches equation and name separately, and an empty half is `*`. This
// engine's coeftable carries no equation for a single-equation model, and
// `transform(*:*mpg* @*2 2)` still transforms `mpg` there (w1b_transform.log
// r9) -- `globMatch("*", "")` is true, so that falls out rather than needing a
// case.
bool trMatch(const std::string& pat, const std::string& eq, const std::string& name) {
  const std::string p = strutil::trim(pat);
  if (p.empty() || p == "*") return true;
  const size_t colon = p.find(':');
  std::string eqPat, xPat;
  if (colon == std::string::npos) {
    xPat = p;
  } else {
    eqPat = strutil::trim(p.substr(0, colon));
    xPat = strutil::trim(p.substr(colon + 1));
  }
  if (eqPat.empty()) eqPat = "*";
  if (xPat.empty()) xPat = "*";
  return strutil::globMatch(eqPat, eq) && strutil::globMatch(xPat, name);
}

// `@` stands for a MATRIX ELEMENT in the ado (`subinstr "@" "\`b'"`, where `b` is
// `bc[i,j]`), which is atomic -- so the substituted literal is PARENTHESISED.
// Without that, `@^2` on a negative coefficient would parse as -(x^2) here and
// as (+x)^2 there, and the sign of every squared transform would differ.
std::string trSubstAt(const std::string& expr, double at) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "(%.17g)", at);
  std::string out;
  for (char c : expr) {
    if (c == '@') out += buf;
    else out += c;
  }
  return out;
}

// Make ONE model's estimates current for the duration of an expression, because
// `_b[]` in a transform resolves per model (y1). Save/restore rather than a
// second `_b[]` resolver: there are already two of those (macros.cpp and
// display_hardening.cpp) and a third keyed to a different context is how they
// come to disagree.
struct TrScopedModel {
  piet::eval::EvalSession& s;
  std::optional<nlohmann::ordered_json> saved;
  TrScopedModel(piet::eval::EvalSession& s_, const nlohmann::ordered_json& snap)
      : s(s_), saved(s_.lastModelResult) {
    s.lastModelResult = snap;
  }
  ~TrScopedModel() { s.lastModelResult = saved; }
  TrScopedModel(const TrScopedModel&) = delete;
  TrScopedModel& operator=(const TrScopedModel&) = delete;
};

}  // namespace

bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err) {
  std::string list, subs;
  trSplitSubopts(dequote(raw), &list, &subs);
  if (!strutil::trim(subs).empty()) {
    // `splitCall` is declared further down this file, so the one suboption is
    // parsed here rather than reaching forward for it.
    const std::string sub = strutil::trim(subs);
    const size_t open = sub.find('(');
    const size_t close = sub.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        strutil::lower(strutil::trim(sub.substr(0, open))) != "pattern") {
      *err = "transform(): the only suboption is pattern(); `" + sub + "` names something else";
      return false;
    }
    const std::string inner = sub.substr(open + 1, close - open - 1);
    for (const auto& w : splitWs(inner)) {
      if (w != "0" && w != "1") {
        // estout.ado:458-460 says so in as many words, and it is r(198)
        // (w1b_transform2.log tr_pat_two).
        *err = "invalid pattern in transform(,pattern()) or eform()";
        return false;
      }
      out->pattern.push_back(w);
    }
  }
  // The ado's own loop, which needs no special case for one or two tokens: with
  // two, the first iteration already has an empty `df` and an empty rest, so
  // (coef, f) slides to (f, df) and the pattern becomes empty.
  const std::vector<std::string> toks = trTokens(list);
  size_t i = 0;
  while (i < toks.size()) {
    std::string coef = toks[i];
    std::string f = (i + 1 < toks.size()) ? toks[i + 1] : std::string();
    std::string df = (i + 2 < toks.size()) ? toks[i + 2] : std::string();
    const bool restEmpty = (i + 3 >= toks.size());
    if (df.empty() && restEmpty) {
      df = f;
      f = coef;
      coef.clear();
    }
    TransformGroup g;
    g.pat = coef;
    g.f = f;
    g.df = df;
    out->groups.push_back(g);
    i += 3;
  }
  return true;
}

TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session) {
  TransformCell out;
  if (spec.empty() || !spec.appliesToModel(modelIdx) || std::isnan(b)) return out;
  std::string eq;
  if (const ojson* row = coefRow(snap, coefName)) eq = row->value("eq", std::string());
  for (const auto& g : spec.groups) {
    if (!trMatch(g.pat, eq, coefName)) continue;
    out.matched = true;
    if (g.f.empty()) break;
    TrScopedModel guard(session.eval, snap);
    // `@` is the coefficient for BOTH halves: the se's multiplier is the
    // derivative evaluated at b, not at the se (estout.ado:3455 reads
    // `bc[i,1]`), which is why `eform` spells itself `exp(@) exp(@)`.
    const std::optional<double> nb = piet::evalScalarExpr(trSubstAt(g.f, b), session);
    if (!nb.has_value() || !std::isfinite(*nb)) {
      out.bMissing = true;  // exp(1946) is `.` in the oracle too (u1's _cons)
    } else {
      out.b = *nb;
    }
    if (!g.df.empty()) {
      const std::optional<double> d = piet::evalScalarExpr(trSubstAt(g.df, b), session);
      if (d.has_value() && std::isfinite(*d)) out.seFactor = std::fabs(*d);
      else out.seFactor = std::numeric_limits<double>::quiet_NaN();
    }
    break;
  }
  return out;
}

namespace {

// ===========================================================================
// estout's cells() sub-grammar (plan design decision 4)
//
//   cells(ITEM ...)  /  cells("ITEM ...")
//   ITEM := name | name(sub ...)
//   name := b | se | t | p | <an estadd matrix name, for estpost tables>
//   sub  := star | par | par(L R) | fmt(#|%fmt) | label(text)
//
// Each ITEM is its own ROW (stacked), which is estout's default layout. The
// forms deliberately NOT accepted, each refused BY NAME rather than ignored:
// `&` (side-by-side), `[ ]` (grouping), `pvalue()`, and equation notation --
// every one of them changes the SHAPE of the table, so accepting and ignoring
// would silently produce a different table than the one asked for.
// ===========================================================================

struct CellSpec {
  std::string name;
  bool star = false;
  bool par = false;
  std::string parL = "(";
  std::string parR = ")";
  std::string fmt = "%9.3f";
  bool fmtGiven = false;
  std::string label;
  // outreg2's DEFAULT number shape: significant digits, floor 0 decimals, with
  // thousands separators (outreg2.ado:5509-5515 `dec` defaults to 3 and
  // :5525-5531 `fmt` defaults to `fc`; :5541-5543 turns the adaptive path off
  // the moment `dec()`/`bdec()` is given). `autoD < 0` means "use fmt".
  int autoD = -1;
};

// Split `name(inner)` into its two halves. Returns false when the parentheses
// do not balance, which is a syntax error rather than a name.
bool splitCall(const std::string& item, std::string* head, std::string* inner) {
  const size_t open = item.find('(');
  if (open == std::string::npos) {
    *head = item;
    inner->clear();
    return true;
  }
  if (item.empty() || item.back() != ')') return false;
  *head = item.substr(0, open);
  *inner = item.substr(open + 1, item.size() - open - 2);
  return true;
}

bool parseCells(const std::string& raw, std::vector<CellSpec>* out, std::string* errOut,
                bool* sideBySideOut = nullptr) {
  const std::string trimmed = strutil::trim(raw);
  const std::string spec = dequote(trimmed);
  // The tokenizer hands the option's argument over VERBATIM (tokenizer.cpp
  // parseOpts takes the text between the parentheses and only strips
  // whitespace), so the quotes are still here to be read -- and dequote, one
  // line down, is what used to erase them before anything could.
  if (sideBySideOut != nullptr) *sideBySideOut = (spec.size() != trimmed.size());
  if (spec.find('&') != std::string::npos) {
    *errOut =
        "estout: cells() side-by-side notation `&` is not implemented in this build; put each "
        "statistic on its own row instead";
    return false;
  }
  // The grouping notation is a bracket at the TOP LEVEL -- `cells([b se])`.
  // A bracket INSIDE a suboption is something else entirely: `par([ ])` names
  // the two delimiters, and that is what the corpus writes. Measured on the 90
  // packages: `par([` appears 327 times across 20 distinct spellings, and a
  // top-level `cells([` appears ZERO times. The depth-blind test this replaces
  // refused all 327 to guard against none of the 0, and the arm that caught it
  // is one of those lines verbatim (w1b_tables_pins.log c7,
  // `cells(se(par([ ]) fmt(2)))`, rc 0 in Stata).
  {
    int depth = 0;
    for (char ch : spec) {
      if (ch == '(') ++depth;
      else if (ch == ')') { if (depth > 0) --depth; }
      else if ((ch == '[' || ch == ']') && depth == 0) {
        *errOut = "estout: cells() grouping notation `[ ]` is not implemented in this build";
        return false;
      }
    }
  }
  for (const auto& item : splitTopLevel(spec)) {
    std::string head, inner;
    if (!splitCall(item, &head, &inner)) {
      *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
      return false;
    }
    if (head.find(':') != std::string::npos) {
      *errOut = "estout: cells() equation notation `" + head + "` is not implemented in this build";
      return false;
    }
    if (head == "pvalue") {
      *errOut = "estout: cells(pvalue()) is not implemented in this build; use `p`";
      return false;
    }
    CellSpec cs;
    cs.name = head;
    for (const auto& sub : splitTopLevel(inner)) {
      std::string sh, si;
      if (!splitCall(sub, &sh, &si)) {
        *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
        return false;
      }
      if (sh == "star") {
        cs.star = true;
      } else if (sh == "par") {
        cs.par = true;
        std::vector<std::string> lr = splitTopLevel(si);
        if (lr.size() == 1) {
          // `par([ ])` -- the two delimiters are LITERAL TEXT, and `[`/`]` are
          // not nesting here, but splitTopLevel counts them as depth and hands
          // the pair back as one token. Re-split on plain whitespace when that
          // happens, and only accept it when it yields exactly two. This is the
          // corpus's own spelling: `par([` appears 327 times across 20 distinct
          // lines, against zero uses of the top-level grouping the depth rule
          // exists for.
          const std::vector<std::string> ws = splitWs(si);
          if (ws.size() == 2) lr = ws;
        }
        if (lr.size() == 2) {
          cs.parL = dequote(lr[0]);
          cs.parR = dequote(lr[1]);
        } else if (!si.empty() && lr.size() != 0) {
          *errOut = "estout: par() takes either no argument or exactly two delimiters";
          return false;
        }
      } else if (sh == "fmt") {
        bool ok = false;
        cs.fmt = normalizeFmt(si, &ok);
        if (!ok) {
          // `fmt(a3)` is Stata's adaptive-width format. Rendering it as three
          // fixed decimals would be a different number of digits than asked
          // for, so it is refused rather than approximated (decision 4).
          *errOut = "estout: fmt(" + si + ") is not implemented in this build; use fmt(#) or a "
                                          "%fmt such as fmt(%9.3f)";
          return false;
        }
        cs.fmtGiven = true;
      } else if (sh == "label") {
        cs.label = dequote(si);
      } else if (!sh.empty()) {
        *errOut = "estout: cells() sub-option `" + sh + "` is not implemented in this build";
        return false;
      }
    }
    out->push_back(cs);
  }
  if (out->empty()) {
    *errOut = "estout: cells() is empty";
    return false;
  }
  return true;
}

// ===========================================================================
// The shared coefficient-body builder
// ===========================================================================

struct BodyOpts {
  std::vector<CellSpec> cells;
  // BUG-487: estout's cells() QUOTE is an operator. `cells(a b)` stacks the
  // items as ROWS; `cells("a b")` puts them SIDE BY SIDE -- k*n columns, one
  // body row per coefficient, ordered model-major (m1.b m1.se m2.b m2.se;
  // tests/eval/golden/aer_w1/w1b_estout_cells.log h2). With ONE item the two
  // shapes coincide, which the capture also pins (h5 == h6).
  bool sideBySide = false;
  std::vector<StarLevel> stars;
  std::optional<std::string> keep, drop, order;
  std::map<std::string, std::string> labels;
  bool useVarLabels = false;
  bool nocons = false;
  // o_plain.txt puts the intercept LAST and calls it `Constant` whether or not
  // `label` was given. Both default OFF so `estout`'s frozen fixture keeps its
  // bytes; only outreg2 turns them on.
  bool consLast = false;
  std::string consLabel;
};

// One body row per (coefficient x cell-spec). A model that does not estimate a
// given coefficient gets a BLANK cell, never a zero -- a mixed-estimator table
// is the plan's own edge case and a zero there is a claim of no effect.
void buildBody(Table* t, const std::vector<Model>& models, const BodyOpts& bo,
               SessionState& session, std::string* errOut = nullptr) {
  std::vector<std::string> names = coefNames(models, bo.keep, bo.drop, bo.order, errOut);
  if (bo.consLast) {
    std::vector<std::string> reordered;
    bool sawCons = false;
    for (const auto& n : names) {
      if (n == "_cons") {
        sawCons = true;
      } else {
        reordered.push_back(n);
      }
    }
    if (sawCons) reordered.push_back("_cons");
    names = reordered;
  }
  if (bo.nocons) {
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) {
                                 return n == "_cons" || n == "(intercept)";
                               }),
                names.end());
  }
  // One (model, coefficient, spec) cell. Shared by both layouts so the two can
  // never disagree about a NUMBER -- only about where it sits.
  auto cellFor = [&](const Model& m, const std::string& coef, const CellSpec& cs,
                     const ojson* r) -> std::optional<std::string> {
    if (r == nullptr) return std::nullopt;
    ojson v;
    if (cs.name == "b" || cs.name == "coef") {
      v = r->contains("b") ? (*r)["b"] : ojson(nullptr);
    } else if (cs.name == "se" || cs.name == "stderr") {
      v = r->contains("se") ? (*r)["se"] : ojson(nullptr);
    } else if (cs.name == "t") {
      v = r->contains("t") ? (*r)["t"] : ojson(nullptr);
    } else if (cs.name == "p") {
      v = r->contains("p") ? (*r)["p"] : ojson(nullptr);
    } else if (m.added["matrices"].contains(cs.name)) {
      const ojson& mat = m.added["matrices"][cs.name];
      if (mat.contains("colnames") && mat.contains("data") && mat["data"].is_array() &&
          !mat["data"].empty()) {
        const ojson& cn = mat["colnames"];
        for (size_t j = 0; j < cn.size(); ++j) {
          if (cn[j].is_string() && cn[j].get<std::string>() == coef) {
            if (j < mat["data"][0].size()) v = mat["data"][0][j];
            break;
          }
        }
      }
    }
    if (v.is_null()) return std::nullopt;
    std::string cell;
    if (cs.autoD >= 0) {
      cell = (v.is_number() && !std::isnan(v.get<double>()))
                 ? sigDigits(v.get<double>(), cs.autoD, /*minDec=*/0, /*comma=*/true)
                 : std::string(kMissingCell);
    } else {
      cell = fmtJson(v, cs.fmt);
    }
    if (cs.star && r->contains("p")) cell += starsFor((*r)["p"], bo.stars);
    if (cs.par) cell = cs.parL + cell + cs.parR;
    return cell;
  };

  // BUG-487, the WIDE layout: one row per coefficient, k*n cells, model-major.
  if (bo.sideBySide) {
    for (const auto& coef : names) {
      TRow row;
      row.kind = TRow::kCoef;
      row.label = labelFor(coef, bo.labels, bo.useVarLabels, session);
      if (coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      bool anything = false;
      for (const auto& m : models) {
        const ojson* r = coefRow(m.snap, coef);
        for (const auto& cs : bo.cells) {
          auto c = cellFor(m, coef, cs, r);
          row.cells.push_back(c.value_or(std::string()));
          if (c.has_value() && !c->empty()) anything = true;
        }
      }
      if (anything) t->body.push_back(row);
    }
    return;
  }

  for (const auto& coef : names) {
    for (size_t ci = 0; ci < bo.cells.size(); ++ci) {
      const CellSpec& cs = bo.cells[ci];
      TRow row;
      row.kind = (ci == 0) ? TRow::kCoef : TRow::kSe;
      // Only the FIRST cell spec is labelled: `x / (0.045)` is one coefficient
      // shown two ways, not two coefficients.
      row.label = (ci == 0) ? labelFor(coef, bo.labels, bo.useVarLabels, session)
                            : (cs.label.empty() ? std::string() : cs.label);
      if (ci == 0 && coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      for (const auto& m : models) {
        auto c = cellFor(m, coef, cs, coefRow(m.snap, coef));
        row.cells.push_back(c.value_or(std::string()));
      }
      // A row where every model is blank carries nothing.
      bool anything = false;
      for (const auto& c : row.cells) {
        if (!c.empty()) anything = true;
      }
      if (anything) t->body.push_back(row);
    }
  }
}

// The footer: `stats(N r2 ...)` in the order given. A key that is an estadd
// LOCAL renders as text ("Controls Yes"), which is decision 2's whole point.
void buildFoot(Table* t, const std::vector<Model>& models, const std::vector<std::string>& stats,
               const std::string& fmt, size_t stride = 1) {
  for (const auto& key : stats) {
    TRow row;
    row.kind = TRow::kStat;
    // BUG-486, axis 7. estout labels a stats() row with the KEY AS TYPED --
    // `r2`, not `R-squared` (w1b_estout_defaults.log g3: `r2 .2933891`). The
    // pretty label belongs to `stats(..., labels(...))`, which the caller has
    // not given. `statLabel` stays for that path and for esttab, which builds
    // its own footer and whose 09-09 capture verified those labels.
    row.label = key;
    bool anyText = false;
    for (const auto& m : models) {
      bool isText = false;
      // ...and `N` follows the SAME running format as everything else: `74`
      // under %9.0g (g3), `74.000` under fmt(3) (g4). The `%9.0f` special case
      // here was a guess and the capture refutes it.
      row.cells.push_back(statCell(m, key, fmt, &isText));
      if (isText) anyText = true;
      // BUG-487: under the wide layout a model owns `stride` columns and the
      // statistic sits in the FIRST of them, the rest blank -- `N  74    74  `
      // against a four-column body (w1b_estout_cells.log h7).
      for (size_t pad = 1; pad < stride; ++pad) row.cells.emplace_back();
    }
    if (anyText) row.kind = TRow::kText;
    t->foot.push_back(row);
  }
}

// Column heads: `mlabels()`/`mtitles()` if given, else the model's depvar (what
// esttab prints), else the stored name.
// `nameHeads` picks which side of the model a derived head comes from. estout
// heads its columns with the STORED NAME (`m1`, `m2` -- e2.txt/e_plain.txt row
// 1); esttab heads them with the dependent variable. This build derived the
// depvar for both, which is BUG-486's second axis.
void buildHeads(Table* t, const std::vector<Model>& models,
                const std::optional<std::string>& mlabels, bool numbers, bool heads,
                bool nameHeads = false) {
  if (numbers) {
    for (size_t i = 0; i < models.size(); ++i) {
      t->colNums.push_back("(" + std::to_string(i + 1) + ")");
    }
  }
  if (!heads) return;
  std::vector<std::string> labels;
  if (mlabels.has_value()) {
    // SPLIT FIRST, DEQUOTE EACH TOKEN -- the rule `parseLabelPairs` states and the
    // fourth site in this tree to have broken it. `mlabels("Model One" "Two")`
    // dequoted FIRST becomes `Model One" "Two`, whose remaining quotes make the
    // splitter's quote tracking swallow the separator, so the two labels came out
    // as `Model` and `One" "Two` (w1b_estout_tex.log r8).
    labels = splitTopLevel(*mlabels);
    for (auto& l : labels) l = dequote(l);
  }
  for (size_t i = 0; i < models.size(); ++i) {
    if (i < labels.size()) {
      t->colHeads.push_back(labels[i]);
      continue;
    }
    if (nameHeads) {
      t->colHeads.push_back(models[i].name);
      continue;
    }
    const std::string dv = models[i].snap.value("depvar", std::string());
    t->colHeads.push_back(dv.empty() ? models[i].name : dv);
  }
}

}  // namespace

namespace {

// ===========================================================================
// Raw-argument recovery
//
// `parsed.varlist` is WHITESPACE-split (tokenizer.cpp:685), so
// `estadd local controls "Yes, with FE"` arrives as four tokens with the quotes
// broken across two of them. Every place below that needs a user STRING reads
// it out of `parsed.raw` instead. The comma cut uses the tokenizer's own
// quote-aware scanner rather than a plain find(','), because a comma inside the
// text is not an option separator.
// ===========================================================================

std::string argsAfterHead(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return {};
  const long long comma = eval::findUnquotedComma(raw);
  std::string body = (comma == -1) ? raw : raw.substr(0, static_cast<size_t>(comma));
  if (e >= body.size()) return {};
  return strutil::trim(body.substr(e));
}

// First `:` outside quotes and parens -- estadd's `... : m1 m2` namelist split.
long long findUnquotedColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i < quoted.size() && quoted[i]) continue;
    if (s[i] == '(' || s[i] == '[') ++depth;
    else if (s[i] == ')' || s[i] == ']') --depth;
    else if (s[i] == ':' && depth == 0) return static_cast<long long>(i);
  }
  return -1;
}

// Peel the leading word off a string, returning it and advancing `rest`.
std::string takeWord(std::string* rest) {
  const std::string s = strutil::trim(*rest);
  const size_t sp = s.find_first_of(" \t");
  if (sp == std::string::npos) {
    *rest = "";
    return s;
  }
  *rest = strutil::trim(s.substr(sp));
  return s.substr(0, sp);
}

// ===========================================================================
// Fit-sample column values (estadd ysumm)
//
// The estimation sample is not the loaded dataset: `regress y x if region<=4`
// fits on a subset, and Stata's `estadd ysumm` summarises e(sample), not the
// frame. LastModel records the two shapes this codebase uses for that -- a
// materialized scratch file (`fitDataPath`) and/or an engine-side `--keepif`
// expression -- and this reproduces the same reconstruction handlers_resample.
// cpp:480-500 does, rather than reading the head and being quietly wrong by a
// few hundred observations (BUG-163's measured failure was 3,106 vs 4,649).
// ===========================================================================

bool fitSampleValues(SessionState& session, const std::string& var,
                     std::vector<double>* out, std::string* errOut) {
  if (!session.currentHbin().has_value()) {
    *errOut = "no dataset in memory";
    return false;
  }
  std::string basePath = *session.currentHbin();
  std::string keepif;
  if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    keepif = lm.fitKeepif;
  }
  if (!keepif.empty()) {
    const std::string filtered = mintScratchPath(session.workdir(), "estadd_ysumm");
    const CobraRunResult cr =
        runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
    if (!cr.ok || !cecon::native_exists(filtered)) {
      *errOut = "could not reconstruct the estimation sample";
      return false;
    }
    basePath = filtered;
  }
  try {
    Datastore ds(basePath);
    if (!ds.has(var)) {
      *errOut = "variable " + var + " not found";
      return false;
    }
    const cecon::Column& c = ds.col(var);
    if (c.dtype == cecon::DType::Factor) {
      *errOut = var + " is a string variable";
      return false;
    }
    const double* d = c.f64();
    const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
    out->clear();
    out->reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      if (!std::isnan(d[i])) out->push_back(d[i]);
    }
  } catch (const std::exception& e) {
    *errOut = e.what();
    return false;
  }
  return true;
}

}  // namespace

// ===========================================================================
// estadd
// ===========================================================================

// ---------------------------------------------------------------------------
// BUG-408 / BUG-409 / BUG-421 — the ONE root cause behind all three.
//
// estadd and estpost published every addition into the TABLE bag
// (`session.estAdded` / the stored estimate's `added` object), which is what
// esttab/estout read — and into nothing else. The three surfaces that answer
// `e(NAME)` for a USER read a different store each:
//
//   `matrix M = e(mean)` / `matlist e(mean)`  -> session.eval.eMatrices
//   `display e(k)`                            -> display_hardening's resolver
//   `` `e(controls)' ``                       -> the macro layer's e() stores
//
// so estpost posted seven matrices nothing could copy (rc 111), estadd posted a
// scalar `display e(k)` answered `.` for, and `estadd local` posted a macro the
// macro layer could not see at all. The table was right and every other reader
// was blind: the two-layer divergence class this file's own header warns about.
//
// The fix is to publish into BOTH: the bag stays exactly as it was (every
// renderer, every frozen table fixture unchanged) and the session e() stores
// gain the same names. `pca` already does this — it is the shape a poster is
// supposed to have.
void mirrorIntoEStores(SessionState& session,
                       const std::vector<std::pair<std::string, double>>& scalars,
                       const std::vector<std::pair<std::string, std::string>>& locals,
                       const std::vector<std::pair<std::string, ojson>>& matrices) {
  for (const auto& [n, v] : scalars) session.eval.setEScalar(n, v);
  for (const auto& [n, v] : locals) session.eval.setEString(n, v);
  for (const auto& [n, m] : matrices) {
    eval::EvalMatrix em;
    if (m.contains("rownames") && m["rownames"].is_array())
      em.rownames = m["rownames"].get<std::vector<std::string>>();
    if (m.contains("colnames") && m["colnames"].is_array())
      em.colnames = m["colnames"].get<std::vector<std::string>>();
    if (m.contains("data") && m["data"].is_array()) {
      for (const auto& row : m["data"]) {
        std::vector<double> r;
        if (row.is_array()) {
          for (const auto& cell : row) {
            // jsonNum's rule, transcribed rather than called: that helper is
            // defined further down this file, and a forward declaration for two
            // lines would be the wider change.
            r.push_back(cell.is_number() ? cell.get<double>()
                                         : std::numeric_limits<double>::quiet_NaN());
          }
        }
        em.data.push_back(std::move(r));
      }
    }
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == n) { kv.second = em; replaced = true; break; }
    }
    if (!replaced) session.eval.eMatrices.emplace_back(n, std::move(em));
  }
}

ojson runEstadd(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstadd.known,
                                            kVocabEstadd.unimplemented)) {
    return ojson(*err);
  }

  std::string args = argsAfterHead(parsed.raw);
  if (args.empty()) {
    // A BARE `estadd` has no sub-verb at all, which is a syntax error rather
    // than an unimplemented feature -- kUnknownSubcommand would name "" as the
    // subcommand.
    return tblSyntax("estadd: a subcommand is required (local, scalar, matrix or ysumm)");
  }

  // `estadd ... : m1 m2` targets ALREADY-STORED estimates rather than the
  // active e(). Split it off before anything else so the sub-verb parser never
  // sees the namelist.
  std::vector<std::string> targets;
  const long long colon = findUnquotedColon(args);
  if (colon >= 0) {
    targets = splitWs(args.substr(static_cast<size_t>(colon) + 1));
    args = strutil::trim(args.substr(0, static_cast<size_t>(colon)));
    if (targets.empty()) {
      return tblSyntax("estadd: `:' must be followed by one or more stored estimate names");
    }
  }

  const std::string sub = strutil::lower(takeWord(&args));

  // The sub-verbs that are real estadd features this build does not compute.
  // `unsupportedCommand(..., kNotImplemented)` -- NOT a bare engineError, whose
  // default code answers rc 1 instead of 199.
  static const std::set<std::string> kNotImplementedSubs = {"mean",  "beta", "vif",
                                                            "expb",  "summ", "sd",
                                                            "svy",   "margins"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (sub != "local" && sub != "scalar" && sub != "matrix" && sub != "ysumm" &&
      sub != "ysummarize") {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  if (!session.lastEstBlock.has_value() && targets.empty()) {
    // Stata answers rc 301 ("last estimates not found") here, which coarseRc's
    // message rules do not know, so it is stated through `reason`.
    return tblErrorRc("estadd: no estimation result in memory. Run a regression first.", 301);
  }

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::string fp =
      session.lastEstBlock.has_value()
          ? SessionState::estAddFingerprint(*session.lastEstBlock)
          : std::string();

  // ---- gather what this sub-verb wants to add -----------------------------
  // Collected into a staging bag first so a failure adds NOTHING: a half-applied
  // `estadd ysumm` would leave e() carrying four of its eight scalars with no
  // diagnostic about the other four.
  std::vector<std::pair<std::string, double>> addScalars;
  std::vector<std::pair<std::string, std::string>> addLocals;
  std::vector<std::pair<std::string, ojson>> addMatrices;

  if (sub == "local") {
    const std::string name = takeWord(&args);
    if (name.empty()) return tblSyntax("estadd local: a name is required");
    if (!strutil::isValidStataIdentifier(name)) {
      return tblError("estadd local: invalid name " + name, "engine_error");
    }
    addLocals.emplace_back(name, dequote(strutil::trim(args)));
  } else if (sub == "scalar") {
    // `estadd scalar k = e(F)` and `estadd scalar k=7` are the same statement.
    std::string rest = args;
    const size_t eq = rest.find('=');
    std::string name, expr;
    if (eq == std::string::npos) {
      name = takeWord(&rest);
      expr = strutil::trim(rest);
    } else {
      name = strutil::trim(rest.substr(0, eq));
      expr = strutil::trim(rest.substr(eq + 1));
    }
    if (name.empty() || expr.empty()) {
      return tblSyntax("estadd scalar: the syntax is `estadd scalar NAME = EXPRESSION'");
    }
    if (!strutil::isValidStataIdentifier(name)) {
      return tblError("estadd scalar: invalid name " + name, "engine_error");
    }
    // ONE e() reader for the whole bundle: `evalScalarExpr` is the chain
    // `display` already runs (whole-token resolution, then e()/_b[]
    // substitution, then arithmetic). A second reader here is how the two
    // e()-scalar tables in this tree started.
    const std::optional<double> v = piet::evalScalarExpr(expr, session);
    if (!v.has_value()) {
      return tblError("estadd scalar: could not evaluate `" + expr + "'");
    }
    addScalars.emplace_back(name, *v);
  } else if (sub == "matrix") {
    std::string rest = args;
    const size_t eq = rest.find('=');
    std::string name, src;
    if (eq == std::string::npos) {
      name = takeWord(&rest);
      src = strutil::trim(rest);
    } else {
      name = strutil::trim(rest.substr(0, eq));
      src = strutil::trim(rest.substr(eq + 1));
    }
    if (name.empty() || src.empty()) {
      return tblSyntax("estadd matrix: the syntax is `estadd matrix NAME = MATRIX'");
    }
    const eval::EvalMatrix* mat = session.eval.findMatrix(src);
    if (mat == nullptr) {
      // coarseRc maps "matrix ... not found" through its "not found" rule to
      // rc 111, which is Stata's own code for an unknown matrix name.
      return tblError("matrix " + src + " not found");
    }
    ojson m;
    m["rownames"] = mat->rownames;
    m["colnames"] = mat->colnames;
    ojson data = ojson::array();
    for (const auto& row : mat->data) data.push_back(row);
    m["data"] = data;
    addMatrices.emplace_back(name, m);
  } else {  // ysumm
    if (!session.lastModel().has_value() && !session.lastEstBlock.has_value()) {
      return tblErrorRc("estadd ysumm: no estimation result in memory", 301);
    }
    std::string depvar;
    if (session.lastEstBlock.has_value()) depvar = session.lastEstBlock->value("depvar", "");
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->depvar;
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->y;
    if (depvar.empty()) {
      return tblError("estadd ysumm: the last estimation does not name a dependent variable");
    }
    std::vector<double> vals;
    std::string ferr;
    if (!fitSampleValues(session, depvar, &vals, &ferr)) {
      return tblError("estadd ysumm: " + ferr);
    }
    const double n = static_cast<double>(vals.size());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    for (double v : vals) sum += v;
    const double mean = n > 0 ? sum / n : nan;
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (double v : vals) {
      const double d = v - mean;
      m2 += d * d;
      m3 += d * d * d;
      m4 += d * d * d * d;
    }
    // Stata's summarize reports the SAMPLE variance (n-1) but POPULATION
    // skewness/kurtosis (n divisor, kurtosis NOT excess -- a normal draw
    // reports ~3, not ~0). Mixing those two conventions is the classic silent
    // wrong number here, so both are spelled out.
    const double var = n > 1 ? m2 / (n - 1) : nan;
    const double sd = n > 1 ? std::sqrt(var) : nan;
    const double m2p = n > 0 ? m2 / n : nan;
    const double skew = (n > 0 && m2p > 0) ? (m3 / n) / std::pow(m2p, 1.5) : nan;
    const double kurt = (n > 0 && m2p > 0) ? (m4 / n) / (m2p * m2p) : nan;
    double mn = nan, mx = nan;
    if (!vals.empty()) {
      mn = *std::min_element(vals.begin(), vals.end());
      mx = *std::max_element(vals.begin(), vals.end());
    }
    addScalars.emplace_back("ymean", mean);
    addScalars.emplace_back("ysd", sd);
    addScalars.emplace_back("ymin", mn);
    addScalars.emplace_back("ymax", mx);
    addScalars.emplace_back("ysum", sum);
    addScalars.emplace_back("yvar", var);
    addScalars.emplace_back("yskew", skew);
    addScalars.emplace_back("ykurt", kurt);
  }

  // ---- decide WHERE it lands ----------------------------------------------
  // `: m1 m2` writes into each named STORED estimate's own bag (a mutable
  // handle, which is why SessionState grew `estimatesGetMutable`); everything
  // else writes the live bag beside the active e().
  std::vector<ojson*> bags;
  if (!targets.empty()) {
    // W2 esttabnames (BUG-552): `estadd ... : A_*` -- the target list is a
    // namelist too (estadd.ado expands it against the stored names). The
    // active results (`.`) are not a stored bag; refused by name below.
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(targets);
    if (ex.rc != 0) return tblErrorRc(ex.message, ex.rc);
    for (const auto& t : ex.names) {
      if (t == ".") {
        return tblError("estadd: `.` in the `: namelist` (the active, unstored results) is not "
                        "supported in this version of PIET; run `estadd` without the colon to "
                        "add to the active results");
      }
      ojson* snap = session.estimatesGetMutable(t);
      if (snap == nullptr) {
        return tblError("estadd: no stored estimation result named " + t);
      }
      if (!snap->contains("added") || !(*snap)["added"].is_object()) {
        (*snap)["added"] = emptyBag();
      }
      for (const char* k : {"scalars", "locals", "matrices"}) {
        if (!(*snap)["added"].contains(k)) (*snap)["added"][k] = ojson::object();
      }
      bags.push_back(&(*snap)["added"]);
    }
  } else {
    if (session.estAddedFp != fp) {
      // A DIFFERENT estimation has replaced e() since the bag was opened.
      // Discarding is the rule; keeping would attach one model's additions to
      // another model's table -- a plausible number on the right-looking row.
      session.estAddedClear();
    }
    // The bag shape is EXACTLY {scalars, locals, matrices}: `runEststo` copies
    // it verbatim into the snapshot and every renderer here reads those three
    // keys, so a fourth key or a missing one is a silently empty table cell.
    for (const char* k : {"scalars", "locals", "matrices"}) {
      if (!session.estAdded.contains(k) || !session.estAdded[k].is_object()) {
        session.estAdded[k] = ojson::object();
      }
    }
    bags.push_back(&session.estAdded);
  }

  // ---- collision check across EVERY target, before writing ANY of them -----
  if (!replace) {
    for (ojson* bag : bags) {
      for (const auto& [n, v] : addScalars) {
        (void)v;
        if ((*bag)["scalars"].contains(n)) {
          // "already defined" is one of coarseRc's own measured message rules
          // (-> rc 110, Stata's confirm-new-name code), so this wording is the
          // rc as much as it is the sentence.
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addLocals) {
        (void)v;
        if ((*bag)["locals"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addMatrices) {
        (void)v;
        if ((*bag)["matrices"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
    }
  }

  for (ojson* bag : bags) {
    for (const auto& [n, v] : addScalars) (*bag)["scalars"][n] = v;
    for (const auto& [n, v] : addLocals) (*bag)["locals"][n] = v;
    for (const auto& [n, v] : addMatrices) (*bag)["matrices"][n] = v;
  }
  // BUG-408/409/421: the live e() gains the same names, so `display e(k)`,
  // `` `e(controls)' `` and `matrix M = e(mean)` can read what the table reads.
  // Only for the LIVE bag: `estadd ... : m1 m2` writes stored estimates, whose
  // e() is not the active one.
  if (targets.empty()) mirrorIntoEStores(session, addScalars, addLocals, addMatrices);
  // MIRROR the scalars into e() itself, so `display e(k)` answers 7 rather than
  // `.`. Stata's estadd genuinely ADDS to e() -- the bag is this build's way of
  // carrying the addition to `eststo`, not a substitute for e() being readable,
  // and a table row the macro layer cannot see is the pangram's own stated test
  // (S28 reads `e(s28k)` back immediately after adding it).
  //
  // Only names the estimate does NOT already carry: `estadd scalar r2 = 5` must
  // not overwrite the fitted R-squared behind `display e(r2)`. The addition
  // still reaches `stats(r2)` through the bag, so nothing is lost by declining
  // to shadow an engine key.
  // The two stores are written TOGETHER on purpose: `eval.lastModelResult` is
  // what macros.cpp's e() resolver reads and `lastEstBlock` is what the table
  // path reads, and updating one without the other is the two-layer divergence
  // this codebase keeps paying for.
  if (targets.empty() && session.lastEstBlock.has_value() && !addScalars.empty()) {
    ojson blk = *session.lastEstBlock;
    for (const auto& [n, v] : addScalars) {
      if (!blk.contains(n)) blk[n] = v;
    }
    session.lastEstBlock = blk;
    if (session.eval.lastModelResult.has_value()) {
      ojson res = *session.eval.lastModelResult;
      for (const auto& [n, v] : addScalars) {
        if (!res.contains(n)) res[n] = v;
      }
      session.eval.lastModelResult = res;
    }
  }
  // Re-stamp the fingerprint from the SHARED helper, unconditionally. It is
  // `SessionState::estAddFingerprint` and not a private copy on purpose:
  // `runEststo` READS it to decide whether the bag still belongs to the active
  // estimate, and two copies differing by one field would make the bag never
  // match -- an `estadd`ed row quietly missing from the table, at rc 0. The
  // mirror above cannot move it: the fingerprint is built from depvar,
  // estimator, n_obs and the coeftable LENGTH, none of which a new scalar key
  // touches.
  if (targets.empty() && session.lastEstBlock.has_value()) {
    session.estAddedFp = SessionState::estAddFingerprint(*session.lastEstBlock);
  }

  // One block per addition, as an ARRAY -- legal here because the arm routes
  // through `pushEnvResult` (which flattens) AND every element is a real block
  // object with a "type" key. See defect class 1 in this file's header.
  ojson out = ojson::array();
  if (!hasFlag(parsed.opts, "quietly")) {
    for (const auto& [n, v] : addScalars) {
      out.push_back(displayBlock("added scalar: e(" + n + ") = " + fmtNum(v, "%9.7g")));
    }
    for (const auto& [n, v] : addLocals) {
      out.push_back(displayBlock("added macro:  e(" + n + ") : \"" + v + "\""));
    }
    for (const auto& [n, v] : addMatrices) {
      (void)v;
      out.push_back(displayBlock("added matrix: e(" + n + ")"));
    }
  }
  return out;
}

// ===========================================================================
// estpost -- the pseudo-estimate (plan design decision 3)
//
// Stata's estpost posts a summary table into e() so that `esttab, cells(mean)`
// can read it. The posted objects are 1xk MATRICES named after the statistic
// (e(mean), e(sd), ...), with the variables as column names -- which is exactly
// the shape `estadd matrix` already stores, so estpost reuses that bag rather
// than inventing a second one.
//
// The numbers come from the SAME cores the native commands use:
//   summarize -> handlers::summarizeCoreStats (handlers.cpp)
//   tabstat   -> the `cobra tabstat --tidy` argv, re-assembled here
// `runTabstatCore` (estimation_m4.cpp:967-1013) is in an anonymous namespace
// and is therefore NOT callable from this TU. Re-assembling its six-line argv
// is cheaper than exposing a symbol out of that hot file, and the SOURCE of the
// numbers -- the engine -- is identical either way, which is the property that
// matters. A native re-implementation of the statistics would not be.
// ===========================================================================

namespace {

// A 1xk matrix in the `added.matrices` shape.
ojson rowMatrix(const std::vector<std::string>& colnames, const std::vector<double>& values) {
  ojson m;
  m["rownames"] = ojson::array({"y1"});
  m["colnames"] = colnames;
  ojson data = ojson::array();
  ojson row = ojson::array();
  for (double v : values) {
    if (std::isnan(v)) {
      row.push_back(nullptr);
    } else {
      row.push_back(v);
    }
  }
  data.push_back(row);
  m["data"] = data;
  return m;
}

double jsonNum(const ojson& v) {
  if (v.is_number()) return v.get<double>();
  return std::numeric_limits<double>::quiet_NaN();
}

// The statistic names estpost summarize posts, in Stata's own order. `N` is
// renamed to `count` because that is the e() name esttab's `cells(count)`
// asks for; the rest keep the summarize block's spelling.
struct SummStat {
  const char* eName;
  const char* blockKey;
  bool detailOnly;
};
const std::vector<SummStat>& summStats() {
  static const std::vector<SummStat> k = {
      {"count", "N", false},  {"sum", "sum", false},  {"mean", "mean", false},
      {"min", "min", false},  {"max", "max", false},  {"sd", "sd", false},
      {"sum_w", "sum_w", false},
      {"p1", "p1", true},     {"p25", "p25", true},   {"p50", "p50", true},
      {"p75", "p75", true},   {"p99", "p99", true},
  };
  return k;
}

// Turn the posted matrices into the on-screen table. One row per variable, one
// column per statistic -- `columns(variables)` transposes it, which is what
// that option means and why it is not a silent no-op here.
Table estpostTable(const std::vector<std::string>& vars,
                   const std::vector<std::string>& statNames, const ojson& matrices,
                   bool byVariables, const std::string& title) {
  Table t;
  t.title = title;
  auto cell = [&](const std::string& stat, size_t varIdx) -> std::string {
    if (!matrices.contains(stat)) return "";
    const ojson& m = matrices.at(stat);
    if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) return "";
    const ojson& row = m["data"][0];
    if (varIdx >= row.size()) return "";
    if (row[varIdx].is_null()) return kMissingCell;
    return fmtNum(jsonNum(row[varIdx]), stat == "count" ? "%9.0f" : "%9.4f");
  };
  if (byVariables) {
    t.colHeads = vars;
    for (const auto& s : statNames) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = s;
      for (size_t v = 0; v < vars.size(); ++v) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  } else {
    t.colHeads = statNames;
    for (size_t v = 0; v < vars.size(); ++v) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = vars[v];
      for (const auto& s : statNames) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  }
  return t;
}

// Install the pseudo-estimate. `lastEstBlock` is what `eststo` snapshots, what
// `estadd` fingerprints, and -- since bundle E -- what a bare `esttab` falls
// back to. estpost therefore has to write it for anything downstream to see
// the table at all.
//
// `session.eststo(...)` is deliberately NOT called: Stata's estpost posts to
// e() and never stores, and minting an `m1` as a side effect would also change
// what a LATER `eststo` numbers itself.
// `setLastModel` is not called either: estpost is not a fitted model, and
// claiming one would let `predict`/`test` address a summary table.
//
// THE COEFTABLE IS NOT EMPTY, and this is a deliberate departure from the
// plan's literal `coeftable:[]`. Stata's `ereturn post` requires a b vector,
// and estpost supplies one -- which is why a bare `esttab` after `estpost
// summarize` prints the means at all. An empty coeftable renders an EMPTY
// TABLE at rc 0, with the statistics sitting unreachable in e(mean). So e(b)
// carries the primary statistic and e(se) the dispersion one, and the 1xk
// matrices are posted alongside exactly as the plan specifies.
struct PseudoCoef {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
};

void postPseudoEstimate(SessionState& session, const std::string& estimator, double nObs,
                        const ojson& matrices, const std::vector<PseudoCoef>& coefs) {
  ojson coeftable = ojson::array();
  for (const auto& c : coefs) {
    ojson row;
    row["name"] = c.name;
    row["b"] = std::isnan(c.b) ? ojson(nullptr) : ojson(c.b);
    row["se"] = std::isnan(c.se) ? ojson(nullptr) : ojson(c.se);
    // No t and no p: a summary statistic has no null hypothesis, and a zero
    // here would draw significance stars onto a column of means.
    row["t"] = nullptr;
    row["p"] = nullptr;
    coeftable.push_back(row);
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["depvar"] = "";
  // An INTEGER, because the shared fingerprint reads n_obs with get<long long>()
  // and because e(N) is a count.
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = coeftable;
  session.lastEstBlock = block;
  // W2 esttabnames: `ereturn post` drops the hidden e(_estimates_name), so
  // these results carry no store name (`esttab .` is the live posting).
  session.activeEstimatesName.clear();
  session.estAddedClear();
  session.estAdded["matrices"] = matrices;
  // BUG-408: estpost is a NEW e(), so the session stores are replaced, not
  // added to — then the posted matrices land in them and `matrix M = e(mean)`
  // resolves. Without the clear, a previous estimation's e(b)/e(V) would sit
  // beside estpost's summary matrices and `ereturn list` would show both.
  session.eval.eScalars.clear();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();
  {
    std::vector<std::pair<std::string, ojson>> mm;
    for (const auto& [k, v] : matrices.items()) mm.emplace_back(k, v);
    mirrorIntoEStores(session, {{"N", static_cast<double>(nObs)}}, {}, mm);
  }
  // Stamped from the SHARED helper, after the block is complete -- the
  // coeftable length is part of the fingerprint.
  session.estAddedFp = SessionState::estAddFingerprint(block);
}

// The two statistics that become e(b) and e(se). `mean`/`sd` when they were
// computed, else the first two requested -- so `estpost tabstat x, stat(sum)`
// still posts something addressable rather than a blank column.
void pickPrimary(const std::vector<std::string>& statNames, std::string* bStat,
                 std::string* seStat) {
  const auto have = [&](const char* n) {
    return std::find(statNames.begin(), statNames.end(), n) != statNames.end();
  };
  *bStat = have("mean") ? "mean" : (statNames.empty() ? std::string() : statNames[0]);
  if (have("sd")) {
    *seStat = "sd";
  } else {
    for (const auto& s : statNames) {
      if (s != *bStat) { *seStat = s; break; }
    }
  }
}

// Read one cell out of a posted 1xk matrix by column position.
double matCell(const ojson& matrices, const std::string& stat, size_t j) {
  if (stat.empty() || !matrices.contains(stat)) return std::numeric_limits<double>::quiet_NaN();
  const ojson& m = matrices.at(stat);
  if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const ojson& row = m["data"][0];
  if (j >= row.size() || row[j].is_null()) return std::numeric_limits<double>::quiet_NaN();
  return jsonNum(row[j]);
}

}  // namespace

ojson runEstpost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstpost.known,
                                            kVocabEstpost.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("estpost: a subcommand is required (summarize or tabstat)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {
      "tabulate", "tab", "ttest", "correlate", "corr", "margins", "ci", "stci", "svy"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  const bool isSumm = (sub == "summarize" || sub == "sum" || sub == "summ");
  const bool isTabstat = (sub == "tabstat");
  if (!isSumm && !isTabstat) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }

  std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (vars.empty()) {
    if (isTabstat) return tblSyntax("estpost tabstat: at least one variable is required");
    vars = session.datastore()->names();
  }
  for (const auto& v : vars) {
    if (!session.datastore()->has(v)) {
      return tblError("variable " + v + " not found", "unknown_variable");
    }
  }

  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  const bool quiet = hasFlag(parsed.opts, "quietly");
  std::optional<std::string> statsOpt = optStr(parsed.opts, "statistics");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stat");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stats");
  const std::optional<std::string> colsOpt = optStr(parsed.opts, "columns");
  bool byVariables = false;
  if (colsOpt.has_value()) {
    const std::string c = strutil::lower(dequote(*colsOpt));
    if (c == "variables" || c == "var") {
      byVariables = true;
    } else if (c != "statistics" && c != "stat" && c != "stats") {
      return tblSyntax("estpost: columns(" + *colsOpt +
                       ") must be columns(statistics) or columns(variables)");
    }
  }

  // `listwise` is casewise deletion ACROSS the variable list, and it is folded
  // into the row filter rather than applied afterwards -- doing it afterwards
  // would summarize each variable over its own complete cases, which is the
  // question `listwise` exists to stop asking.
  std::string ifExprText = parsed.ifExpr.value_or(std::string());
  if (hasFlag(parsed.opts, "listwise")) {
    std::string cond;
    for (const auto& v : vars) {
      if (!cond.empty()) cond += " & ";
      cond += "!missing(" + v + ")";
    }
    ifExprText = ifExprText.empty() ? cond : ("(" + ifExprText + ") & (" + cond + ")");
  }
  std::optional<std::string> effIf;
  if (!ifExprText.empty()) effIf = ifExprText;

  std::string dataPath = *session.currentHbin();
  try {
    const auto filt = applyIfAndInFilter(session, dataPath, effIf, parsed.inRange);
    if (!filt.ok) return inRangeRejectError(filt, parsed.inRange.value_or(""), "estpost");
    dataPath = filt.dataPath;
  } catch (const CobraError& ce) {
    return tblError(cobraErrorMessage(ce), ce.code);
  }

  ojson matrices = ojson::object();
  std::vector<std::string> statNames;
  double nObs = 0;

  if (isSumm) {
    if (byOpt.has_value()) {
      // estpost summarize has no by() in Stata; saying so names the command
      // that does, instead of accepting the option and summarizing the pooled
      // sample under a grouped heading.
      return tblSyntax(
          "estpost summarize: by() is not part of estpost summarize; use `estpost tabstat "
          "..., by(g)'");
    }
    ojson summ;
    try {
      // The SAME shape runSummarize uses (handlers.cpp:1280-1310): the session's
      // own Datastore when nothing was filtered, and a temporary one wrapped in
      // a StatelessRead when it was. Opening a fresh Datastore on the head
      // unconditionally would evict the resident frame and force the very
      // re-materialisation the resident plane exists to avoid.
      if (dataPath == *session.currentHbin()) {
        summ = summarizeCoreStats(*session.datastore(), vars, /*detail=*/true);
      } else {
        Datastore tmp(dataPath);
        SessionState::StatelessRead stateless(session);
        summ = summarizeCoreStats(tmp, vars, /*detail=*/true);
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost summarize: ") + e.what());
    }
    if (isErrorBlock(summ)) return summ;
    const ojson& stats = summ.contains("stats") ? summ["stats"] : ojson::object();
    // A factor variable has no mean; summarizeCoreStats reports N with null
    // statistics for it (handlers.cpp:1193-1208) and estpost carries that
    // through as MISSING, never as zero.
    const bool wantDetail = hasFlag(parsed.opts, "detail");
    for (const auto& st : summStats()) {
      if (st.detailOnly && !wantDetail) continue;
      std::vector<double> vals;
      bool any = false;
      for (const auto& v : vars) {
        double d = std::numeric_limits<double>::quiet_NaN();
        if (stats.contains(v) && stats[v].contains(st.blockKey)) {
          d = jsonNum(stats[v][st.blockKey]);
          if (!std::isnan(d)) any = true;
        }
        vals.push_back(d);
      }
      if (!any && std::string(st.eName) != "count") continue;
      matrices[st.eName] = rowMatrix(vars, vals);
      statNames.push_back(st.eName);
    }
    for (const auto& v : vars) {
      if (stats.contains(v) && stats[v].contains("N")) {
        nObs = std::max(nObs, jsonNum(stats[v]["N"]));
      }
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < vars.size(); ++j) {
        coefs.push_back(PseudoCoef{vars[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_summarize", nObs, matrices, coefs);
    }
  } else {
    // The `cobra tabstat --tidy` argv, re-assembled (see the block comment
    // above for why it is re-assembled and not called).
    std::vector<std::string> args = {"tabstat", "--vars", joinWith(vars, ",")};
    if (byOpt.has_value() && !byOpt->empty()) {
      args.push_back("--by");
      args.push_back(dequote(*byOpt));
    }
    std::string statCsv = "mean,sd,min,max,n";
    if (statsOpt.has_value() && !statsOpt->empty()) {
      statCsv = joinWith(splitTopLevel(dequote(*statsOpt), ' '), ",");
      // `statistics(mean, sd)` -- a comma-separated list already; collapse the
      // double separator rather than sending an empty stat name to the engine.
      std::vector<std::string> cleaned;
      for (const auto& piece : splitTopLevel(statCsv, ',')) {
        const std::string p = strutil::trim(piece);
        if (!p.empty()) cleaned.push_back(p);
      }
      statCsv = joinWith(cleaned, ",");
    }
    args.push_back("--stats");
    args.push_back(statCsv);
    args.push_back("--data");
    args.push_back(dataPath);
    args.push_back("--tidy");

    ojson raw;
    try {
      raw = runCobraJson(args);
    } catch (const CobraError& ce) {
      return tblError(cobraErrorMessage(ce), ce.code);
    }
    if (raw.contains("stats") && raw["stats"].is_array()) {
      for (const auto& s : raw["stats"]) {
        if (s.is_string()) statNames.push_back(s.get<std::string>());
      }
    }
    if (raw.contains("n_obs")) nObs = jsonNum(raw["n_obs"]);

    const bool grouped = byOpt.has_value() && raw.contains("groups") && raw["groups"].is_array();
    const bool nototal = hasFlag(parsed.opts, "nototal");
    std::vector<std::string> colnames;
    std::map<std::string, std::vector<double>> byStat;
    auto addBlock = [&](const std::string& prefix, const ojson& block) {
      for (const auto& v : vars) {
        colnames.push_back(prefix.empty() ? v : (prefix + ":" + v));
        for (const auto& s : statNames) {
          double d = std::numeric_limits<double>::quiet_NaN();
          if (block.contains(v) && block[v].contains(s)) d = jsonNum(block[v][s]);
          byStat[s].push_back(d);
        }
      }
    };
    if (grouped) {
      for (const auto& g : raw["groups"]) {
        const std::string gname =
            g.contains("value") ? fmtCount(g["value"]) : std::string("g");
        if (g.contains("stats")) addBlock(gname, g["stats"]);
      }
      if (!nototal && raw.contains("total")) addBlock("Total", raw["total"]);
    } else if (raw.contains("total")) {
      addBlock("", raw["total"]);
    }
    for (const auto& s : statNames) {
      matrices[s] = rowMatrix(colnames, byStat[s]);
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < colnames.size(); ++j) {
        coefs.push_back(PseudoCoef{colnames[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_tabstat", nObs, matrices, coefs);
    }
    if (!colnames.empty()) vars = colnames;
  }

  if (quiet) return ojson::array();
  const Table t = estpostTable(vars, statNames, matrices,
                               byVariables, isSumm ? "Summary statistics"
                                                   : "Summary statistics (tabstat)");
  return echoBlock(renderFixed(t));
}

namespace {

// The active e() as a Model, with the intercept renamed the way `makeSnapshot`
// does -- `_cons` is the spelling every table and every user writes.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut) {
  if (!session.lastEstBlock.has_value()) {
    if (session.lastModel().has_value() && session.lastModel()->result.has_value()) {
      // A fit that filled LastModel but not lastEstBlock (five TUs assign the
      // latter directly) still has its raw result cached.
      ojson snap = *session.lastModel()->result;
      out->name = cmd;
      out->snap = snap;
      out->added = emptyBag();
      out->escalars = buildEScalars(snap);
      return true;
    }
    *errOut = tblErrorRc(cmd + ": no estimation result in memory. Run a regression first.", 301);
    return false;
  }
  ojson snap = *session.lastEstBlock;
  if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
    for (auto& row : snap["coeftable"]) {
      if (row.contains("name") && row["name"].is_string() &&
          row["name"].get<std::string>() == "(intercept)") {
        row["name"] = "_cons";
      }
    }
  }
  out->name = cmd;
  out->snap = snap;
  out->added = emptyBag();
  if (session.estAddedFp == SessionState::estAddFingerprint(*session.lastEstBlock)) {
    out->added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!out->added.contains(k) || !out->added[k].is_object()) out->added[k] = ojson::object();
  }
  out->escalars = buildEScalars(snap);
  if (!out->escalars.contains("N") && snap.contains("n_obs")) {
    out->escalars["N"] = jsonNum(snap["n_obs"]);
  }
  return true;
}

// One place where a rendered table becomes a file. `append` reads the existing
// bytes FIRST and hands the concatenation to the atomic writer, so the
// destination is never opened for writing while it is still a source (the
// dest==source law, BUG-138).
ojson emitTable(const std::string& cmd, SessionState& session, const std::string& userPath,
                const std::string& bytes, bool replace, bool append) {
  const std::string resolved = resolveUserPath(session, userPath);
  ojson guard = guardOverwrite(cmd, resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  std::string payload = bytes;
  if (append && cecon::native_exists(resolved)) {
    bool ok = false;
    const std::string existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError(cmd + ": cannot read " + userPath + " to append to it", "data_io");
    payload = existing;
    if (!payload.empty() && payload.back() != '\n') payload += "\n";
    payload += bytes;
  }
  ojson werr = writeTextFileAtomic(cmd, resolved, userPath, payload);
  if (!werr.is_null()) return werr;
  // The USER's spelling, never the resolved absolute path: bundle D echoed the
  // resolved path and baked a build machine's workdir into a golden.
  return displayBlock("(output written to " + userPath + ")");
}

}  // namespace

// ===========================================================================
// estout
// ===========================================================================

ojson runEstout(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstout.known,
                                            kVocabEstout.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);

  ojson err;
  std::vector<Model> models = collectModels(session, "estout", words, &err);
  if (models.empty()) return err;

  // `transform()` rewrites the COEFFICIENT VECTOR before any row is built, which
  // is what the ado does (estout.ado:2564 calls TransformCoefs on `bc`) and what
  // keeps this out of the two writers' cell code. estout and esttab measurably
  // agree here (w1b_transform2.log y3-y6 against w1b_transform.log r2/r3/v1/u1).
  TransformSpec trSpec;
  if (auto tr = optStr(parsed.opts, "transform")) {
    std::string trErr;
    if (!parseTransformOpt(*tr, &trSpec, &trErr)) return tblSyntax(trErr);
    for (size_t mi = 0; mi < models.size(); ++mi) {
      if (!models[mi].snap.contains("coeftable") || !models[mi].snap["coeftable"].is_array()) {
        continue;
      }
      for (auto& row : models[mi].snap["coeftable"]) {
        if (!row.contains("b") || !row["b"].is_number()) continue;
        const double b0 = row["b"].get<double>();
        const TransformCell tc = transformOneCell(trSpec, mi, row.value("name", std::string()), b0,
                                                  models[mi].snap, session);
        if (!tc.matched) continue;
        if (tc.bMissing) row["b"] = ojson(nullptr);
        else if (tc.b.has_value()) row["b"] = *tc.b;
        if (tc.seFactor.has_value() && row.contains("se") && row["se"].is_number()) {
          const double se = row["se"].get<double>();
          if (std::isnan(*tc.seFactor)) row["se"] = ojson(nullptr);
          else row["se"] = se * *tc.seFactor;
        }
      }
    }
  }

  // ---- style --------------------------------------------------------------
  // BUG-486, axis 1. estout's own default is `tab`; `fixed` is what ESTTAB
  // passes when it wraps estout, and this build had taken the wrapper's default
  // for the command's. Measured: `estout m1 m2 using f.txt, replace` with no
  // style writes a TAB-separated file (w1b_tables/e2.txt this session,
  // aer_b4/tables/e_plain.txt on 09-09 -- byte-compatible three months apart).
  std::string style = "tab";
  if (auto s = optStr(parsed.opts, "style")) style = strutil::lower(dequote(*s));
  if (style == "html" || style == "smcl" || style == "mmd" || style == "md") {
    // Named, not generic: the missing thing is a WRITER for that markup, and
    // this build has none. Writing a differently-formatted file under the
    // requested style would be the silent-wrong-output shape.
    return ojson(eval::refusedCommand("estout", "no_" + style + "_writer"));
  }
  if (style != "tex" && style != "fixed" && style != "tab") {
    return tblSyntax("estout: style(" + style + ") must be tex, fixed or tab");
  }

  // ---- cells --------------------------------------------------------------
  bool fmtOk = true;
  // BUG-486, axis 6. `%9.3f` is esttab's rounding; estout prints the estimate
  // at Stata's own default general format -- `-49.51222`, `1.746559`,
  // `3673.06` in e2.txt, three different decimal counts in one column, which is
  // what %9.0g does and %9.3f cannot. `fmtNum` already routes `%9.0g` to
  // `fmtStataDefaultG`, the same rule `display` uses.
  std::string globalFmt = "%9.0g";
  if (auto f = optStr(parsed.opts, "fmt")) {
    globalFmt = normalizeFmt(*f, &fmtOk);
    if (!fmtOk) {
      return tblSyntax("estout: fmt(" + *f +
                       ") is not implemented in this build; use fmt(#) or a %fmt");
    }
  }

  BodyOpts bo;
  std::vector<std::string> starMarks;
  bo.stars = estoutStars();
  if (auto sl = optStr(parsed.opts, "starlevels")) {
    bo.stars = parseStarLevels(*sl, estoutStars(), &starMarks);
  }

  // BUG-486, the format-inheritance rule. estout carries ONE current format,
  // set by the last `fmt()` it saw and defaulting to %9.0g; a cell that gives
  // no fmt takes the running one, not the global default. Measured, and the
  // two arms disagree with each other, which is what makes the rule readable:
  //   cells(b(fmt(3)) se(par))  -> b -49.512 AND se (86.156)   -- both 3dp
  //   cells(b se)               -> b -49.51222, se 86.15604    -- both %9.0g
  // (tests/eval/golden/aer_w1/w1b_estout_defaults.log g1/g2, and e5b.txt from
  // the first capture). Assigning globalFmt to every fmt-less spec instead --
  // which is what the first cut of this fix did -- renders g1's se as %9.0g
  // and is a NEW divergence introduced by fixing the default.
  std::string runningFmt = globalFmt;
  if (auto c = optStr(parsed.opts, "cells")) {
    std::string cerr;
    if (!parseCells(*c, &bo.cells, &cerr, &bo.sideBySide)) return tblSyntax(cerr);
    for (auto& cs : bo.cells) {
      if (cs.fmtGiven) {
        runningFmt = cs.fmt;
      } else {
        cs.fmt = runningFmt;
      }
    }
  } else {
    // BUG-486, axis 5. estout's default is `cells(b)` -- no stars. The
    // capture is unambiguous: e2.txt's coefficient column is `-49.51222`,
    // `1.746559`, `3673.06`, with no mark anywhere in the file, and the star
    // legend appears only under `legend`/`starlevels` (e5.txt). This build
    // printed `b(star)`, which is esttab's default, not estout's. The bare
    // `se`/`t`/`p` flags still add a parenthesised second row.
    CellSpec b;
    b.name = "b";
    b.fmt = globalFmt;
    b.star = false;
    bo.cells.push_back(b);
    const bool wantSe = hasFlag(parsed.opts, "se");
    const bool wantT = hasFlag(parsed.opts, "t");
    const bool wantP = hasFlag(parsed.opts, "p");
    if (wantSe || wantT || wantP) {
      CellSpec s;
      s.name = wantSe ? "se" : (wantT ? "t" : "p");
      s.fmt = globalFmt;
      s.par = true;
      bo.cells.push_back(s);
    }
  }

  bo.keep = optStr(parsed.opts, "keep");
  bo.drop = optStr(parsed.opts, "drop");
  bo.order = optStr(parsed.opts, "order");
  bo.useVarLabels = hasFlag(parsed.opts, "label") || hasFlag(parsed.opts, "labels");
  if (auto vl = optStr(parsed.opts, "varlabels")) bo.labels = parseLabelPairs(*vl);
  // W1a-4: `rename(old new ...)` is the same kind of map `varlabels()` fills, so
  // it goes into the same one -- `labelFor` already consults it first and leaves a
  // name it does not hold alone, which is exactly the measured behaviour for a
  // rename whose source is not in the model (w1b_estout_opts2.log y b, rc 0 and
  // the table unchanged).
  if (auto rn = optStr(parsed.opts, "rename")) {
    for (const auto& [from, to] : parseLabelPairs(*rn)) bo.labels[from] = to;
  }

  Table t;
  if (auto ti = optStr(parsed.opts, "title")) t.title = dequote(*ti);
  // BUG-486, axis 3. The default is NONUMBERS -- e_plain.txt has no (1)/(2)
  // row -- and `numbers` turns it on. W5 landed both OPTIONS correctly and
  // left the DEFAULT inverted on purpose, naming the reason in this comment:
  // flipping it rewrites bytes pinned by tests/kitchensink/freeze/estout.json,
  // which was outside that task's write manifest, and it was routed onward as
  // a re-pin request. The request was never filed anywhere I can find, so the
  // named deferral had no landing site and survived four days as a shipped
  // divergence. It is flipped here and the fixture is re-captured WITH the
  // oracle as the stated reason, which is the only thing that licenses moving
  // a self-captured golden.
  //
  // `nonumbers` still wins when both are typed: e_numbers_off.txt is
  // `estout ... , numbers nonumbers` and carries no (1)/(2) row.
  const bool estoutNumbers =
      hasFlag(parsed.opts, "numbers") && !hasFlag(parsed.opts, "nonumbers");
  buildHeads(&t, models, optStr(parsed.opts, "mlabels"), /*numbers=*/estoutNumbers,
             /*heads=*/true, /*nameHeads=*/true);
  // BUG-487: under the wide layout each model owns `stride` columns. Its NAME
  // sits in the first and the rest are blank -- `\tm1\t\t\tm2\t\t` for three
  // items (w1b_estout_cells.log h9) -- so the heads built above are spread out
  // rather than rebuilt, and mlabels()/numbers keep whatever they produced.
  const size_t stride = bo.sideBySide ? std::max<size_t>(bo.cells.size(), 1) : 1;
  if (stride > 1) {
    auto spread = [&](std::vector<std::string>* v) {
      std::vector<std::string> wide;
      for (const auto& h : *v) {
        wide.push_back(h);
        for (size_t pad = 1; pad < stride; ++pad) wide.emplace_back();
      }
      *v = wide;
    };
    spread(&t.colHeads);
    spread(&t.colNums);
  }
  // BUG-486, axis 4: the cell-name row. One entry per COLUMN; stacked specs
  // join with `/`, which is what esttab's own `b/se` collabel shows
  // (w1b_tables/c1.txt). The side-by-side spelling `cells("b se")` gives one
  // column per (model x spec) with an unjoined label -- that is BUG-487, and
  // until `parseCells` can tell the two spellings apart this row follows the
  // stacked reading, which is what this build renders.
  if (bo.sideBySide) {
    // ...and the cell-name row names EACH column: `b se b se` (h2), not the
    // joined `b/se` the stacked layout carries (h1).
    t.colLabels.clear();
    for (size_t mi = 0; mi < models.size(); ++mi) {
      for (const auto& cs : bo.cells) t.colLabels.push_back(cs.name);
    }
  } else {
    std::string joined;
    for (const auto& cs : bo.cells) {
      if (!joined.empty()) joined += "/";
      joined += cs.name;
    }
    t.colLabels.assign(models.size(), joined);
  }
  if (auto cl = optStr(parsed.opts, "collabels")) {
    // collabels() names the CELLS, not the models -- `collabels(none)` removes
    // the row and leaves the model heads standing (w1b_tables/e7.tex). Before
    // BUG-486 this option overwrote the model heads, so `collabels(none)` put
    // the literal word `none` where `m1` belonged.
    const std::string raw = strutil::lower(strutil::trim(dequote(*cl)));
    if (raw == "none" || raw.empty()) {
      t.colLabels.clear();
    } else {
      std::vector<std::string> labs = splitTopLevel(dequote(*cl));
      for (size_t i = 0; i < labs.size() && i < t.colLabels.size(); ++i) {
        t.colLabels[i] = dequote(labs[i]);
      }
    }
  }
  // BUG-486, axis 2. `_cons` LAST, in every captured estout file. This is the
  // same defect W5 fixed for esttab (selftest x.1: "the engine's coeftable puts
  // the intercept first and this writer followed it"); the flag it added exists
  // here already and was left off for estout with the frozen fixture as the
  // reason, which the re-capture below settles.
  bo.consLast = true;
  std::string bodyErr;
  buildBody(&t, models, bo, session, &bodyErr);
  // r(111), not an empty table: a keep() pattern that selects nothing is a
  // typo, and Stata says so (w1b_keepglob.log kg_glob_none).
  if (!bodyErr.empty()) return tblErrorRc(bodyErr, 111);

  if (auto st = optStr(parsed.opts, "stats")) {
    // ...and the footer takes the SAME running format the cells left behind:
    // `stats(N r2)` alone is `74` / `.2933891` (%9.0g), while the same option
    // after `cells(b(fmt(3)) se(par))` is `74.000` / `0.293`
    // (w1b_estout_defaults.log g3 vs g4). One format, carried forward.
    buildFoot(&t, models, splitWs(dequote(*st)), runningFmt, stride);
  }
  if (auto n = optStr(parsed.opts, "note")) t.notes.push_back(dequote(*n));

  // W1a-4: `legend` adds the star legend, and ONLY when the cells actually carry
  // stars -- `cells(b) legend` adds nothing (w1b_estout_opts2.log y8). The order
  // is DESCENDING cutoff, the opposite of the ladder's own (`* p<0.05, **
  // p<0.01, *** p<0.001`, y6), and a `starlevels()` ladder prints its levels
  // VERBATIM: `+ 0.10` is `+ p<0.10` and not `p<0.1` (y7), so the option's own
  // tokens are re-read for the text rather than the parsed doubles reformatted.
  if (hasFlag(parsed.opts, "legend")) {
    bool anyStarCell = false;
    for (const auto& cs : bo.cells) {
      if (cs.star) anyStarCell = true;
    }
    if (anyStarCell) {
      std::string leg;
      if (auto sl = optStr(parsed.opts, "starlevels")) {
        const std::vector<std::string> toks = splitWs(dequote(*sl));
        for (size_t i = 0; i + 1 < toks.size(); i += 2) {
          if (!leg.empty()) leg += ", ";
          leg += toks[i] + " p<" + toks[i + 1];
        }
      }
      if (leg.empty()) {
        std::vector<StarLevel> desc(bo.stars.begin(), bo.stars.end());
        std::reverse(desc.begin(), desc.end());
        leg = starNote(desc);
      }
      if (!leg.empty()) t.notes.push_back(leg);
    }
  }

  // `extracols(n ...)` inserts an EMPTY column after original column n, the stub
  // being column 1 -- so `extracols(1)` puts one between the stub and the first
  // model and `extracols(2)` between the two models (w1b_estout_opts2.log
  // y2/y3/y4, read with delimiter(" | ") because `type` in a log expands tabs and
  // the first capture of this was unreadable). `extracols(0)` behaves as 1 (y5).
  if (auto xc = optStr(parsed.opts, "extracols")) {
    std::vector<size_t> at;
    for (const auto& tok : splitWs(dequote(*xc))) {
      try {
        const int n = std::stoi(tok);
        if (n >= 0) at.push_back(static_cast<size_t>(n == 0 ? 1 : n));
      } catch (const std::exception&) {
      }
    }
    // Descending, so each insertion cannot move the next one's index.
    std::sort(at.begin(), at.end(), std::greater<size_t>());
    auto insertInto = [&](std::vector<std::string>* v, size_t col) {
      const size_t idx = col - 1;  // column 1 is the stub, which is not in `cells`
      if (idx > v->size()) v->resize(idx);
      v->insert(v->begin() + static_cast<long>(std::min(idx, v->size())), std::string());
    };
    for (size_t col : at) {
      if (col == 0) continue;
      insertInto(&t.colNums, col);
      insertInto(&t.colHeads, col);
      insertInto(&t.colLabels, col);
      for (auto& r : t.body) insertInto(&r.cells, col);
      for (auto& r : t.foot) insertInto(&r.cells, col);
    }
  }

  // ---- render -------------------------------------------------------------
  EstoutHooks hooks;
  if (auto v = optStr(parsed.opts, "prehead")) hooks.prehead = dequote(*v);
  if (auto v = optStr(parsed.opts, "posthead")) hooks.posthead = dequote(*v);
  if (auto v = optStr(parsed.opts, "prefoot")) hooks.prefoot = dequote(*v);
  if (auto v = optStr(parsed.opts, "postfoot")) hooks.postfoot = dequote(*v);
  if (auto v = optStr(parsed.opts, "begin")) hooks.begin = dequote(*v);

  std::string bytes;
  if (style == "tex") {
    // W1a-4: estout's OWN tex geometry (see renderEstoutTex). `nolabel` wins over
    // `label` when both are given, which is the arm that separates them --
    // w1b_estout_opts.log q3 renders variable NAMES for `label nolabel`.
    const bool labelsOn = hasFlag(parsed.opts, "label") && !hasFlag(parsed.opts, "nolabel");
    size_t stubW = labelsOn ? 20 : 12;
    size_t cellW = 12;
    if (auto vw = optStr(parsed.opts, "varwidth")) {
      try {
        const int n = std::stoi(dequote(*vw));
        if (n > 0 && n <= 200) stubW = static_cast<size_t>(n);
      } catch (const std::exception&) {
      }
    }
    if (auto mw = optStr(parsed.opts, "modelwidth")) {
      try {
        const int n = std::stoi(dequote(*mw));
        if (n > 0 && n <= 200) cellW = static_cast<size_t>(n);
      } catch (const std::exception&) {
      }
    }
    bool anyStar = false;
    for (const auto& cs : bo.cells) {
      if (cs.star) anyStar = true;
    }
    bytes = renderEstoutTex(t, stubW, cellW, anyStar ? 3 : 0, hooks);
  } else if (style == "tab") {
    bytes = renderDelim(t, '\t', hooks);
  } else {
    bytes = renderFixed(t, hooks);
  }

  if (!usingPath.has_value()) return echoBlock(bytes);
  return emitTable("estout", session, *usingPath, bytes, hasFlag(parsed.opts, "replace"),
                   hasFlag(parsed.opts, "append"));
}

// ===========================================================================
// esttab ... using FILE   (B4-W5)
//
// The dispatcher hands this the block `runEsttab` ALREADY built, so the file
// and the screen cannot disagree about a COEFFICIENT: every b/se/t/p cell below
// is read out of that block and nothing is re-estimated here. What the writer
// owns is esttab's own PRESENTATION contract -- which statistic sits in the
// parentheses, how many digits it carries, which rows exist and in what order,
// and the legend -- and that contract is now pinned to real StataBE output:
// tests/eval/golden/aer_b4/tables/t_*.txt|tex, 16 arms captured 2026-09-09 by
// milfpins_oracle_t.do / _t2.do against the installed esttab 2.1.4 + estout
// 3.33. Where this file and that capture disagree, the capture is right.
//
// The defaults the capture REFUTED (all four were live in this build, all four
// are silent -- the table renders, it is just not esttab's table):
//   * the parenthesised statistic is `t`, not the standard error
//     (t_plain.txt legend: "t statistics in parentheses"), esttab.ado:687
//     `if "`tfmt'"=="" local tfmt 2` -> %11.2f, so (-0.57), not (86.156);
//   * the coefficient format is `a3`, estout's ADAPTIVE significant-digit
//     format (esttab.ado:549 `if "`bfmt'"=="" local bfmt a3`), not %9.3f:
//     -49.51 / 1.747 / 1946.1, three different decimal counts in one column;
//   * the default footer is N alone -- no R-squared row (t_plain.txt);
//   * `_cons` is the LAST coefficient row, not the first.
// The star ladder (* 0.05 ** 0.01 *** 0.001, esttab.ado:28) was already right
// in `estoutStars()`; only the legend's ORDER was reversed against the capture.
//
// SCOPE NOTE, stated because silence is what this campaign removes: the JSON
// block `runEsttab` returns to the SCREEN still carries this build's older
// defaults (se in parentheses, %9.3f, an R-squared footer row, `_cons` first).
// Correcting it changes bytes pinned by tests/kitchensink/freeze/esttab.json
// and coefplot.json, which are outside this task's write manifest. The FILE
// writer is the surface the 27 captured arms measure and the surface the AER
// corpus writes; the screen block's divergence is named in the W5 report as a
// re-pin request, not fixed here behind MILF's back.
// ===========================================================================

namespace {

// A number format that is either FIXED (`fmt(3)` -> %12.3f, esttab.ado's
// integer-format shorthand) or ADAPTIVE (`a3`). esttab's defaults use both in
// the same table: b is adaptive, t is fixed at 2.
struct EtFmt {
  bool adaptive = true;
  int digits = 3;  // significant digits when adaptive, decimals when fixed.
  // BUG-407 / D4: estout's own default is Stata's general format, which is
  // neither of the two above -- `-49.51222`, nine characters, as many decimals
  // as fit. esttab's `a3` never reaches a cells() table, so this mode exists
  // only for that path and shares `fmtStataDefaultG` with `display` and with
  // estout's own writer, so the three cannot drift apart.
  bool stataG = false;
  std::string apply(double v) const {
    if (std::isnan(v)) return kMissingCell;
    if (stataG) return piet::fmtStataDefaultG(v);
    if (adaptive) return sigDigits(v, digits, /*minDec=*/1, /*comma=*/false);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
  }
};

// `b(3)` / `t(%9.2f)` / `ci(3)`: an integer is a decimal count, a `%fmt` is a
// %fmt, and `a#` is the adaptive format. Anything else keeps the default rather
// than half-applying -- the same rule parseStarLevels already follows.
EtFmt parseEtFmt(const std::string& raw, EtFmt fallback) {
  const std::string s = strutil::trim(dequote(raw));
  if (s.empty()) return fallback;
  if (s[0] == 'a') {
    EtFmt f;
    f.adaptive = true;
    f.digits = 3;
    if (s.size() > 1) {
      try {
        size_t pos = 0;
        const int n = std::stoi(s.substr(1), &pos);
        if (pos + 1 == s.size() && n >= 0 && n <= 12) f.digits = n;
      } catch (...) {
        return fallback;
      }
    }
    return f;
  }
  if (s[0] == '%') {
    // Only the %w.df shape appears in the corpus; the decimals are what matter.
    const size_t dot = s.find('.');
    if (dot == std::string::npos || dot + 1 >= s.size()) return fallback;
    try {
      size_t pos = 0;
      const int n = std::stoi(s.substr(dot + 1), &pos);
      if (n >= 0 && n <= 12) {
        EtFmt f;
        f.adaptive = false;
        f.digits = n;
        return f;
      }
    } catch (...) {
    }
    return fallback;
  }
  try {
    size_t pos = 0;
    const int n = std::stoi(s, &pos);
    if (pos == s.size() && n >= 0 && n <= 12) {
      EtFmt f;
      f.adaptive = false;
      f.digits = n;
      return f;
    }
  } catch (...) {
  }
  return fallback;
}

// --- esttab's star ladder ---------------------------------------------------
// The THRESHOLD TEXT is carried, not re-formatted from the double: t_star.txt's
// legend reads "* p<0.10", and `%g` of 0.10 is "0.1". esttab echoes what the
// user typed in starlevels(), so the token is kept verbatim.
struct EtStar {
  std::string mark;
  double cutoff = 0.0;
  std::string text;
};

std::vector<EtStar> esttabDefaultStars() {
  return {{"*", 0.05, "0.05"}, {"**", 0.01, "0.01"}, {"***", 0.001, "0.001"}};
}

// `starlevels(* 0.10 ** 0.05 *** 0.01)` -- mark/threshold pairs, kept in the
// order given (which is the order the legend prints).
std::vector<EtStar> parseEtStars(const std::string& spec, const std::vector<EtStar>& fallback) {
  const std::vector<std::string> toks = splitWs(dequote(spec));
  if (toks.size() < 2 || toks.size() % 2 != 0) return fallback;
  std::vector<EtStar> out;
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    try {
      size_t pos = 0;
      const double th = std::stod(toks[i + 1], &pos);
      if (pos != toks[i + 1].size()) return fallback;
      out.push_back(EtStar{dequote(toks[i]), th, toks[i + 1]});
    } catch (...) {
      return fallback;
    }
  }
  return out;
}

std::string etStarsFor(double p, const std::vector<EtStar>& ladder) {
  if (std::isnan(p)) return "";
  std::string mark;
  double best = std::numeric_limits<double>::infinity();
  for (const auto& lv : ladder) {
    if (p < lv.cutoff && lv.cutoff <= best) {
      best = lv.cutoff;
      mark = lv.mark;
    }
  }
  return mark;
}

// "* p<0.05, ** p<0.01, *** p<0.001" -- ascending mark width, which is
// DESCENDING cutoff, and the order t_plain.txt prints.
std::string etStarNote(const std::vector<EtStar>& ladder, bool tex) {
  std::vector<EtStar> sorted = ladder;
  std::sort(sorted.begin(), sorted.end(),
            [](const EtStar& a, const EtStar& b) { return a.cutoff > b.cutoff; });
  std::string s;
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i) s += ", ";
    if (tex) {
      s += "\\sym{" + sorted[i].mark + "} \\(p<" + sorted[i].text + "\\)";
    } else {
      s += sorted[i].mark + " p<" + sorted[i].text;
    }
  }
  return s;
}

// --- the table esttab lays out ---------------------------------------------
struct EtCell {
  std::string v;
  std::string star;
};
struct EtRow {
  std::string label;
  std::vector<EtCell> cells;
  bool gapAfter = false;
  // W1a-5e: `labcol2()` inserts ONE extra column between the stub and the
  // model columns -- 456 corpus uses. Empty on every row that does not carry a
  // value, which is every head row but the numbers one, every aux row and every
  // footer row (w1b_labcol2.log k1/k8).
  std::string labcol;
};

// One column is " " + right(value, modelwidth) + left(stars, starwidth).
// varwidth/modelwidth are NOT derived from the content: esttab.ado:34-35 fixes
// them at 12 (20 under `label`) and 12 (22 for a default-cells `ci` table,
// esttab.ado:481 `2*modelwidth-2`). Deriving them from the widest cell was the
// first thing tried here and it gets t_label.txt's 52-column rule wrong -- the
// widest label there is 13, and the rule is 20 because the OPTION says 20.
struct EtLayout {
  // `labcol2()`'s field. 13 wide in the fixed renderer and its content is
  // CENTRED, measured off the oracle's own columns: the rule grows 44 -> 57,
  // `Extra` sits at 16-20 and `1.5` at 17-19 inside the field 12..24, which is
  // centring and not either padding (w1b_esttab_resid.log y15).
  bool labcol2On = false;
  static constexpr size_t kLabcol2Width = 13;
  size_t varwidth = 12;
  size_t modelwidth = 12;
  size_t starwidth = 3;
};

// BUG-490: esttab ABBREVIATES a stub label that does not fit, and the rule is
// not "truncate" -- a 13-character label comes back as 10+`~`+1 while a
// 31-character one comes back as 6+`~`+5. Measured across twelve arms at two
// widths (tests/eval/golden/aer_w1/w1b_esttab_abbrev2.log): keep the maximal
// TRAILING RUN OF DIGITS -- or, when there is none, the last character -- then
// as much of the head as fits, joined by `~`, to exactly `w` characters.
// Mid-string digits are not a tail (`abc123defg~p`) and underscores are
// ordinary characters (`log_income~a`). The same rule applies to a variable
// NAME, not only a label (arm `name_default`: `a_very_lon~e`).
//
// The one shape the capture does NOT cover is a label whose trailing digit run
// is longer than the width itself; the head would vanish. The cap below keeps
// at least one head character, which is a well-formedness choice and not a
// measurement, and it is written here rather than left implicit.
std::string etAbbrev(const std::string& s, size_t w) {
  if (s.size() <= w || w < 3) return s;
  size_t k = 0;
  while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[s.size() - 1 - k]))) ++k;
  if (k == 0) k = 1;
  if (k > w - 2) k = w - 2;
  return s.substr(0, w - 1 - k) + "~" + s.substr(s.size() - k);
}

std::string etPadRight(const std::string& s, size_t w) {
  return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}
std::string etPadLeft(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// Centre `s` in `w` columns, the extra space going to the RIGHT when the
// remainder is odd -- which is what `Extra` (5 in 13, 4 left / 4 right) and
// `1.5` (3 in 13, 5 left / 5 right) both show.
std::string etCentre(const std::string& s, size_t w) {
  if (s.size() >= w) return s;
  const size_t left = (w - s.size()) / 2;
  return std::string(left, ' ') + s + std::string(w - s.size() - left, ' ');
}

std::string etFixedLine(const std::string& label, const std::vector<EtCell>& cells, size_t k,
                        const EtLayout& lay, const std::string& labcol = std::string()) {
  std::string line = etPadRight(label, lay.varwidth);
  if (lay.labcol2On) line += etCentre(labcol, EtLayout::kLabcol2Width);
  for (size_t c = 0; c < k; ++c) {
    const EtCell& cell = c < cells.size() ? cells[c] : EtCell{};
    line += " " + etPadLeft(cell.v, lay.modelwidth) + etPadRight(cell.star, lay.starwidth);
  }
  return line;
}

std::string etTexLine(const std::string& label, const std::vector<EtCell>& cells, size_t k,
                      const EtLayout& lay, const std::string& labcol = std::string()) {
  std::string line = etPadRight(label, lay.varwidth);
  // In tex the extra column is a plain `&` cell centred in the model width --
  // NOT wrapped in \multicolumn the way the head cells are (k5:
  // `            &   Extra    &\multicolumn{1}{c}{(1)}`).
  if (lay.labcol2On) line += "&" + etCentre(labcol, lay.modelwidth);
  for (size_t c = 0; c < k; ++c) {
    const EtCell& cell = c < cells.size() ? cells[c] : EtCell{};
    const std::string sym = cell.star.empty() ? std::string() : "\\sym{" + cell.star + "}";
    // 9 = the width of "\sym{***}", the widest mark this ladder emits; t_tex.tex
    // pads every star field to it so the `\\` line ends align.
    line += "&" + etPadLeft(cell.v, lay.modelwidth) + etPadRight(sym, 9);
  }
  return line + "\\\\";
}

}  // namespace

ojson writeEsttabFile(const ojson& esttabBlock, const std::string& path,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                      SessionState& session) {
  if (!esttabBlock.is_object() || !esttabBlock.contains("coef_rows")) {
    return tblError("esttab: nothing to write");
  }
  const std::string userPath = dequote(path);

  std::vector<std::string> names;
  if (esttabBlock.contains("names") && esttabBlock["names"].is_array()) {
    for (const auto& n : esttabBlock["names"]) {
      if (n.is_string()) names.push_back(n.get<std::string>());
    }
  }
  // `runEsttab` falls back to the ACTIVE estimate when the caller named no
  // models and the store is empty (`estpost summarize x y` then a bare
  // `esttab`), and the label it reports for that is NOT a stored name. Looking
  // it up in the store would answer "model not found" for a table that just
  // rendered on screen, so the active estimate is resolved directly here when
  // the store does not know the name.
  std::vector<Model> models;
  bool allStored = !names.empty();
  for (const auto& n : names) {
    // W2 esttabnames: "." is the expanded `esttab .` (active, unstored results);
    // collectModels resolves it from e() in its column position.
    if (n != "." && session.estimatesGet(n) == nullptr) allStored = false;
  }
  if (allStored) {
    ojson err;
    models = collectModels(session, "esttab", names, &err);
    if (models.empty()) return err;
  } else {
    Model m;
    ojson err;
    if (!activeModel(session, "esttab", &m, &err)) return err;
    if (!names.empty()) m.name = names[0];
    models.push_back(m);
  }
  const size_t k = models.size();

  // ---- format mode --------------------------------------------------------
  const std::string ext = lowerExt(userPath);
  // W1a-3: `style(tex)` reaches the same writer `tex`/`booktabs` already do;
  // `style(fixed)` is the default and was measured byte-identical to a plain
  // run (w1b_esttab_display.log x4). Anything else is refused BY NAME rather
  // than silently rendered in a style the caller did not ask for.
  std::string styleOpt;
  if (auto st = optStr(opts, "style")) styleOpt = strutil::lower(dequote(*st));
  if (!styleOpt.empty() && styleOpt != "tex" && styleOpt != "fixed") {
    return ojson(eval::unsupportedOption(
        "style", "esttab: style(" + styleOpt + ") has no writer in this build; only tex and "
                 "fixed are implemented"));
  }
  const bool wantTex = hasFlag(opts, "tex") || hasFlag(opts, "booktabs") || ext == ".tex" ||
                       styleOpt == "tex";
  const bool wantCsv = hasFlag(opts, "csv") || ext == ".csv";
  // W1a-5b: `nolabel` cancels the label TEXT. It does NOT cancel the width:
  // `label nolabel` prints `mpg`/`price` in a table still ruled at 52 columns,
  // because `label` widened the stub to 20 and nothing narrows it back
  // (w1b_stats_grammar.log s11 vs s12 vs s13). So the two effects are read from
  // two different flags rather than one.
  const bool labelWidth = hasFlag(opts, "label");
  const bool useLabels = labelWidth && !hasFlag(opts, "nolabel");
  if (useLabels) {
    // `session.eval.varLabels` is a PROJECTION of the live varlist card, and
    // dispatcher.cpp:2666-2674 refreshes it only for a line carrying a colon
    // plus `label`/`local`/`global` (BUG-208's gate, written for `local l :
    // variable label x`). `esttab ..., label` carries neither, so before this
    // call the projection was EMPTY and the option rendered variable NAMES
    // under a request for variable LABELS -- t_label.txt, 10 cells. The refresh
    // is paid only when `label` was actually asked for, which keeps the MAXPERF
    // zero-Datastore assertion on ordinary verbs intact.
    // The same gap is still live for `estout ..., label` and `outreg2 ...,
    // label` (handlers below); named in the W5 report, not fixed behind it.
    session.syncEvalVarLabels();
  }

  // ---- which statistic goes in the parentheses ----------------------------
  // esttab.ado:568-575: the aux row is `t` unless exactly one of z/se/p/ci says
  // otherwise. `not` removes the row entirely (t_not.txt).
  enum class Aux { kNone, kT, kSe, kP, kCi };
  Aux aux = Aux::kT;
  const bool hasCi = hasFlag(opts, "ci") || optStr(opts, "ci").has_value();
  if (hasFlag(opts, "se")) aux = Aux::kSe;
  if (hasFlag(opts, "p")) aux = Aux::kP;
  if (hasCi) aux = Aux::kCi;
  if (hasFlag(opts, "not")) aux = Aux::kNone;
  const bool wantBeta = hasFlag(opts, "beta") || optStr(opts, "beta").has_value();

  EtFmt bFmt;  // a3
  if (auto b = optStr(opts, "b")) bFmt = parseEtFmt(*b, bFmt);
  EtFmt betaFmt{false, 3};  // esttab.ado:557 `if "`betafmt'"=="" local betafmt 3`
  if (auto b = optStr(opts, "beta")) betaFmt = parseEtFmt(*b, betaFmt);
  EtFmt auxFmt;
  switch (aux) {
    case Aux::kT: auxFmt = EtFmt{false, 2}; break;   // esttab.ado:687 tfmt 2
    case Aux::kP: auxFmt = EtFmt{false, 3}; break;   // esttab.ado:690 pfmt 3
    case Aux::kSe: auxFmt = bFmt; break;             // esttab.ado:689 sefmt = bfmt
    case Aux::kCi: auxFmt = bFmt; break;             // esttab.ado:691 cifmt = bfmt
    case Aux::kNone: break;
  }
  if (auto v = optStr(opts, "t")) auxFmt = parseEtFmt(*v, auxFmt);
  if (auto v = optStr(opts, "se")) auxFmt = parseEtFmt(*v, auxFmt);
  if (auto v = optStr(opts, "p")) auxFmt = parseEtFmt(*v, auxFmt);

  // ---- W1a-2 / BUG-407: cells(), and its sugar main()/aux() ---------------
  //
  // esttab has NO cell renderer of its own. When `cells()` is absent it BUILDS
  // a spec out of main/aux and hands that to estout -- esttab.ado:550-658, and
  // the translation is literal: `main(se 2)` becomes `se(fmt(2))`, `aux(t 2)`
  // becomes `t(fmt(2) par)`, and with neither it is `b(fmt(a3) star)` over
  // `t(fmt(2) par)`. So one path serves all three here and the estout-side
  // parser is reused rather than a second grammar written
  // (`parseCells`/`CellSpec`, already the only thing that reads this syntax).
  //
  // WHAT IS DELIBERATELY NOT DONE. Only ONE or TWO items are accepted, because
  // that is what this writer's row model is: a coefficient row and an aux row.
  // A three-item spec, and the quoted side-by-side form (which is what `wide`
  // becomes at esttab.ado:657), are REFUSED BY NAME -- accepting either and
  // rendering two rows would be the silent-wrong-layout shape BUG-407 was
  // filed against in the first place.
  std::string mainStat = wantBeta ? "beta" : "b";
  bool cellsGiven = false;
  std::string mainNoteName, auxNoteName, cellNameRow;
  std::optional<std::string> cellParL, cellParR;      // aux row, under cells()
  std::optional<std::string> mainParL, mainParR;      // main row, under cells()
  // `aux(none)` is NOT `not`. `not` removes the aux row and every gap with it
  // (t_not.txt). `aux(none)` names a cell estout does not know, so the row is
  // still EMITTED -- 44 columns of spaces -- and the inter-coefficient gap
  // survives (w1b_tables_pins.log c4, lines 5/8/11/14 are blank-but-present).
  // The first cut of W1a-2 mapped both to the same enum and silently dropped
  // four rows out of that table; the adversarial read of the ado is what
  // caught it, and this flag is the difference.
  bool auxRowBlank = false;
  bool cellStarMain = !hasFlag(opts, "nostar");
  {
    std::vector<CellSpec> specs;
    bool haveSpecs = false;
    if (auto c = optStr(opts, "cells")) {
      std::string cerr;
      bool sideBySide = false;
      if (!parseCells(*c, &specs, &cerr, &sideBySide)) return tblSyntax(cerr);
      if (sideBySide) {
        return ojson(eval::unsupportedOption(
            "cells", "esttab: the QUOTED cells(\"a b\") form is estout's side-by-side layout "
                     "(the same thing `wide` becomes), and this writer renders a coefficient row "
                     "over an aux row; it is refused rather than rendered stacked"));
      }
      if (specs.empty() || specs.size() > 2) {
        return ojson(eval::unsupportedOption(
            "cells", "esttab: this build renders a coefficient row and one aux row, so cells() "
                     "takes one or two items; got " + std::to_string(specs.size())));
      }
      haveSpecs = true;
      cellsGiven = true;
    } else if (optStr(opts, "main").has_value() || optStr(opts, "aux").has_value()) {
      // esttab.ado:565-569 and :584-586 -- `main`/`aux` are `name [fmt]`.
      auto tok = [](const std::string& raw) {
        const std::vector<std::string> t = splitWs(dequote(raw));
        return t;
      };
      CellSpec m;
      m.name = mainStat;
      if (auto mo = optStr(opts, "main")) {
        const std::vector<std::string> t = tok(*mo);
        if (!t.empty()) m.name = strutil::lower(t[0]);
        if (t.size() > 1) {
          bool ok = true;
          m.fmt = normalizeFmt(t[1], &ok);
          m.fmtGiven = ok;
          if (!ok) return tblSyntax("esttab: main(" + *mo + ") -- the second word is a format");
        }
      }
      m.star = cellStarMain;
      specs.push_back(m);
      if (auto ao = optStr(opts, "aux")) {
        const std::vector<std::string> t = tok(*ao);
        CellSpec a;
        a.name = t.empty() ? std::string("t") : strutil::lower(t[0]);
        if (t.size() > 1) {
          bool ok = true;
          a.fmt = normalizeFmt(t[1], &ok);
          a.fmtGiven = ok;
          if (!ok) return tblSyntax("esttab: aux(" + *ao + ") -- the second word is a format");
        }
        a.par = true;
        specs.push_back(a);
      } else {
        // esttab.ado:588 `if `"`aux'"'=="" local aux t` -- a bare main() still
        // carries the default aux row. Read off the ado, not captured: the two
        // captured arms (c4, c5) both name aux explicitly.
        CellSpec a;
        a.name = "t";
        a.par = true;
        specs.push_back(a);
      }
      haveSpecs = true;
    }
    if (haveSpecs) {
      // BUG-407 / D4, the one that prints a WRONG NUMBER. Under `cells()`
      // esttab does not apply its OWN defaults at all -- esttab.ado:1050 hands
      // `macval(cells)` to estout verbatim, so estout's rules govern: the
      // format default is %9.0g, not esttab's adaptive `a3`, and a spec with no
      // fmt() inherits the RUNNING format from the spec before it. Oracle c3,
      // `cells(b(star) p(par fmt(3)))`: the coefficient is `-49.51222`, which
      // is %9.0g; the first cut of this fix rendered `-49.51` because it left
      // esttab's a3 in place. c1 is the other half of the same rule -- `se(par)`
      // with no fmt takes fmt(%9.3f) from the `b` before it and prints
      // `(86.156)`, three decimals, not %9.0g.
      if (cellsGiven) {
        bFmt = EtFmt{};
        bFmt.stataG = true;
        auxFmt = bFmt;
      }
      const CellSpec& m = specs[0];
      mainStat = (m.name == "coef") ? "b" : m.name;
      mainNoteName = mainStat;
      if (m.fmtGiven) bFmt = parseEtFmt(m.fmt, bFmt);
      if (cellsGiven) {
        cellStarMain = m.star && !hasFlag(opts, "nostar");
        // A one-item cells() puts its statistic in the MAIN row, and `par` then
        // belongs to that row: `cells(se(par([ ]) fmt(2)))` renders `[86.16]`
        // as the coefficient cell with no aux row at all (c7). The first cut of
        // this fix wired `par` to the aux row only and the selftest caught it
        // on exactly that arm.
        if (m.par) {
          mainParL = m.parL;
          mainParR = m.parR;
        }
      }
      if (mainStat != "b" && mainStat != "beta" && mainStat != "se" && mainStat != "t" &&
          mainStat != "p") {
        return ojson(eval::unsupportedOption(
            "cells", "esttab: `" + mainStat + "` is not a statistic this writer can put in the "
                     "coefficient row (b, beta, se, t, p)"));
      }
      {
        std::string joined;
        for (const auto& cs : specs) {
          if (!joined.empty()) joined += "/";
          joined += cs.name;
        }
        if (cellsGiven) cellNameRow = joined;
      }
      if (specs.size() < 2) {
        aux = Aux::kNone;
      } else {
        const CellSpec& a = specs[1];
        auxNoteName = a.name;
        if (a.name == "t") aux = Aux::kT;
        else if (a.name == "se" || a.name == "stderr") aux = Aux::kSe;
        else if (a.name == "p") aux = Aux::kP;
        else if (a.name == "ci") aux = Aux::kCi;
        else if (a.name == "none") {
          aux = Aux::kNone;   // no statistic to read...
          auxRowBlank = true;  // ...but the row still occupies its line
        }
        else {
          return ojson(eval::unsupportedOption(
              "cells", "esttab: `" + a.name + "` is not a statistic this writer can put in the "
                       "aux row (t, se, p, ci, none)"));
        }
        if (aux != Aux::kNone) {
          auxFmt = a.fmtGiven
                       ? parseEtFmt(a.fmt, auxFmt)
                       : (cellsGiven ? bFmt  // the RUNNING format, c1's `(86.156)`
                                     : (aux == Aux::kT ? EtFmt{false, 2}
                                                       : (aux == Aux::kP ? EtFmt{false, 3}
                                                                         : bFmt)));
          if (cellsGiven) {
            // Under cells() the parentheses are the `par` SUBOPTION's, not a
            // default: `cells(b(star) p(par fmt(3)))` parenthesises and
            // `cells(b se)` does not. Outside cells() the aux row is always
            // parenthesised (esttab.ado:600-640 builds `par` into the spec).
            if (a.par) {
              cellParL = a.parL;
              cellParR = a.parR;
            } else {
              cellParL = std::string();
              cellParR = std::string();
            }
          }
        }
      }
    }
  }
  if (auto v = optStr(opts, "ci")) auxFmt = parseEtFmt(*v, auxFmt);

  std::vector<EtStar> stars = esttabDefaultStars();
  if (auto sl = optStr(opts, "starlevels")) stars = parseEtStars(*sl, stars);
  if (auto st = optStr(opts, "star")) stars = parseEtStars(*st, stars);
  const bool noStars = hasFlag(opts, "nostar");

  // `level(#)` labels the interval AND, in real esttab, selects it. This build
  // reads the interval the ESTIMATOR stored (see the Aux::kCi arm below), so a
  // level other than the estimation's own cannot be honoured -- and printing a
  // 95% interval under a "90% confidence intervals" legend is exactly the
  // silent-wrong-answer shape. Refused by name instead.
  int level = 95;
  if (auto lv = optStr(opts, "level")) {
    int n = 0;
    try {
      n = std::stoi(dequote(*lv));
    } catch (...) {
      n = 0;
    }
    if (n != 95) {
      return ojson(eval::unsupportedOption(
          "level",
          "esttab reports the confidence interval the estimation stored (its own level, 95 "
          "by default); this build cannot re-derive it at another level, so level(" +
              dequote(*lv) + ") is refused rather than mislabelled"));
    }
    level = n;
  }

  // ---- rename()/refcat(): both are coefficient-name keyed ------------------
  // SPLIT FIRST, DEQUOTE EACH TOKEN. `substitute("_cons" "CONST")` arrives as
  // the eight characters `"_cons" "CONST"`, whose first and last characters are
  // both `"` -- so dequoting the WHOLE value before splitting produces the two
  // tokens `_cons"` and `"CONST` and the substitution silently never matches.
  // That was live in t_mlab/t_mgroups/t_subst on the first pass here.
  std::map<std::string, std::string> renameMap;
  if (auto rn = optStr(opts, "rename")) {
    const std::vector<std::string> toks = splitTopLevel(*rn);
    for (size_t i = 0; i + 1 < toks.size(); i += 2) {
      renameMap[dequote(toks[i])] = dequote(toks[i + 1]);
    }
  }
  std::vector<std::pair<std::string, std::string>> refcats;
  if (auto rc = optStr(opts, "refcat")) {
    const std::vector<std::string> toks = splitTopLevel(*rc);
    for (size_t i = 0; i + 1 < toks.size(); i += 2) {
      refcats.emplace_back(dequote(toks[i]), dequote(toks[i + 1]));
    }
  }
  std::map<std::string, std::string> coefLabels;
  if (auto cl = optStr(opts, "coeflabels")) coefLabels = parseLabelPairs(*cl);
  // W1a-3: `varlabels()` is estout's spelling of the same name/label pairs (11
  // papers / 152 uses write it). Merged rather than aliased so a line carrying
  // both keeps every pair; `coeflabels()` wins a collision because it is
  // esttab's own option name.
  if (auto vl = optStr(opts, "varlabels")) {
    for (const auto& kv : parseLabelPairs(*vl)) coefLabels.emplace(kv.first, kv.second);
  }

  // ---- row set ------------------------------------------------------------
  std::vector<std::string> wanted;
  for (const auto& cr : esttabBlock["coef_rows"]) wanted.push_back(cr.value("name", std::string()));
  // `_cons` LAST. The engine's coeftable puts the intercept first; Stata's e(b)
  // puts it last and every one of the 27 captured tables shows it last.
  {
    std::vector<std::string> reordered;
    for (const auto& n : wanted) {
      if (n != "_cons") reordered.push_back(n);
    }
    if (std::find(wanted.begin(), wanted.end(), "_cons") != wanted.end()) {
      reordered.push_back("_cons");
    }
    wanted = reordered;
  }
  // `beta` implies noconstant (esttab.ado:401-403): the standardized table in
  // t_beta.txt has no `_cons` row at all.
  if (wantBeta) {
    wanted.erase(std::remove(wanted.begin(), wanted.end(), "_cons"), wanted.end());
  }
  // ONE filter, shared with estout/outreg2's `coefNames` (BUG-496).
  {
    std::string selErr;
    applyCoefSelection(&wanted, optStr(opts, "keep"), optStr(opts, "drop"), optStr(opts, "order"),
                       &selErr);
    if (!selErr.empty()) return tblErrorRc(selErr, 111);
  }

  // W1a-4: `indicate("Label = varlist"[, labels(present absent)])` -- ~47 corpus
  // uses over 5 papers, almost all `indicate("Full control = Sex_M", labels(Yes))`.
  // It REPLACES the named coefficients with ONE row per group saying whether they
  // were in that model. Measured (tests/eval/golden/aer_w1/w1b_indicate.log):
  //   n1  the `hi`/`lo` rows are GONE and `Controls  Yes  Yes` sits after `_cons`
  //       and before the stats block, with a blank line above it
  //   n6  a model that does NOT have them says `No` -- which is the arm that
  //       settles labels()'s two halves, since with every model carrying the
  //       varlist both cells read Yes and labels(Yes) is indistinguishable from
  //       the default (n2 is byte-identical to n1)
  //   n7  with no `=` the row's label is the VARLIST text itself
  //   n4  the varlist takes GLOBS: `h*` removes `hi` and leaves `lo` a coefficient
  //   n5  two quoted groups make two rows
  struct IndGroup {
    std::string label;
    std::vector<std::string> pats;
  };
  std::vector<IndGroup> indGroups;
  std::string indYes = "Yes", indNo = "No";
  if (auto ind = optStr(opts, "indicate")) {
    const std::string raw = *ind;
    // The `, labels()` suboption is at the first TOP-LEVEL comma; the groups
    // themselves are quoted, so their own commas and spaces are safe.
    std::string groupsPart = raw, subPart;
    {
      int depth = 0;
      bool inQ = false;
      for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') inQ = !inQ;
        if (!inQ && (c == '(' || c == '[')) ++depth;
        if (!inQ && (c == ')' || c == ']')) --depth;
        if (!inQ && depth == 0 && c == ',') {
          groupsPart = raw.substr(0, i);
          subPart = raw.substr(i + 1);
          break;
        }
      }
    }
    if (!strutil::trim(subPart).empty()) {
      const std::string sub = strutil::trim(subPart);
      const size_t open = sub.find('(');
      const size_t close = sub.rfind(')');
      if (open == std::string::npos || close == std::string::npos || close < open ||
          strutil::lower(strutil::trim(sub.substr(0, open))) != "labels") {
        return tblSyntax("esttab indicate(): the only suboption is labels(); `" + sub +
                         "` names something else");
      }
      const std::vector<std::string> ls = splitTopLevel(sub.substr(open + 1, close - open - 1));
      if (!ls.empty()) indYes = dequote(ls[0]);
      if (ls.size() > 1) indNo = dequote(ls[1]);
    }
    for (const auto& g : splitTopLevel(groupsPart)) {
      const std::string txt = dequote(g);
      IndGroup ig;
      const size_t eq = txt.find('=');
      const std::string vars = (eq == std::string::npos) ? txt : txt.substr(eq + 1);
      ig.label = (eq == std::string::npos) ? strutil::trim(txt) : strutil::trim(txt.substr(0, eq));
      for (const auto& v : splitWs(vars)) ig.pats.push_back(v);
      if (!ig.pats.empty()) indGroups.push_back(ig);
    }
    // The coefficients a group stands for leave the body.
    std::vector<std::string> keptNames;
    for (const auto& n : wanted) {
      bool taken = false;
      for (const auto& ig : indGroups) {
        for (const auto& pat : ig.pats) {
          if (strutil::globMatch(pat, n)) taken = true;
        }
      }
      if (!taken) keptNames.push_back(n);
    }
    wanted = keptNames;
  }
  // W1a-3: `nocons`/`noconstant` drop the intercept's rows (x6 against the
  // plain control; the two spellings are byte-identical to each other).
  if (hasFlag(opts, "nocons") || hasFlag(opts, "noconstant")) {
    wanted.erase(std::remove(wanted.begin(), wanted.end(), "_cons"), wanted.end());
  }
  // ...and `noomit`/`noomitted` drop a coefficient the fit OMITTED (x13 vs
  // x14). The marker available here is the engine's, not Stata's: cobra gives a
  // collinear column b=0 se=0 (Stata gives b=0 with MISSING se, which is
  // BUG-491), so the test is that pair, and it is written out rather than
  // hidden behind a helper because it is the weakest thing in this function.
  // A coefficient is dropped only when EVERY model that carries it omitted it:
  // the mixed case is UNMEASURED (the capture fits one model) and keeping the
  // row is the side that loses no number.
  if (hasFlag(opts, "noomit") || hasFlag(opts, "noomitted")) {
    std::vector<std::string> kept;
    for (const auto& n : wanted) {
      bool sawCell = false, allOmitted = true;
      for (const auto& cr : esttabBlock["coef_rows"]) {
        if (cr.value("name", std::string()) != n) continue;
        if (!cr.contains("cells") || !cr["cells"].is_array()) continue;
        for (const auto& c : cr["cells"]) {
          if (!c.is_object() || !c.contains("b") || !c["b"].is_number()) continue;
          sawCell = true;
          const double b = c["b"].get<double>();
          const double se = (c.contains("se") && c["se"].is_number()) ? c["se"].get<double>() : 1.0;
          if (!(b == 0.0 && se == 0.0)) allOmitted = false;
        }
      }
      if (!(sawCell && allOmitted)) kept.push_back(n);
    }
    wanted = kept;
  }

  // ---- beta: b * sd(x)/sd(y) over e(sample) -------------------------------
  // The receipt (milf-coord/plans/b4-receipts-20260909/esttab-beta) settles the
  // owner and the formula: `esttab, beta` calls `estadd beta, replace`, and
  // estadd.ado:434/440/441 forms b * r(sd of x) / r(sd of y) from two
  // `summarize`s that both carry `if e(sample)`. Its REFUTE.md kills the N-vs-N-1
  // divisor as a test -- the common factor cancels in the ratio -- so the only
  // thing this code can get wrong is the SAMPLE, and that is what is guarded.
  //
  // PIET's estimates store carries no e(sample) mask, so the sample is
  // RECONSTRUCTED as the joint complete cases of {depvar, regressors} in the
  // dataset now in memory. That is exact for a plain `regress` with no if/in
  // and no intervening data change -- and it is CHECKED, one-sidedly: the
  // reconstructed row count must equal the model's own e(N). When it does not,
  // beta is REFUSED by name instead of printing a number from the wrong rows.
  std::map<std::string, std::map<std::string, double>> betaVals;  // model -> coef -> beta
  if (wantBeta) {
    if (!session.currentHbin().has_value()) {
      return ojson(eval::unsupportedOption(
          "beta", "esttab, beta needs sd(x)/sd(y) over e(sample) and no dataset is in memory"));
    }
    for (size_t mi = 0; mi < models.size(); ++mi) {
      const Model& m = models[mi];
      const std::string depvar = m.snap.value("depvar", std::string());
      double nObs = 0.0;
      if (m.snap.contains("n_obs") && m.snap["n_obs"].is_number()) {
        nObs = m.snap["n_obs"].get<double>();
      }
      if (depvar.empty() || nObs <= 1.0) {
        return ojson(eval::unsupportedOption(
            "beta", "esttab, beta needs e(sample) and model " + m.name +
                        " carries no dependent variable or no observation count"));
      }
      std::vector<std::string> vars{depvar};
      for (const auto& n : wanted) {
        if (n != "_cons") vars.push_back(n);
      }
      std::string ferr;
      try {
        // `session.datastore()`, not a fresh `Datastore(currentHbin())`: a
        // resident head is a VIRTUAL token with no file behind it until
        // ensureMaterializedHead() snapshots it, and opening the token directly
        // is the "cannot open <token>" failure BUG-124 documents. This is the
        // same accessor every other row-reading handler in this build uses.
        Datastore& ds = *session.datastore();
        std::map<std::string, const double*> cols;
        int64_t nrows = -1;
        for (const auto& v : vars) {
          if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) {
            ferr = "sd(" + v + ") is not available: " + v +
                   " is not a numeric variable in the dataset now in memory";
            break;
          }
          const cecon::Column& c = ds.col(v);
          const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (nrows < 0) nrows = n;
          if (n != nrows) {
            ferr = "the dataset columns disagree on length";
            break;
          }
          cols[v] = c.f64();
        }
        if (ferr.empty()) {
          std::vector<size_t> rows;
          for (int64_t i = 0; i < nrows; ++i) {
            bool ok = true;
            for (const auto& kv : cols) {
              if (std::isnan(kv.second[i])) ok = false;
            }
            if (ok) rows.push_back(static_cast<size_t>(i));
          }
          if (static_cast<double>(rows.size()) != nObs) {
            ferr = "this build reconstructs e(sample) as the complete cases of the model's own "
                   "variables, and for " + m.name + " that is " + std::to_string(rows.size()) +
                   " rows against e(N)=" + std::to_string(static_cast<long long>(nObs)) +
                   " -- the sample cannot be reproduced, so no standardized coefficient is printed";
          } else {
            auto sdOf = [&](const std::string& v) {
              const double* p = cols[v];
              double s = 0.0;
              for (size_t i : rows) s += p[i];
              const double mean = s / static_cast<double>(rows.size());
              double m2 = 0.0;
              for (size_t i : rows) {
                const double d = p[i] - mean;
                m2 += d * d;
              }
              return std::sqrt(m2 / (static_cast<double>(rows.size()) - 1.0));
            };
            const double sdy = sdOf(depvar);
            for (const auto& cr : esttabBlock["coef_rows"]) {
              const std::string cn = cr.value("name", std::string());
              if (cn == "_cons" || cols.count(cn) == 0) continue;
              if (!cr.contains("cells") || mi >= cr["cells"].size()) continue;
              const ojson& cell = cr["cells"][mi];
              if (cell.is_null() || !cell.contains("b") || !cell["b"].is_number()) continue;
              betaVals[m.name][cn] = cell["b"].get<double>() * sdOf(cn) / sdy;
            }
          }
        }
      } catch (const std::exception& e) {
        ferr = std::string("the dataset could not be read (") + e.what() + ")";
      }
      if (!ferr.empty()) return ojson(eval::unsupportedOption("beta", "esttab, beta: " + ferr));
    }
  }

  // ---- head rows ----------------------------------------------------------
  // W1a-5e: `labcol2(v1 v2 ..., title(T))`. The values are POSITIONAL over the
  // coefficient rows and the title rides in the numbers head row (k1 vs k2:
  // without a title that row's field is blank).
  std::vector<std::string> labcol2Vals;
  std::string labcol2Title;
  const bool labcol2On = optStr(opts, "labcol2").has_value();
  if (auto lc = optStr(opts, "labcol2")) {
    const std::vector<std::string> halves = splitTopLevel(*lc, ',');
    if (!halves.empty()) {
      for (const auto& t : splitTopLevel(strutil::trim(halves[0]))) {
        labcol2Vals.push_back(dequote(t));
      }
    }
    for (size_t h = 1; h < halves.size(); ++h) {
      const std::string sub = strutil::trim(halves[h]);
      const size_t at = sub.find("title(");
      if (at == std::string::npos) continue;
      const size_t close = sub.find(')', at);
      if (close != std::string::npos) {
        labcol2Title = dequote(sub.substr(at + 6, close - at - 6));
      }
    }
  }

  std::vector<EtRow> heads;
  // mgroups("G1" "G2", pattern(1 1)) -- a spanning row ABOVE the numbers row.
  if (auto mg = optStr(opts, "mgroups")) {
    const std::string raw = *mg;
    std::string labelPart = raw;
    std::string subPart;
    const std::vector<std::string> halves = splitTopLevel(raw, ',');
    if (halves.size() >= 2) {
      labelPart = halves[0];
      subPart = halves[1];
      for (size_t i = 2; i < halves.size(); ++i) subPart += "," + halves[i];
    }
    std::vector<std::string> labs = splitTopLevel(strutil::trim(labelPart));
    for (auto& l : labs) l = dequote(l);
    std::vector<int> pattern(k, 1);
    const size_t pat = subPart.find("pattern(");
    if (pat != std::string::npos) {
      const size_t close = subPart.find(')', pat);
      if (close != std::string::npos) {
        const std::vector<std::string> pt =
            splitWs(subPart.substr(pat + 8, close - pat - 8));
        for (size_t i = 0; i < pt.size() && i < k; ++i) pattern[i] = (pt[i] == "1") ? 1 : 0;
      }
    }
    EtRow r;
    size_t next = 0;
    for (size_t c = 0; c < k; ++c) {
      EtCell cell;
      if (pattern[c] == 1 && next < labs.size()) cell.v = labs[next++];
      r.cells.push_back(cell);
    }
    heads.push_back(r);
  }
  if (!hasFlag(opts, "nonumbers")) {
    EtRow r;
    r.labcol = labcol2Title;   // k1 puts `Extra` here; k2 leaves it blank
    for (size_t c = 0; c < k; ++c) r.cells.push_back(EtCell{"(" + std::to_string(c + 1) + ")", ""});
    heads.push_back(r);
  }
  if (!hasFlag(opts, "nomtitles")) {
    std::vector<std::string> labs;
    std::optional<std::string> ml = optStr(opts, "mlabels");
    if (!ml.has_value()) ml = optStr(opts, "mtitles");
    if (ml.has_value()) {
      labs = splitTopLevel(*ml);
      for (auto& l : labs) l = dequote(l);
    }
    EtRow r;
    for (size_t c = 0; c < k; ++c) {
      std::string h;
      if (c < labs.size()) {
        h = labs[c];
      } else if (hasFlag(opts, "nodepvars")) {
        // `nodepvars` heads the columns with the STORED NAME (t_nodep.txt: m1/m2).
        h = models[c].name;
      } else {
        h = models[c].snap.value("depvar", std::string());
        if (h.empty()) h = models[c].name;
        if (useLabels) {
          const std::string lbl = session.eval.varLabels.get(h);
          if (!lbl.empty()) h = lbl;
        }
      }
      r.cells.push_back(EtCell{h, ""});
    }
    heads.push_back(r);
  }
  // collabels("coef") adds a further head row, one label repeated across the
  // columns (t_collab.txt). Only the single-label form is captured; a longer
  // list emits one row per label, which is the same rule extended.
  if (auto cl = optStr(opts, "collabels")) {
    std::vector<std::string> labs = splitTopLevel(*cl);
    for (auto& l : labs) l = dequote(l);
    for (const auto& l : labs) {
      EtRow r;
      for (size_t c = 0; c < k; ++c) r.cells.push_back(EtCell{l, ""});
      heads.push_back(r);
    }
  } else if (cellsGiven && !cellNameRow.empty()) {
    // BUG-407 / D3: under cells() estout's CELL-NAME row comes through, joined
    // with `/` when the spec stacks two items -- `b/se` (c1), `b/p` (c3), `se`
    // alone (c7). esttab injects `collabels(none)` to suppress it only on the
    // no-cells path (esttab.ado:732-733), which is exactly why this row appears
    // here and nowhere else.
    EtRow r;
    for (size_t c = 0; c < k; ++c) r.cells.push_back(EtCell{cellNameRow, ""});
    heads.push_back(r);
  }

  // ---- body ---------------------------------------------------------------
  auto cellFor = [&](const ojson& cr, size_t mi) -> const ojson* {
    if (!cr.contains("cells") || mi >= cr["cells"].size()) return nullptr;
    const ojson& c = cr["cells"][mi];
    return c.is_null() ? nullptr : &c;
  };
  auto num = [](const ojson& c, const char* key) {
    return (c.contains(key) && c[key].is_number()) ? c[key].get<double>()
                                                   : std::numeric_limits<double>::quiet_NaN();
  };

  std::vector<EtRow> body;
  size_t coefSeen = 0;  // labcol2()'s positional counter over coefficient rows
  const bool auxOn = (aux != Aux::kNone) || auxRowBlank;
  for (const auto& coefName : wanted) {
    for (const auto& [refVar, refLab] : refcats) {
      if (refVar != coefName) continue;
      EtRow r;
      r.label = refLab;
      for (size_t c = 0; c < k; ++c) r.cells.push_back(EtCell{"ref.", ""});  // estout.ado:278
      r.gapAfter = true;
      body.push_back(r);
    }
    const ojson* src = nullptr;
    for (const auto& cr : esttabBlock["coef_rows"]) {
      if (cr.value("name", std::string()) == coefName) src = &cr;
    }
    if (src == nullptr) continue;
    std::string label = coefName;
    auto rn = renameMap.find(coefName);
    if (rn != renameMap.end()) label = rn->second;
    auto ex = coefLabels.find(coefName);
    if (ex != coefLabels.end()) {
      label = ex->second;
    } else if (useLabels) {
      if (coefName == "_cons") {
        label = "Constant";
      } else {
        const std::string lbl = session.eval.varLabels.get(label);
        if (!lbl.empty()) label = lbl;
      }
    }
    EtRow bRow, aRow;
    // BUG-490: the stub is abbreviated unless `noabbrev` says otherwise. The
    // width is the SAME expression the renderer uses below -- one constant, so
    // the two cannot drift into abbreviating to a width the table does not
    // have.
    bRow.label = hasFlag(opts, "noabbrev") ? label : etAbbrev(label, labelWidth ? 20 : 12);
    bool anyAux = false;
    for (size_t mi = 0; mi < k; ++mi) {
      EtCell bc, ac;
      const ojson* c = cellFor(*src, mi);
      if (c != nullptr) {
        const double p = num(*c, "p");
        if (wantBeta) {
          auto bm = betaVals.find(models[mi].name);
          if (bm != betaVals.end()) {
            auto bv = bm->second.find(coefName);
            if (bv != bm->second.end()) bc.v = betaFmt.apply(bv->second);
          }
        } else if (c->value("omitted", false)) {
          bc.v = "(omitted)";
        } else if (mainStat == "se") {
          bc.v = bFmt.apply(num(*c, "se"));
        } else if (mainStat == "t") {
          bc.v = bFmt.apply(num(*c, "t"));
        } else if (mainStat == "p") {
          bc.v = bFmt.apply(num(*c, "p"));
        } else {
          bc.v = bFmt.apply(num(*c, "b"));
        }
        // W1a-2: under cells() the star is the spec's `star` SUBOPTION, not the
        // default -- `cells(b(star fmt(%9.3f)) se(par))` stars the coefficient
        // (c1) and a bare `cells(b se)` does not. `nostar` still wins.
        if (!bc.v.empty() && (cellsGiven ? cellStarMain : !noStars)) {
          bc.star = etStarsFor(p, stars);
        }
        if (mainParL.has_value() && !bc.v.empty() && bc.v != kMissingCell) {
          bc.v = *mainParL + bc.v + mainParR.value_or(")");
        }
        switch (aux) {
          case Aux::kNone: break;
          case Aux::kT: ac.v = auxFmt.apply(num(*c, "t")); break;
          case Aux::kSe: ac.v = auxFmt.apply(num(*c, "se")); break;
          case Aux::kP: ac.v = auxFmt.apply(num(*c, "p")); break;
          case Aux::kCi: {
            // The STORED interval, not b +/- z*se. estout reads e(ci_l)/e(ci_u),
            // which the estimator built from the t distribution on its own
            // residual df; rebuilding it from a Normal quantile here moved every
            // bound in t_ci.txt by ~3 (mpg: [-221.302,122.278] became
            // [-218.375,119.351]) -- inside no tolerance, and invisible without
            // the capture. So the interval is READ, never re-derived.
            const ojson* row = coefRow(models[mi].snap, coefName);
            if (row != nullptr && row->contains("ci_lo") && row->contains("ci_hi") &&
                (*row)["ci_lo"].is_number() && (*row)["ci_hi"].is_number()) {
              ac.v = "[" + auxFmt.apply((*row)["ci_lo"].get<double>()) + "," +
                     auxFmt.apply((*row)["ci_hi"].get<double>()) + "]";
            }
            break;
          }
        }
        if (aux != Aux::kCi && !ac.v.empty() && ac.v != kMissingCell) {
          // cellParL/R are engaged only under cells(), where the delimiters are
          // the spec's (`par([ ])` gives brackets, c7) and their ABSENCE means
          // no wrapping at all. Everywhere else the aux row is parenthesised,
          // which is what esttab builds into the spec it writes for itself.
          ac.v = cellParL.value_or("(") + ac.v + cellParR.value_or(")");
        } else if (aux != Aux::kCi && ac.v == kMissingCell) {
          ac.v.clear();
        }
        if (bc.v == kMissingCell) {
          bc.v.clear();
          bc.star.clear();
        }
      }
      if (!ac.v.empty()) anyAux = true;
      bRow.cells.push_back(bc);
      aRow.cells.push_back(ac);
    }
    bool anyB = false;
    for (const auto& cc : bRow.cells) {
      if (!cc.v.empty()) anyB = true;
    }
    if (!anyB) continue;
    // The blank line between coefficients exists only when the coefficient owns
    // more than one row: t_not.txt (`not`) has NO gaps at all, and that arm is
    // what distinguishes "gap after every coefficient" from "gap after the aux
    // row" -- the two rules agree on every other captured table.
    // W1a-5e: one value per COEFFICIENT, not per row -- the aux row under it
    // keeps a blank field (k1).
    if (labcol2On) {
      if (coefSeen < labcol2Vals.size()) bRow.labcol = labcol2Vals[coefSeen];
      ++coefSeen;
    }
    body.push_back(bRow);
    // BUG-407 / D2: the blank line between coefficients is esttab's, added at
    // esttab.ado:495-496 only when `nocellsopt==1`. Under cells() the table has
    // no gaps at all -- c1 and c3 run their coefficients consecutively.
    if (auxOn && (anyAux || auxRowBlank)) {
      // ...and a blank aux row still pads to the table's width, so the gap
      // rule below sees a two-row coefficient exactly as it does for a filled
      // one. `anyAux` alone would have dropped it.
      if (auxRowBlank) {
        aRow.cells.assign(k, EtCell{});
      }
      // W1a-5c: `nogaps` removes the blank line between coefficients, and it
      // was ACCEPTED AND IGNORED -- the written file still carried all three
      // (w1b_esttab_resid.log y6 against its control). That is the
      // accept-and-ignore shape this repo refuses to ship, and the name had
      // been sitting in the KNOWN list the whole time. `gaps` is the DEFAULT
      // and therefore a validated no-op (y5).
      aRow.gapAfter = !cellsGiven && !hasFlag(opts, "nogaps");
      body.push_back(aRow);
    }
  }
  // The indicate rows sit at the END of the body, after `_cons` and before the
  // stats rule, with a blank line above them (w1b_indicate.log n1). `Yes` when
  // the model has ANY coefficient the group names and `No` when it has none --
  // read off the cell, which is null for a coefficient the model does not carry.
  if (!indGroups.empty()) {
    if (!body.empty()) body.back().gapAfter = true;
    for (const auto& ig : indGroups) {
      EtRow r;
      r.label = ig.label;
      for (size_t c = 0; c < k; ++c) {
        bool present = false;
        for (const auto& cr : esttabBlock["coef_rows"]) {
          const std::string nm = cr.value("name", std::string());
          bool hit = false;
          for (const auto& pat : ig.pats) {
            if (strutil::globMatch(pat, nm)) hit = true;
          }
          if (!hit || !cr.contains("cells") || c >= cr["cells"].size()) continue;
          if (!cr["cells"][c].is_null()) present = true;
        }
        r.cells.push_back(EtCell{present ? indYes : indNo, ""});
      }
      // Each group's row takes a gap after it, like a coefficient block -- two
      // groups render with a blank line BETWEEN them (n5). The clearing of the
      // last row's gap below removes the one that would sit on the stats rule.
      r.gapAfter = true;
      body.push_back(r);
    }
  }
  if (!body.empty()) body.back().gapAfter = false;

  // ---- footer -------------------------------------------------------------
  // esttab.ado:665-680: `stats(N)` with the SHORT label set by default and the
  // LONG one under `label` (esttab.ado:26-27 -> "N" / "Observations").
  std::vector<EtRow> foot;
  {
    // W1a-5b: `stats()` carries a SUB-GRAMMAR that this build was splitting on
    // whitespace and rendering as rows. `stats(N r2, labels("Obs" "R2")
    // fmt(0 3))` became five garbage footer rows -- `r2,` / `labels("Obs"` /
    // `"R2")` / `fmt(0` / `3)` -- at rc 0, which is a wrong table nothing
    // stopped. Measured (tests/eval/golden/aer_w1/w1b_stats_grammar{,2}.log):
    //   * the comma splits KEYS from SUBOPTIONS.
    //   * labels() is POSITIONAL; a short list leaves the remaining keys with
    //     their own name (d3), a long one ignores the extras (d4).
    //   * fmt() is positional too, and a SINGLE entry applies to every key
    //     (d1: fmt(2 5) -> 74.00 / 0.29339; d2: fmt(4) -> 74.0000 / 0.2934).
    //   * `s()` is estout's abbreviation of the same option (s7).
    //   * `scalars("name label")` carries its own label pairs (s10, d10).
    std::vector<std::string> statKeys;
    std::vector<std::string> userLabels;
    std::vector<std::string> userFmts;
    std::map<std::string, std::string> pairLabels;   // scalars("key label")
    std::set<std::string> scalarKeys;                 // the rows sfmt governs
    auto parseStatSpec = [&](const std::string& raw, bool isScalars) {
      const std::vector<std::string> halves = splitTopLevel(raw, ',');
      const std::string keyPart = halves.empty() ? raw : halves[0];
      for (const auto& tok : splitTopLevel(strutil::trim(keyPart))) {
        // `scalars("r2 R-squared")` is a quoted NAME + LABEL pair; a bare token
        // is just a name (s9 vs s10).
        const std::string inner = dequote(tok);
        const std::vector<std::string> parts = splitWs(inner);
        if (isScalars && inner.size() != tok.size() && parts.size() >= 2) {
          statKeys.push_back(parts[0]);
          pairLabels[parts[0]] = joinWith(std::vector<std::string>(parts.begin() + 1, parts.end()),
                                          " ");
        } else {
          for (const auto& p : parts) statKeys.push_back(p);
        }
      }
      if (isScalars) {
        for (const auto& k : statKeys) scalarKeys.insert(k);
      }
      for (size_t h = 1; h < halves.size(); ++h) {
        const std::string sub = strutil::trim(halves[h]);
        auto take = [&](const char* name, std::vector<std::string>* out) {
          const std::string pre = std::string(name) + "(";
          const size_t at = sub.find(pre);
          if (at == std::string::npos) return;
          const size_t close = sub.find(')', at);
          if (close == std::string::npos) return;
          for (const auto& t : splitTopLevel(sub.substr(at + pre.size(),
                                                        close - at - pre.size()))) {
            out->push_back(dequote(t));
          }
        };
        take("labels", &userLabels);
        take("label", &userLabels);
        take("fmt", &userFmts);
      }
    };
    std::optional<std::string> statSpec = optStr(opts, "stats");
    if (!statSpec.has_value()) statSpec = optStr(opts, "s");  // estout's abbreviation
    bool fromScalars = false;
    if (!statSpec.has_value()) {
      statSpec = optStr(opts, "scalars");
      fromScalars = statSpec.has_value();
    }
    if (statSpec.has_value()) parseStatSpec(*statSpec, fromScalars);
    // `sfmt()` formats the SCALARS rows and nothing else. Twelve arms across
    // three captures showed it doing nothing against stats() keys, N, r2, the
    // r2/ar2 flags, `plain`, or `b()/se()`; the pair that identifies it is
    // w1b_sfmt_rule.log q5 vs q4 (`scalars(mymean)` 6165.3 -> `sfmt(5)`
    // 6165.25700). Positional, with the LAST entry repeating for extras --
    // esttab.ado:700-702, and q6/q7 measure both halves.
    std::vector<std::string> sFmts;
    if (auto sf = optStr(opts, "sfmt")) {
      for (const auto& t : splitTopLevel(dequote(*sf))) sFmts.push_back(dequote(t));
    }
    // The user's keys, in their order, are what labels()/fmt() count over; the
    // rows the FLAGS add are not part of that counting and keep the flags' own
    // pretty labels.
    const std::vector<std::string> userKeys = statKeys;
    std::map<std::string, size_t> userPos;
    for (size_t i = 0; i < userKeys.size(); ++i) userPos.emplace(userKeys[i], i);

    if (hasFlag(opts, "r2") && std::find(statKeys.begin(), statKeys.end(), "r2") == statKeys.end()) {
      statKeys.push_back("r2");
    }
    if (hasFlag(opts, "ar2") &&
        std::find(statKeys.begin(), statKeys.end(), "ar2") == statKeys.end()) {
      statKeys.push_back("ar2");
    }
    // W1a-5d: `pr2` / `pr2(#)` -- the pseudo-R2 row. Measured
    // (w1b_esttab_resid.log y8/y9): the label is `pseudo R-sq` and the default
    // is three decimals, with `pr2(4)` giving four. The LABELLED spelling is
    // UNMEASURED -- the captured arms carry no `label` -- so `label` leaves it
    // alone rather than inventing a long form to match R-squared's.
    const bool wantPr2 = hasFlag(opts, "pr2") || optStr(opts, "pr2").has_value();
    if (wantPr2 && std::find(statKeys.begin(), statKeys.end(), "pr2") == statKeys.end()) {
      statKeys.push_back("pr2");
    }
    if (!hasFlag(opts, "noobs") &&
        std::find(statKeys.begin(), statKeys.end(), "N") == statKeys.end()) {
      statKeys.insert(statKeys.begin(), "N");
    }
    auto pick = [](const std::vector<std::string>& v, size_t i) {
      // positional, with the LAST entry repeating -- esttab.ado:700-702, and
      // measured for both fmt() (d2: one entry for two keys) and sfmt (q7).
      return v.empty() ? std::string() : v[std::min(i, v.size() - 1)];
    };
    for (const auto& key : statKeys) {
      EtRow r;
      auto up = userPos.find(key);
      const bool isUserKey = up != userPos.end();
      auto pl = pairLabels.find(key);
      if (pl != pairLabels.end()) {
        r.label = pl->second;                       // scalars("key label")
      } else if (isUserKey && up->second < userLabels.size()) {
        r.label = userLabels[up->second];           // labels(), positional
      } else if (key == "N") {
        r.label = useLabels ? "Observations" : "N";
      } else if (!isUserKey && key == "r2") {
        r.label = useLabels ? "R-squared" : "R-sq";  // the FLAG's label...
      } else if (!isUserKey && (key == "ar2" || key == "r2_a")) {
        r.label = useLabels ? "Adjusted R-squared" : "adj. R-sq";
      } else if (!isUserKey && key == "pr2") {
        r.label = "pseudo R-sq";   // y8; the `label` long form is unmeasured
      } else {
        r.label = key;                               // ...a named key keeps its own
      }
      // The footer default is PER KEY, and the two captures that look like one
      // rule are not: esttab.ado:668-670 gives `r2`/`ar2` a fixed THREE
      // DECIMALS in a non-plain call while everything else takes `bfmt`, which
      // is `a3`. On the oracle's data the two agree (r2 = .2933891 renders
      // `0.293` either way, s1) and only a different fixture separates them --
      // r2 = .0098126 is `0.010` under three decimals and `0.00981` under a3.
      // `scalars(mymean)` on 6165.257 is `6165.3` (q5), which only a3 gives.
      const bool isR2Key = (key == "r2" || key == "ar2" || key == "r2_a" || key == "pr2");
      std::string fmt = (key == "N") ? "%9.0f" : (isR2Key ? "%9.3f" : "a3");
      // `pr2(4)` carries its own decimal count (y9), the same way `b(3)` does.
      if (key == "pr2") {
        if (auto pv = optStr(opts, "pr2")) {
          bool ok = true;
          const std::string norm = normalizeFmt(dequote(*pv), &ok);
          if (ok) fmt = norm;
        }
      }
      const std::string chosen = isUserKey && !userFmts.empty()
                                     ? pick(userFmts, up->second)
                                     : ((scalarKeys.count(key) != 0 && !sFmts.empty())
                                            ? pick(sFmts, isUserKey ? up->second : 0)
                                            : std::string());
      if (!chosen.empty()) {
        bool ok = true;
        // `a#` passes through as itself; everything else goes through the
        // number/%fmt normaliser the rest of this file uses.
        const std::string norm =
            (!chosen.empty() && chosen[0] == 'a') ? chosen : normalizeFmt(chosen, &ok);
        if (ok) fmt = norm;
      }
      for (const auto& m : models) {
        bool isText = false;
        const std::string v = statCell(m, key, fmt, &isText);
        r.cells.push_back(EtCell{v, ""});
      }
      foot.push_back(r);
    }
  }

  // ---- notes --------------------------------------------------------------
  std::vector<std::string> notes;
  if (!hasFlag(opts, "nonotes")) {
    std::string thenote;
    if (wantBeta) thenote = "Standardized beta coefficients";
    std::string auxNote;
    switch (aux) {
      case Aux::kNone: break;
      case Aux::kT: auxNote = (wantTex ? "\\textit{t} statistics" : "t statistics"); break;
      case Aux::kSe: auxNote = "Standard errors"; break;
      case Aux::kP: auxNote = (wantTex ? "\\textit{p}-values" : "p-values"); break;
      case Aux::kCi: auxNote = std::to_string(level) + "% confidence intervals"; break;
    }
    if (!auxNote.empty()) {
      auxNote += (aux == Aux::kCi) ? " in brackets" : " in parentheses";
      if (!thenote.empty()) thenote += "; ";
      thenote += auxNote;
    }
    // W1a-2, the legend, measured three ways. With cells() there is NO
    // coefficient/parentheses note at all (c1, c3 carry only the star line) --
    // esttab.ado:513 gates that note on `nocellsopt`. With main()/aux() the
    // note takes a different SHAPE from the default, naming both statistics:
    // "se coefficients; none in parentheses" (c4) and "b coefficients; t in
    // parentheses" (c5). `aux(none)` really does put the word `none` in it,
    // which is the kind of thing only a capture tells you.
    if (!cellsGiven && !mainNoteName.empty()) {
      thenote = mainNoteName + " coefficients; " +
                (auxNoteName.empty() ? std::string("t") : auxNoteName) + " in parentheses";
    }
    // Under cells() there are NO notes at all -- not the coefficient line and
    // not the star legend. esttab.ado:513 gates the whole `legend` local on
    // `nocellsopt`, and all four captured cells() arms (c1, c3, c7, c8) end at
    // the rule under N with nothing after it, while the main/aux arms (c4, c5)
    // carry both lines. Suppressing only the first one would have left a star
    // legend Stata does not print, which is the shape a reader diffs first.
    // NOT measured: whether an explicit `starlevels()` alongside cells() brings
    // the star line back. The ado gates on nocellsopt regardless of it, and
    // that is what this follows.
    if (!cellsGiven) {
      if (!thenote.empty()) notes.push_back(thenote);
      if (!noStars) notes.push_back(etStarNote(stars, wantTex));
    }
  }
  if (auto an = optStr(opts, "addnotes")) {
    for (const auto& n : splitTopLevel(*an)) notes.push_back(dequote(n));
  }

  // ---- substitute(): a literal text replacement on what is ABOUT to render --
  // Applied to labels, heads and cells (not to the padding), which keeps the
  // column rule the width esttab.ado:34 fixes rather than the width of the
  // replacement text. t_subst.txt cannot separate the two (`_cons` and `CONST`
  // are both 5 characters); this is the choice that keeps the layout constant.
  {
    std::vector<std::pair<std::string, std::string>> subs;
    if (auto sb = optStr(opts, "substitute")) {
      const std::vector<std::string> toks = splitTopLevel(*sb);
      for (size_t i = 0; i + 1 < toks.size(); i += 2) {
        subs.emplace_back(dequote(toks[i]), dequote(toks[i + 1]));
      }
    }
    if (!subs.empty()) {
      auto apply = [&](std::string& s) {
        for (const auto& [from, to] : subs) {
          if (from.empty()) continue;
          size_t at = 0;
          while ((at = s.find(from, at)) != std::string::npos) {
            s.replace(at, from.size(), to);
            at += to.size();
          }
        }
      };
      for (auto* group : {&heads, &body, &foot}) {
        for (auto& r : *group) {
          apply(r.label);
          for (auto& c : r.cells) apply(c.v);
        }
      }
      for (auto& n : notes) apply(n);
    }
  }

  // ---- render -------------------------------------------------------------
  EtLayout lay;
  // BUG-407 / D5: the three-column star field is esttab's default, and under
  // cells() it exists only if a spec says `star`. c1 (`b(star ...)`) rules at
  // 44 and c7 (`se(par([ ]) fmt(2))`, no star anywhere) rules at 38 -- the
  // difference is exactly 2 x 3.
  if (cellsGiven && !cellStarMain) lay.starwidth = 0;
  // W1a-4: `nostar` zeroes it too, and this was missing. The rule line measures
  // the whole geometry: the oracle's nostar table rules at 38 = 12 + 2*(1+12+0)
  // and its starred one at 44 = 12 + 2*(1+12+3) (w1b_indicate.log n0 against
  // w1b_headfoot.log pb). This build wrote 44 for both, so every nostar table was
  // SIX characters wide with three trailing spaces on every row -- 230 corpus
  // uses over 22 papers, which is the widest-reaching byte divergence this bundle
  // has found. Keyed on the OPTION, not on whether any cell happens to carry a
  // mark, which is the same rule the cells() line above uses; a starred table
  // whose coefficients are all insignificant is not measured and not claimed.
  if (noStars) lay.starwidth = 0;
  lay.labcol2On = labcol2On;
  lay.varwidth = labelWidth ? 20 : 12;  // esttab.ado:34, and s12: the WIDTH
                                        // survives `nolabel`, the text does not
  lay.modelwidth = (aux == Aux::kCi) ? 22 : 12;  // esttab.ado:35 / :481 (2*12-2)

  const std::optional<std::string> preheadO = optStr(opts, "prehead");
  const std::optional<std::string> postheadO = optStr(opts, "posthead");
  const std::optional<std::string> prefootO = optStr(opts, "prefoot");
  const std::optional<std::string> postfootO = optStr(opts, "postfoot");

  std::ostringstream o;
  if (wantCsv) {
    // esttab's csv/tab modes have NO fixed widths (esttab.ado:94-95,123-124):
    // the cells are the same, the padding is not there.
    auto emit = [&](const std::string& label, const std::vector<EtCell>& cells,
                    const std::string& labcol = std::string()) {
      o << delimCell(label, ',');
      if (labcol2On) o << "," << delimCell(labcol, ',');
      for (size_t c = 0; c < k; ++c) {
        o << ",";
        if (c < cells.size()) o << delimCell(cells[c].v + cells[c].star, ',');
      }
      o << "\n";
    };
    for (const auto& r : heads) emit(r.label, r.cells, r.labcol);
    for (const auto& r : body) {
      emit(r.label, r.cells, r.labcol);
      if (r.gapAfter) o << "\n";
    }
    for (const auto& r : foot) emit(r.label, r.cells, r.labcol);
    for (const auto& n : notes) o << delimCell(n, ',') << "\n";
  } else if (wantTex) {
    // `booktabs` swaps the rule macros and nothing else. Not captured by the T
    // batch (its tex arm is plain `tex`), but it is pinned by this build's own
    // AER-TABLES (iii) selftest, so the three spellings are kept explicit here
    // rather than left to fall through to \hline.
    const bool bt = hasFlag(opts, "booktabs");
    const std::string topRule = bt ? "\\toprule" : "\\hline\\hline";
    const std::string midRule = bt ? "\\midrule" : "\\hline";
    const std::string botRule = bt ? "\\bottomrule" : "\\hline\\hline";
    // W1a-4: `prehead()` replaces the ENTIRE opening block -- the brace, the
    // \def\sym line, the tabular opener AND the top rule -- not just the rule.
    // Measured: `prehead("PREHEAD")` writes a file whose FIRST line is PREHEAD
    // (w1b_headfoot.log p1 against the control p0). Emitting the opener anyway
    // and replacing only the rule is what this build did, and since the corpus's
    // own spelling IS a tabular opener -- `prehead(\begin{tabular}{l*{5}{c}}
    // \hline \hline)`, 128 uses over 12 papers -- every one of those files came
    // out with TWO \begin{tabular} lines, which is not compilable LaTeX.
    if (!preheadO.has_value()) {
      o << "{\n";
      o << "\\def\\sym#1{\\ifmmode^{#1}\\else\\(^{#1}\\)\\fi}\n";
    }
    // W1a-5c: the column LETTER is esttab's `alignment()`/`align()`. Measured:
  // `alignment(S)` writes `l*{2}{S}` (w1b_esttab_resid.log y12) and
  // `alignment(c)` is byte-identical to the control, `c` being the default
  // (y11). Whatever the caller gives is written through -- siunitx's `S` and
  // dcolumn's `D{.}{.}{-1}` are both legal column types this build cannot
  // validate and must not mangle.
  std::string alignLetter = "c";
  if (auto al = optStr(opts, "alignment")) alignLetter = dequote(*al);
  if (auto al = optStr(opts, "align")) alignLetter = dequote(*al);
  // k5: `labcol2()` adds a plain `c` between the stub and the model columns.
  if (preheadO.has_value()) {
      // An EMPTY prehead() is a blank line, not nothing (p6).
      o << dequote(*preheadO) << "\n";
    } else {
      o << "\\begin{tabular}{l" << (labcol2On ? "c" : "") << "*{" << k << "}{" << alignLetter
        << "}}\n";
      o << topRule << "\n";
    }
    for (const auto& r : heads) {
      std::string line = etPadRight(r.label, lay.varwidth);
      // k5: the labcol2 head cell is a PLAIN centred cell, not a
      // \multicolumn -- `            &   Extra    &\multicolumn{1}{c}{(1)}`.
      if (lay.labcol2On) line += "&" + etCentre(r.labcol, lay.modelwidth);
      for (size_t c = 0; c < k; ++c) {
        line += "&\\multicolumn{1}{c}{" + (c < r.cells.size() ? r.cells[c].v : std::string()) + "}";
      }
      o << line << "\\\\\n";
    }
    o << (postheadO.has_value() ? dequote(*postheadO) : midRule) << "\n";
    for (const auto& r : body) {
      o << etTexLine(texEscape(r.label), r.cells, k, lay, r.labcol) << "\n";
      if (r.gapAfter) o << "[1em]\n";
    }
    if (!foot.empty()) {
      o << (prefootO.has_value() ? dequote(*prefootO) : midRule) << "\n";
      for (const auto& r : foot) {
        o << etTexLine("\\(" + r.label + "\\)", r.cells, k, lay, r.labcol) << "\n";
      }
    }
    // `postfoot()` replaces the whole CLOSING block -- bottom rule, legend,
    // `\end{tabular}` and the closing brace. p4's file ENDS at POSTFOOT: there
    // is no `\end{tabular}` under it. This build wrote both, so the 141 corpus
    // uses of `postfoot([1em])` and `postfoot(\hline \hline \end{tabular})`
    // each produced a file with a duplicated or stray tabular close.
    if (postfootO.has_value()) {
      o << dequote(*postfootO) << "\n";
    } else {
      o << botRule << "\n";
      for (const auto& n : notes) {
        o << "\\multicolumn{" << (k + 1) << "}{l}{\\footnotesize " << n << "}\\\\\n";
      }
      o << "\\end{tabular}\n}\n";
    }
  } else {
    const size_t total = lay.varwidth + (lay.labcol2On ? EtLayout::kLabcol2Width : 0) +
                         k * (1 + lay.modelwidth + lay.starwidth);
    const std::string rule(total, '-');
    o << (preheadO.has_value() ? dequote(*preheadO) : rule) << "\n";
    for (const auto& r : heads) o << etFixedLine(r.label, r.cells, k, lay, r.labcol) << "\n";
    o << (postheadO.has_value() ? dequote(*postheadO) : rule) << "\n";
    for (const auto& r : body) {
      o << etFixedLine(r.label, r.cells, k, lay, r.labcol) << "\n";
      if (r.gapAfter) o << "\n";
    }
    if (!foot.empty()) {
      o << (prefootO.has_value() ? dequote(*prefootO) : rule) << "\n";
      for (const auto& r : foot) o << etFixedLine(r.label, r.cells, k, lay, r.labcol) << "\n";
    }
    // postfoot() replaces the CLOSING BLOCK, legend included: t_head.txt ends at
    // POSTFOOT with no star legend under it.
    if (postfootO.has_value()) {
      o << dequote(*postfootO) << "\n";
    } else {
      o << rule << "\n";
      for (const auto& n : notes) o << n << "\n";
    }
  }

  ojson result = emitTable("esttab", session, userPath, o.str(), hasFlag(opts, "replace"),
                           hasFlag(opts, "append"));
  // The layout-only options: ACCEPTED so a corpus line that spells them runs,
  // and SAID ONCE so nobody reads the resulting table as having honoured them.
  static const char* kEsttabLayoutOnly[] = {"nolegend",   "nolines",  "page",     "align",
                                            "width",      "wrap",     "modelwidth",
                                            "varwidth",   "extracols"};
  std::vector<std::string> seen;
  for (const char* n : kEsttabLayoutOnly) {
    if (hasFlag(opts, n) || optStr(opts, n).has_value()) seen.push_back(n);
  }
  if (!seen.empty()) {
    ojson arr = ojson::array();
    arr.push_back(result);
    arr.push_back(displayBlock("(note: " + joinWith(seen, ", ") +
                               " accepted and not applied by this build -- the table's widths, "
                               "rules and legend are esttab's defaults)"));
    return arr;
  }
  return result;
}

// ===========================================================================
// outreg2
//
// outreg2 reports the ACTIVE estimation, not the estimates store, and its
// `append` crosses SESSIONS -- run the same do-file twice and the second run's
// append lands on the first run's file. So the `.txt` ledger IS the source of
// truth for append (plan decision 7); an in-memory ledger would answer
// differently on the second run, which is the rejected alternative.
// ===========================================================================

namespace {

// `addtext(Controls, Yes, Year FE, Yes)` / `addstat(F, e(F), R2adj, e(r2_a))`
// -- both are FLAT comma lists of alternating name and value.
std::vector<std::pair<std::string, std::string>> parseCommaPairs(const std::string& spec) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec), ',');
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out.emplace_back(strutil::trim(dequote(strutil::trim(toks[i]))),
                     strutil::trim(dequote(strutil::trim(toks[i + 1]))));
  }
  return out;
}

// Parse a `.txt` ledger THIS BUILD wrote back into a Table. A file that does
// not match the layout is rejected by name rather than half-parsed: an append
// onto a misread table would silently drop the earlier columns.
bool parseOutreg2Txt(const std::string& text, Table* t) {
  std::vector<std::string> lines;
  {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  if (lines.size() < 2) return false;
  // Row 1 is the column numbers, row 2 is `VARIABLES <heads>`. Both are
  // structural: without them there is no column count to append to.
  std::vector<std::string> nums;
  {
    std::string l = lines[0];
    size_t pos = 0;
    while (pos <= l.size()) {
      const size_t tab = l.find('\t', pos);
      const std::string cell = l.substr(pos, tab == std::string::npos ? std::string::npos
                                                                      : tab - pos);
      nums.push_back(cell);
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
  }
  if (nums.empty() || !nums[0].empty()) return false;
  nums.erase(nums.begin());
  if (nums.empty() || nums[0] != "(1)") return false;
  t->colNums = nums;

  std::vector<std::string> head;
  {
    std::string l = lines[1];
    size_t pos = 0;
    while (pos <= l.size()) {
      const size_t tab = l.find('\t', pos);
      head.push_back(l.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
  }
  if (head.empty() || head[0] != "VARIABLES") return false;
  head.erase(head.begin());
  t->colHeads = head;

  const size_t k = t->colNums.size();
  auto rowOf = [&](const std::string& l, TRow::Kind kind) {
    TRow r;
    r.kind = kind;
    size_t pos = 0;
    std::vector<std::string> cells;
    while (pos <= l.size()) {
      const size_t tab = l.find('\t', pos);
      cells.push_back(l.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
    if (!cells.empty()) {
      r.label = cells[0];
      cells.erase(cells.begin());
    }
    cells.resize(k);
    r.cells = cells;
    return r;
  };

  // Everything after the header is body until the run of statistic rows, then
  // notes. `Observations` is the marker for the footer; the trailing note lines
  // are the ones with no tab at all.
  bool inFoot = false;
  for (size_t i = 2; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (strutil::trim(l).empty()) continue;
    if (l.find('\t') == std::string::npos) {
      t->notes.push_back(l);
      continue;
    }
    // A note line is now `text\t\t...` (the renderer emits it as a row of empty
    // cells), so "has no tab" no longer identifies one. Every cell empty AND we
    // are past the statistics block is the note shape; before the footer the
    // same silhouette belongs to `stats()`'s equation header, which is a body
    // row and must stay one.
    if (inFoot) {
      bool allEmpty = true;
      for (size_t q = l.find('\t'); q != std::string::npos;) {
        const size_t nq = l.find('\t', q + 1);
        const std::string cell =
            l.substr(q + 1, nq == std::string::npos ? std::string::npos : nq - q - 1);
        if (!cell.empty()) allEmpty = false;
        q = nq;
      }
      if (allEmpty) {
        t->notes.push_back(l.substr(0, l.find('\t')));
        continue;
      }
    }
    // The footer marker is whichever statistic row comes first: `noobs` removes
    // Observations, and without this second spelling an appended R-squared row
    // would land in the coefficient body.
    if (l.compare(0, 13, "Observations\t") == 0 || l.compare(0, 10, "R-squared\t") == 0 ||
        l.compare(0, 19, "Adjusted R-squared\t") == 0) {
      inFoot = true;
    }
    if (inFoot) {
      t->foot.push_back(rowOf(l, TRow::kStat));
    } else {
      t->body.push_back(rowOf(l, l.empty() || l[0] == '\t' ? TRow::kSe : TRow::kCoef));
    }
  }
  return !t->body.empty() || !t->foot.empty();
}

// Merge a freshly-built one-column table into a parsed ledger, matching rows by
// LABEL. A coefficient the earlier columns do not have becomes a NEW row whose
// earlier cells are blank -- blank, never zero.
//
// Matching runs on COEFFICIENT/STATISTIC pairs, not on a flat row scan, because
// a standard-error row's label is EMPTY by construction: `x1 / (0.045)`. A flat
// scan either matches every blank label to the first blank row (every SE lands
// on the wrong coefficient) or advances a cursor and then cannot go back for a
// coefficient the second model reordered -- which duplicates `_cons` the moment
// model 2 adds a regressor. So each coefficient row carries its own SE row with
// it, addressed by the coefficient's index.
void appendColumn(Table* base, const Table& add) {
  const size_t k = base->colNums.size();
  base->colNums.push_back("(" + std::to_string(k + 1) + ")");
  base->colHeads.push_back(add.colHeads.empty() ? std::string() : add.colHeads[0]);
  for (auto& r : base->body) r.cells.resize(k + 1);
  for (auto& r : base->foot) r.cells.resize(k + 1);

  auto cellOf = [](const TRow& r) { return r.cells.empty() ? std::string() : r.cells[0]; };

  for (size_t i = 0; i < add.body.size(); ++i) {
    if (add.body[i].kind == TRow::kSe) continue;  // consumed with its coefficient
    const TRow& coef = add.body[i];
    const TRow* se = (i + 1 < add.body.size() && add.body[i + 1].kind == TRow::kSe)
                         ? &add.body[i + 1]
                         : nullptr;
    size_t at = base->body.size();
    for (size_t j = 0; j < base->body.size(); ++j) {
      if (base->body[j].kind != TRow::kSe && base->body[j].label == coef.label) {
        at = j;
        break;
      }
    }
    if (at < base->body.size()) {
      base->body[at].cells[k] = cellOf(coef);
      if (se != nullptr && at + 1 < base->body.size() && base->body[at + 1].kind == TRow::kSe) {
        base->body[at + 1].cells[k] = cellOf(*se);
      }
      continue;
    }
    TRow nc = coef;
    nc.cells.assign(k + 1, std::string());
    nc.cells[k] = cellOf(coef);
    base->body.push_back(nc);
    if (se != nullptr) {
      TRow ns = *se;
      ns.cells.assign(k + 1, std::string());
      ns.cells[k] = cellOf(*se);
      base->body.push_back(ns);
    }
  }

  // Footer labels are unique (Observations, R-squared, one per addtext/addstat
  // name), so a plain label match is right there and needs no pairing.
  for (const auto& r : add.foot) {
    size_t at = base->foot.size();
    for (size_t j = 0; j < base->foot.size(); ++j) {
      if (base->foot[j].label == r.label) {
        at = j;
        break;
      }
    }
    if (at < base->foot.size()) {
      base->foot[at].cells[k] = cellOf(r);
      continue;
    }
    TRow nr = r;
    nr.cells.assign(k + 1, std::string());
    nr.cells[k] = cellOf(r);
    base->foot.push_back(nr);
  }
}

// outreg2's `.txt` is a FIXED layout (the plan's Stata-semantics section spells
// it out), not renderDelim's generic one: the blank separator lines and the
// literal `VARIABLES` head are what this build's own `append` parser reads back.
std::string renderOutreg2Txt(const Table& t) {
  const size_t k = t.ncols();
  std::ostringstream o;
  auto row = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = label;
    for (size_t c = 0; c < k; ++c) {
      line += "\t";
      if (c < cells.size()) line += cells[c];
    }
    o << line << "\n";
  };
  row("", t.colNums);
  row("VARIABLES", t.colHeads);
  // outreg2's separators are ROWS of empty cells, not empty lines: o_plain.txt's
  // third line is a single TAB, and its two legend lines each carry a trailing
  // tab. This build wrote bare "\n" and an untabbed note, which is one cell
  // wrong on four lines of every table it has ever written.
  row("", {});
  for (const auto& r : t.body) row(r.label, r.cells);
  row("", {});
  for (const auto& r : t.foot) row(r.label, r.cells);
  for (const auto& n : t.notes) row(n, {});
  return o.str();
}

}  // namespace

ojson runOutreg2(const eval::ParsedCommand& parsed, SessionState& session) {
  // BEFORE the option net, deliberately. `word` is a real outreg2 option, so it
  // sits in the `unimplemented` set -- and the net would therefore answer
  // "valid Stata option, not yet implemented" (rc 198) for it. The honest
  // answer is that the whole OUTPUT FORMAT has no writer here, which is a
  // command-level refusal at rc 199, so it has to be decided first.
  if (hasFlag(parsed.opts, "word")) {
    return ojson(eval::refusedCommand("outreg2", "no_rtf_writer"));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutreg2.known,
                                            kVocabOutreg2.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("outreg2: `using FILENAME' is required");
  }

  Model m;
  ojson err;
  if (!activeModel(session, "outreg2", &m, &err)) return err;

  const bool replace = hasFlag(parsed.opts, "replace");
  const bool append = hasFlag(parsed.opts, "append");

  int dec = 3;
  auto readDec = [&](const char* key, int fallback) {
    if (auto v = optStr(parsed.opts, key)) {
      try {
        size_t pos = 0;
        const int n = std::stoi(*v, &pos);
        if (pos == v->size() && n >= 0 && n <= 12) return n;
      } catch (...) {
      }
    }
    return fallback;
  };
  dec = readDec("dec", 3);
  const int bdec = readDec("bdec", dec);
  const int sdec = readDec("sdec", dec);
  const std::string bFmt = "%9." + std::to_string(bdec) + "f";
  const std::string sFmt = "%9." + std::to_string(sdec) + "f";
  // B4-W5. outreg2's default is NOT %9.3f. outreg2.ado:5509-5515 sets every
  // `*dec` to 3 and :5525-5531 sets every `*fmt` to `fc`, and :5541-5543 says
  // the adaptive path is switched off only when `dec()`/`bdec()` is given -- so
  // the DEFAULT column is significant digits with thousands separators, which
  // is why o_plain.txt reads -49.51 / 1.747 / 1,946 and not -49.512 / 1.747 /
  // 1946.069. `auto(#)` moves the digit count; o_auto4.txt is the arm.
  const bool decGiven = optStr(parsed.opts, "dec").has_value() ||
                        optStr(parsed.opts, "bdec").has_value();
  const int autoD = readDec("auto", 3);
  const bool autoOn = !decGiven && !hasFlag(parsed.opts, "noauto");
  // `adec(#)` formats the added statistics and NOTHING else: measured against
  // dec(), which moves R-squared to 0.2934 while the statistic stays at one
  // decimal (w1b_adec_rule.log c1). outreg2.ado:466 declares it
  // `numlist int >=0 <=11 max=1`, and each of those bounds carries its own rc
  // and its own message -- all four measured (w1b_adec_rule.log d2/g1/g2/g3).
  int adec = -1;
  if (auto av = optStr(parsed.opts, "adec")) {
    const std::vector<std::string> toks = splitWs(dequote(*av));
    if (toks.size() != 1) return tblErrorRc("adec() invalid -- single number required", 123);
    size_t pos = 0;
    double d = 0;
    bool numeric = true;
    try {
      d = std::stod(toks[0], &pos);
    } catch (const std::exception&) {
      numeric = false;
    }
    if (!numeric || pos != toks[0].size()) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    if (d != std::trunc(d)) {
      return tblErrorRc("adec() invalid -- invalid number, noninteger value", 126);
    }
    if (d < 0 || d > 11) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    adec = static_cast<int>(d);
    // outreg2.ado:1401. `e()` satisfies this guard too -- it funnels its scalars
    // through addstat() -- but `e()` is an unimplemented option on this command,
    // so addstat() is the only way to reach here (w1b_adec_rule.log e1/e2).
    if (!optStr(parsed.opts, "addstat")) {
      return tblErrorRc("cannot choose adec option without addstat option", 198);
    }
  }
  // `tdec(#)` given disables the adaptive path for the parenthesised row alone
  // (outreg2.ado:5545-5547 `if "`tdec0''"=="" & "`noauto''"~="noauto"`).
  const bool tdecGiven = optStr(parsed.opts, "tdec").has_value();
  const int tdec = readDec("tdec", 3);

  // `stats(coef pval)` names WHICH statistics the two rows carry. The first is
  // the coefficient row, the second is the parenthesised one, and the note
  // names the second one literally -- o_stats.txt's legend is "pval in
  // parentheses", not "p-values in parentheses".
  std::vector<std::string> statsSpec;
  if (auto st = optStr(parsed.opts, "stats")) statsSpec = splitWs(dequote(*st));
  const bool wantTstat = hasFlag(parsed.opts, "tstat");
  std::string parenName = "se";
  if (wantTstat) parenName = "tstat";
  if (statsSpec.size() >= 2) parenName = statsSpec[1];
  auto statKeyOf = [](const std::string& n) -> std::string {
    if (n == "coef" || n == "b") return "b";
    if (n == "se" || n == "stderr") return "se";
    if (n == "tstat" || n == "t") return "t";
    if (n == "pval" || n == "p") return "p";
    return std::string();
  };

  // ---- the ONE new column -------------------------------------------------
  Table col;
  {
    std::string head = m.snap.value("depvar", std::string());
    if (auto ct = optStr(parsed.opts, "ctitle")) head = dequote(*ct);
    col.colNums.push_back("(1)");
    col.colHeads.push_back(head);

    BodyOpts bo;
    bo.stars = outreg2Stars();  // *** p<0.01 -- NOT estout's ladder
    if (hasFlag(parsed.opts, "noaster")) bo.stars.clear();
    bo.keep = optStr(parsed.opts, "keep");
    bo.nocons = hasFlag(parsed.opts, "nocons");
    bo.useVarLabels = hasFlag(parsed.opts, "label");
    bo.consLast = true;
    bo.consLabel = "Constant";
    CellSpec b;
    b.name = statsSpec.empty() ? std::string("b") : statKeyOf(statsSpec[0]);
    if (b.name.empty()) {
      return ojson(eval::unsupportedOption(
          "stats", "outreg2 stats(): this build knows coef, se, tstat and pval; `" +
                       statsSpec[0] + "` names none of them"));
    }
    b.fmt = bFmt;
    if (autoOn) b.autoD = autoD;
    b.star = !hasFlag(parsed.opts, "noaster");
    CellSpec s;
    s.name = statKeyOf(parenName);
    if (s.name.empty()) {
      return ojson(eval::unsupportedOption(
          "stats", "outreg2 stats(): this build knows coef, se, tstat and pval; `" + parenName +
                       "` names none of them"));
    }
    s.fmt = (s.name == "t" || s.name == "p") ? ("%9." + std::to_string(tdec) + "f") : sFmt;
    if (autoOn && !(s.name == "t" && tdecGiven)) s.autoD = (s.name == "t") ? tdec : autoD;
    s.par = true;
    bo.cells.push_back(b);
    bo.cells.push_back(s);
    // `stats()` puts an equation header above the body: o_stats.txt carries a
    // `price` row and a blank row before `mpg`. Reproduced from the capture --
    // the mechanism inside outreg2 is not read off its source here, only the
    // shape it wrote, and this comment is the honest statement of that.
    if (!statsSpec.empty()) {
      TRow eq;
      eq.kind = TRow::kText;
      eq.label = m.snap.value("depvar", std::string());
      eq.cells.push_back("");
      col.body.push_back(eq);
      TRow blank;
      blank.kind = TRow::kText;
      blank.cells.push_back("");
      col.body.push_back(blank);
    }
    std::string bodyErr;
    buildBody(&col, {m}, bo, session, &bodyErr);
    if (!bodyErr.empty()) return tblErrorRc(bodyErr, 111);

    if (!hasFlag(parsed.opts, "noobs")) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = "Observations";
      r.cells.push_back(m.escalars.contains("N") ? fmtCount(m.escalars["N"]) : std::string());
      col.foot.push_back(r);
    }
    if (!hasFlag(parsed.opts, "nor2")) {
      const bool adj = hasFlag(parsed.opts, "adjr2");
      const char* key = adj ? "r2_a" : "r2";
      TRow r;
      r.kind = TRow::kStat;
      r.label = adj ? "Adjusted R-squared" : "R-squared";
      r.cells.push_back(m.escalars.contains(key) ? fmtNum(m.escalars[key].get<double>(), bFmt)
                                                 : std::string());
      col.foot.push_back(r);
    }
    if (auto at = optStr(parsed.opts, "addtext")) {
      for (const auto& [name, val] : parseCommaPairs(*at)) {
        TRow r;
        r.kind = TRow::kText;
        r.label = name;
        r.cells.push_back(val);
        col.foot.push_back(r);
      }
    }
    if (auto as = optStr(parsed.opts, "addstat")) {
      const std::vector<std::pair<std::string, std::string>> stats = parseCommaPairs(*as);
      for (size_t j = 0; j < stats.size(); ++j) {
        const std::string& name = stats[j].first;
        const std::string& expr = stats[j].second;
        // The SAME resolver `display` uses -- addstat("F", e(F)) is a display
        // expression, and a second e() reader here is exactly the two-layer
        // split this codebase keeps paying for.
        const std::optional<double> v = piet::evalScalarExpr(expr, session);
        TRow r;
        r.kind = TRow::kStat;
        r.label = name;
        if (!v.has_value()) {
          // Unresolvable is MISSING, not 0: `addstat(F, e(F))` after a model
          // with no F must not report an F of zero.
          r.cells.push_back(std::string(kMissingCell));
        } else if (adec >= 0) {
          const AdecCell cell = outreg2AdecCell(*v, adec);
          if (cell.seps == 1) {
            // BUG-493, followed bug-for-bug on the User's 2026-09-18 ruling. The
            // separator re-enters outreg2's addstat parse as a field separator
            // and the command dies. WHICH rc it dies with is a property of where
            // the extra field lands: a TRAILING pair is left without a value
            // (`invalid syntax`, r(198)); an earlier one pushes the NEXT pair's
            // name into a value slot, where outreg2 evaluates it as a variable
            // (`B not found`, r(111)). Both measured -- w1b_adec_rule.log
            // d4/corpus_big and d3. The rc and the refusal are the ado's; the
            // message on the 198 side is ours, because `invalid syntax` names
            // nothing a user could act on.
            if (j + 1 < stats.size()) {
              return tblErrorRc(stats[j + 1].first + " not found", 111);
            }
            return tblErrorRc(
                "outreg2: `" + name + "' is " + cell.text + " under adec(" +
                    std::to_string(adec) +
                    "), and outreg2 re-reads that thousands separator as an addstat() field"
                    " separator -- Stata answers `invalid syntax' here too. Lower adec() until"
                    " the separator no longer fits, or scale the statistic.",
                198);
          }
          if (cell.seps >= 2) {
            // The one place this build does NOT follow the ado. Two separators
            // make the re-parsed field count EVEN, so outreg2 does not fail --
            // it writes a table in which `addstat(M, 9999999) adec(0)` reports M
            // as 9 and invents a second row named 999 (w1b_adec_rule3.log t5,
            // rc 0). Following THAT bug-for-bug would mean publishing a wrong
            // number, which is a different thing from publishing nothing.
            return tblErrorRc(
                "outreg2: `" + name + "' is " + cell.text + " under adec(" +
                    std::to_string(adec) +
                    "), which outreg2 2.x mis-parses into two rows with a truncated value (it"
                    " reports 9 for 9999999). This build refuses rather than write that table;"
                    " lower adec() or scale the statistic.",
                198);
          }
          r.cells.push_back(cell.text);
        } else {
          // BUG-494. With no adec() the ado leaves the statistic to its ADAPTIVE
          // format, not to dec(): `addstat(M, 1.5) dec(4)` renders the statistic
          // `1.500` while R-squared obeys dec() and goes to 0.2934
          // (w1b_adec_rule.log c2), and 6165.257 renders `6165`
          // (w1b_outreg2_split3.log z7). `noauto` and `dec()` do not reach it
          // (w1b_adec_rule2.log n11/n14) but `auto(#)` does (n12/n13), so the
          // digit count is autoD and the switch is NOT autoOn.
          r.cells.push_back(sigDigits(*v, autoD, /*minDec=*/0, /*comma=*/false));
        }
        col.foot.push_back(r);
      }
    }
    // The legend names the parenthesised statistic: "Standard errors" by
    // default, "t-statistics" under `tstat`, and the literal `stats()` token
    // otherwise (o_plain / o_tstat / o_stats).
    std::string parenNote = "Standard errors in parentheses";
    if (!statsSpec.empty() && statsSpec.size() >= 2 && statsSpec[1] != "se") {
      parenNote = statsSpec[1] + " in parentheses";
    } else if (wantTstat) {
      parenNote = "t-statistics in parentheses";
    }
    col.notes.push_back(parenNote);
    if (!hasFlag(parsed.opts, "noaster")) {
      col.notes.push_back(starNote(outreg2Stars()));
    }
  }

  // ---- the ledger ---------------------------------------------------------
  const std::string txtUser = ensureExt(*usingPath, ".txt");
  const std::string txtResolved = resolveUserPath(session, txtUser);

  Table table;
  if (append) {
    if (!cecon::native_exists(txtResolved)) {
      return tblError("outreg2: " + txtUser +
                      " does not exist (append needs the file this build wrote)");
    }
    bool ok = false;
    // READ IT FULLY FIRST. The destination is also the source here, which is
    // exactly the question BUG-138 says to ask of every write; the temp+rename
    // writer below never opens it while these bytes are still needed.
    const std::string existing = readWholeFile(txtResolved, &ok);
    if (!ok) return tblError("outreg2: cannot read " + txtUser, "data_io");
    Table base;
    if (!parseOutreg2Txt(existing, &base)) {
      return tblError("outreg2: " + txtUser +
                      " is not an outreg2 table (append needs the file this build wrote)");
    }
    base.notes = col.notes;  // the note block is regenerated, never duplicated
    appendColumn(&base, col);
    table = base;
  } else {
    ojson guard = guardOverwrite("outreg2", txtResolved, txtUser, replace, false);
    if (!guard.is_null()) return guard;
    table = col;
  }
  if (auto ti = optStr(parsed.opts, "title")) table.title = dequote(*ti);

  ojson out = ojson::array();
  {
    ojson werr = writeTextFileAtomic("outreg2", txtResolved, txtUser, renderOutreg2Txt(table));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock("(output written to " + txtUser + ")"));
  }

  // ---- the optional companion formats -------------------------------------
  if (hasFlag(parsed.opts, "tex")) {
    const std::string texUser = ensureExt(*usingPath, ".tex");
    TexStyle ts;
    // `tex(frag)`/`tex(fragment)` drops the \documentclass wrapper.
    // `tex(pretty)`/`tex(landscape)` are accepted and IGNORED, with the note
    // below saying so once -- they change page furniture this writer does not
    // emit, and refusing the whole command over them would be worse.
    const std::optional<std::string> texArg = optStr(parsed.opts, "tex");
    std::string texMode = texArg.has_value() ? strutil::lower(dequote(*texArg)) : std::string();
    ts.align = TexStyle::kSpaced;  // \begin{tabular}{l c c}
    ts.fragment = (texMode == "frag" || texMode == "fragment");
    // Non-fragment outreg2 .tex is a COMPILABLE document, which is what
    // outreg2 itself writes; `tex(frag)` is exactly the request to drop the
    // \documentclass wrapper so the table can be \input into another file.
    ts.standalone = !ts.fragment;
    if (texMode == "pretty" || texMode == "landscape") {
      out.push_back(displayBlock("(note: tex(" + texMode +
                                 ") is accepted but not applied by this build)"));
    }
    const std::string texResolved = resolveUserPath(session, texUser);
    ojson guard = guardOverwrite("outreg2", texResolved, texUser, replace || append, false);
    if (!guard.is_null()) return guard;
    ojson werr = writeTextFileAtomic("outreg2", texResolved, texUser, renderTex(table, ts));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock("(output written to " + texUser + ")"));
  }
  if (hasFlag(parsed.opts, "excel")) {
    // Tab-separated text under an `.xls` extension -- which is what outreg2
    // ITSELF writes. Excel opens it; no xlsx writer is needed, and pretending
    // to write a real workbook would be the dishonest option.
    const std::string xlsUser = ensureExt(*usingPath, ".xls");
    const std::string xlsResolved = resolveUserPath(session, xlsUser);
    ojson guard = guardOverwrite("outreg2", xlsResolved, xlsUser, replace || append, false);
    if (!guard.is_null()) return guard;
    ojson werr = writeTextFileAtomic("outreg2", xlsResolved, xlsUser, renderDelim(table, '\t'));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock("(output written to " + xlsUser + ")"));
  }
  return out;
}

// ===========================================================================
// texsave -- the DATASET as a LaTeX table
// ===========================================================================

ojson runTexsave(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexsave.known,
                                            kVocabTexsave.unimplemented)) {
    return ojson(*err);
  }
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("texsave: `using FILENAME' is required");
  }

  Datastore* ds = session.datastore();
  std::vector<std::string> cols = words;
  if (cols.empty()) cols = ds->names();
  for (const auto& v : cols) {
    if (!ds->has(v)) return tblError("variable " + v + " not found", "unknown_variable");
  }

  const bool useLabels = hasFlag(parsed.opts, "varlabels");
  const bool nofix = hasFlag(parsed.opts, "nofix");

  Table t;
  if (auto ti = optStr(parsed.opts, "title")) t.title = dequote(*ti);
  if (auto al = optStr(parsed.opts, "align")) t.align = dequote(*al);
  // texsave's leftmost column is a DATA column, so it occupies the Table's stub
  // (the label slot) and its variable name occupies `stubHead`. Leaving the
  // stub head blank -- which is right for an estimates table -- would drop the
  // first variable's name silently.
  {
    std::vector<std::string> heads;
    for (const auto& c : cols) heads.push_back(useLabels ? labelFor(c, {}, true, session) : c);
    if (!heads.empty()) {
      t.stubHead = heads[0];
      t.colHeads.assign(heads.begin() + 1, heads.end());
    }
  }

  int hlineEvery = 0;
  if (auto hl = optStr(parsed.opts, "hlines")) {
    try {
      size_t pos = 0;
      hlineEvery = std::stoi(dequote(*hl), &pos);
    } catch (...) {
      hlineEvery = 0;
    }
  }

  const int64_t n = ds->nRows();
  for (int64_t r = 0; r < n; ++r) {
    TRow row;
    row.kind = TRow::kText;
    for (size_t c = 0; c < cols.size(); ++c) {
      const cecon::Column& col = ds->col(cols[c]);
      std::string cell;
      if (col.dtype == cecon::DType::Factor) {
        const int32_t code = col.codes()[r];
        if (code >= 0) cell = col.levels[static_cast<size_t>(code)];
      } else {
        const double v = col.f64()[r];
        // A Stata missing renders as `.`, never as 0 or an empty cell.
        cell = std::isnan(v) ? kMissingCell : fmtNum(v, "%9.4g");
      }
      if (c == 0) {
        row.label = cell;
      } else {
        row.cells.push_back(cell);
      }
    }
    t.body.push_back(row);
    if (hlineEvery > 0 && ((r + 1) % hlineEvery == 0) && r + 1 < n) {
      TRow rule;
      rule.kind = TRow::kRule;
      t.body.push_back(rule);
    }
  }
  if (auto hlText = optStr(parsed.opts, "headerlines")) {
    for (const auto& line : splitTopLevel(dequote(*hlText))) t.notes.push_back(dequote(line));
  }
  if (auto fn = optStr(parsed.opts, "footnote")) t.notes.push_back(dequote(*fn));

  TexStyle ts;
  ts.fragment = hasFlag(parsed.opts, "frag");
  ts.escape = !nofix;  // `nofix` = the user has already escaped their own text
  const std::string bytes = renderTex(t, ts);
  return emitTable("texsave", session, ensureExt(*usingPath, ".tex"), bytes,
                   hasFlag(parsed.opts, "replace"), false);
}

// ===========================================================================
// texdoc / tex
//
// The open file is a PATH held in `session.eval.globals`, and every `tex` line
// opens-appends-closes. At the five-line scale the corpus uses that costs
// nothing measurable, and it avoids adding a SessionState field on a surface
// this lane does not own. The cost, stated: `$__piet_texdoc_path` is visible to
// `macro list`. That is the trade the plan made (decision 9).
// ===========================================================================

namespace {
const char* const kTexdocPathKey = "__piet_texdoc_path";
}  // namespace

ojson runTexdoc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexdoc.known,
                                            kVocabTexdoc.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("texdoc: a subcommand is required (init or close)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {"do", "stlog", "local", "graph",
                                                            "write", "substitute", "append"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }

  if (sub == "close") {
    if (session.eval.globals.get(kTexdocPathKey).empty()) {
      return tblError("texdoc close: no texdoc file is open");
    }
    const std::string path = session.eval.globals.get(kTexdocPathKey);
    session.eval.globals.erase(kTexdocPathKey);
    return displayBlock("(texdoc closed " + path + ")");
  }
  if (sub != "init") {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  std::vector<std::string> words(parsed.varlist.begin() + 1, parsed.varlist.end());
  // `texdoc init using f.tex` and `texdoc init f.tex` are both written in the
  // wild; `using` is optional here, unlike everywhere else in this file.
  std::optional<std::string> path = takeUsing(words);
  if (!path.has_value() && !words.empty()) path = dequote(words[0]);
  if (!path.has_value() || path->empty()) {
    return tblSyntax("texdoc init: a filename is required");
  }
  const std::string userPath = ensureExt(*path, ".tex");
  const std::string resolved = resolveUserPath(session, userPath);
  const bool replace = hasFlag(parsed.opts, "replace") || hasFlag(parsed.opts, "force");
  const bool append = hasFlag(parsed.opts, "append");
  ojson guard = guardOverwrite("texdoc init", resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  if (!append) {
    // TRUNCATE at init, so a `replace` that is never followed by a `tex` line
    // still leaves an empty file rather than the previous run's contents.
    ojson werr = writeTextFileAtomic("texdoc init", resolved, userPath, std::string());
    if (!werr.is_null()) return werr;
  }
  session.eval.globals.set(kTexdocPathKey, resolved);
  return displayBlock("(texdoc opened " + userPath + ")");
}

ojson runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session) {
  // NO reject net here, and that is deliberate rather than an omission: `tex`
  // takes no options at all -- everything after the verb is literal TeX -- so
  // the tokenizer's comma split (`tex Hello, world`) produces a phantom option
  // key from ordinary prose. Reading the whole raw remainder is the only way
  // the text survives, and there is no option vocabulary to be silent about.
  std::string text;
  {
    const std::string& raw = parsed.raw;
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = b;
      while (e < raw.size() &&
             (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
        ++e;
      }
      if (e < raw.size()) text = raw.substr(e + 1);
    }
  }
  // A bare `tex` writes a blank line, which is how texdoc users separate
  // paragraphs; it is not an error.

  const std::string resolved = session.eval.globals.get(kTexdocPathKey);
  if (resolved.empty()) {
    return tblError("tex: no texdoc file is open (run `texdoc init FILENAME' first)");
  }
  bool ok = false;
  std::string existing;
  if (cecon::native_exists(resolved)) {
    existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError("tex: cannot read the open texdoc file", "data_io");
  }
  existing += text;
  existing += "\n";
  // Same temp+rename writer as every other file in this TU: the destination IS
  // the source on every `tex` line after the first.
  ojson werr = writeTextFileAtomic("tex", resolved, "the open texdoc file", existing);
  if (!werr.is_null()) return werr;
  return ojson::array();  // silent success -- texdoc lines produce no output
}

// ===========================================================================
// frmttable -- a session MATRIX as a LaTeX table (TeX only)
// ===========================================================================

// --- frmttable's own LaTeX, which is not estout's ---------------------------
// This writer used to hand its Table to `renderTex`, the estout/esttab emitter,
// and the two shapes are not the same table. Measured against the ado
// (tests/eval/golden/aer_w1/w1b_frmttable_{base,names,cells2}.log), on the
// SIMPLEST possible call -- `frmttable using f.tex, statmat(N) sdec(2) tex`:
//   oracle  \begin{tabular}{ccc}
//           \hline \noalign{\smallskip}1.00 & 2.00 & 3.00\\
//   before  \begin{tabular}{lccc}
//           \hline
//            & c1 & c2 & c3 \\
//           \hline
//           r1 & 1.00 & 2.00 & 3.00 \\
// -- a stub column and a header row that the oracle does not emit at all, plus a
// different rule idiom. Four things had to be measured before this could be
// written, and each one is a rule no amount of reading the option list gives:
//
//  1. DEFAULT-SPELLED matrix names count as ABSENT. Every Stata matrix has
//     names, and `matrix N = (1,2,3\4,5,6)` carries `r1 r2` / `c1 c2 c3` -- for
//     which the oracle emits no stub and no header (base h1). Setting those SAME
//     names explicitly changes nothing (names i2), and naming them anything else
//     turns the stub or the header on (i3 rownames only, i4 colnames only).
//  2. The TABLE WIDTH is the wider of the data (plus stub) and the longest
//     ctitles row: `ctitles("", "c1", "c2", "c3")` over a three-column matrix
//     with no stub gives `{cccc}` and pads every data row with a trailing empty
//     cell (base h5), while a SHORTER ctitles pads the header instead (i8).
//  3. `rtitles` longer than the matrix ADDS rows (ia: a third row of empty data).
//  4. The default numeric format carries THOUSANDS SEPARATORS -- 1234.5 at
//     sdec(2) is `1,234.50` with no sfmt() at all (cells2 k6), which this build
//     was dropping. And the sdec() list's LAST entry repeats for the remaining
//     columns (k2: `sdec(0,1)` over three columns is 0, 1, 1), while extra
//     entries are ignored (k4).
namespace {

// frmttable's `%fc`: fixed decimals with thousands separators on the integer
// part. A missing value is an EMPTY cell, which is not a flourish -- paper 004
// builds three of its rows out of `J(1,7,.)` spacers (base j1/j2).
std::string frmtCell(double v, int dec) {
  if (std::isnan(v)) return std::string();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  const size_t dot = out.find('.');
  const size_t start = (!out.empty() && out[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? out.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    out.insert(at, ",");
  }
  return out;
}

// `rtitles("a" \ "b")` / `ctitles("", "x" \ "", "y")`: rows are separated by a
// top-level backslash and cells within a row by a comma. Quote-aware on both,
// which is what lets a cell hold LaTeX -- `"\makecell[l]{\it{Panel A}}"` is one
// cell and its backslashes are not separators (names i7).
std::vector<std::vector<std::string>> frmtTitleRows(const std::string& raw) {
  std::vector<std::vector<std::string>> rows;
  // SPLIT FIRST, DEQUOTE EACH CELL -- the same rule `parseLabelPairs` states a
  // few hundred lines up, and the first cut of this function broke it. Dequoting
  // `"a" \ "b"` FIRST strips the outer pair and leaves `a" \ "b`, whose remaining
  // quotes make the splitter's quote tracking swallow the separator, so every
  // row collapsed into one cell reading `a" \ "b`. It reproduced on nine of the
  // nineteen oracle arms at once.
  for (const auto& r : splitTopLevel(raw, '\\')) {
    std::vector<std::string> cells;
    for (const auto& c : splitTopLevel(r, ',')) cells.push_back(dequote(strutil::trim(c)));
    rows.push_back(cells);
  }
  return rows;
}

// Stata's own default names for an unnamed matrix, which frmttable treats as no
// names at all.
bool frmtDefaultNames(const std::vector<std::string>& names, const char* stem) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] != stem + std::to_string(i + 1)) return false;
  }
  return !names.empty();
}

}  // namespace

ojson runFrmttable(const eval::ParsedCommand& parsed, SessionState& session) {
  // Ordered before the option net for the same reason outreg2's `word` check is
  // (see there): a missing WRITER is a command-level refusal at rc 199, and the
  // net would otherwise answer an option-level rc 198 first.
  // frmttable's DEFAULT output is RTF (.doc) -- the oracle's own no-`tex` arm
  // writes `{\rtf1\ansi\deff0...}` (base h8). This build has no RTF writer, so
  // that form is refused BY NAME rather than writing LaTeX under a .doc
  // extension, which is the silent-wrong-output shape.
  if (!hasFlag(parsed.opts, "tex")) {
    return ojson(eval::refusedCommand("frmttable", "no_rtf_writer"));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFrmttable.known,
                                            kVocabFrmttable.unimplemented)) {
    return ojson(*err);
  }
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("frmttable: `using FILENAME' is required");
  }
  const std::optional<std::string> statmat = optStr(parsed.opts, "statmat");
  if (!statmat.has_value()) {
    // The no-`statmat` forms are the accumulate model (`replay`, or `addrows`
    // building a table from nothing), which this build does not carry; saying so
    // beats "statmat is required by this build", which reads as a syntax rule.
    return tblSyntax("frmttable: statmat(MATRIX) is required by this build -- the forms that "
                     "build a table WITHOUT one (replay, addrows) need frmttable's stored-table "
                     "accumulator, which is not implemented");
  }
  const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*statmat));
  if (mat == nullptr) return tblError("matrix " + dequote(*statmat) + " not found");

  // sdec(): a comma list, per column, LAST ENTRY REPEATING (cells2 k2/k4).
  std::vector<int> sdec;
  if (auto sd = optStr(parsed.opts, "sdec")) {
    for (const auto& tok : splitTopLevel(dequote(*sd), ',')) {
      try {
        size_t pos = 0;
        const int n = std::stoi(strutil::trim(tok), &pos);
        if (n >= 0 && n <= 12) sdec.push_back(n);
      } catch (const std::exception&) {
      }
    }
  }
  if (sdec.empty()) sdec.push_back(3);
  auto decFor = [&](size_t c) { return sdec[std::min(c, sdec.size() - 1)]; };

  std::vector<std::vector<std::string>> ctRows;
  if (auto ct = optStr(parsed.opts, "ctitles")) ctRows = frmtTitleRows(*ct);
  std::vector<std::string> rtitles;
  if (auto rt = optStr(parsed.opts, "rtitles")) {
    for (const auto& row : frmtTitleRows(*rt)) rtitles.push_back(row.empty() ? std::string() : row[0]);
  }

  const bool rowNamesReal = !mat->rownames.empty() && !frmtDefaultNames(mat->rownames, "r");
  const bool colNamesReal = !mat->colnames.empty() && !frmtDefaultNames(mat->colnames, "c");
  const bool stub = !rtitles.empty() || rowNamesReal;
  if (ctRows.empty() && colNamesReal) {
    std::vector<std::string> head;
    if (stub) head.push_back(std::string());
    for (const auto& c : mat->colnames) head.push_back(c);
    ctRows.push_back(head);
  }

  const size_t dataCols = mat->data.empty() ? mat->colnames.size() : mat->data[0].size();
  size_t width = dataCols + (stub ? 1 : 0);
  for (const auto& r : ctRows) width = std::max(width, r.size());
  const size_t nRows = std::max(mat->data.size(), rtitles.size());

  auto joinRow = [&](std::vector<std::string> cells) {
    cells.resize(width);
    return joinWith(cells, " & ") + "\\\\";
  };

  std::vector<std::string> lines;
  const bool fragment = hasFlag(parsed.opts, "fragment");
  const bool nocenter = hasFlag(parsed.opts, "nocenter");
  if (!fragment) {
    lines.push_back("\\documentclass[]{article}\\pagestyle{empty}\\begin{document}");
  }
  if (!nocenter) lines.push_back("\\begin{center}");
  if (auto ti = optStr(parsed.opts, "title")) {
    lines.push_back("\\smallskip\\begin{large}" + dequote(*ti) + "\\end{large}\\\\");
    lines.push_back("\\smallskip");
  }
  std::string spec = stub ? "l" : "";
  for (size_t c = (stub ? 1 : 0); c < width; ++c) spec += "c";
  lines.push_back("\\begin{tabular}{" + spec + "}");

  // The rule idiom: the FIRST line inside the tabular opens with
  // `\hline \noalign{\smallskip}` whatever it is, and when header rows exist the
  // first DATA row carries `\noalign{\smallskip}\hline \noalign{\smallskip}`.
  bool first = true;
  for (const auto& hr : ctRows) {
    const std::string body = joinRow(hr);
    lines.push_back((first ? "\\hline \\noalign{\\smallskip}" : "") + body);
    first = false;
  }
  for (size_t r = 0; r < nRows; ++r) {
    std::vector<std::string> cells;
    if (stub) {
      cells.push_back(r < rtitles.size()
                          ? rtitles[r]
                          : (r < mat->rownames.size() && rowNamesReal ? mat->rownames[r]
                                                                      : std::string()));
    }
    if (r < mat->data.size()) {
      for (size_t c = 0; c < mat->data[r].size(); ++c) {
        cells.push_back(frmtCell(mat->data[r][c], decFor(c)));
      }
    }
    std::string prefix;
    if (first) prefix = "\\hline \\noalign{\\smallskip}";
    else if (r == 0 && !ctRows.empty()) prefix = "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}";
    lines.push_back(prefix + joinRow(cells));
    first = false;
  }
  lines.push_back("\\noalign{\\smallskip}\\hline\\end{tabular}\\\\");
  if (auto n = optStr(parsed.opts, "note")) {
    lines.push_back("\\begin{footnotesize}" + dequote(*n) + "\\end{footnotesize}\\\\");
    lines.push_back("\\smallskip");
  }
  if (!nocenter) lines.push_back("\\end{center}");
  if (!fragment) lines.push_back("\\end{document}");

  std::string body;
  for (const auto& l : lines) body += l + "\n";
  return emitTable("frmttable", session, ensureExt(*usingPath, ".tex"), body,
                   hasFlag(parsed.opts, "replace"), false);
}

// ===========================================================================
// regsave / parmest -- the estimates as a DATASET (plan design decision 9)
//
// One builder, two column vocabularies. The rows go out as CSV, `cobra convert`
// turns them into an .hbin, and then either `export-dta` writes the requested
// file (session untouched) or `session.load` makes the table the dataset in
// memory. The CI columns come from the coeftable the ENGINE produced -- this
// build does not own an inverse-t, and recomputing a 90% interval from a 95%
// fit with a normal quantile would be a quietly wrong number, which is why a
// `level()` other than the fitted one is refused below rather than approximated.
// ===========================================================================

namespace {

struct DsColumn {
  std::string name;
  bool isString = false;
  std::vector<double> num;
  std::vector<std::string> str;
};

// CSV escaping for the convert step. `cobra convert --delim ,` reads a plain
// RFC4180 file; a variable name or label containing a comma would otherwise
// shift every later column by one.
std::string csvCell(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// Build the .hbin. Returns the path, or an error block through `errOut`.
bool materializeDataset(SessionState& session, const std::string& cmd,
                        const std::vector<DsColumn>& cols, size_t nRows, std::string* pathOut,
                        ojson* errOut) {
  if (cols.empty() || nRows == 0) {
    *errOut = tblError(cmd + ": the estimation has no coefficients to save");
    return false;
  }
  // mintScratchPath always names a ".hbin"; the CSV beside it has to be
  // registered under its OWN name or the scratch census counts it as an orphan
  // (BUG-389 -- sweepOrphanScratch only removes paths it knows).
  const std::string hbin = mintScratchPath(session.workdir(), cmd + "_ds");
  std::string csv = mintScratchPath(session.workdir(), cmd + "_csv");
  forgetScratchMint(csv);
  csv = csv.substr(0, csv.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      *errOut = tblError(cmd + ": could not write the intermediate table", "data_io");
      return false;
    }
    for (size_t c = 0; c < cols.size(); ++c) {
      if (c) f << ",";
      f << csvCell(cols[c].name);
    }
    f << "\n";
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (size_t r = 0; r < nRows; ++r) {
      for (size_t c = 0; c < cols.size(); ++c) {
        if (c) f << ",";
        if (cols[c].isString) {
          f << csvCell(r < cols[c].str.size() ? cols[c].str[r] : std::string());
        } else {
          const double v = r < cols[c].num.size() ? cols[c].num[r]
                                                  : std::numeric_limits<double>::quiet_NaN();
          // An EMPTY field is how the loader spells a missing value; writing
          // "nan" would land a literal token in a numeric column and writing 0
          // would invent an estimate.
          if (!std::isnan(v)) f << v;
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      *errOut = tblError(cmd + ": failed writing the intermediate table (disk full?)", "data_io");
      return false;
    }
  }
  const CobraRunResult cr =
      runCobra({"convert", "--data", csv, "--out", hbin, "--delim", ","});
  if (!cr.ok || !cecon::native_exists(hbin)) {
    *errOut = tblError(cmd + ": could not build the result dataset");
    return false;
  }
  *pathOut = hbin;
  return true;
}

// Common tail: `using`/`saving()` writes a .dta and leaves the session alone;
// otherwise the built table BECOMES the dataset in memory.
ojson finishDataset(SessionState& session, const std::string& cmd, const std::string& hbin,
                    const std::optional<std::string>& outUser, bool replace, bool appendUsing,
                    const std::vector<std::string>& newVars) {
  if (outUser.has_value() && !outUser->empty()) {
    const std::string userPath = ensureExt(*outUser, ".dta");
    const std::string resolved = resolveUserPath(session, userPath);
    // `append` is a legitimate reason for the destination to already exist, so
    // it counts as permission here -- without this an `append` onto the file it
    // was asked to extend errors with "already exists".
    ojson guard = guardOverwrite(cmd, resolved, userPath, replace, appendUsing);
    if (!guard.is_null()) return guard;
    std::string src = hbin;
    if (appendUsing && cecon::native_exists(resolved)) {
      // `append` stacks onto the EXISTING saved table -- one row block per
      // model across a do-file, which is what regsave's append means.
      //
      // The existing file is a `.dta`, and `cobra append --using` opens its
      // argument with Dataset::open, which reads `.hbin` ONLY (src/io/
      // columnar.cpp:724 parses an hbin header). Handing it the .dta directly
      // fails; worse, a failure here after the export below would already have
      // replaced the file the append was supposed to extend. So it is imported
      // first, and every step is checked before anything touches the
      // destination.
      const std::string prior = mintScratchPath(session.workdir(), cmd + "_prior");
      const CobraRunResult imp =
          runCobra({"import-dta", "--data", resolved, "--out", prior});
      if (!imp.ok || !cecon::native_exists(prior)) {
        return tblError(cmd + ": cannot read " + userPath +
                        " to append to it (append needs a .dta this build can read)");
      }
      const std::string merged = mintScratchPath(session.workdir(), cmd + "_app");
      const CobraRunResult cr =
          runCobra({"append", "--data", prior, "--using", src, "--out", merged});
      if (!cr.ok || !cecon::native_exists(merged)) {
        return tblError(cmd + ": could not append to " + userPath);
      }
      src = merged;
    }
    // export-dta writes the destination directly, so it goes to a sibling temp
    // first and is renamed into place -- the same guarantee every other writer
    // in this file gives, and the reason a failed export cannot destroy the
    // file it was meant to replace.
    const std::string tmp = resolved + ".piet_tmp";
    const CobraRunResult cr = runCobra({"export-dta", "--data", src, "--out", tmp});
    if (!cr.ok || !cecon::native_exists(tmp)) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": could not write " + userPath +
                      " (this build needs a cobra engine with `export-dta')");
    }
    std::error_code mvEc;
    std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
    if (mvEc) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath, "data_io");
    }
    return displayBlock("file " + userPath + " saved");
  }
  // No destination: the table replaces the dataset in memory. `droppedVars` is
  // every column that was there before, because none of them survive.
  std::vector<std::string> dropped;
  if (session.datastore() != nullptr) dropped = session.datastore()->names();
  session.load(hbin);
  return mutationResult(session, cmd, newVars, dropped);
}

// The coefficient rows of the ACTIVE estimation, for both builders.
struct CoefRowView {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
  double t = std::numeric_limits<double>::quiet_NaN();
  double p = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
};

std::vector<CoefRowView> coefViews(const ojson& snap) {
  std::vector<CoefRowView> out;
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return out;
  for (const auto& r : snap["coeftable"]) {
    CoefRowView v;
    v.name = r.value("name", std::string());
    if (v.name == "(intercept)") v.name = "_cons";
    if (r.contains("b")) v.b = jsonNum(r["b"]);
    if (r.contains("se")) v.se = jsonNum(r["se"]);
    if (r.contains("t")) v.t = jsonNum(r["t"]);
    if (r.contains("p")) v.p = jsonNum(r["p"]);
    if (r.contains("ci_lo")) v.lo = jsonNum(r["ci_lo"]);
    if (r.contains("ci_hi")) v.hi = jsonNum(r["ci_hi"]);
    out.push_back(v);
  }
  return out;
}

// `level(#)` other than the level the model was FITTED at cannot be honoured
// without an inverse-t this build does not have. Refused by name; the fix the
// message names (refit with level(#)) produces the right interval.
std::optional<ojson> rejectForeignLevel(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& cmd) {
  const std::optional<std::string> lv = optStr(opts, "level");
  if (!lv.has_value()) return std::nullopt;
  const std::string s = strutil::trim(dequote(*lv));
  if (s == "95" || s == "95.0") return std::nullopt;
  return tblError(cmd + ": level(" + s +
                  ") is not implemented in this build; the confidence limits come from the fit, "
                  "so refit the model with level(" + s + ") instead");
}

}  // namespace

ojson runRegsave(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRegsave.known,
                                            kVocabRegsave.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  if (!activeModel(session, "regsave", &m, &err)) return err;
  if (auto lvErr = rejectForeignLevel(parsed.opts, "regsave")) return *lvErr;

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  const std::vector<CoefRowView> rows = coefViews(m.snap);

  std::vector<DsColumn> cols;
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  {
    DsColumn v{"var", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  {
    DsColumn& c = addNum("coef");
    for (const auto& r : rows) c.num.push_back(r.b);
  }
  {
    DsColumn& c = addNum("stderr");
    for (const auto& r : rows) c.num.push_back(r.se);
  }
  if (hasFlag(parsed.opts, "tstat")) {
    DsColumn& c = addNum("tstat");
    for (const auto& r : rows) c.num.push_back(r.t);
  }
  if (hasFlag(parsed.opts, "pval")) {
    DsColumn& c = addNum("pval");
    for (const auto& r : rows) c.num.push_back(r.p);
  }
  if (hasFlag(parsed.opts, "ci")) {
    DsColumn& lo = addNum("ci_lower");
    for (const auto& r : rows) lo.num.push_back(r.lo);
    DsColumn& hi = addNum("ci_upper");
    for (const auto& r : rows) hi.num.push_back(r.hi);
  }
  {
    DsColumn& c = addNum("N");
    const double n = m.escalars.contains("N") ? m.escalars["N"].get<double>()
                                              : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(n);
  }
  {
    DsColumn& c = addNum("r2");
    const double r2 = m.escalars.contains("r2") ? m.escalars["r2"].get<double>()
                                                : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(r2);
  }
  if (auto al = optStr(parsed.opts, "addlabel")) {
    for (const auto& [name, val] : parseCommaPairs(*al)) {
      DsColumn c{name, true, {}, {}};
      for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(val);
      cols.push_back(c);
    }
  }
  if (hasFlag(parsed.opts, "cmdline")) {
    DsColumn c{"cmdline", true, {}, {}};
    const std::string cl = m.snap.value("estimator", std::string()) + " " +
                           m.snap.value("depvar", std::string());
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(strutil::trim(cl));
    cols.push_back(c);
  }

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "regsave", cols, rows.size(), &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "regsave", hbin, usingPath, hasFlag(parsed.opts, "replace"),
                       hasFlag(parsed.opts, "append"), newVars);
}

ojson runParmest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabParmest.known,
                                            kVocabParmest.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  if (!activeModel(session, "parmest", &m, &err)) return err;
  if (auto lvErr = rejectForeignLevel(parsed.opts, "parmest")) return *lvErr;

  // `saving("f.dta", replace)` -- the replace lives INSIDE the parentheses,
  // which is parmest's own syntax and not the outer `replace` option.
  std::optional<std::string> savePath;
  bool saveReplace = hasFlag(parsed.opts, "replace");
  if (auto sv = optStr(parsed.opts, "saving")) {
    const std::vector<std::string> parts = splitTopLevel(*sv, ',');
    if (!parts.empty()) savePath = dequote(strutil::trim(parts[0]));
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string w = strutil::lower(strutil::trim(parts[i]));
      if (w == "replace" || w == "repl") saveReplace = true;
    }
  }

  const std::vector<CoefRowView> rows = coefViews(m.snap);
  const double dfr = m.escalars.contains("df_r") ? m.escalars["df_r"].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();

  std::vector<DsColumn> cols;
  {
    DsColumn v{"parm", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  { DsColumn& c = addNum("estimate"); for (const auto& r : rows) c.num.push_back(r.b); }
  { DsColumn& c = addNum("stderr");   for (const auto& r : rows) c.num.push_back(r.se); }
  {
    DsColumn& c = addNum("dof");
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(dfr);
  }
  { DsColumn& c = addNum("t");        for (const auto& r : rows) c.num.push_back(r.t); }
  { DsColumn& c = addNum("p");        for (const auto& r : rows) c.num.push_back(r.p); }
  // `min95`/`max95` are parmest's names for the fitted 95% limits; a foreign
  // level() was already refused above, so the names cannot lie about the level.
  { DsColumn& c = addNum("min95");    for (const auto& r : rows) c.num.push_back(r.lo); }
  { DsColumn& c = addNum("max95");    for (const auto& r : rows) c.num.push_back(r.hi); }
  if (hasFlag(parsed.opts, "label")) {
    DsColumn c{"label", true, {}, {}};
    for (const auto& r : rows) c.str.push_back(session.eval.varLabels.get(r.name));
    cols.push_back(c);
  }
  if (auto idn = optStr(parsed.opts, "idnum")) {
    DsColumn& c = addNum("idnum");
    double v = std::numeric_limits<double>::quiet_NaN();
    try {
      size_t pos = 0;
      v = std::stod(dequote(*idn), &pos);
      if (pos != dequote(*idn).size()) v = std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
    }
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(v);
  }
  if (auto ids = optStr(parsed.opts, "idstr")) {
    DsColumn c{"idstr", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(dequote(*ids));
    cols.push_back(c);
  }
  if (hasFlag(parsed.opts, "omit")) {
    // `omit` drops the coefficients the fit omitted (a null b), which is what
    // it means; without it they stay in the table as missing rows.
    std::vector<size_t> keep;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (!std::isnan(rows[i].b)) keep.push_back(i);
    }
    for (auto& c : cols) {
      DsColumn nc{c.name, c.isString, {}, {}};
      for (size_t i : keep) {
        if (c.isString) {
          nc.str.push_back(i < c.str.size() ? c.str[i] : std::string());
        } else {
          nc.num.push_back(i < c.num.size() ? c.num[i]
                                            : std::numeric_limits<double>::quiet_NaN());
        }
      }
      c = nc;
    }
  }
  const size_t nRows = cols.empty() ? 0 : (cols[0].isString ? cols[0].str.size()
                                                            : cols[0].num.size());

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "parmest", cols, nRows, &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "parmest", hbin, savePath, saveReplace, false, newVars);
}

// ===========================================================================
// mat2txt
// ===========================================================================

ojson runMat2txt(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMat2txt.known,
                                            kVocabMat2txt.unimplemented)) {
    return ojson(*err);
  }
  const std::optional<std::string> matName = optStr(parsed.opts, "matrix");
  if (!matName.has_value()) return tblSyntax("mat2txt: matrix(NAME) is required");
  const std::optional<std::string> saving = optStr(parsed.opts, "saving");
  if (!saving.has_value()) return tblSyntax("mat2txt: saving(FILENAME) is required");

  const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*matName));
  if (mat == nullptr) return tblError("matrix " + dequote(*matName) + " not found");

  bool fmtOk = true;
  // mat2txt's own default is %10.0g -- a general format, not three decimals.
  std::string fmt = "%10.0g";
  if (auto f = optStr(parsed.opts, "format")) {
    fmt = normalizeFmt(*f, &fmtOk);
    if (!fmtOk) return tblSyntax("mat2txt: format(" + *f + ") is not a supported %fmt");
  }

  Table t;
  if (auto ti = optStr(parsed.opts, "title")) t.title = dequote(*ti);
  t.colHeads = mat->colnames;
  for (size_t r = 0; r < mat->data.size(); ++r) {
    TRow row;
    row.kind = TRow::kStat;
    row.label = r < mat->rownames.size() ? mat->rownames[r] : ("r" + std::to_string(r + 1));
    for (double v : mat->data[r]) row.cells.push_back(fmtNum(v, fmt));
    t.body.push_back(row);
  }
  if (auto n = optStr(parsed.opts, "note")) t.notes.push_back(dequote(*n));

  return emitTable("mat2txt", session, ensureExt(dequote(*saving), ".txt"), renderDelim(t, '\t'),
                   hasFlag(parsed.opts, "replace"), hasFlag(parsed.opts, "append"));
}

// ===========================================================================
// erepost
// ===========================================================================

ojson runErepost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabErepost.known,
                                            kVocabErepost.unimplemented)) {
    return ojson(*err);
  }
  if (!session.lastEstBlock.has_value()) {
    return tblErrorRc("erepost: no estimation result in memory. Run a regression first.", 301);
  }
  std::string args = argsAfterHead(parsed.raw);
  if (args.empty()) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");

  // `erepost b = M` / `erepost V = M`. Both spellings are accepted; only `b` is
  // implemented, and `V` is refused BY NAME rather than accepted-and-ignored,
  // because a table built from a reposted b with the OLD V would report
  // standard errors that belong to different coefficients.
  const size_t eq = args.find('=');
  if (eq == std::string::npos) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");
  const std::string lhs = strutil::trim(args.substr(0, eq));
  const std::string rhs = strutil::trim(args.substr(eq + 1));
  if (lhs == "V" || lhs == "v") {
    return ojson(eval::unsupportedCommand("erepost V=", std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (lhs != "b") return tblSyntax("erepost: only `erepost b = MATRIX' is supported");

  const eval::EvalMatrix* mat = session.eval.findMatrix(rhs);
  if (mat == nullptr) return tblError("matrix " + rhs + " not found");
  if (mat->data.empty()) return tblError("erepost: matrix " + rhs + " is empty");

  // Stata's e(b) is 1 x k, so a k x 1 column vector is read as the same vector
  // rather than rejected -- but anything with two real dimensions is not a
  // coefficient vector and says so with both shapes named.
  std::vector<double> vals;
  std::vector<std::string> names;
  if (mat->data.size() == 1) {
    vals = mat->data[0];
    names = mat->colnames;
  } else if (mat->data[0].size() == 1) {
    for (const auto& row : mat->data) vals.push_back(row.empty() ? 0.0 : row[0]);
    names = mat->rownames;
  } else {
    return tblError("erepost: " + rhs + " is " + std::to_string(mat->data.size()) + "x" +
                    std::to_string(mat->data[0].size()) + " but e(b) is a vector");
  }

  ojson block = *session.lastEstBlock;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) {
    return tblError("erepost: the last estimation has no coefficient vector");
  }
  const size_t k = block["coeftable"].size();
  if (vals.size() != k) {
    return tblError("erepost: " + rhs + " is 1x" + std::to_string(vals.size()) +
                    " but e(b) has " + std::to_string(k) + " columns");
  }

  const bool rename = hasFlag(parsed.opts, "rename");
  for (size_t i = 0; i < k; ++i) {
    ojson& row = block["coeftable"][i];
    row["b"] = std::isnan(vals[i]) ? ojson(nullptr) : ojson(vals[i]);
    // NO V was reposted, so se/t/p no longer belong to these coefficients.
    // They become NULL -- Stata's missing -- and NOT 0: a zero standard error
    // reads as an infinitely precise estimate, and a zero p as significance.
    row["se"] = nullptr;
    row["t"] = nullptr;
    row["p"] = nullptr;
    if (row.contains("ci_lo")) row["ci_lo"] = nullptr;
    if (row.contains("ci_hi")) row["ci_hi"] = nullptr;
    if (rename && i < names.size() && !names[i].empty()) row["name"] = names[i];
  }
  session.lastEstBlock = block;
  // LastModel caches the raw result separately, and `setLastModel` is the only
  // path that also mirrors it into `eval.lastModelResult` -- which is what
  // `_b[x]` and `display e(...)` read. Updating one and not the other is the
  // two-layer divergence this codebase keeps paying for.
  if (session.lastModel().has_value()) {
    LastModel lm = *session.lastModel();
    if (lm.result.has_value()) {
      ojson res = *lm.result;
      if (res.contains("coeftable") && res["coeftable"].is_array() &&
          res["coeftable"].size() == k) {
        for (size_t i = 0; i < k; ++i) {
          res["coeftable"][i] = block["coeftable"][i];
        }
        lm.result = res;
        session.setLastModel(lm);
      }
    }
  }
  return displayBlock("(e(b) reposted from " + rhs +
                      "; e(V) is not reposted, so e(se) is missing)");
}

}  // namespace piet::handlers
