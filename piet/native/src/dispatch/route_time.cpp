// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_time(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // M3a handler-surface expansion (ROADMAP M3a item 4). Each arm mirrors
  // commands.py's classify() routing for the same command word.

  if (cmd == "xtset" || cmd == "tsset" || cmd == "tset") {
    const auto& known = cmd == "xtset" ? kVocabXtset.known : kVocabTsset.known;
    commands::runTimeDeclaration(cmd, parsed, session, known, blocks);
    return true;
  }
  return false;
}

}
