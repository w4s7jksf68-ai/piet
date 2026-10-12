#include "piet/program.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>   // BUG-360: ScopeGuard reaps this call's tempfiles
#include <regex>
#include <sstream>
#include <system_error>

#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-360 reap)

namespace piet::program {

namespace {

constexpr int kMaxRecursion = 100;

// ---------------------------------------------------------------------------
// P4: eclass/sclass support.
//
// ProgramDef (piet/session_state.h) carries only the pre-existing `rclass`
// bool — this stint's file manifest does NOT include session_state.h (or
// any other header), so the struct cannot grow an `eclass`/`sclass` field.
// Two reserved-string conventions stand in for the header change that would
// otherwise be needed, both confined to this translation unit's own state
// flow (captureProgams -> runProgram -> handleReturnStmt):
//
//   kProgClassSentinelPrefix — captureProgams() APPENDS one inert trailing
//     comment line to an eclass/sclass program's body (never prepended, so
//     no real body line's position/number shifts). runProgram() has full
//     access to progDef.body already, so it recovers the class from that
//     trailing line with no new struct field.
//
//   kProgClassMarkerKey — runProgram() stages the recovered class onto a
//     reserved-name hidden local (a leading space can never occur in a real
//     Stata local name; see reLocalDef in eval/macros.cpp) on the callee's
//     freshly-pushed scope dict. handleReturnStmt()'s ereturn/sreturn
//     honesty checks read it back while that scope is live; ScopeGuard's
//     restore (below) discards it for free on return, exactly like any
//     other local — this is the SAME save/restore shape the rclass
//     r()/e() flush already uses, just carried on session.eval.locals
//     instead of a dedicated field.
//
// kSClassKeyPrefix must stay byte-identical to eval/macros.cpp's own copy
// (no shared header for this one either) — see that file's note. Both
// pieces exist ONLY because sreturn-local values need a persistent string
// store and "no new session stores" (P4 task constraint) rules out adding
// a dedicated one; session.eval.rStrings already IS such a store (used by
// `return local`/r()), so sclass values live there under a prefixed key
// that can never collide with a real r()-name (those are captured by an
// identifier-only regex — see reRRefDelim/reRRef in eval/macros.cpp).
const std::string kProgClassSentinelPrefix = "* __piet_progclass__ ";
const std::string kProgClassMarkerKey = " __piet_progclass";
const std::string kSClassKeyPrefix = "\x01s:";


// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// `program [define] NAME [, OPTION]` header line.
// Matches: program define myname / program myname / program define myname,
// rclass / program myname , rclass
const std::regex& reProgHeader() {
  // AER-SCOPE1 bundle A: `prog` and `prog def NAME` are what four AER packages
  // write (180 occurrences), and a program header is matched HERE, in the
  // pre-tokenize capture pass, so a registry abbreviation would never be
  // consulted. `de`/`def` join `define` for the same reason.
  static const std::regex re(R"(^prog(?:ram)?\s+(?:de(?:f(?:ine)?)?\s+)?(\w+)(?:\s*,\s*(\S+))?)",
                              std::regex::icase);
  return re;
}

// `end` terminator (whole line, any case).
const std::regex& reEnd() {
  static const std::regex re(R"(^end\s*$)", std::regex::icase);
  return re;
}

// `[capture] program drop NAME` — the redefine-idiom drop that PRECEDES a
// `program define NAME` (see captureProgams doc comment).
const std::regex& reProgDrop() {
  // `capt prog drop mht_rw` is the corpus's own spelling of the redefine idiom;
  // capture's truncations were already accepted here, program's were not.
  static const std::regex re(
      R"(^(?:cap(?:t(?:u(?:r(?:e)?)?)?)?\s+)?prog(?:ram)?\s+drop\s+(\w+)\s*$)",
      std::regex::icase);
  return re;
}

}  // namespace

// BUG-342 — see program.h. The prefix is a Stata comment, so a stray sentinel
// that ever escaped the interception in doexec.cpp would be inert rather than
// an honest-looking command; and the reserved token can never collide with a
// user line, since preprocessDo has already stripped every real comment by the
// time captureProgams inserts these.
const std::string kProgInstallSentinelPrefix = "* __piet_proginstall__ ";

bool isProgInstallSentinel(const std::string& line, std::string* nameOut) {
  if (line.compare(0, kProgInstallSentinelPrefix.size(), kProgInstallSentinelPrefix) != 0)
    return false;
  if (nameOut) *nameOut = line.substr(kProgInstallSentinelPrefix.size());
  return true;
}

// ---------------------------------------------------------------------------
// Capture pass (runs in executeDoSource BEFORE brace aggregation)

std::vector<std::string> captureProgams(const std::vector<std::string>& lines, SessionState& session) {
  std::vector<std::string> out;
  size_t i = 0;
  size_t n = lines.size();

  while (i < n) {
    const std::string& line = lines[i];
    std::string stripped = trim(line);

    std::smatch m;
    if (std::regex_search(stripped, m, reProgHeader())) {
      std::string progName = m[1].str();

      // BS-014: `program dir` / `program list` / bare `program drop NAME`
      // are management/list forms, not definitions — reProgHeader()'s
      // optional `define` group means it ALSO (wrongly) matches these bare
      // forms, capturing the reserved subcommand word as if it were a NEW
      // program name, then the body-consuming loop below hunts for a
      // matching `end` that will never come — silently swallowing every
      // remaining line of the do-file as a bogus program body. Bare (no
      // explicit `define` keyword) dir/list/drop fall through untouched to
      // the normal per-line path at the bottom of this function instead
      // (dispatcher.cpp's own `program dir`/`drop` pre-pass handles those
      // two at dispatch time; a bare `program list` reaches an honest
      // unsupported-command gap). The explicit `program define dir` form is
      // NOT affected — only the ambiguous bare spelling is exempted here.
      static const std::regex reDefineKeyword(R"(^prog(?:ram)?\s+de(?:f(?:ine)?)?\s+)",
                                             std::regex::icase);
      std::string progNameLower = lower(progName);
      bool isReservedBareForm = !std::regex_search(stripped, reDefineKeyword) &&
                                (progNameLower == "dir" || progNameLower == "list" ||
                                 progNameLower == "drop");
      if (isReservedBareForm) {
        out.push_back(line);
        ++i;
        continue;
      }

      std::string progOption = m[2].matched ? lower(trim(m[2].str())) : std::string();
      // Strip a leading/trailing comma defensively (mirrors Python's
      // `.lower().strip(",")`; the regex group itself excludes the comma,
      // but keep the strip for parity with the oracle's exact transform).
      while (!progOption.empty() && progOption.front() == ',') progOption.erase(progOption.begin());
      while (!progOption.empty() && progOption.back() == ',') progOption.pop_back();
      bool isRclass = (progOption == "rclass");
      // P4: eclass/sclass — see the kProgClassSentinelPrefix note above for
      // why this can't just be a new ProgramDef bool.
      bool isEclass = (progOption == "eclass");
      bool isSclass = (progOption == "sclass");

      // Redefine idiom: strip a `[capture] program drop NAME` that
      // immediately precedes this definition (skipping blank/comment
      // lines). In sequential Stata that drop targets a PRE-EXISTING NAME;
      // in this pre-pass it would otherwise run LATER (after the defines
      // are pulled out it becomes adjacent to the driver) and delete the
      // just-captured program, so it is stripped. A drop AFTER a define
      // (genuine mid-script undefine) is left in place.
      long long k = static_cast<long long>(out.size()) - 1;
      while (k >= 0) {
        std::string ls = trim(out[static_cast<size_t>(k)]);
        if (ls.empty() || ls.rfind("*", 0) == 0 || ls.rfind("//", 0) == 0) {
          --k;
          continue;
        }
        std::smatch md;
        if (std::regex_search(ls, md, reProgDrop()) && lower(md[1].str()) == lower(progName)) {
          out.erase(out.begin() + k);
        }
        break;
      }

      // Consume body lines until we hit `end`.
      std::vector<std::string> body;
      ++i;
      while (i < n) {
        const std::string& bline = lines[i];
        std::string bstripped = trim(bline);
        ++i;
        if (std::regex_match(bstripped, reEnd())) {
          break;  // end of program
        }
        body.push_back(bline);
      }

      // P4: append the eclass/sclass sentinel AFTER every real body line was
      // consumed above — see kProgClassSentinelPrefix's note.
      if (isEclass) {
        body.push_back(kProgClassSentinelPrefix + "eclass");
      } else if (isSclass) {
        body.push_back(kProgClassSentinelPrefix + "sclass");
      }

      // Store (or replace) the definition.
      ProgramDef def;
      def.name = progName;
      def.rclass = isRclass;
      def.body = std::move(body);
      session.programs[progName] = def;
      // BUG-342: keep an untouched copy that `clear all` / `program drop _all`
      // cannot reach, and leave ONE sentinel where the block stood. Reaching it
      // during execution re-installs the copy, so the definition takes effect at
      // its own source position instead of at preprocess time. Registering into
      // session.programs here as well is deliberate: it keeps every pre-existing
      // path (program dir, a body that calls a program defined further down)
      // behaving exactly as before, and makes the install a no-op re-store in
      // every do-file that never clears the table.
      session.programArchive[progName] = std::move(def);
      ++session.programsEpoch;
      out.push_back(kProgInstallSentinelPrefix + progName);
      // Body lines consumed; only the sentinel goes to `out`.
      continue;
    }

    // Bare `end` outside a program block — drop silently.
    if (std::regex_match(stripped, reEnd())) {
      ++i;
      continue;
    }

    out.push_back(line);
    ++i;
  }

  return out;
}

// ---------------------------------------------------------------------------
// Invocation

std::map<std::string, std::string> buildArgs(const std::string& argstr) {
  std::map<std::string, std::string> mapping;
  mapping["0"] = argstr;
  std::string rest = argstr;
  int idx = 1;
  while (!trim(rest).empty() && idx <= 20) {
    std::string prev = rest;
    auto [tok, newRest] = eval::scanOneToken(rest);
    rest = newRest;
    if (rest == prev) break;  // no progress (defensive) — avoid an infinite loop
    // Store the token even when EMPTY: an explicit `""` argument is a real
    // positional that holds a slot. Dropping it shifts every later `1'..`9'
    // reference — mirrors the oracle's comment verbatim (see program.py).
    mapping[std::to_string(idx)] = tok;
    ++idx;
  }
  return mapping;
}

namespace {

// RAII scope guard: always restores session.eval.locals from scopeStack and
// session.progCallDepth, even if dispatchFn/executeDoSource throws. Mirrors
// Python's try/finally around push_scope()/pop_scope() in run_program.
class ScopeGuard {
 public:
  ScopeGuard(SessionState& session, int callerDepth) : session_(session), callerDepth_(callerDepth) {}

  ~ScopeGuard() {
    // BUG-360: erase the tempfiles THIS program minted, before anything else
    // and unconditionally — the early return below is about scope restoration,
    // and a program whose scope stack is already unwound has still created
    // files that are now unreachable. Stata: "When the program or do-file
    // concludes, any datasets created with these assigned names are erased",
    // and for tempfile specifically it happens "regardless of whether your
    // program ends with an error" ([P] macro) — which is exactly why this sits
    // in the destructor, on the throwing path too, rather than after the body.
    //
    // Anything minted at a depth DEEPER than our caller's belongs to this call
    // (or to a nested program that already returned and, if it had its own
    // entries, already took them). Best-effort, error_code'd, never throws out
    // of a destructor — the same discipline as the two reapers this joins
    // (doexec.cpp's TempdirReapGuard, parallel.cpp's per-task remove_all).
    if (!session_.mintedTempfiles.empty()) {
      auto& mt = session_.mintedTempfiles;
      auto keep = mt.begin();
      for (auto it = mt.begin(); it != mt.end(); ++it) {
        if (it->first > callerDepth_) {
          std::error_code rmEc;
          std::filesystem::remove(cecon::to_native_path(it->second), rmEc);
          // BUG-384: the worker's virtual-save table held a ref-snapshot (and a
          // whole root mmap) for every tempfile ever saved, with no eraser —
          // unbounded hidden disk retention once the GC unlinks mapped roots.
          // The tempfile's Stata death is the contract point; forget it there.
          session_.residentVirtualDrop(it->second);
        } else {
          if (keep != it) *keep = std::move(*it);
          ++keep;
        }
      }
      mt.erase(keep, mt.end());
    }
    if (session_.scopeStack.empty()) return;
    session_.eval.locals = std::move(session_.scopeStack.back());
    session_.scopeStack.pop_back();
    session_.progCallDepth = callerDepth_;
  }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

 private:
  SessionState& session_;
  int callerDepth_;
};

}  // namespace

ojson runProgram(const ProgramDef& progDef, const std::string& argstr, SessionState& session,
                  const DispatchFn& dispatchFn) {
  // --- Recursion guard ---
  int depth = session.progCallDepth;
  if (depth >= kMaxRecursion) {
    ojson err = eval::engineError("program " + progDef.name + ": recursion depth exceeded " +
                                   std::to_string(kMaxRecursion));
    ojson out = ojson::array();
    out.push_back(std::move(err));
    return out;
  }
  session.progCallDepth = depth + 1;

  // --- Snapshot and reset returnStaging ---
  std::map<std::string, std::pair<std::string, std::string>> callerStaging = session.returnStaging;
  session.returnStaging.clear();

  // --- Build scope args ---
  std::map<std::string, std::string> args = buildArgs(argstr);

  // --- Push scope ---
  session.scopeStack.push_back(session.eval.locals);
  eval::OrderedDict fresh;
  for (const auto& [k, v] : args) fresh.set(k, v);
  session.eval.locals = std::move(fresh);

  // P4: recover eclass/sclass from captureProgams' trailing sentinel (see
  // kProgClassSentinelPrefix's note) and stage it onto a hidden local of
  // THIS call's fresh scope so handleReturnStmt's ereturn/sreturn honesty
  // checks can see it while the body runs. `progClass` (the plain C++
  // local, not the hidden Stata local) survives past the scope-pop below
  // for the flush step further down.
  std::string progClass;
  if (!progDef.body.empty() && progDef.body.back().rfind(kProgClassSentinelPrefix, 0) == 0) {
    progClass = progDef.body.back().substr(kProgClassSentinelPrefix.size());
    session.eval.locals.set(kProgClassMarkerKey, progClass);
  }

  ojson allBlocks = ojson::array();
  {
    // --- Pop scope (always), via RAII so this runs even if the call below
    // throws (mirrors the Python try/finally).
    ScopeGuard guard(session, depth);
    std::string source;
    for (size_t i = 0; i < progDef.body.size(); ++i) {
      if (i) source += "\n";
      source += progDef.body[i];
    }
    doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatchFn);
    allBlocks = result.blocks;
  }  // <- guard fires here: pop_scope + restore progCallDepth

  // --- Flush return values AFTER pop_scope (so caller scope sees them) ---
  // "scalar"/"local" (rclass, S7 precedent) are UNCHANGED — same guard, same
  // bodies, just re-homed one level deeper into the per-kind branch below so
  // the P4 "escalar"/"slocal" kinds can share the same loop. "escalar"/
  // "slocal" are only ever staged by ereturn/sreturn, and those already
  // self-gate on progClass in handleReturnStmt (a mismatched class never
  // reaches session.returnStaging at all) — the progClass re-check here is
  // defense in depth, not the primary gate.
  if (!session.returnStaging.empty()) {
    for (const auto& [rname, kindVal] : session.returnStaging) {
      const std::string& kind = kindVal.first;
      const std::string& val = kindVal.second;
      if (progDef.rclass && kind == "scalar") {
        double d = 0.0;
        try {
          d = std::stod(val);
        } catch (...) {
          d = std::nan("");
        }
        session.eval.rValues.set(rname, d);
      } else if (progDef.rclass && kind == "local") {
        session.eval.rStrings.set(rname, val);
      } else if (progClass == "eclass" && kind == "escalar") {
        double d = 0.0;
        try {
          d = std::stod(val);
        } catch (...) {
          d = std::nan("");
        }
        session.eval.setEScalar(rname, d);
      } else if (progClass == "sclass" && kind == "slocal") {
        session.eval.rStrings.set(kSClassKeyPrefix + rname, val);
      }
    }
  }

  // Restore caller's staging (nested rclass programs layer correctly).
  session.returnStaging = std::move(callerStaging);

  return allBlocks;
}

// ---------------------------------------------------------------------------
// program drop / program dir (native handler)

ojson handleProgramManage(const std::string& sub, const std::vector<std::string>& names,
                           SessionState& session) {
  if (sub == "dir") {
    std::string text;
    if (!session.programs.empty()) {
      std::vector<std::string> keys;
      keys.reserve(session.programs.size());
      for (const auto& [k, v] : session.programs) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      for (size_t i = 0; i < keys.size(); ++i) {
        if (i) text += " ";
        text += keys[i];
      }
    } else {
      text = "(no programs defined)";
    }
    ojson b;
    b["type"] = "display";
    b["text"] = text;
    return b;
  }

  if (sub == "drop") {
    std::vector<std::string> dropped;
    std::vector<std::string> missing;
    for (const auto& name : names) {
      std::optional<std::string> match = findProgram(session, name);
      if (match.has_value()) {
        session.programs.erase(*match);
        ++session.programsEpoch;
        dropped.push_back(*match);
      } else {
        missing.push_back(name);
      }
    }
    if (!missing.empty()) {
      std::string joined;
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i) joined += ", ";
        joined += missing[i];
      }
      return eval::engineError("program drop: program(s) not found: " + joined);
    }
    std::string joined;
    for (size_t i = 0; i < dropped.size(); ++i) {
      if (i) joined += " ";
      joined += dropped[i];
    }
    ojson b;
    b["type"] = "display";
    b["text"] = "dropped: " + joined;
    return b;
  }

  return eval::engineError("program " + sub + ": unknown subcommand");
}

// ---------------------------------------------------------------------------
// S7: return scalar/local step-0 handlers

bool handleReturnStmt(const std::string& rawLine, SessionState& session, ojson& outErrorBlock) {
  outErrorBlock = ojson();

  std::string stripped = trim(rawLine);

  // A2/A3: this function is SEVEN regex searches, and it sits in the dispatch
  // prologue ahead of the macro-definition intercept — so every `local` line in
  // every loop body paid all seven to be told "not mine". A symbolized profile
  // put **24% of a pure-macro loop's samples inside this call**. Every form it
  // handles begins with one of five literal keywords, so the first token settles
  // it. Kept HERE rather than at the call site so every caller inherits the
  // gate, and written as a first-token test (not a `find`) so a keyword
  // appearing later in a line — inside a string, as an argument — cannot be
  // mistaken for one of these statements, exactly as the anchored `^` patterns
  // below require.
  {
    size_t e = stripped.find_first_of(" \t");
    std::string head = stripped.substr(0, e == std::string::npos ? stripped.size() : e);
    for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head != "return" && head != "ereturn" && head != "sreturn" && head != "syntax" &&
        head != "tokenize") {
      return false;
    }
  }

  // `return scalar NAME = EXPR`
  {
    static const std::regex re(R"(^return\s+scalar\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      std::string name = m[1].str();
      std::string expr = trim(m[2].str());

      // Evaluate arithmetic if possible.
      if (eval::hasMacroRefs(expr)) {
        eval::ExpandResult expanded = eval::expandPass(expr, session.eval);
        if (!expanded.isError) expr = expanded.line;
      }
      std::optional<std::string> val = eval::evalArith(expr, &session.eval);  // WS-C: return scalar n = b[1,1]
      double floatVal;
      try {
        floatVal = val.has_value() ? std::stod(*val) : std::stod(expr);
      } catch (...) {
        floatVal = std::nan("");
      }
      // BUG-423 (adjudicated 2026-09-06, User: "버그는 재정하고").
      //
      // AT THE TOP LEVEL THERE IS NOTHING TO RETURN FROM. Bundle B read that as
      // "so set r() immediately", on the strength of a comment here that said
      // "Real Stata sets r() immediately at the top level". That claim was never
      // measured, and it is FALSE: StataBE 19.5 answers **r(151)**, "non r-class
      // program may not set r()", for `return scalar q = 3` in a do-file. The
      // wave pin bundle B's own plan asked for is what measured it.
      //
      // The ruling is to refuse, and the reason is the product's premise rather
      // than a preference: accepting what Stata refuses is the ONE direction of
      // divergence that silently produces a do-file which cannot be run in
      // Stata. Nothing is lost — a program that wants to return a value still
      // stages and flushes below, which is the path every r-class program takes
      // and the path Stata itself documents.
      //
      // progCallDepth is the discriminator because it is the ONE counter that
      // answers "is a program on the stack" (runProgram increments it around
      // every call, including recursive ones).
      if (session.progCallDepth == 0) {
        outErrorBlock = eval::errorBlock(
            "engine_error", "return scalar: non r-class program may not set r()", std::nullopt,
            std::nullopt, std::nullopt, "r(151)");
        return true;
      }
      // Store the staged value as its canonical string form; runProgram's
      // flush parses it back with std::stod. Use a round-trip-safe
      // formatting (max precision) so the double survives the string hop
      // unchanged for any finite value, and "nan" for NaN (std::stod("nan")
      // parses back to NaN on all supported platforms).
      char buf[64];
      if (std::isnan(floatVal)) {
        session.returnStaging[name] = {"scalar", "nan"};
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", floatVal);
        session.returnStaging[name] = {"scalar", std::string(buf)};
      }
      return true;
    }
  }

  // `return local NAME value`
  {
    static const std::regex re(R"(^return\s+local\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      std::string name = m[1].str();
      std::string val = m[2].matched ? trim(m[2].str()) : std::string();

      // Stata expands macro refs in the value at the point `return local`
      // runs (in the program's local scope) — e.g.
      //   tempfile pf
      //   return local pfpath "`pf'"
      // must store the resolved temp path, not the literal `pf'. This
      // step-0 handler fires BEFORE 0b macro-expansion (dispatcher.cpp), so
      // the ref is still unexpanded here — expand it ourselves, mirroring
      // the `return scalar` branch above.
      if (eval::hasMacroRefs(val)) {
        eval::ExpandResult expanded = eval::expandPass(val, session.eval);
        if (!expanded.isError) val = expanded.line;
      }

      // Strip ONE layer of balanced delimiting quotes, Stata-faithfully:
      //   return local x "age edu income"   stores  age edu income
      //   return local x `"compound"'       stores  compound
      // so a downstream `use "`r(x)'"` / `r(x)' resolves to a clean token
      // rather than a nested compound-quote artifact (`"…"') that macro
      // hardening would reject. Bare values (no delimiters) pass through
      // as-is.
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      } else if (val.size() >= 4 && val.rfind("`\"", 0) == 0 && val.substr(val.size() - 2) == "\"'") {
        val = val.substr(2, val.size() - 4);
      }
      // BUG-423, the `return local` twin: same discriminator, same ruling. Stata
      // refuses at the top level with r(151) — measured on StataBE 19.5, 2026-09-06
      // — and the comment that used to sit here claiming otherwise was the
      // unmeasured half of the same pair. See the long note on `return scalar`.
      if (session.progCallDepth == 0) {
        outErrorBlock = eval::errorBlock(
            "engine_error", "return local: non r-class program may not set r()", std::nullopt,
            std::nullopt, std::nullopt, "r(151)");
        return true;
      }
      session.returnStaging[name] = {"local", val};
      return true;
    }
  }

  // ---------------------------------------------------------------------
  // P4: ereturn scalar / sreturn local / syntax varlist(min=N) / tokenize.
  //
  // These four all belong at this SAME step-0 interception point as
  // `return scalar`/`return local` above — dispatcher.cpp calls
  // handleReturnStmt() on the raw line before macro expansion, before
  // try_capture_definition, and before eval::tokenize()'s command-word
  // gate, and this stint's file manifest has no header to add a second
  // call site through, so every new construct is folded into this one
  // function's body rather than a new dispatcher arm (would need
  // dispatcher.cpp, off limits) or a second exported hook (would need
  // program.h, off limits).

  // `ereturn scalar NAME = EXPR` — same evaluation path as `return scalar`
  // above (duplicated rather than shared, deliberately: the S7 return-
  // scalar path is a proven, freeze-pinned path and this stint does not
  // touch it). Gated on the CURRENT program being eclass, via the hidden
  // local runProgram() stages — see kProgClassMarkerKey's note at the top
  // of this file. Honest reject outside an eclass program (or at top
  // level, where the marker is simply absent) rather than a silent no-op.
  {
    static const std::regex re(R"(^ereturn\s+scalar\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      const std::string* cls = session.eval.locals.find(kProgClassMarkerKey);
      if (cls == nullptr || *cls != "eclass") {
        outErrorBlock = eval::engineError(
            "ereturn scalar: not allowed, not in an eclass program.");
        return true;
      }
      std::string name = m[1].str();
      std::string expr = trim(m[2].str());

      if (eval::hasMacroRefs(expr)) {
        eval::ExpandResult expanded = eval::expandPass(expr, session.eval);
        if (!expanded.isError) expr = expanded.line;
      }
      std::optional<std::string> val = eval::evalArith(expr, &session.eval);
      double floatVal;
      try {
        floatVal = val.has_value() ? std::stod(*val) : std::stod(expr);
      } catch (...) {
        floatVal = std::nan("");
      }
      // "escalar" kind (not "scalar") — runProgram's flush routes it to
      // session.eval.eScalars, NOT session.eval.rValues, so `ereturn` never
      // shows up under r(). Same round-trip-safe %.17g formatting as
      // `return scalar` above.
      char buf[64];
      if (std::isnan(floatVal)) {
        session.returnStaging[name] = {"escalar", "nan"};
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", floatVal);
        session.returnStaging[name] = {"escalar", std::string(buf)};
      }
      return true;
    }
  }

  // `sreturn local NAME value` — same staging path as `return local` above
  // (again duplicated, not shared, for the same reason). Gated on the
  // current program being sclass.
  {
    static const std::regex re(R"(^sreturn\s+local\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, re)) {
      const std::string* cls = session.eval.locals.find(kProgClassMarkerKey);
      if (cls == nullptr || *cls != "sclass") {
        outErrorBlock = eval::engineError(
            "sreturn local: not allowed, not in an sclass program.");
        return true;
      }
      std::string name = m[1].str();
      std::string val = m[2].matched ? trim(m[2].str()) : std::string();

      if (eval::hasMacroRefs(val)) {
        eval::ExpandResult expanded = eval::expandPass(val, session.eval);
        if (!expanded.isError) val = expanded.line;
      }
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      } else if (val.size() >= 4 && val.rfind("`\"", 0) == 0 && val.substr(val.size() - 2) == "\"'") {
        val = val.substr(2, val.size() - 4);
      }
      // "slocal" kind -> runProgram's flush routes it into
      // session.eval.rStrings under the kSClassKeyPrefix-reserved key so
      // eval/macros.cpp's `s(name)' reader can find it without colliding
      // with any real r()-name.
      session.returnStaging[name] = {"slocal", val};
      return true;
    }
  }

  // `syntax varlist(min=N)` — minimal subset (P4 task scope: nothing else).
  // Binds the caller's varlist (Stata's `0', already set by buildArgs from
  // the program's own argstr) into the local `varlist' after checking the
  // minimum count; any OTHER `syntax` spec (max=, options, if/in/using,
  // [...] blocks) is an explicit reject naming what's unsupported — never a
  // silent partial bind, per the task brief's "a program that silently sees
  // the wrong varlist produces a wrong regression" warning.
  {
    static const std::regex reVarlistMin(
        R"(^syntax\s+varlist\s*\(\s*min\s*=\s*(\d+)\s*\)\s*$)", std::regex::icase);
    std::smatch m;
    if (std::regex_match(stripped, m, reVarlistMin)) {
      long long minN = 0;
      try {
        minN = std::stoll(m.str(1));
      } catch (...) {
        minN = 0;
      }
      std::string argstr = session.eval.locals.get("0", "");
      std::istringstream argIss(argstr);
      std::vector<std::string> vars;
      std::string tok;
      while (argIss >> tok) vars.push_back(tok);
      if (static_cast<long long>(vars.size()) < minN) {
        outErrorBlock = eval::engineError(
            "syntax: varlist requires a minimum of " + std::to_string(minN) +
            " variable(s); " + std::to_string(vars.size()) + " specified.");
        return true;
      }
      std::string joined;
      for (size_t i = 0; i < vars.size(); ++i) {
        if (i) joined += " ";
        joined += vars[i];
      }
      session.eval.locals.set("varlist", joined);
      return true;
    }
    static const std::regex reSyntaxAny(R"(^syntax\b)", std::regex::icase);
    if (std::regex_search(stripped, m, reSyntaxAny)) {
      outErrorBlock = eval::engineError(
          "syntax: only `syntax varlist(min=N)` is implemented in this engine "
          "(no max=, options, if/in/using, or [...] blocks) — rejecting rather "
          "than risk a silently wrong partial bind.");
      return true;
    }
  }

  // `tokenize STRING[, ...]` — splits STRING on whitespace and binds
  // numbered locals `1'..`N' (20-slot cap, matching buildArgs' own cap
  // just above in this file: tokenize and program positional args share
  // ONE numbered-local namespace in real Stata, so reusing the same
  // ceiling is consistency, not a new invented limit). Any previously
  // bound numbered locals beyond the new count are cleared first, so a
  // shorter re-tokenize doesn't leave stale trailing values reachable.
  // Runs on the RAW line, well before eval::tokenize()'s own command-word
  // gate would otherwise reject "tokenize" outright (eval/tokenizer.cpp's
  // macroStmts() list) — that gate is simply never reached for this word
  // now, left in place as a defensive fallback, not touched.
  {
    static const std::regex reTokenizeCmd(R"(^tokenize(?:\s+(.*))?$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(stripped, m, reTokenizeCmd)) {
      std::string rest = m[1].matched ? m.str(1) : std::string();

      // Split off a comma-introduced option tail the same quote-aware way
      // every other Stata command does; `parse()` and friends are not
      // implemented, so any such tail is an honest reject, not a silent
      // ignore.
      long long commaPos = eval::findUnquotedComma(rest);
      std::string arg = commaPos == -1 ? rest : rest.substr(0, static_cast<size_t>(commaPos));
      std::string optsTail =
          commaPos == -1 ? std::string() : trim(rest.substr(static_cast<size_t>(commaPos) + 1));
      if (!optsTail.empty()) {
        outErrorBlock = eval::engineError(
            "tokenize: options (`, " + optsTail +
            "`) are not implemented in this engine — only plain whitespace "
            "splitting of the string argument is supported.");
        return true;
      }

      arg = trim(arg);
      // A double-quoted argument's outer quotes are delimiters, not
      // content — `tokenize one two three` (no quotes) is equally legal
      // Stata and needs no stripping.
      if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
        arg = arg.substr(1, arg.size() - 2);
      }
      if (eval::hasMacroRefs(arg)) {
        eval::ExpandResult expanded = eval::expandPass(arg, session.eval);
        if (!expanded.isError) arg = expanded.line;
      }

      for (int i = 1; i <= 20; ++i) session.eval.locals.erase(std::to_string(i));
      std::istringstream argIss(arg);
      std::string tok;
      int idx = 1;
      while (idx <= 20 && (argIss >> tok)) {
        session.eval.locals.set(std::to_string(idx), tok);
        ++idx;
      }
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Internal helpers

std::optional<std::string> findProgram(const SessionState& session, const std::string& name) {
  std::string nameLower = lower(name);
  for (const auto& [k, v] : session.programs) {
    if (lower(k) == nameLower) return k;
  }
  return std::nullopt;
}

}  // namespace piet::program
