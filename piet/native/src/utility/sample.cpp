// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/dispatcher.h"  // marksample / tsspell cond(): a dispatched `generate`
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

#include "support.h"
#include "options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// marksample
// ===========================================================================
//
// PROGRAM-INTERNAL ONLY, and this is the one refusal in the bundle that exists
// to prevent a SILENT wrong answer rather than to admit a gap. `marksample`
// reads the local macro `varlist`, which is set by `syntax` -- and `syntax`
// only runs inside a program. At top level that local does not exist, the
// condition collapses to "1", and every observation gets marked: a `touse` that
// selects everything, which every later command then honours. Refusing when
// progCallDepth is 0 is the only honest answer.
ojson runMarksample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarksample.known,
                                            kVocabMarksample.unimplemented))
    return ojson(*err);
  if (parsed.varlist.size() != 1)
    return utilSyntax("marksample: syntax is `marksample name [, novarlist]`");
  if (session.progCallDepth == 0)
    return utilSyntax(
        "marksample: not allowed outside a program -- it reads the local macro `varlist`, which "
        "only `syntax` sets, and outside a program that local does not exist. Every observation "
        "would be marked.");
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string name = parsed.varlist[0];
  ojson tv = runTempvar({name}, session);
  if (const ojson* e = firstError(tv)) return *e;
  const std::string* minted = session.eval.locals.find(name);
  if (minted == nullptr || minted->empty())
    return utilError("marksample: could not mint a temporary variable name", "internal");

  std::string cond = "1";
  if (!hasFlag(parsed.opts, "novarlist")) {
    const std::string* vl = session.eval.locals.find("varlist");
    const std::vector<std::string> vars = vl ? splitWs(*vl) : std::vector<std::string>();
    if (!vars.empty()) {
      cond.clear();
      for (const auto& v : vars) {
        if (!cond.empty()) cond += " & ";
        cond += "!missing(" + v + ")";
      }
    }
  }
  // The `if`/`in` and weight the enclosing program's `syntax` bound (program.cpp
  // bindSyntax: the locals `if` = "if EXP", `in` = "in RANGE", `weight`/`exp`)
  // fold into the marker as Stata's marksample does: a row outside them is 0,
  // and so is a row with a missing or zero weight.
  const std::string* ifL = session.eval.locals.find("if");
  const std::string* inL = session.eval.locals.find("in");
  const std::string* wL = session.eval.locals.find("weight");
  const std::string* eL = session.eval.locals.find("exp");
  if (wL != nullptr && !wL->empty() && eL != nullptr && !eL->empty()) {
    std::string w = *eL;
    const size_t eq = w.find('=');
    if (eq != std::string::npos) w = w.substr(eq + 1);
    cond = "(" + cond + ") & !missing(" + w + ") & (" + w + ") != 0";
  }
  std::string qual;
  if (ifL != nullptr && !ifL->empty()) qual += " " + *ifL;
  if (inL != nullptr && !inL->empty()) qual += " " + *inL;
  ojson blocks =
      dispatch("quietly generate byte " + *minted + " = (" + cond + ")" + qual, session);
  if (const ojson* e = firstError(blocks)) return *e;
  if (!qual.empty()) {
    blocks = dispatch("quietly replace " + *minted + " = 0 if missing(" + *minted + ")", session);
    if (const ojson* e = firstError(blocks)) return *e;
  }
  return silent();
}


}  // namespace piet::handlers
