// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "prefix_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool interceptResampling(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // 0a-bis. Resampling prefixes (P6) — intercepted BEFORE macro expansion.
  //
  // `bootstrap mean=r(mean), reps(30) seed(...): summarize y` carries an
  // expression list that must be evaluated ONCE PER REPLICATE, after that
  // replicate's inner command has posted its r()/e()/_b[]. Step 0b below would
  // substitute those references here and now — against whatever the PREVIOUS
  // command left, or erroring as unset — which is a different question from the
  // one the user asked. So the exp_list is carried verbatim into the driver;
  // the options and the inner command ARE expanded (they legitimately hold
  // locals, e.g. `reps(`R')` or `: regress `depvar' x`).
  {
    dprof::Scope __z(dprof::kResample);
    static const std::regex reResamplePrefix(
        R"(^\s*(bootstrap|jackknife|permute|rolling|statsby)\b(.*)$)", std::regex::icase);
    std::smatch rm;
    if (std::regex_match(rawCmd, rm, reResamplePrefix)) {
      const std::string method = lower(trimWs(rm.str(1)));
      std::string rest = rm.str(2);
      // The prefix colon is the first one outside quotes AND outside brackets:
      // an exp_list element `_b[treat:lower]` (an equation-striped coefficient)
      // carries a colon of its own (lane BS; StataBE scratch-r150/lee/bs/o5.log U1).
      size_t colonPos = std::string::npos;
      {
        int depth = 0;
        const std::vector<bool> quoted = eval::quotedSpanMask(rest);  // compound quotes too
        for (size_t i = 0; i < rest.size(); ++i) {
          const char ch = rest[i];
          if (i < quoted.size() && quoted[i]) continue;
          if (ch == '(' || ch == '[' || ch == '{') ++depth;
          else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) --depth;
          else if (ch == ':' && depth == 0) {
            colonPos = i;
            break;
          }
        }
      }
      // A bare `bootstrap` (no colon) is not the prefix form — let it fall
      // through to the unsupported-command path, which is what Stata's own
      // "command bootstrap is unrecognized" corresponds to here.
      if (colonPos != std::string::npos) {
        std::string headPart = trimWs(rest.substr(0, colonPos));
        std::string innerRaw = trimWs(rest.substr(colonPos + 1));
        auto [expAndVar, optsOpt] = splitTopLevelComma(headPart);
        std::string expListRaw = trimWs(expAndVar);
        std::string optsRaw = optsOpt.has_value() ? trimWs(*optsOpt) : std::string();
        // `permute VAR explist` — the permutation variable sits between the
        // command word and the expression list.
        std::string permuteVar;
        if (method == "permute") {
          size_t sp = expListRaw.find_first_of(" \t");
          if (sp == std::string::npos) {
            blocks.push_back(ojson(eval::engineError(
                "permute: usage is `permute PERMVAR name=exp [...] [, options]: command`")));
            return true;
          }
          permuteVar = trimWs(expListRaw.substr(0, sp));
          expListRaw = trimWs(expListRaw.substr(sp));
        }
        // Expand the halves that MAY hold macros; the exp_list stays raw.
        auto expandHalf = [&](const std::string& in, bool* failed) -> std::string {
          if (in.empty() || !eval::hasMacroRefs(in)) return in;
          eval::ExpandResult er = eval::expandMacros(in, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            *failed = true;
            return in;
          }
          return er.line;
        };
        bool failed = false;
        optsRaw = expandHalf(optsRaw, &failed);
        if (failed) return true;
        std::string innerCmd = expandHalf(innerRaw, &failed);
        if (failed) return true;

        ojson res = (method == "statsby")
                        ? handlers::runStatsby(expListRaw, optsRaw, innerCmd, session)
                    : (method == "rolling")
                        ? handlers::runRolling(expListRaw, optsRaw, innerCmd, session)
                        : handlers::runResample(method, permuteVar, expListRaw, optsRaw, innerCmd,
                                                session);
        if (res.is_array()) {
          for (const auto& b : res) blocks.push_back(b);
        } else {
          blocks.push_back(res);
        }
        return true;
      }
    }
  }
  return false;
}


bool interceptRandomization(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd) {


  // ------------------------------------------------------------------
  // 0a-ter. `ritest` (E3, handlers_e3_ritest.cpp) -- intercepted before macro
  // expansion for the resampling prefixes' reason: its exp_list (`_b[tri]`,
  // `e(n)`) is evaluated after every replicate, never against the e() the
  // previous command left. The driver expands every other part itself.
  {
    static const std::regex reRitest(R"(^\s*ritest(\s.*)?$)", std::regex::icase);
    std::smatch rm;
    if (std::regex_match(rawCmd, rm, reRitest)) {
      ojson res = handlers::runRitestLine(rm[1].matched ? rm.str(1) : std::string(), session);
      for (const auto& b : res) blocks.push_back(b);
      return true;
    }
  }
  return false;
}

}
