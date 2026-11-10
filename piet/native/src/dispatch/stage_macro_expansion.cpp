// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool interceptDefinition(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd) {

  {
    dprof::Scope __z(dprof::kCaptureDef);
    // The inline `` `: display ...' `` form, spliced in before anything reads
    // the line; the line is then dispatched again with the typed echo kept.
    if (rawCmd.find("`:") != std::string::npos) {
      std::optional<ojson> inlineErr;
      const std::string spliced = substituteInlineDisplay(rawCmd, session, inlineErr);
      if (inlineErr) {
        blocks.push_back(*inlineErr);
        return true;
      }
      if (spliced != rawCmd) {
        ojson inner = dispatchImpl(spliced, session);
        if (inner.is_array() && !inner.empty() && inner[0].is_object() &&
            inner[0].value("type", std::string()) == "echo")
          inner[0] = echo;
        { blocks = inner; return true; }
      }
    }
    {
      ojson cLocalOut;  // FU-602: `c_local NAME ...` writes the caller's scope
      if (tryCLocal(rawCmd, session, cLocalOut)) { blocks = cLocalOut; return true; }
    }
    {
      ojson extDisplayOut;  // BUG-513: `local NAME : display ...`
      if (tryExtDisplayCapture(rawCmd, session, extDisplayOut)) { blocks = extDisplayOut; return true; }
    }
    eval::MacroFnError mfe;
    if (eval::tryCaptureDefinition(substituteCRefs(rawCmd, session), session.eval, &mfe)) {
      // BUG-216/217: a consumed-but-FAILED extended macro function. `reason` is the
      // exact-code channel eval::coarseRc() honours (its BUG-187 note), so `_rc`
      // comes back as Stata's own 101/111 instead of the generic 1.
      if (mfe.failed()) {
        // BUG-339: carry the offending line as `context`. This return builds a
        // FRESH array, so the entry echo is dropped — and under `capture
        // noisily` the wrapper strips inner echoes too, which is exactly how
        // 160 of these per corpus run showed up in a log with nothing naming
        // their source. `context` costs nothing and is contract-legal.
        { blocks = ojson::array({ojson(eval::errorBlock("engine_error", mfe.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe.rc + ")"))}); return true; }
      }
      { blocks = ojson::array(); return true; }
    }
  }
  return false;
}


bool expandCommandMacros(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::string& lineAsTyped,
    bool& haveVerbatim) {

  {
  dprof::Scope __z(dprof::kExpand);
  if (eval::hasMacroRefs(rawCmd)) {
    eval::ExpandResult r = eval::expandMacros(rawCmd, session.eval, false, /*keepVerbatim=*/true);
    if (r.isError) {
      // A display whose later piece fails to expand prints its earlier pieces
      // first (displayPartialBlock).
      if (isDisplayLine(rawCmd)) {
        if (auto partial = displayPartialBlock(rawArgsAfterCmd(rawCmd), r.error, session, false))
          blocks.push_back(*partial);
      }
      blocks.push_back(r.error);
      return true;
    }
    cmdVerbatim = r.line;
    haveVerbatim = true;
    cmdForDispatch = eval::decodeVerbatim(r.line);
    if (!r.typed.empty()) lineAsTyped = eval::decodeVerbatim(r.typed);
  }

  // ------------------------------------------------------------------
  // 0b-bis. BUG-176 (P2-PREP): re-apply the two PRE-expansion matchers above
  // to the line macro expansion just produced.
  //
  // Steps (c)/(d) (tempfile/tempname/tempvar) and 0a (tryCaptureDefinition)
  // both pattern-match the RAW line — deliberately, so the names they bind
  // stay literal identifiers (see those steps' own comments). But that makes
  // the whole matcher blind whenever the COMMAND WORD itself arrives through
  // a macro: `global L "tempvar t1"` + `capture quietly $L` reached step (c)
  // as the literal token `$L`, missed, and fell through to
  // eval/tokenizer.cpp's macro-statement keyword net, which rejects
  // "tempvar" unconditionally -> `unsupported_macro`, rc 198. The `scalar`
  // twin fell past the list/dir-only `scalar` arm further down and returned
  // rc 199. Both are the same defect: expansion happens AFTER the matchers
  // that need to see what it produced. Measured on a do-file that dispatches
  // `capture quietly $BENCH_LINE`.
  //
  // Guarded on `cmdForDispatch != rawCmd`, so the literal path is untouched:
  // a line with no macro refs never re-enters here at all, and one whose
  // expansion is a no-op is byte-identically excluded. Nor can this
  // double-fire — every arm above RETURNS on a match, so reaching this point
  // proves the raw line matched none of them. The only lines that can be
  // consumed here are precisely those whose leading keyword came out of a
  // macro, which is the class being fixed.
  //
  // Deliberately NOT re-applied: steps (e) `return scalar`/`return local`,
  // (f) `program drop|dir`, and 0a-bis's resampling prefixes. Each has
  // ordering semantics of its own (0a-bis in particular MUST see its
  // exp_list unexpanded), and none was implicated by a measurement — left
  // for a follow-up that can measure them.
  if (cmdForDispatch != rawCmd) {
    ojson u4Out;
    if (tryTempDeclIntercept(cmdForDispatch, session, echo, u4Out)) { blocks = u4Out; return true; }
    if (handlers::tryMacroVerbIntercept(cmdForDispatch, session, echo, u4Out, true)) {
      { blocks = u4Out; return true; }  // AER tier-1, the macro-expanded `macro ...` line
    }
    // BUG-216/217: same error channel as step 0a's site. This is the arm that sees
    // a colon form whose COMMAND WORD arrived through a macro, so it needs the
    // same rc translation or that shape would still store literal text.
    {
      ojson extDisplayOut;  // BUG-513, the macro-delivered command word (BUG-176's class)
      if (tryExtDisplayCapture(cmdForDispatch, session, extDisplayOut)) { blocks = extDisplayOut; return true; }
    }
    eval::MacroFnError mfe2;
    if (eval::tryCaptureDefinition(substituteCRefs(cmdForDispatch, session), session.eval,
                                   &mfe2)) {
      if (mfe2.failed()) {
        // BUG-339 twin of the step-0a site above: name the line that failed.
        { blocks = ojson::array({ojson(eval::errorBlock("engine_error", mfe2.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe2.rc + ")"))}); return true; }
      }
      { blocks = ojson::array(); return true; }
    }
  }
  }
  return false;
}


bool finishVerbatimLine(ojson& blocks,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::string& lineAsTyped,
    bool& haveVerbatim) {
  // dprof kExpand
  // EM3b: a line with no macro reference never reached expandMacros' compound-quote
  // normalization; a `"..."' holding no '"' is a plain literal here too.
  if (!haveVerbatim) {
    if (cmdForDispatch.find("`\"") != std::string::npos) {
      std::string folded = eval::normalizeCompoundQuotes(cmdForDispatch);
      if (folded.size() != cmdForDispatch.size()) lineAsTyped = cmdForDispatch;  // BUG-1527
      cmdForDispatch = std::move(folded);
    }
    cmdVerbatim = cmdForDispatch;
  }
  return false;
}


bool interceptXiPrefix(SessionState& session,
    ojson& blocks,
    std::string& cmdForDispatch) {

  // `xi [, opts] : <command>` (AER bundle H). Intercepted BEFORE tokenize
  // because the tokenizer would read `xi:` as the command word and the wrapped
  // line as its varlist. Materializes the _I* columns, then re-dispatches the
  // wrapped command with each i. term replaced by the names it produced -- which
  // is what makes `xi: regress y i.region` estimate on real columns and report
  // `_Iregion_2` in its coefficient table, exactly as Stata does.
  {
    static const std::regex reXiPrefix(R"(^\s*xi\s*(?:,\s*([^:]*?))?\s*:\s*(\S.*)$)",
                                       std::regex::icase);
    std::smatch mxi;
    if (std::regex_match(cmdForDispatch, mxi, reXiPrefix)) {
      const std::string optText = mxi.str(1);
      // xi-glued-comma: `xi: qreg2 y i.mr, cl(cid)` (w2j_e3_qreg2.log:1672, rc 0)
      // glues the options comma to the last i. term, so the token `i.mr,` named a
      // variable `mr,` (r(111)). The first top-level comma is set off by a space;
      // the rewritten line keeps the options verbatim after it.
      const std::string wrapped = [](std::string w) {
        int depth = 0;
        bool inQuote = false;
        for (size_t i = 0; i < w.size(); ++i) {
          const char c = w[i];
          if (c == '"') inQuote = !inQuote;
          if (inQuote) continue;
          if (c == '(') {
            ++depth;
          } else if (c == ')') {
            --depth;
          } else if (c == ',' && depth == 0) {
            if (i > 0 && !std::isspace(static_cast<unsigned char>(w[i - 1]))) w.insert(i, " ");
            break;
          }
        }
        return w;
      }(mxi.str(2));
      std::vector<std::pair<std::string, eval::OptValue>> xiOpts;
      if (optText.find("noomit") != std::string::npos) {
        eval::OptValue b;
        b.isBool = true;
        b.boolValue = true;
        xiOpts.emplace_back("noomit", b);
      }
      static const std::regex rePfx(std::string(R"(prefix\(\s*()") + eval::kRxName + R"()\s*\))",
                                    std::regex::icase);
      std::smatch mp;
      // A Unicode stub is a name (core/ident.h); a non-name is not taken, as the
      // ASCII pattern never took one.
      if (std::regex_search(optText, mp, rePfx) && strutil::isValidStataIdentifier(mp.str(1))) {
        eval::OptValue p;
        p.strValue = mp.str(1);
        xiOpts.emplace_back("prefix", p);
      }
      std::vector<std::string> terms;
      {
        std::istringstream ts(wrapped);
        std::string tk;
        while (ts >> tk)
          if (tk.compare(0, 2, "i.") == 0) {
            // a glued option comma (`i.c, fe`) is not part of the term
            const size_t comma = tk.find(',');
            terms.push_back(comma == std::string::npos ? tk : tk.substr(0, comma));
          }
      }
      if (terms.empty()) {
        // No i. term to expand: Stata still runs the command. Forward it
        // unchanged rather than inventing an error.
        { blocks = dispatchImpl(wrapped, session); return true; }
      }
      std::string rewritten, xiDisplay;
      ojson xerr =
          handlers::runXiPrefix(terms, xiOpts, wrapped, session, &rewritten, &xiDisplay);
      if (!xerr.is_null()) {
        blocks.push_back(xerr);
        return true;
      }
      if (!xiDisplay.empty()) {
        ojson db;
        db["type"] = "display";
        db["text"] = xiDisplay;
        blocks.push_back(db);
      }
      ojson inner = dispatchImpl(rewritten, session);
      if (inner.is_array())
        for (const auto& b : inner) blocks.push_back(b);
      else if (!inner.is_null())
        blocks.push_back(inner);
      return true;
    }
  }
  return false;
}

}
