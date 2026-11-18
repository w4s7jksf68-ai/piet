// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
//
// Stored-estimate snapshot helpers and the two error blocks shared by every
// reader of the estimates store: stored_estimates_commands.cpp,
// estimates_save_use.cpp, tables/esttab_block.cpp,
// graph/coefplot_estimates_block.cpp and tables/estimates_table.cpp. Defined
// once in stored_estimates_snapshot.cpp (moved verbatim out of the former
// monolithic stored-estimates source).
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace stored_estimates_detail {

ojson engineErrorBlockEt(const std::string& message, const std::string& code = "engine_error");
ojson estNamelistError(const SessionState::EstNamelist& ex);
ojson makeSnapshot(const ojson& estBlock);
ojson escalarsFrom(const ojson& estBlock);
// The live e() scalar store's numeric entries added to `esc` where it has no
// value of that name: the scalars a fit posts to the store only (regress's
// e(ll), e(ll_0), e(rank), e(sum_w)) stay readable from a stored model
// (estout stats(), aic/bic). Only for the e() of the block being stored.
void mergeLiveEScalars(ojson& esc, const SessionState& session);
// The live e() macros (name -> text; an empty one is not posted), as `: e(macros)`
// names them: what estout's stats() reads for a string statistic (`stats(vce)`).
ojson emacrosFrom(const SessionState& session);
ojson emptyAddedBag();

}  // namespace stored_estimates_detail

}  // namespace piet::handlers
