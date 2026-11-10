// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/variables.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// notes
// ===========================================================================
//
// Straight onto `session.characteristics`, in Stata's own storage shape:
// `_dta[note0]` is the count and `_dta[note1]`, `_dta[note2]`, ... the texts;
// a variable's notes live under its own name. That shape is what makes
// persistence FREE -- `save f.dta` carries the char table verbatim through the
// export-dta sidecar and `use` restores it, with no code in this file.

namespace {

std::string charGet(SessionState& session, const std::string& var, const std::string& name) {
  for (const auto& c : session.characteristics)
    if (c.varname == var && c.charname == name) return c.value;
  return std::string();
}

void charSet(SessionState& session, const std::string& var, const std::string& name,
             const std::string& value) {
  for (auto& c : session.characteristics) {
    if (c.varname == var && c.charname == name) {
      c.value = value;
      return;
    }
  }
  session.characteristics.push_back(DtaCharacteristic{var, name, value});
}

int noteCount(SessionState& session, const std::string& var) {
  const std::string s = charGet(session, var, "note0");
  long long v = 0;
  if (s.empty() || !parseStrictInt(s, &v) || v < 0) return 0;
  return static_cast<int>(v);
}

// [D] notes: the word TS in a note's text is replaced by the date and time
// (`notes: ..., TS` -> `..., 21 Oct 2026 14:05`; w2ao_label_family lb_notes_ts).
std::string noteTimestamp(const std::string& text) {
  static const std::regex reTs(R"((^|\s)TS(?=\s|$))");
  if (!std::regex_search(text, reTs)) return text;
  const std::time_t now = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%e %b %Y %H:%M", &tmv);
  std::string out;
  std::string rest = text;
  std::smatch m;
  while (std::regex_search(rest, m, reTs)) {
    out += m.prefix().str() + m.str(1) + buf;
    rest = m.suffix().str();
  }
  return out + rest;
}

void noteAdd(SessionState& session, const std::string& var, const std::string& textIn) {
  const std::string text = noteTimestamp(textIn);
  const int n = noteCount(session, var) + 1;
  charSet(session, var, "note" + std::to_string(n), text);
  charSet(session, var, "note0", std::to_string(n));
}

void noteDropAll(SessionState& session, const std::string& var) {
  const int n = noteCount(session, var);
  auto& v = session.characteristics;
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const DtaCharacteristic& c) {
                           if (c.varname != var) return false;
                           if (c.charname == "note0") return true;
                           for (int i = 1; i <= n; ++i)
                             if (c.charname == "note" + std::to_string(i)) return true;
                           return false;
                         }),
          v.end());
}

std::string renderNotes(SessionState& session, const std::vector<std::string>& targets) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& tgt : targets) {
    const int n = noteCount(session, tgt);
    if (n == 0) continue;
    if (!first) oss << "\n";
    first = false;
    oss << tgt << ":\n";
    for (int i = 1; i <= n; ++i) {
      oss << "  " << i << ". " << charGet(session, tgt, "note" + std::to_string(i)) << "\n";
    }
  }
  return oss.str();
}

}  // namespace

ojson runNotes(const eval::ParsedCommand& parsed, SessionState& session) {
  // NOT `parsed.opts`: everything after a `notes:` colon is note TEXT, and the
  // tokenizer's comma rule would carve an option list out of the user's own
  // sentence. The raw line is the only faithful source here.
  static const std::regex reHead(R"(^\s*(?:notes?)\s*(:)?\s*([\s\S]*)$)", std::regex::icase);
  std::smatch m;
  if (!std::regex_search(parsed.raw, m, reHead))
    return rsSyntax("notes: syntax is `notes[ varname]: text`, `notes list [varlist]` or "
                    "`notes drop _dta|varname`");
  const bool colonOnHead = m[1].matched;
  std::string rest = trimOf(m.str(2));

  if (colonOnHead) {
    if (rest.empty()) return rsSyntax("notes: `notes:` needs some text after the colon");
    noteAdd(session, "_dta", rest);
    return silent();
  }

  auto allTargets = [&]() {
    std::vector<std::string> targets{"_dta"};
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr)
      for (const auto& nm : ds->names()) targets.push_back(nm);
    return targets;
  };

  if (rest.empty()) {
    const std::string text = renderNotes(session, allTargets());
    // Nothing recorded prints nothing, not a blank line.
    if (text.empty()) return silent();
    return displayBlock(text);
  }

  const std::vector<std::string> words = splitWs(rest);
  const std::string verb = lowerOf(words[0]);

  if (verb == "list") {
    std::vector<std::string> targets;
    if (words.size() == 1) {
      targets = allTargets();
    } else {
      for (size_t i = 1; i < words.size(); ++i) targets.push_back(words[i]);
    }
    const std::string text = renderNotes(session, targets);
    if (text.empty()) return silent();
    return displayBlock(text);
  }

  if (verb == "drop") {
    if (words.size() < 2)
      return rsSyntax("notes drop: name what to drop from (`notes drop _dta` or `notes drop "
                      "varname`)");
    // `notes drop varname in 3` drops ONE numbered note; this build drops all
    // of a target's notes or none, so the numbered form is refused by name
    // rather than quietly widened into "drop everything".
    for (size_t i = 1; i < words.size(); ++i)
      if (lowerOf(words[i]) == "in")
        return ojson(eval::unsupportedOption(
            "in", "`notes drop ... in #` (dropping one numbered note) is not implemented; this "
                  "build drops every note on a target."));
    for (size_t i = 1; i < words.size(); ++i) noteDropAll(session, words[i]);
    return silent();
  }

  if (verb == "replace" || verb == "search" || verb == "renumber") {
    return ojson(eval::unsupportedCommand("notes " + verb, std::nullopt, std::nullopt,
                                          "notes_" + verb,
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // `notes varname: text` — the colon may be glued to the name or stand alone.
  const size_t colon = rest.find(':');
  if (colon == std::string::npos)
    return rsSyntax("notes: syntax is `notes[ varname]: text`, `notes list [varlist]` or "
                    "`notes drop _dta|varname`");
  const std::string target = trimOf(rest.substr(0, colon));
  const std::string text = trimOf(rest.substr(colon + 1));
  if (target.empty()) {
    if (text.empty()) return rsSyntax("notes: `notes:` needs some text after the colon");
    noteAdd(session, "_dta", text);
    return silent();
  }
  if (splitWs(target).size() != 1)
    return rsSyntax("notes: `" + target + "` — one variable name (or `_dta`) before the colon");
  // An empty text after `varname:` is rc 0 and adds nothing (w2ao_label_family
  // lb_note_empty `note h_seq:` rc 0, note count unchanged).
  if (text.empty()) return silent();
  if (target != "_dta") {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (!ds->has(target)) return rsError("variable " + target + " not found");
  }
  noteAdd(session, target, text);
  return silent();
}

}  // namespace piet::handlers
