// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   postutil  session.postfiles; `clear` is runPostclose per open handle.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers/io.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "language/command_reply_support.h"

namespace {

// postutil takes no options.
const piet::eval::OptVocab& kVocabPostutil =
    piet::eval::registerOptVocab({"postutil"}, {{}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

// ===========================================================================
// postutil
// ===========================================================================
//
// Implements `postutil clear | dir` per post's help file; identified against
// StataBE probes (w2ax_t1_official.log postutil_*; envp1 pu_*). `clear` CLOSES
// every open postfile -- envp1 pu_file/pu_desc: the file is on disk afterwards
// holding the two posted rows -- so it is runPostclose for each handle, never a
// discard. `dir` prints the open handle names, one per line. Only the full
// words: envp1 `postutil d` and `postutil cl` are both r(198), as is any other
// word or a trailing argument.
nlohmann::ordered_json runPostutil(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPostutil.known,
                                            kVocabPostutil.unimplemented))
    return ojson(*err);
  const std::vector<std::string> toks = splitWs(parsed.argText);
  if (toks.size() == 1 && toks[0] == "clear") {
    std::vector<std::string> handles;
    for (const auto& kv : session.postfiles) handles.push_back(kv.first);
    for (const auto& h : handles) {
      ojson r = runPostclose(h, session);
      if (const ojson* e = firstError(r)) return *e;
    }
    return ojson::array();
  }
  if (toks.size() == 1 && toks[0] == "dir") {
    // Stata lists the most recently opened first (envp1 pu_dir2: hb then ha);
    // the store does not record opening order, so the names come in its own
    // (name) order. Rc and content agree; only a multi-handle order can differ.
    std::vector<std::string> names;
    for (const auto& kv : session.postfiles) names.push_back(kv.first);
    if (names.empty()) return ojson::array();
    return displayBlock(joinWith(names, "\n"));
  }
  if (toks.size() == 1 && toks[0] == "d")
    return envError("nothing found where name expected", 198);
  // KNOWN (review item 9's rc table): every other shape is the generic r(198).
  return envError("invalid syntax", 198);
}

}  // namespace piet::handlers
