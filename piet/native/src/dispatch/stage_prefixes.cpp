// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "prefix_support.h"
#include "results_support.h"
#include "session_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool interceptOutputPrefixes(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // Pre-0. `capture`/`quietly`/`noisily` prefix stripping (dispatcher.py's
  // same-named step, items 4-5). Must run BEFORE step (a) continue-sentinel
  // and 0a try_capture_definition so a prefixed line's remainder is what
  // those steps see.
  {
    // BS-016: cap/capt/captu/captur/capture are all legal Stata
    // abbreviations of `capture` (previously only cap/capture were
    // recognised, silently rejecting captu/captur/capt as unknown commands).
    static const std::set<std::string> kCapturePrefixes = {"capture", "captur", "captu", "capt",
                                                             "cap"};
    // N1(a) (BETA Phase C batch C1): Stata accepts n/no/noi/nois/noisi/
    // noisil/noisily as legal abbreviations of `noisily` — this set
    // previously had only {noisily, nois, n}, missing no/noi/noisi/noisil.
    // `noi` is by far the single most common spelling in practice
    // (`cap noi <cmd>`); its absence meant `cap noi X` dispatched "noi" as
    // an unknown command, whose error capture then swallowed X's entire
    // output (rc=199, zero output) — exactly the "cap noi suppresses
    // everything" symptom the C1 probes showed. The isQuietly classifier
    // just below still only lists the QUIETLY-class spellings explicitly;
    // every newly-added noisily-class spelling correctly falls to its
    // `else` (noisily) branch by construction, so no change needed there.
    static const std::set<std::string> kQuietlyPrefixes = {"quietly", "qui",    "quie",   "quiet",
                                                             "noisily", "noisil", "noisi",  "nois",
                                                             "noi",     "no",     "n"};

    // N1(b): does `remainder`'s OWN leading prefix chain hold a noisily?
    // Real Stata: bare `capture cmd` suppresses ALL output — success or
    // error; `capture noisily cmd` shows it (both on success and on error).
    // BUG-1434: a noisily anywhere in the chain after the capture shows the
    // error (StataBE 19.5: `capture quietly noisily error 460` prints it), and
    // the inner dispatch has already decided the output by the chain's last
    // word (`capture noisily quietly display "a3"` prints nothing).
    auto isNoisilyRooted = [](const std::string& remainder) {
      static const std::set<std::string> kNoisilyClass = {"noisily", "noisil", "noisi", "nois",
                                                            "noi",     "no",     "n"};
      static const std::set<std::string> kQuietOrCapture = {
          "quietly", "qui", "quie", "quiet", "capture", "captur", "captu", "capt", "cap"};
      std::string rest = trimWs(remainder);
      while (!rest.empty()) {
        size_t sp = rest.find_first_of(" \t");
        std::string tok = lower(rstripColon(sp == std::string::npos ? rest : rest.substr(0, sp)));
        if (kNoisilyClass.count(tok) != 0) return true;
        if (kQuietOrCapture.count(tok) == 0 || sp == std::string::npos) return false;
        rest = trimWs(rest.substr(sp));
      }
      return false;
    };

    // BUG-526 (W2 labelglued; oracle w2_cmdtriage.log section D): a prefix
    // delivered THROUGH a macro. `local q quietly` + `` `q' summarize a `` is
    // rc 0 with r(N)=12 (macro_quietly); `global qui "qui"` + `${qui} generate
    // double mq = 1` is rc 0 (global_qui); `local cp capture` + `` `cp' drop
    // nosuchvar `` is rc 0 where the bare `drop nosuchvar` is r(111)
    // (macro_capture against ctl_drop_missing). Stata expands macros before it
    // looks for the prefix; this block ran on the RAW line, missed, and step
    // 0b's expansion then handed the tokenizer a command literally named
    // `quietly` -> r(199), an uncaptured stop (paper 073 alone carries 2,981
    // `${qui} cmd` lines).
    //
    // Only the HEAD token is expanded here, and the line re-enters dispatch
    // only when that head became a capture/quietly/noisily word. The rest of
    // the line stays raw and is expanded exactly once, by the inner dispatch's
    // own step 0b -- so a macro in the remainder is not expanded twice, and a
    // head that expands to an ordinary command, or to nothing, keeps today's
    // path (0b expands the whole line and the command runs). The inner echo
    // is dropped: Stata logs the line as typed, and `blocks` already holds it.
    {
      const std::string rawTrim = trimWs(rawCmd);
      if (!rawTrim.empty() && (rawTrim[0] == '`' || rawTrim[0] == '$')) {
        const size_t sp = rawTrim.find_first_of(" \t");
        const std::string head = sp == std::string::npos ? rawTrim : rawTrim.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : rawTrim.substr(sp);
        if (eval::hasMacroRefs(head)) {
          eval::ExpandResult er = eval::expandMacros(head, session.eval);
          if (!er.isError) {
            const std::string expHead = trimWs(er.line);
            const size_t hs = expHead.find_first_of(" \t");
            const std::string word =
                lower(rstripColon(hs == std::string::npos ? expHead : expHead.substr(0, hs)));
            if (!word.empty() &&
                (kCapturePrefixes.count(word) != 0 || kQuietlyPrefixes.count(word) != 0)) {
              ojson inner = dispatchImpl(expHead + rest, session);
              if (inner.is_array()) {
                for (const auto& b : inner) {
                  if (b.is_object() && b.value("type", std::string()) == "echo") continue;
                  blocks.push_back(b);
                }
              } else if (!inner.is_null()) {
                blocks.push_back(inner);
              }
              return true;
            }
          }
        }
      }
    }

    std::optional<std::string> capRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kCaptureTable(kCapturePrefixes);
      capRemainder = stripPrefix(rawCmd, kCaptureTable);
    }
    if (capRemainder.has_value()) {
      ojson innerBlocks = dispatch(*capRemainder, session);
      // BUG-116 class (A11 sweep, oracle-verified): a NESTED `capture` that
      // already failed tags its error block captured:true (BS-127) — that
      // statement, AS A COMMAND, still completed without escaping an error
      // (capture never propagates one), so it must not read as THIS
      // capture's own wrapped-command error. Confirmed against real Stata
      // (StataBE 19.5): `capture capture noisily regress y nosuchvar`
      // leaves _rc==0, not the inner regress's rc — the same
      // !captured guard doexec.cpp/loops.cpp's runCaptureBlock use.
      const ojson* errBlock = nullptr;
      for (const auto& b : innerBlocks) {
        if (b.value("type", "") == "error" && !b.value("captured", false)) {
          errBlock = &b;
          break;
        }
      }
      // `exit` with code 0 is no error, and capture does not hold it: StataBE
      // `capture exit 0` ends the do-file (or the program) all the same.
      if (errBlock != nullptr && errBlock->value("message", std::string()) == "exit") {
        { blocks = ojson::array({echo, *errBlock}); return true; }
      }
      // BS-100: coarse Stata rc map (piet::eval::coarseRc) replaces the old
      // boolean-collapsed "_rc=1" hardcode — see errors.h's doc comment for
      // the mapping + BS-118's post-beta exact-code scope.
      session.eval.locals.set("_rc", errBlock != nullptr ? eval::coarseRc(*errBlock) : "0");

      // N1(b): bare `capture cmd` suppresses ALL output regardless of
      // success/failure; `capture noisily cmd` shows it regardless of
      // success/failure. Previously the success path always forwarded
      // output (ignoring noisily) and the error path always suppressed it
      // (also ignoring noisily) — backwards for `capture noisily cmd`,
      // which must show output on BOTH outcomes.
      if (isNoisilyRooted(*capRemainder)) {
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") {
            // BS-127: an error block that reaches the caller under a
            // `capture` prefix is SHOWN (noisily) but must not HALT a
            // running do-file — real Stata shows the message, sets _rc,
            // and continues. doexec's stop-on-first-error loop skips
            // blocks carrying this tag.
            if (b.value("type", "") == "error") {
              // `exit N`'s halt sentinel is no message: StataBE `capture
              // noisily exit 7` prints nothing and leaves _rc 7.
              static const std::regex reExitSentinel(R"(^exit(\s+-?\d+)?$)");
              if (std::regex_match(b.value("message", std::string()), reExitSentinel)) continue;
              ojson tagged = b;
              tagged["captured"] = true;
              out.push_back(tagged);
              continue;
            }
            out.push_back(b);
          }
        }
        { blocks = out; return true; }
      }
      // BUG-350: a plain `capture` drops the error block entirely — that is
      // correct Stata behaviour and stays. What was missing is that NOTHING
      // recorded it, so a run could hide a disk-full and still look complete.
      // Count it here, at the one place it is dropped, and keep the first
      // message for the end-of-run summary.
      // BUG-1338: the message of an inline `: fn'/`=exp' that failed while the
      // capture line itself expanded still prints (StataBE prints `variable
      // nosuch not found` for `capture display "C`: type nosuch'"`, and for
      // `capture quietly display ...`); one from a line run inside the captured
      // command does not (`capture myq`, whose body expands `=nosuchZ', is
      // silent). That line is the remainder itself or the tail of its prefix
      // chain; a program's body line is neither.
      const std::string capLine = trimWs(*capRemainder);
      const auto isOwnLineNote = [&capLine](const ojson& b) {
        if (!b.is_object() || !b.value("inline_note", false)) return false;
        const std::string of = trimWs(b.value("inline_note_of", std::string()));
        if (of.empty() || of.size() > capLine.size()) return false;
        if (capLine.compare(capLine.size() - of.size(), of.size(), of) != 0) return false;
        if (of.size() == capLine.size()) return true;
        const char before = capLine[capLine.size() - of.size() - 1];
        return before == ' ' || before == '\t' || before == ':';
      };
      ojson out = ojson::array({echo});
      for (const auto& b : innerBlocks) {
        if (b.is_object() && b.value("type", std::string()) == "error") {
          piet::counters::noteSwallowedError(b.value("message", std::string()));
        }
        if (isOwnLineNote(b)) out.push_back(b);
      }
      { blocks = out; return true; }
    }

    std::optional<std::string> quiRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kQuietlyTable(kQuietlyPrefixes);
      quiRemainder = stripPrefix(rawCmd, kQuietlyTable);
    }
    if (quiRemainder.has_value()) {
      std::string firstWord = trimWs(rawCmd);
      size_t sp2 = firstWord.find_first_of(" \t");
      std::string fw = lower(rstripColon(sp2 == std::string::npos ? firstWord : firstWord.substr(0, sp2)));
      // N1(a): this classifier only needs to list the QUIETLY-class
      // spellings explicitly — every noisily-class spelling (including the
      // ones added to kQuietlyPrefixes above: no/noi/noisi/noisil) already
      // falls to the `else` (noisily) branch below by construction, so the
      // expanded abbreviation set needs no change here.
      bool isQuietly = (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet");
      // BUG-189: same reason as the braced form — the filters below run after
      // the inner dispatch has already teed to `log using`, so the scope must be
      // declared around the dispatch itself, not applied to its result.
      ojson innerBlocks;
      {
        SessionState::LogVerbosityScope __lv(session, isQuietly);
        innerBlocks = dispatch(*quiRemainder, session);
      }
      ojson out = ojson::array({echo});

      // BS-040: if the remainder is ITSELF another quietly/noisily-prefixed
      // command, the recursive dispatch() call above has ALREADY resolved
      // its effective visibility — Stata nests these prefixes with the
      // INNERMOST keyword winning (`quietly noisily cmd` must show cmd's
      // output; the reverse must hide it). Re-applying THIS layer's own
      // filter on top of an already-resolved inner result would let an
      // outer `quietly` re-hide what an inner `noisily` correctly chose to
      // show (the exact bug). Detect that case and pass the inner result
      // through untouched instead — only the INNERMOST layer in a chain
      // (whose own remainder is NOT itself quiet/noisy-prefixed) actually
      // applies a filter, via the unchanged isQuietly branch below.
      // BUG-1434: a capture inside the chain resolves the rest the same way
      // (`quietly capture noisily display "a2"` prints a2, StataBE 19.5).
      static const PrefixTable kQuietlyTable2(kQuietlyPrefixes);
      static const PrefixTable kCaptureTable2(kCapturePrefixes);
      if (stripPrefix(*quiRemainder, kQuietlyTable2).has_value() ||
          stripPrefix(*quiRemainder, kCaptureTable2).has_value()) {
        for (const auto& b : innerBlocks) out.push_back(b);
        loops::settleNoisily(out, session);
        { blocks = out; return true; }
      }

      // BUG-189: hand this layer's verdict to the log tee one frame out. Only
      // the innermost layer reaches here (the chained case returned above
      // without touching the flag), so what the tee sees is the keyword that
      // actually decided visibility.
      session.setLineVisibility(isQuietly ? SessionState::LineVis::Quiet
                                          : SessionState::LineVis::Noisy);
      if (isQuietly) {
        for (const auto& b : innerBlocks) {
          std::string t = b.value("type", "");
          if (t == "error" || t == "echo" || b.value("inline_note", false) ||
              b.value("noisily", false))
            out.push_back(b);
        }
        loops::settleNoisily(out, session);
      } else {
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
        // Inside a quiet scope this is a nested noisily's output (BUG-1434).
        loops::markNoisily(out, session);
      }
      { blocks = out; return true; }
    }
  }
  return false;
}


bool interceptEstimatePrefix(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // Pre-0b. `eststo [NAME] [, options]: command` (BS-020/BS-095) — same
  // unified-prefix philosophy as capture/quietly/noisily above (recognise on
  // the raw line, dispatch the remainder exactly once, apply this prefix's own
  // post-processing): run the remainder FIRST so handlers::runEststo
  // (eststo_esttab.cpp) snapshots the FRESH result, then store it.
  //
  // W2 eststoprefix (BUG-520). This arm used to be the regex
  // `^eststo\s+(\w+)\s*:` on the RAW line, which is blind to everything but a
  // literal name. Three shapes fell past it to the flat `eststo` arm, which
  // never dispatches a remainder:
  //   eststo: regress y w              bare -- one of the commonest corpus lines
  //   eststo mac`i': regress y w       a macro in NAME (unexpanded here: `\w+`
  //                                    stops at the backtick)
  //   eststo t0, title("x"): regress   options before the colon
  // With inner options the flat arm refused the INNER command's first option,
  // r(198). Without them it stored the STALE previous model under a corrupted
  // key (`regress`, `mac3:`) at rc 0 and never ran the command -- silent wrong.
  // Oracle, tests/eval/golden/aer_w2/w2_eststo_prefix.log: A2 bare, A3/A4 local
  // in NAME (A4 in a loop, inner `vce(cluster id)`), A5 global in NAME, A6
  // options before the colon incl. a QUOTED colon, A7 prefix(), A8 a failing
  // inner command stores nothing and returns its own rc.
  //
  // So the split is structural now: first word `eststo`, then the first colon
  // outside quotes and outside a macro reference. Macros are expanded in the
  // HEAD ONLY. The remainder goes to dispatch() raw, because dispatch expands it
  // itself (step 0b) and expanding twice is a bug of its own (`\`'-deferred
  // macros, `++i'). Head first, remainder second is also Stata's left-to-right
  // order, which `eststo m`++i': regress y x`i'` can observe.
  //
  // The no-colon forms -- `eststo`, `eststo NAME`, `eststo dir`, `eststo
  // drop|clear [NAME]` -- never enter here, and dir/drop/clear in the NAME
  // position are still left to the flat arm (which now refuses the colon shape
  // by name instead of storing under it).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    const std::string trimmedRaw = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    const size_t wordEnd = trimmedRaw.find(':') == std::string::npos
                               ? std::string::npos
                               : trimmedRaw.find_first_of(" \t,:");
    if (wordEnd != std::string::npos && lower(trimmedRaw.substr(0, wordEnd)) == "eststo") {
      const std::string rest = trimmedRaw.substr(wordEnd);
      const size_t colonPos = findEststoPrefixColon(rest);
      std::string headRaw =
          colonPos == std::string::npos ? std::string() : trimWs(rest.substr(0, colonPos));
      // Checked on the RAW head, before any expansion, so a line this arm hands
      // back to the flat arm has had no macro side effect from here.
      const size_t subEnd = headRaw.find_first_of(" \t,");
      const std::string sub0 = lower(headRaw.substr(0, subEnd));
      if (colonPos != std::string::npos && sub0 != "dir" && sub0 != "drop" && sub0 != "clear") {
        const std::string eststoRemainder = trimWs(rest.substr(colonPos + 1));
        // The community-name gate (WS-B) sits after tokenize, which this arm
        // returns before -- so it is asked here. Without this, `set extcompat
        // off` gated `eststo m1` and let `eststo m1: regress y x` through (it
        // always had, for the literal-name form; bare `eststo:` would have made
        // that the commonest line in the corpus). The INNER command meets the
        // gate on its own, inside its own dispatch.
        if (!settings::extcompatEnabled()) {
          if (const registry::CommunityName* cn = registry::findCommunityName("eststo")) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command", registry::communityGateMessage(*cn), std::string("eststo"))));
            return true;
          }
        }
        if (eval::hasMacroRefs(headRaw)) {
          eval::ExpandResult er = eval::expandMacros(headRaw, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            return true;
          }
          headRaw = er.line;
        }
        // The head is parsed and validated BEFORE the command runs, as
        // eststo.ado's `syntax` does: a bad head is r(198) and nothing ran.
        auto [namePart, optsPart] = splitTopLevelComma(headRaw);
        std::vector<std::string> nameToks;
        {
          std::istringstream ns(namePart);
          std::string tk;
          while (ns >> tk) nameToks.push_back(tk);
        }
        // `\w+`, exactly what the regex this replaces accepted -- no oracle arm
        // measures an invalid name, so the accepted set is neither widened nor
        // narrowed here. An EMPTY name part is the bare form (A2). A macro that
        // expands to nothing mid-name (`est`undefined'` -> `est`) is a legal name.
        // A byte >= 0x80 is a name byte too (core/ident.h): `eststo نموذج: ...`.
        static const std::regex reEststoName(std::string("^") + eval::kRxNameChar + "+$");
        if (nameToks.size() > 1 ||
            (nameToks.size() == 1 && !std::regex_match(nameToks[0], reEststoName))) {
          blocks.push_back(ojson(eval::engineError(
              "eststo: `" + trimWs(namePart) + "` is not one name -- the prefix form is "
              "`eststo [name] [, options]: command`",
              "syntax_error")));
          return true;
        }
        EststoStoreOpts storeOpts;
        if (optsPart.has_value()) {
          std::vector<std::pair<std::string, eval::OptValue>> headOpts;
          try {
            headOpts = eval::parseOpts(trimWs(*optsPart));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return true;
          }
          // The head never reaches the tokenizer's resolution point, so it gets
          // the same canonicalization here -- otherwise `ti("x")` would pass the
          // net below and then miss readEststoStoreOpts' `k == "title"` (BUG-231's
          // mechanism), and the BUG-233 late-resolution guard would count it.
          eval::canonicalizeForCommand(headOpts, "eststo");
          if (auto err = readEststoStoreOpts(headOpts, &storeOpts)) {
            blocks.push_back(*err);
            return true;
          }
        }
        ojson out = ojson::array({echo});
        bool hasError = false;
        // `eststo NAME:` with nothing after the colon is the flat store --
        // eststo.ado only runs a command when `s(after)` is non-empty.
        if (!eststoRemainder.empty()) {
          ojson innerBlocks = dispatch(eststoRemainder, session);
          for (const auto& b : innerBlocks) {
            // is_object() first: value() on anything else throws type_error.306
            // and takes the whole process with it.
            const std::string t = b.is_object() ? b.value("type", std::string()) : std::string();
            if (t == "error") hasError = true;
            if (t != "echo") out.push_back(b);
          }
        }
        // A8: a failing inner command stores NOTHING (`estimates table bad1` is
        // r(111) afterwards) and the line's rc is the inner command's own. The
        // auto-name counter is not consumed either: the bare store after two
        // failures is est10, the very next number.
        if (!hasError) {
          std::optional<std::string> eststoNameOpt;
          if (!nameToks.empty()) eststoNameOpt = nameToks[0];
          out.push_back(ojson(handlers::runEststo(eststoNameOpt, storeOpts.title, session,
                                                  storeOpts.autoPrefix)));
          pushEstStoreMutation(out.back(), out, session);
        }
        { blocks = out; return true; }
      }
    }
  }
  return false;
}


bool interceptVersionPrefix(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // Pre-0c. `version #: command` (BS-041/BS-095) — same unification as
  // eststo NAME: above. Bare `version #` (no colon) is the flat if-chain's
  // `cmd=="set"||cmd=="version"||cmd=="compress"` arm (BUG-1459). This block only
  // fires when a colon+remainder is actually present, which today silently
  // vanishes in its entirety (the whole line no-ops, remainder included —
  // e.g. `version 14: regress y x` runs nothing and reports nothing).
  // `version` has no suppression semantics of its own, so on success this
  // is a transparent passthrough (mirrors `noisily`'s shape: drop the
  // remainder's own echo, keep everything else, including any error).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    // BUG-1330: `version #, user: command` is the prefix too; the options sit
    // between the number and the colon.
    static const std::regex reVersionColon(R"(^version\s+([^\s,:]+)\s*(?:,([^:]*))?:\s*(.+)$)",
                                           std::regex::icase);
    std::smatch vm;
    std::string trimmedRawV = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawV.find(':') != std::string::npos &&
        std::regex_search(trimmedRawV, vm, reVersionColon)) {
      std::string versionRemainder = trimWs(vm.str(3));
      if (!versionRemainder.empty()) {
        // BUG-1015: the inner command runs under that version (a command whose
        // syntax depends on it reads session.callerVersion: table's c()).
        // BUG-1330: a number `version #` refuses is refused here too
        // (`version 0.5: ...` r(9), `version 1e1: ...` r(198)).
        // The line is not expanded yet: the number and options are, here
        // (`version `v': ...` in a loop).
        std::string head[2] = {vm.str(1), vm.str(2)};
        for (std::string& part : head) {
          if (!eval::hasMacroRefs(part)) continue;
          eval::ExpandResult er = eval::expandMacros(part, session.eval);
          if (er.isError) { blocks = ojson::array({echo, er.error}); return true; }
          part = trimWs(er.line);
        }
        double vnum = 0.0;
        if (auto bad = parseVersionNumber(head[0], &vnum)) { blocks = ojson::array({echo, *bad}); return true; }
        bool user = false;
        std::istringstream opts(head[1]);
        for (std::string t; opts >> t;) user = user || t == "user";
        // BUG-1330: and c(version) reports it there (kVersionLocal), the
        // caller's coming back after; c(userversion) too, except where
        // `version #` leaves it (versionSetsUserVersion). With `user` only
        // c(userversion) is that number, in a program too, and the version in
        // force stays (StataBE 19.5: P1|15|16 under `version 15`, then 15|15;
        // `version 16, user: table ..., contents()` is still r(198)).
        struct VersionScope {
          // session.callerVersion (BUG-1459) is what table reads; the locals are c()'s.
          double saved;
          SessionState& session;
          std::optional<std::string> savedLocal[2];
          VersionScope(double v, bool user, SessionState& s) : saved(s.callerVersion), session(s) {
            const char* const names[2] = {kVersionLocal, kUserVersionLocal};
            for (int k = 0; k < 2; ++k)
              if (const std::string* cur = session.eval.locals.find(names[k])) savedLocal[k] = *cur;
            if (!user) {
              session.callerVersion = v;
              session.eval.locals.set(kVersionLocal, eval::fmtNumeric(v));
            }
            if (user || versionSetsUserVersion(session))
              session.eval.locals.set(kUserVersionLocal, eval::fmtNumeric(v));
          }
          ~VersionScope() {
            session.callerVersion = saved;
            const char* const names[2] = {kVersionLocal, kUserVersionLocal};
            for (int k = 0; k < 2; ++k) {
              if (savedLocal[k])
                session.eval.locals.set(names[k], *savedLocal[k]);
              else
                session.eval.locals.erase(names[k]);
            }
          }
        } versionScope(vnum, user, session);
        ojson innerBlocks = dispatch(versionRemainder, session);
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
        { blocks = out; return true; }
      }
    }
  }
  return false;
}


bool interceptSurveyPrefix(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // P7 — the `svy:` prefix. Intercepted here, AFTER macro expansion would have
  // happened for the resampling prefixes but structurally alongside the other
  // simple `KEYWORD: command` forms (eststo/version): svy carries no exp_list,
  // so nothing has to be deferred per-replicate and the ordinary expansion
  // order is correct. The design itself lives in SessionState (svyset).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    static const std::regex reSvyColon(R"(^\s*svy\s*(?:,[^:]*)?:\s*(.+)$)",
                                        std::regex::icase);
    std::smatch sm;
    std::string trimmedRawS = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawS.find(':') != std::string::npos &&
        std::regex_search(trimmedRawS, sm, reSvyColon)) {
      blocks.push_back(ojson(handlers::runSvyPrefix(trimWs(sm.str(1)), session)));
      return true;
    }
  }
  return false;
}

}
