// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Table command routing, moved from dispatcher.cpp. Common parsing, prefix
// handling, result flattening and the surrounding state lifecycle stay there.
#include "piet/table_commands.h"

#include "piet/handlers.h"
#include "piet/session_state.h"

namespace piet::commands {

using ojson = nlohmann::ordered_json;

std::optional<ojson> runTableCommand(const std::string& cmd,
                                   const eval::ParsedCommand& parsed, SessionState& session) {
  if (cmd == "estadd") return handlers::runEstadd(parsed, session);
  if (cmd == "estpost") {
    // BUG-733: a successful estpost has posted a new e() (postPseudoEstimate)
    // with no fitted model behind it; the next model install drops it
    // (SessionState::supersedePostedE). estpost itself is unchanged.
    ojson r = handlers::runEstpost(parsed, session);
    const bool refused = r.is_object() && r.value("type", std::string()) == "error";
    if (!refused && session.lastEstBlock.has_value() &&
        session.lastEstBlock->value("estimator", std::string()).rfind("estpost_", 0) == 0) {
      session.ePostedLive = true;
      // round 3: postPseudoEstimate replaced e() wholesale, so nothing sits on
      // top of a fit any more.
      session.eOnFitNames.clear();
    }
    return r;
  }
  if (cmd == "estout") return handlers::runEstout(parsed, session);
  if (cmd == "outreg2") return handlers::runOutreg2(parsed, session);
  if (cmd == "texsave") return handlers::runTexsave(parsed, session);
  if (cmd == "texdoc") return handlers::runTexdoc(parsed, session);
  if (cmd == "tex") return handlers::runTexdocLine(parsed, session);
  if (cmd == "frmttable") return handlers::runFrmttable(parsed, session);
  if (cmd == "regsave") return handlers::runRegsave(parsed, session);
  if (cmd == "parmest") return handlers::runParmest(parsed, session);
  if (cmd == "mat2txt") return handlers::runMat2txt(parsed, session);
  return std::nullopt;
}

}  // namespace piet::commands
