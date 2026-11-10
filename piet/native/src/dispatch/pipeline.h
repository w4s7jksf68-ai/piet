// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"
namespace piet {
nlohmann::ordered_json dispatchImpl(const std::string&, SessionState&);
}
namespace piet::dispatch_detail {
bool route_io(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_describe(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_data_names(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_data_generate(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask);

bool route_estimation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd,
    const std::string& graphLineTyped);

bool route_time(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_session(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_environment(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_data_rows(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool route_data_combine(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_results(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool route_extensions(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_tabulation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_postestimation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool route_graphics(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& graphLineTyped,
    QualifierColumnGuard& conditionMask);

bool route_table_family(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool interceptControlBlocks(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptDoRun(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptOutputPrefixes(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptEstimatePrefix(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptVersionPrefix(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptSurveyPrefix(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptProgramCompletion(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptArguments(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptArgumentMacros(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptShellEscape(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptMacroDeclarations(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptProgramReturn(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptProgramDefinition(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptInputBlock(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptMata(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptParallelPrefix(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptDefinition(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptResampling(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool interceptRandomization(SessionState& session,
    ojson& blocks,
    const std::string& rawCmd);

bool expandCommandMacros(SessionState& session,
    ojson& echo,
    ojson& blocks,
    const std::string& rawCmd,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::string& lineAsTyped,
    bool& haveVerbatim);

bool finishVerbatimLine(ojson& blocks,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::string& lineAsTyped,
    bool& haveVerbatim);

bool interceptXiPrefix(SessionState& session,
    ojson& blocks,
    std::string& cmdForDispatch);

bool tokenizeCommand(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    std::string& cmdForDispatch,
    std::string& cmdVerbatim,
    std::optional<RcHiddenScope>& rcHidden);

bool foldCommandCondition(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask);

bool noteConditionProfile(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks);

bool resolveCommandOptions(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks);

bool checkCommunityCommand(std::string& cmdTyped,
    ojson& blocks);

bool normalizeGtoolsAlias(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool executeByPrefix(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    QualifierColumnGuard& conditionMask);

bool resolveProgramCommand(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& echo,
    ojson& blocks,
    std::string& cmdForDispatch,
    std::string& lineAsTyped);

bool commandNeedsNoData(const std::string& cmd);

bool countWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool assertWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks);

bool parseEmptySave(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks,
    bool& saveEmptyOk);

bool allowEmptyGenerate(std::string& cmd,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk);

bool validateEmptyDataset(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk);

bool requireCommandData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData);

bool checkCommandArity(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    ojson& blocks);
}
