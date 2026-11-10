// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Arc F — the seven QE / RD commands' PIET half: parse the Stata line, call the
// already-merged cobra verb, and print the table exactly as Stata prints it.
//
// NO STATISTICS LIVE HERE. Every number in every table below is read straight
// out of the engine's JSON; this file's whole job is the three things a human
// actually sees — the Stata syntax it accepts, the column geometry it prints,
// and the e()/rc it leaves behind.
//
//   rdrobust y running, c(0)          -> cobra rdrobust
//   rddensity running, c(0)           -> cobra rddensity
//   rdplot y running, c(0)            -> cobra rdplot          (table only; the
//                                        plot coordinates in the JSON are a
//                                        sibling arc's GRAPH half)
//   binscatter y x1, nquantiles(20)   -> cobra binscatter      (prints nothing but
//                                        its savedata notes, warnings and
//                                        reportreg tables; posts e())
//   eventstudyinteract y F2 .. L4, absorb() cohort() control_cohort() vce()
//   did_imputation y id t g, horizons() pretrends() autosample
//   sdid y id t treated, vce() reps() seed()
//
// RENDERING CONTRACT. Handlers here return an ARRAY of blocks: one `display`
// block per printed line, then ONE structured block last. That is the
// bootstrap_prefix.cpp precedent (P6 wave) and it is what makes the log tee
// and the console show the Stata table rather than a JSON key dump — the tee
// (session_state.cpp::logTee) renders `display` blocks as their text and
// everything else as `[type]` + an indented dump. It also composes with
// `quietly` for free: the quietly filter keeps only error/echo blocks, so
// `quietly rdrobust ...` prints nothing without this file knowing about it.
//
// LAYOUT AUTHORITY. internal capture p3_stata.log lines
// 152-378 (live Stata 19.5). Every column width below was measured off that
// file, not guessed. The two RD header blocks and rddensity's binomial ladder
// are genuinely unique shapes, so they are laid out per-command here rather
// than by bending the shared coefficient-table renderer; the ordinary
// coefficient tables (eventstudyinteract / did_imputation) reuse the EXACT
// widths bootstrap_prefix.cpp already established for Stata's 78-column
// regression table, and every number goes through fmtStataDefaultG — the one
// %9.0g renderer the whole product shares.
//
// P-VALUES. Not computed here. The engine returns a p for every row of every
// one of these commands, so there is no normal-CDF in this file at all.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace engine_report_detail {

// Engine-result plumbing shared by the rdrobust/rddensity/rdplot, binscatter,
// eventstudyinteract, did_imputation and sdid handlers. Definitions (and their
// comments) live in engine_report_support.cpp.

ojson qeError(const std::string& message, const std::string& code = "engine_error");
ojson qeErrorBlocks(const std::string& message, const std::string& code = "engine_error");
std::string padLeft(const std::string& s, size_t w);
std::string padRight(const std::string& s, size_t w);
std::string g9(double v);
std::string g9cell(double v);
std::string fixedN(double v, int n);
std::string f2(double v);
std::string f3(double v);
std::string f4(double v);
std::string f5(double v);
std::string commaGroup(int64_t v);
double num(const ojson& j, const char* key, double fallback = 0.0);
int64_t inum(const ojson& j, const char* key, int64_t fallback = 0);
std::string str(const ojson& j, const char* key, const std::string& fallback = "");
std::string coefHeader(const std::string& errWord, const std::string& stat,
                       const std::string& lvl);
std::optional<double> mapNum(const ojson& j, const char* obj, const std::string& key);
std::string levelWord(double level);

// --- block emission ----------------------------------------------------------

struct Printer {
  ojson lines = ojson::array();
  void operator()(const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  }
};

void postEClass(SessionState& session, const std::string& cobraCmd, const std::string& depvar,
                const std::string& dataPath, ojson result, bool ePosted = true,
                const std::string& weightVar = std::string(), const std::string& wtype = std::string());
ojson coefRow(const std::string& name, std::optional<double> b, std::optional<double> se,
              std::optional<double> t, std::optional<double> p, std::optional<double> lo,
              std::optional<double> hi);

std::optional<std::string> optStr(const eval::ParsedCommand& parsed, const std::string& name);
bool optFlag(const eval::ParsedCommand& parsed, const std::string& name);
std::vector<std::string> splitWsOrComma(const std::string& s);
std::vector<std::string> orderedCoefNames(const ojson& r, const std::vector<std::string>& wanted);
std::string joinComma(const std::vector<std::string>& v);
void pushOpt(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
             const std::string& stataOpt, const std::string& flag);
void pushLevel(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
               const SessionState& session);
void pushKeepif(std::vector<std::string>& argv, const eval::ParsedCommand& parsed);

// One call site for "run the engine, or turn its refusal into an honest PIET
// error". The engine already refuses unsupported options BY NAME
// ("rdrobust: --kernel epanechnikov is not supported ..."); that message is
// surfaced verbatim rather than being replaced with a generic one.
struct EngineCall {
  bool ok = false;
  ojson raw;
  ojson error;
};
EngineCall callEngine(std::vector<std::string> argv);

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
