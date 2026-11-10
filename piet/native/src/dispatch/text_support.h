// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

ojson echoBlock(const std::string& cmd);

std::string lower(std::string s);

std::string rstripColon(std::string s);

std::string rawWithCanonicalCmd(const std::string& raw, const std::string& typed,
                                const std::string& canonical);

std::string rawArgsAfterCmd(const std::string& raw);

std::string genExprText(const eval::ParsedCommand& parsed);

bool fileExistsLocal(const std::string& path);

std::string devTreeSampleFatalitiesCsv();

std::string dispatcherNextPath(const std::string& workdir, const std::string& prefix);

bool hasNorCapN(const std::string& expr);

std::string trimWs(const std::string& s);

std::string splitGluedHead(const std::string& line);

LabelSubMatch resolveLabelSub(const std::string& sub, std::string* canonical);

std::string rawWithCanonicalLabelSub(const std::string& raw, const std::string& typedSub,
                                     const std::string& canonicalSub);

void pushEnvResult(ojson& blocks, const ojson& r);

}  // namespace piet::dispatch_detail
