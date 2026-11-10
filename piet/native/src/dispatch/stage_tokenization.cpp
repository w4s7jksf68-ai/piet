// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/variables.h"
#include "piet/handlers/vce_types.h"
namespace piet::dispatch_detail {
bool tokenizeCommand(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::optional<RcHiddenScope>& rcHidden) {
  // ifReadsCallerRc, for this command's run
  {
    dprof::Scope __z(dprof::kTokenize);
    // BUG-1049: a user program parses its own arguments (`0'), so a compound
    // literal holding a '"' is the program's to read, not the tokenizer's to
    // refuse; the call needs only the program's name.
    const std::string headWord0 = [&] {
      const std::string t = trimWs(cmdForDispatch);
      return t.substr(0, t.find_first_of(" \t"));
    }();
    try {
      parsed = eval::tokenize(cmdVerbatim);
    } catch (const eval::TokenizerError& te) {
      if (te.block.value("token", "") == "`\"" &&
          program::findProgram(session, headWord0).has_value()) {
        parsed = eval::ParsedCommand{};
        parsed.cmd = headWord0;
        parsed.raw = cmdForDispatch;
      } else {
        if (te.block.value("code", "") == "syntax_error" &&
            te.block.value("message", "").find("comment") != std::string::npos) {
          { blocks = ojson::array(); return true; }  // pure comment lines produce nothing
        }
        // BUG-535: under a by-prefix, `in` is Stata's own refusal — "'in' may not
        // be combined with 'by'", r(190) — for every sub-command
        // (w2_bykeep.log arms by_keep_in / by_keep_if_in), not PIET's generic
        // "qualifier unsupported" r(198). The tokenizer rejects `in` on a
        // by-line because `by` has no registry row; only the rc and the words
        // change here, the line still does not run.
        if (te.block.value("code", "") == "unsupported_option" &&
            te.block.value("token", "").rfind("in ", 0) == 0) {
          const std::string head = lower(trimWs(cmdForDispatch).substr(
              0, trimWs(cmdForDispatch).find_first_of(" \t")));
          if (head == "by" || head == "bys" || head == "byso" || head == "bysor" ||
              head == "bysort") {
            blocks.push_back(ojson(eval::errorBlock(
                "syntax_error", "'in' may not be combined with 'by'", te.block.value("token", ""),
                std::nullopt, std::nullopt, "r(190)")));
            return true;
          }
        }
        blocks.push_back(ojson(te.block));
        return true;
      }
    }
    session.leafCmdExpanded = cmdForDispatch;  // the fit hook's e(cmdline)
    // BUG-428/432: the row layer gets the matrix/scalar substitution the macro
    // layer has always had. One call site per expression that reaches the row
    // evaluator — the `if` here, `assert`'s claim and gen/replace's expression.
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::optional<ojson> rowErr;
      // A by-prefixed line's sub-command decides at the by: driver.
      const std::string rowCmd = registry::resolveCanonical(rstripColon(lower(parsed.cmd)));
      const bool byLine = rowCmd == "by" || rowCmd == "bys" || rowCmd == "byso" ||
                          rowCmd == "bysor" || rowCmd == "bysort";
      const bool readsRc = ifReadsCallerRc(rowCmd);
      if (!readsRc && !byLine && substituteRcValue(*parsed.ifExpr, session) != *parsed.ifExpr)
        rcHidden.emplace();
      parsed.ifExpr = substituteRowExprRefs(*parsed.ifExpr, session, &rowErr, readsRc);
      if (rowErr.has_value()) {  // BUG-456: r(111) / r(109), never a silent `.`/0
        blocks.push_back(*rowErr);
        return true;
      }
    }
    cmdTyped = rstripColon(lower(parsed.cmd));
    // BUG-105 (WS-K) — THE ONE COMMAND-NAME RESOLUTION POINT. Every arm below,
    // plus kNoDataCommands and kFixedArityCommands, now compares the CANONICAL
    // spelling, so adding a Stata abbreviation is a one-line registry edit
    // instead of the two-file (registry + if-chain literal) edit the 07-31
    // BUG-105 stopgap needed. Non-registry tokens (by/bysort, the pre-tokenize
    // keywords) come back unchanged, and a community-package name comes back
    // unchanged on purpose — see registry.h's resolveCanonical() comment.
    // Reverting this one line is the seeded-RED leg selftest's
    // testWsKAbbrevResolutionIsLoadBearing() asserts against.
    cmd = registry::resolveCanonical(cmdTyped);

    // BUG-450 — THE VARLIST EXPANSION POINT, the third sibling of the command
    // and option resolution points above and below it.
    //
    // `loops::expandVarlist` calls itself "the ONE varlist wildcard/range
    // expander", and until now its callers were the loop header, `ds`/`unab`
    // and the handful of AER handlers written against it directly. The plain
    // data verbs were not among them: `drop t*` answered `variable t* not
    // found` r(111) while `foreach v of varlist t*` expanded correctly in the
    // same session on the same data. In the ninety-package AER corpus that is
    // 1,214 `drop` lines and 989 `keep` lines carrying a wildcard, plus the
    // range forms — the ordinary way that corpus subsets a dataset.
    //
    // Two guards keep this from being a blanket change:
    //   1. The ROW must opt in (`varlistIsPlain`). `rename t* T*` is Stata's
    //      rename-GROUPS syntax and must reach its arm unexpanded; estimation
    //      rows carry `i.x`/`L.x` in the same field. See registry.cpp's list.
    //   2. The TOKEN must actually look like a pattern. A varlist with no `*`,
    //      `?`, `-` or `_all` is passed through byte-identically, so the
    //      common case is untouched and cannot regress.
    // A no-match answers r(111), which is what Stata answers.
    if (!parsed.varlist.empty()) {
      const registry::CommandSpec* spec = registry::findByName(cmd);
      if (spec != nullptr && spec->varlistIsPlain) {
        // BUG-525, second half (oracle corr_glued == ctl_corr: r(rho)
        // .0171895075182102 both ways; levelsof_glued -> `0 1 2`): Stata's
        // varlist parser accepts ONE pair of parentheses around a plain
        // varlist, and the glued heads deliver exactly that shape once the
        // word is split (`corr (a c)`, `levelsof (g)`). PIET forwarded `(a`
        // and `c)` as names and answered "column '(a' not found". Unwrapped
        // here, at the one varlist site -- but gated on its OWN allow-list
        // (kParenVarlistUnwrap), not on `varlistIsPlain`: the oracle has an
        // arm for exactly corr_glued and levelsof_glued, not for the other
        // thirteen plain-varlist rows (drop/keep/summarize/destring/...),
        // and reusing the broad flag would have changed their answer for
        // `(x1 x2)`-shaped input with no oracle arm behind it (review
        // finding, W2 labelglued fix round). The whole field must be the
        // one group -- an inner paren, `=` or quote leaves it alone -- so
        // `test (a = b)` and `(y1 x) (y2 x)` shapes are untouched even on a
        // plain row.
        static const std::unordered_set<std::string> kParenVarlistUnwrap = {"corr", "levelsof"};
        if (kParenVarlistUnwrap.count(cmd) != 0 &&
            parsed.varlist.front().front() == '(' && parsed.varlist.back().back() == ')') {
          std::string joined;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) joined += ' ';
            joined += parsed.varlist[i];
          }
          const std::string inner =
              joined.size() >= 2 ? joined.substr(1, joined.size() - 2) : std::string();
          if (!inner.empty() && inner.find_first_of("()=\"`'") == std::string::npos) {
            std::vector<std::string> unwrapped;
            std::istringstream is(inner);
            for (std::string tk; is >> tk;) unwrapped.push_back(tk);
            if (!unwrapped.empty()) parsed.varlist = unwrapped;
          }
        }
        bool patterned = false;
        for (const auto& t : parsed.varlist) {
          if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
              t.find('-') != std::string::npos || lower(t) == "_all") {
            patterned = true;
            break;
          }
        }
        if (patterned && session.hasData()) {
          std::string spec_str;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) spec_str += ' ';
            spec_str += parsed.varlist[i];
          }
          loops::VarlistExpandResult ex = loops::expandVarlist(spec_str, session);
          if (ex.isError) {
            blocks.push_back(ex.error);
            return true;
          }
          parsed.varlist = ex.tokens;
        }
        // Variable-name abbreviation ([U] 11.2) for every plain varlist: a name
        // that is no variable but the unique prefix of one stands for it, several
        // are r(111) "ambiguous abbreviation", a name matching nothing is left as
        // typed for the command's own refusal (handlers::resolveByKeyNames, the
        // rule keep/drop/rename and the by keys use). StataBE ec.do: `tab
        // edu_category` tabulates edu_category_BL (007's Stats_for_paper.do).
        if (session.hasData()) {
          if (auto err = handlers::resolveByKeyNames(session, parsed.varlist)) {
            blocks.push_back(*err);
            return true;
          }
        }
      } else if (spec != nullptr && session.hasData()) {
        // A single-equation fit's `syntax varlist(fv ts)` expands a range or a
        // wildcard like any varlist (`xtreg y x1-x3, fe`: w2d_uncovered.log
        // xtreg_fe_panelonly_034 b[x1] 2.0995 over x1 x2 x3). These rows keep
        // `i.x`/`L.x`/`a#b`/`(x = z)` in the same field, so only a token that is
        // a pure name pattern -- letters, digits, `_`, `*`, `?`, `~`, and an
        // inner `-` -- is expanded, each on its own; every other token passes
        // through byte-identically. Post-estimation rows (lincom/test/nlcom/
        // margins) are not listed: `x1-x2` is an expression there.
        static const std::unordered_set<std::string> kFitVarlistPatterns = {
            "regress", "areg",    "xtreg",  "reghdfe", "logit",   "probit", "logistic",
            "poisson", "nbreg",   "qreg",   "tobit",   "newey",   "prais",  "glm",
            "xtlogit", "xtprobit", "xtpoisson", "ologit", "oprobit", "mlogit", "cloglog",
            "ppmlhdfe", "ivregress", "ivreg2", "ivreghdfe", "feols", "fepois", "xtivreg2"};
        if (kFitVarlistPatterns.count(cmd) != 0) {
          const auto isNamePattern = [](const std::string& t) {
            if (lower(t) == "_all") return true;
            if (t.empty() || t.front() == '-' || t.back() == '-') return false;
            // core/ident.h: `ملاح*` / `دخل_2020-ق3` are name patterns too.
            if (!(cecon::ident::isIdentStartByte(t.front()) || t.front() == '*' ||
                  t.front() == '?'))
              return false;
            bool pattern = false;
            for (char ch : t) {
              if (ch == '*' || ch == '?' || ch == '-' || ch == '~') {
                pattern = true;
              } else if (!cecon::ident::isIdentByte(ch)) {
                return false;
              }
            }
            return pattern;
          };
          std::vector<std::string> out;
          bool changed = false;
          for (const auto& t : parsed.varlist) {
            if (!isNamePattern(t)) {
              out.push_back(t);
              continue;
            }
            loops::VarlistExpandResult ex = loops::expandVarlist(t, session);
            if (ex.isError) {
              blocks.push_back(ex.error);
              return true;
            }
            out.insert(out.end(), ex.tokens.begin(), ex.tokens.end());
            changed = true;
          }
          if (changed) parsed.varlist = std::move(out);
          // Variable-name abbreviation ([U] 11.2) in a fit's varlist, depvar
          // included: StataBE uni-oracle/fa.do `regress y miss_unemployed` fits
          // miss_unemployed_BL (F1), and so do `i.edu_category`,
          // `c.miss_unemployed#i.edu_cat`, `L.miss_unemployed` and reghdfe
          // (F2-F5, e(b) carries the full names); `edu` / `i.edu` with two
          // candidates is "edu ambiguous abbreviation" r(111) (F7/F8). Each `#`
          // atom resolves the name after its operator (handlers::
          // resolveByKeyNames, the rule the plain varlists use). DESIGN
          // DECISION (no oracle arm): a token holding `(`, `)` or `=` (an IV
          // block, `ib(last).x`) and ivregress's estimator word pass unchanged.
          const auto isPlainName = [](const std::string& s) {
            if (s.empty() || !cecon::ident::isIdentStartByte(s.front())) return false;
            for (char ch : s)
              if (!cecon::ident::isIdentByte(ch)) return false;
            return true;
          };
          for (size_t i = (cmd == "ivregress" ? 1 : 0); i < parsed.varlist.size(); ++i) {
            std::string& tok = parsed.varlist[i];
            if (tok.find_first_of("()=") != std::string::npos) continue;
            std::string rebuilt;
            size_t pos = 0;
            while (pos <= tok.size()) {
              size_t hash = tok.find('#', pos);
              if (hash == std::string::npos) hash = tok.size();
              std::string atom = tok.substr(pos, hash - pos);
              const size_t dot = atom.rfind('.');
              std::string name = dot == std::string::npos ? atom : atom.substr(dot + 1);
              if (isPlainName(name)) {
                std::vector<std::string> one = {name};
                if (auto err = handlers::resolveByKeyNames(session, one)) {
                  blocks.push_back(*err);
                  return true;
                }
                atom = (dot == std::string::npos ? std::string() : atom.substr(0, dot + 1)) +
                       one.front();
              }
              rebuilt += atom;
              size_t next = hash;
              while (next < tok.size() && tok[next] == '#') rebuilt += tok[next++];
              if (hash == tok.size()) break;
              pos = next;
            }
            tok = rebuilt;
          }
          // BUG-1437: the cluster variables of vce(cluster ...) and cluster() are
          // variable names too, abbreviated by the same rule: StataBE 19.5
          // (2026-11-03) fits `vce(cluster kgr)` and `cluster(kgr)` on kgroup
          // with every one of regress, areg, logit, probit, poisson, nbreg,
          // xtreg, ivregress, reghdfe and ppmlhdfe (e(clustvar) kgroup), and
          // vce(cluster kgroup) with cluster(kgr) is the one variable twice. Only
          // a unique prefix is rewritten here; a name matching nothing or
          // several is left as typed for the fit's own parser, which words it
          // (clusterSpecRefusal / clusterSpecRefusalM4).
          for (auto& [k, v] : parsed.opts) {
            if (v.isBool) continue;
            const bool vceOpt = k == "vce" || k == "vcov";
            if (!vceOpt && k != "cluster") continue;
            std::string head;
            std::string rest = v.strValue;
            if (vceOpt) {
              std::string vceRest;
              if (handlers::canonicalVceType(v.strValue, &vceRest) != "cluster" || vceRest.empty())
                continue;
              std::istringstream hs(v.strValue);
              hs >> head;
              rest = vceRest;
            }
            std::vector<std::string> names;
            std::istringstream rs(rest);
            for (std::string t; rs >> t;) names.push_back(t);
            bool rewrote = false;
            for (auto& n : names) {
              if (!isPlainName(n)) continue;
              std::vector<std::string> one = {n};
              if (handlers::resolveByKeyNames(session, one).has_value()) continue;
              if (one.front() != n) {
                n = one.front();
                rewrote = true;
              }
            }
            if (!rewrote) continue;
            std::string joined = head;
            for (const auto& n : names) joined += (joined.empty() ? "" : " ") + n;
            v.strValue = joined;
          }
        }
      }
      // fvgroup-operator: Stata's grouped factor operator `i.(a b c#d)` distributes
      // the operator over every term and every `#` atom (`i.a i.b i.c#i.d`); the
      // 069 form `qreg y z i.(tr month sid#cash#risk clinic), q(.37)` is rc 0
      // (w2j_e3_qreg2.log:3103). Estimation rows only; an old-style IV block
      // `(y = z)` carries no operator prefix and is left alone.
      if (spec != nullptr && spec->family == registry::Family::Estimation)
        parsed.varlist = expandFactorGroupTokens(parsed.varlist);
    }
  }
  return false;
}

}
