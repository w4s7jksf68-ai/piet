// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data duplicates route.
#include "data_combine_internal.h"
namespace piet::handlers {
using namespace data_combine_detail;


ojson runDuplicates(const std::string& sub, const std::optional<std::vector<std::string>>& varlist,
                    const std::optional<std::string>& genvar, SessionState& session,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange) {
  std::string subLower = lower(sub);
  if (subLower == "report") return runDuplicatesReport(varlist, session, ifExpr, inRange);
  if (subLower == "drop") return runDuplicatesDrop(varlist, session, ifExpr, inRange);
  if (subLower == "tag") {
    if (!genvar.has_value() || genvar->empty()) {
      return engineErrorBlock("duplicates tag: ,gen(NEWVAR) required");
    }
    return runDuplicatesTag(varlist, *genvar, session, ifExpr, inRange);
  }
  if (subLower == "list") return runDuplicatesList(varlist, session, ifExpr, inRange);
  if (subLower == "examples") return runDuplicatesExamples(varlist, session, ifExpr, inRange);
  return engineErrorBlock(
      "duplicates " + sub +
      ": unknown subcommand. Use: duplicates report|examples|drop|tag|list [VARLIST]");
}
}
