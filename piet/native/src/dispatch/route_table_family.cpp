// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_table_family(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // Table-command family. Keep the single result-shape boundary here:
  // pushEnvResult flattens handler arrays and drops handled null JSON blocks.
  if (auto result = commands::runTableCommand(cmd, parsed, session)) {
    pushEnvResult(blocks, *result);
    return true;
  }
  return false;
}

}
