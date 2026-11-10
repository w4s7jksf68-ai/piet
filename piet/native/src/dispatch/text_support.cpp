// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {


ojson echoBlock(const std::string& cmd) {
  dprof::noteJsonNode();
  ojson e;
  e["type"] = "echo";
  // A folded `mata:` block (mata::captureMataBlocks) is one line joined by a
  // control byte; show it the way Stata's log does, one `: stmt` per line. The
  // byte occurs in no other line, so every other echo is unchanged.
  e["text"] = ".  " + (cmd.find(mata::kMataStmtSep) == std::string::npos &&
                               cmd.find(mata::kMataUnterminated) == std::string::npos
                           ? cmd
                           : mata::renderFoldedMataForEcho(cmd));
  return e;
}


// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }


std::string rstripColon(std::string s) {
  while (!s.empty() && s.back() == ':') s.pop_back();
  return s;
}


// BUG-105 (WS-K) — the escape hatch for handlers that re-parse the RAW line
// with their OWN command-word pattern instead of reading the routed token.
//
// The 2026-07-31 design assumed normalising the routed token at the single `cmd`
// formation site subsumed every second copy of a spelling set. Measured: it does
// for the if-chain arms, kNoDataCommands and kFixedArityCommands, but NOT for
// two handlers that hold their own copy —
//   · handlers_m3.cpp's runMatrixDef (five `^\s*mat(?:rix)?\s+…` regexes)
//   · handlers_m3.cpp's runLabelDefine (`^label\s+define\s+`)
// — because they receive `parsed.raw`, never `cmd`. Under the widened sets
// `matr list X` and `labe define L 1 "a"` would have fallen through to those
// handlers' generic "unsupported form" rejects: a WRONG ANSWER wearing a
// plausible message, which is worse than the r(199) they used to get.
//
// Rewriting only the LEADING token keeps the spelling knowledge in the registry
// and leaves those handlers' patterns untouched. Case-insensitive on the token
// (the tokenizer lowercases the routed name; the raw line keeps the user's
// case), and a no-op whenever the typed spelling is already canonical — so
// every non-abbreviated line is byte-for-byte unchanged.
std::string rawWithCanonicalCmd(const std::string& raw, const std::string& typed,
                                const std::string& canonical) {
  if (typed == canonical || typed.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  if (raw.size() - b < typed.size()) return raw;
  for (size_t i = 0; i < typed.size(); ++i) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(raw[b + i]))) != typed[i]) {
      return raw;
    }
  }
  size_t e = b + typed.size();
  // Must be a whole token, not a prefix of a longer word (`sav` inside `saved`).
  if (e < raw.size() && cecon::ident::isIdentByte(raw[e])) {  // `diقيمة` is one word too
    return raw;
  }
  return raw.substr(0, b) + canonical + raw.substr(e);
}


// BUG-251 (WS-Q) — the argument tail of a command line, derived WITHOUT any
// knowledge of the command's spelling.
//
// BUG-232 fixed four sites that located the command word in `parsed.raw` with a
// LITERAL (`find("save")`, `find("label")`), by switching them to the typed
// spelling `cmdTyped`. That closes the reachable crashes, but it leaves the
// mechanism intact: every one of those sites still asks "where in this line is
// the command word?" and still indexes off the answer with no npos guard. The
// answer is only ever "at the front", because eval::tokenize() sets
// `pc.raw = strip(line)` — the raw it hands the arms is the very line the
// command word was tokenized out of, leading whitespace already gone.
//
// So the honest formulation is not "find the word" but "skip the leading
// token", which is what every one of these call sites actually wanted. It
// carries ZERO spelling knowledge, so it is not a second copy of any spelling
// set and cannot drift from the registry; and it cannot throw, whatever the
// line looks like.
//
// The token ends at the first character that is not [A-Za-z0-9_], NOT at the
// first space — deliberately, to stay byte-compatible with the `find(cmd) +
// cmd.size()` arithmetic it replaces: `dis(2+2)` must still yield `(2+2)` and
// `cd/tmp` must still yield `/tmp`, which a whitespace-delimited split would
// swallow whole.
//
// WHY THE OLD SHAPE WAS A CRASH AND NOT MERELY WRONG: `find()` returns npos ==
// SIZE_MAX, and the sites add a small constant to it. That addition WRAPS —
// `npos + 3` is 2, not a huge number — so `substr(npos + 3)` silently reads
// from offset 2 for any line at least 2 long (a wrong answer, quietly), and
// throws std::out_of_range only for a line SHORTER than that. Measured: with
// `use` given its Stata abbreviations, the one-character line `u` threw
// `cpp_exception: basic_string` and killed the whole --do run, while `us` and
// `u , clear` returned plausible output from the wrong offset. A bare
// `raw.substr(labelPos)` with labelPos == npos throws unconditionally.
std::string rawArgsAfterCmd(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  size_t e = b;
  while (e < raw.size() && cecon::ident::isIdentByte(raw[e])) ++e;  // core/ident.h
  if (e == b) return std::string();  // no leading identifier token at all
  size_t a = raw.find_first_not_of(" \t", e);
  // strutil::trim, not trimWs() — the latter is this file's thin wrapper over
  // it and is not declared until further down.
  return (a == std::string::npos) ? std::string() : strutil::trim(raw.substr(a));
}




// generate/replace expression text (FU-639/FU-652): joinVarlist(varlist) puts
// one blank between the blank-split tokens, which collapses a run of blanks
// inside a string literal (`generate b = "x  y"` stored "x y"). argText is the
// same text as typed; blank runs outside quotes still collapse to one blank, so
// everything downstream sees the joinVarlist spelling except inside literals.
// A varlist that no longer matches argText (rewritten after tokenize) keeps the
// joinVarlist text.
std::string genExprText(const eval::ParsedCommand& parsed) {
  std::istringstream iss(parsed.argText);
  std::vector<std::string> toks;
  std::string tok;
  while (iss >> tok) toks.push_back(tok);
  if (toks != parsed.varlist) return joinVarlist(parsed.varlist);
  const std::string& a = parsed.argText;
  const std::vector<bool> quoted = eval::quotedSpanMask(a);
  std::string out;
  bool pendingBlank = false;
  for (size_t i = 0; i < a.size(); ++i) {
    const bool ws = std::isspace(static_cast<unsigned char>(a[i])) != 0;
    if (ws && !quoted[i]) {
      pendingBlank = !out.empty();
      continue;
    }
    if (pendingBlank) out += ' ';
    pendingBlank = false;
    out += a[i];
  }
  return out;
}


// --- sysuse (mission 3, hardening queue) path helpers -----------------------
// `sysuse DATASET` resolves a bundled sample dataset by bare name, no path.
// PIET ships exactly one today — piet/packaging/sample/fatalities.csv, the
// SAME fixture main.cpp's runUiSmoke() already loads bundle-first via
// piet::bundleResourcesDir()+"/sample/fatalities.csv", dev-tree
// piet/packaging/sample/fatalities.csv otherwise (see main.cpp:957-968).
// This mirrors that exact resolution order rather than inventing a second
// convention; the dev-tree anchor itself is shared (piet/bundle_paths.h,
// devTreePath) since BUG-1939 showed that a per-site __FILE__ anchor breaks
// identically at every site under -ffile-prefix-map.
bool fileExistsLocal(const std::string& path) {
  if (path.empty()) return false;
  std::ifstream f(cecon::to_native_path(path));
  return f.good();
}


std::string devTreeSampleFatalitiesCsv() {
  // BUG-1939: resolved through piet::devTreePath (executable-relative with the
  // compile-time anchor only as a first candidate), not through __FILE__ alone.
  return piet::devTreePath("/piet/packaging/sample/fatalities.csv");
}


// bysort.py's needs_bygen: `_n` or `_N` as a whole identifier token anywhere
// in expr (not part of a longer identifier). A conservative scan mirroring
// the oracle's regex intent without pulling in <regex> for two literals.
std::string dispatcherNextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}


bool hasNorCapN(const std::string& expr) {
  // A byte >= 0x80 is a name byte (core/ident.h): `_nملاحظة` is a name, not `_n`.
  auto isIdentChar = [](char c) { return cecon::ident::isIdentByte(c); };
  for (size_t i = 0; i + 1 < expr.size() + 1; ++i) {
    for (const char* needle : {"_n", "_N"}) {
      size_t len = 2;
      if (i + len > expr.size()) continue;
      if (expr.compare(i, len, needle) != 0) continue;
      bool leftOk = (i == 0) || !isIdentChar(expr[i - 1]);
      bool rightOk = (i + len >= expr.size()) || !isIdentChar(expr[i + len]);
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}


// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// lower() wrapper above for why this stays a thin pass-through.
std::string trimWs(const std::string& s) { return strutil::trim(s); }


// BUG-525 (W2 labelglued; oracle w2_cmdtriage.log arms di_glued, corr_glued,
// levelsof_glued, erase_glued): Stata ends the command word at the first
// character that cannot be part of a name, so `di("glued")`, `corr(a c)`,
// `levelsof(g), local(lv)` and `erase"ct_tmp.dta"` all run (rc 0 each; the
// glued corr's r(rho) equals the spaced control's). PIET cut the word at
// whitespace, so each was looked up as a command literally named
// `di("glued")` -> r(199). Normalising the RAW line once, at the top of
// dispatch, is what makes every downstream reader agree: the prefix
// strippers, the tokenizer, and the handlers that re-parse parsed.raw against
// their own `^cmd\s+` anchors (runErase, handleDisplay, runLabelDefine ...).
// Only the leading name is touched, and only when `(` or `"` is glued to it;
// a line that does not begin with a name (`*`, `//`, a macro reference, a
// quote, a digit) comes back byte-identically. The echo block is built from
// the line as typed, before this runs, so the log shows what the user wrote.
std::string splitGluedHead(const std::string& line) {
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size()) return line;
  if (!cecon::ident::isIdentStartByte(line[b])) return line;  // core/ident.h
  size_t e = b + 1;
  while (e < line.size() && cecon::ident::isIdentByte(line[e])) ++e;
  if (e >= line.size() || (line[e] != '(' && line[e] != '"')) return line;
  return line.substr(0, e) + " " + line.substr(e);
}

constexpr LabelSubFloor kLabelSubFloors[] = {
    {"data", 2}, {"variable", 3}, {"define", 2}, {"values", 3},   {"dir", 2},      {"list", 1},
    {"copy", 4}, {"drop", 4},     {"rename", 6}, {"save", 4},     {"language", 4},
};

// `sub` is the typed token, lowercased. The floors are chosen so that at its
// minimum every subcommand's prefix is unique (`de`/`di`/`da` at 2, `l` at 1 --
// `language` needs `lang`), so the first floor-satisfying hit is the only one.
LabelSubMatch resolveLabelSub(const std::string& sub, std::string* canonical) {
  bool sawPrefix = false;
  for (const auto& f : kLabelSubFloors) {
    const std::string full(f.full);
    if (sub.empty() || sub.size() > full.size() || full.compare(0, sub.size(), sub) != 0) continue;
    sawPrefix = true;
    if (sub.size() >= f.minLen) {
      *canonical = full;
      return LabelSubMatch::kResolved;
    }
  }
  return sawPrefix ? LabelSubMatch::kTooShort : LabelSubMatch::kUnknown;
}


// BUG-524: rawWithCanonicalCmd's idea one token further in. runLabelDefine
// anchors on `^label\s+define\s+` in the raw line, so a subcommand typed as an
// abbreviation is rewritten to its full spelling there (after the command word
// has been). Whole-token match only; anything unexpected returns the line
// unchanged, exactly as the command-word helper does.
std::string rawWithCanonicalLabelSub(const std::string& raw, const std::string& typedSub,
                                     const std::string& canonicalSub) {
  if (typedSub == canonicalSub || typedSub.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  size_t e1 = raw.find_first_of(" \t", b);
  if (e1 == std::string::npos) return raw;
  size_t s = raw.find_first_not_of(" \t", e1);
  if (s == std::string::npos) return raw;
  size_t e = raw.find_first_of(" \t", s);
  if (e == std::string::npos) e = raw.size();
  if (lower(raw.substr(s, e - s)) != typedSub) return raw;
  return raw.substr(0, s) + canonicalSub + raw.substr(e);
}

void pushEnvResult(ojson& blocks, const ojson& r) {
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else if (!r.is_null()) {
      blocks.push_back(r);
    }
  }

}
namespace piet {
using namespace dispatch_detail;


// Export only the shared token joiner from the surrounding private helpers.

std::string joinVarlist(const std::vector<std::string>& varlist) {
  std::string s;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) s += " ";
    s += varlist[i];
  }
  return s;
}
}
