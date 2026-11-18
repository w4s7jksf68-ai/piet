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
#include "piet/loops.h"  // expandVarlist
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

std::string joinWs(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& x : v) out += (out.empty() ? "" : " ") + x;
  return out;
}

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

bool noteExists(SessionState& session, const std::string& var, int i) {
  for (const auto& c : session.characteristics)
    if (c.varname == var && c.charname == "note" + std::to_string(i)) return true;
  return false;
}

void charErase(SessionState& session, const std::string& var, const std::string& name) {
  auto& v = session.characteristics;
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const DtaCharacteristic& c) { return c.varname == var && c.charname == name; }),
          v.end());
}

// The highest note number still present (note0 afterwards), 0 when none is.
void noteSetLast(SessionState& session, const std::string& var, int upTo) {
  int last = 0;
  for (int i = upTo; i >= 1; --i)
    if (noteExists(session, var, i)) {
      last = i;
      break;
    }
  if (last > 0) charSet(session, var, "note0", std::to_string(last));
  else charErase(session, var, "note0");
}

// notes drop var [in range]: the notes i1..i2 that exist; returns how many went.
int noteDropRange(SessionState& session, const std::string& var, int i1, int i2) {
  const int n = noteCount(session, var);
  if (n == 0 && charGet(session, var, "note0").empty()) return 0;
  const int hi = std::min(i2, n);
  const int lo = i1 == 10000 ? n : i1;
  int dropped = 0;
  for (int i = lo; i <= hi; ++i)
    if (noteExists(session, var, i)) {
      charErase(session, var, "note" + std::to_string(i));
      ++dropped;
    }
  noteSetLast(session, var, n);
  return dropped;
}

// notes renumber: the notes that remain become 1..k.
void noteRenumber(SessionState& session, const std::string& var) {
  const int n = noteCount(session, var);
  if (n <= 0) return;
  std::vector<std::string> texts;
  for (int i = 1; i <= n; ++i)
    if (noteExists(session, var, i)) texts.push_back(charGet(session, var, "note" + std::to_string(i)));
  for (int i = 1; i <= n; ++i) charErase(session, var, "note" + std::to_string(i));
  for (size_t i = 0; i < texts.size(); ++i) charSet(session, var, "note" + std::to_string(i + 1), texts[i]);
  if (texts.empty()) charErase(session, var, "note0");
  else charSet(session, var, "note0", std::to_string(texts.size()));
}

// `in` of notes drop/list: `#`, `#/#`, with f and l for the first and last note.
bool noteParseIn(const std::vector<std::string>& words, int* i1, int* i2, std::string* why) {
  std::string joined;
  for (const auto& w : words) joined += w;
  auto el = [&](const std::string& t, int* out) {
    if (t == "f" || t == "F") {
      *out = 1;
      return true;
    }
    if (t == "l" || t == "L") {
      *out = 10000;
      return true;
    }
    long long v = 0;
    if (parseStrictInt(t, &v) && v >= 1 && v <= 9999) {
      *out = static_cast<int>(v);
      return true;
    }
    *why = t + ":  invalid range element";
    return false;
  };
  const size_t slash = joined.find('/');
  if (slash == std::string::npos) {
    if (!el(joined, i1)) return false;
    *i2 = *i1;
    return true;
  }
  if (!el(joined.substr(0, slash), i1) || !el(joined.substr(slash + 1), i2)) return false;
  if (*i1 > *i2) {
    *why = "in " + joined + ":  invalid";
    return false;
  }
  return true;
}

// A note as `notes list` sets it: the number and its text aligned at column 7, the text wrapped
// at the line width with the same indent.
std::string renderNote(int i, const std::string& text) {
  const std::string num = std::to_string(i) + ".";
  const size_t k = std::to_string(i).size();
  const size_t p0 = k < 3 ? 3 - k : 0;
  const size_t cont = k > 3 ? 7 : 6;
  std::string line = std::string(p0, ' ') + num;
  std::string out;
  bool first = true;
  size_t col = line.size();
  std::string cur = line;
  auto flush = [&]() {
    out += (out.empty() ? "" : "\n") + cur;
  };
  // pad to the text column
  cur += std::string(cont > cur.size() ? cont - cur.size() : 1, ' ');
  col = cur.size();
  std::istringstream words(text);
  std::string w;
  bool lineHasWord = false;
  while (words >> w) {
    if (lineHasWord && col + 1 + w.size() > 80) {
      flush();
      cur = std::string(cont, ' ');
      col = cur.size();
      lineHasWord = false;
    }
    if (lineHasWord) {
      cur += " ";
      ++col;
    }
    cur += w;
    col += w.size();
    lineHasWord = true;
  }
  (void)first;
  flush();
  return out;
}

std::string renderNotes(SessionState& session, const std::vector<std::string>& targets, int i1 = 1,
                        int i2 = 9999) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& tgt : targets) {
    const int n = std::min(noteCount(session, tgt), i2);
    int j = -1;
    // `l` starts a LIST range past every note (it only means "the last note" for notes drop), so a
    // range that starts at l prints nothing.
    for (int i = i1; i <= n; ++i)
      if (noteExists(session, tgt, i)) {
        j = i;
        break;
      }
    if (j < 0) continue;
    oss << (first ? "" : "\n") << "\n" << tgt << ":";
    first = false;
    for (int i = j; i <= n; ++i)
      if (noteExists(session, tgt, i)) oss << "\n" << renderNote(i, charGet(session, tgt, "note" + std::to_string(i)));
  }
  return oss.str();
}

}  // namespace

// `char [define] VARNAME[CHARNAME] [text]` (also `_dta[...]`): sets a characteristic; an empty text
// deletes it. A dataset with a characteristic edited is changed (vs-stata bank: `char price[x] 1`
// before findname, describe). `char list` and `char rename` are not done by this build.
ojson runChar(const eval::ParsedCommand& parsed, SessionState& session) {
  std::string rest = strutil::trim(parsed.raw);
  {
    const size_t sp = rest.find_first_of(" \t");
    rest = sp == std::string::npos ? std::string() : strutil::trim(rest.substr(sp));
  }
  {
    const size_t sp = rest.find_first_of(" \t");
    const std::string first = strutil::lower(rest.substr(0, sp));
    if (first == "define")
      rest = sp == std::string::npos ? std::string() : strutil::trim(rest.substr(sp));
    else if (first == "list" || first == "l" || first == "li" || first == "lis") {
      // `char list [VARNAME[]]`: each variable's name, then its characteristics.
      std::string target;
      if (sp != std::string::npos) {
        std::string arg = strutil::trim(rest.substr(sp));
        if (arg.size() >= 2 && arg.compare(arg.size() - 2, 2, "[]") == 0) arg = arg.substr(0, arg.size() - 2);
        target = arg;
      }
      std::vector<std::string> order;
      for (const auto& c : session.characteristics)
        if (std::find(order.begin(), order.end(), c.varname) == order.end()) order.push_back(c.varname);
      std::string text;
      for (const auto& v : order) {
        if (!target.empty() && v != target) continue;
        text += (text.empty() ? "" : "\n") + v + ":";
        for (const auto& c : session.characteristics) {
          if (c.varname != v) continue;
          std::string label = "  " + v + "[" + c.charname + "]:";
          if (label.size() < 30) label.append(30 - label.size(), ' ');
          text += "\n" + label + c.value;
        }
      }
      if (text.empty()) return ojson::array();
      ojson out;
      out["type"] = "display";
      out["text"] = text;
      return out;
    } else if (first == "rename" || first == "ren" || first == "rena" || first == "renam")
      return ojson(eval::unsupportedOption("char " + first,
                                           "this build defines, lists and deletes characteristics only"));
  }
  static const std::regex reHead(R"(^([A-Za-z_][A-Za-z0-9_]*)\[([^\]]*)\](.*)$)");
  std::smatch m;
  if (!std::regex_match(rest, m, reHead))
    return ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt, std::nullopt,
                                  std::nullopt, "r(198)"));
  const std::string var = m.str(1);
  const std::string cname = strutil::trim(m.str(2));
  std::string text = strutil::trim(m.str(3));
  if (text.size() >= 4 && text.compare(0, 2, "`\"") == 0 && text.compare(text.size() - 2, 2, "\"'") == 0)
    text = text.substr(2, text.size() - 4);
  else if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    text = text.substr(1, text.size() - 2);
  if (var != "_dta") {
    Datastore* ds = session.hasData() ? session.datastore() : nullptr;
    if (ds == nullptr || !ds->has(var))
      return ojson(eval::errorBlock("engine_error", "variable " + var + " not found", std::nullopt,
                                    std::nullopt, std::nullopt, "r(111)"));
  }
  if (cname.empty() || !strutil::isValidStataIdentifier(cname))
    return ojson(eval::errorBlock("syntax_error", "invalid name", std::nullopt, std::nullopt,
                                  std::nullopt, "r(198)"));
  auto& chars = session.characteristics;
  if (text.empty()) {
    chars.erase(std::remove_if(chars.begin(), chars.end(),
                               [&](const DtaCharacteristic& c) {
                                 return c.varname == var && c.charname == cname;
                               }),
                chars.end());
  } else {
    charSet(session, var, cname, text);
  }
  session.datasetChanged = true;
  return ojson::array();
}


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
    if (rest.empty()) return displayBlock("  (no note added)");
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

  // `notes [list] [varlist] [in range]` and `notes drop [varlist] [in range]`: the words up to `in` name
  // the targets (`_dta`, `_all`, variables), the rest is the range of note numbers.
  auto splitTargets = [&](size_t from, std::vector<std::string>* names, int* i1, int* i2,
                          ojson* err) -> bool {
    size_t inAt = words.size();
    for (size_t i = from; i < words.size(); ++i)
      if (lowerOf(words[i]) == "in") {
        inAt = i;
        break;
      }
    *i1 = 1;
    *i2 = 9999;
    if (inAt < words.size()) {
      std::string why;
      const std::vector<std::string> rng(words.begin() + static_cast<long>(inAt) + 1, words.end());
      if (rng.empty() || !noteParseIn(rng, i1, i2, &why)) {
        *err = rsErrorRc(why.empty() ? "in : invalid" : why, 198);
        return false;
      }
    }
    bool hasDta = false;
    std::vector<std::string> vars;
    std::vector<std::string> others;  // the list words that are not _dta
    for (size_t i = from; i < inAt; ++i) {
      if (words[i] == "_dta") {
        hasDta = true;
        continue;
      }
      others.push_back(words[i]);
      // `_all` is every variable; with no data in memory it names none.
      if (words[i] != "_all" || session.hasData()) vars.push_back(words[i]);
    }
    // A list that is `_dta`, or only `_all`, also holds the dataset's own notes -- after the variables.
    const bool addDta = hasDta || (others.size() == 1 && others[0] == "_all");
    std::vector<std::string> expanded;
    if (!vars.empty()) {
      if (!session.hasData()) {
        *err = ojson(eval::noDataLoaded());
        return false;
      }
      loops::VarlistExpandResult ex = loops::expandVarlist(joinWs(vars), session);
      if (ex.isError) {
        *err = ex.error;
        return false;
      }
      // A name that is no variable is r(111) "variable NAME not found"; a unique prefix of
      // one is that variable, a prefix of several is ambiguous (also r(111)).
      Datastore* vds = session.datastore();
      for (const auto& tok : ex.tokens) {
        if (vds == nullptr || vds->has(tok)) {
          expanded.push_back(tok);
          continue;
        }
        std::vector<std::string> hits;
        for (const auto& nm : vds->names())
          if (!tok.empty() && nm.compare(0, tok.size(), tok) == 0) hits.push_back(nm);
        if (hits.size() == 1) {
          expanded.push_back(hits[0]);
          continue;
        }
        *err = ojson(eval::errorBlock(
            "engine_error",
            hits.empty() ? "variable " + tok + " not found" : tok + " ambiguous abbreviation",
            std::nullopt, std::nullopt, std::nullopt, "r(111)"));
        return false;
      }
    }
    names->clear();
    for (const auto& v : expanded)
      if (std::find(names->begin(), names->end(), v) == names->end()) names->push_back(v);
    if (addDta) names->push_back("_dta");
    return true;
  };

  if (verb == "list") {
    std::vector<std::string> targets;
    int i1 = 1, i2 = 9999;
    ojson err;
    if (!splitTargets(1, &targets, &i1, &i2, &err)) return err;
    if (targets.empty()) targets = allTargets();
    const std::string text = renderNotes(session, targets, i1, i2);
    if (text.empty()) return silent();
    return displayBlock(text);
  }

  if (verb == "drop") {
    std::vector<std::string> targets;
    int i1 = 1, i2 = 9999;
    ojson err;
    if (!splitTargets(1, &targets, &i1, &i2, &err)) return err;
    // a target list is required (error 198)
    if (targets.empty()) return rsErrorRc("varlist required", 198);
    int dropped = 0;
    for (const auto& t : targets) dropped += noteDropRange(session, t, i1, i2);
    return displayBlock("  (" + std::to_string(dropped) + (dropped == 1 ? " note" : " notes") + " dropped)");
  }

  if (verb == "renumber") {
    if (words.size() < 2)
      return rsErrorRc("nothing found where varname or _dta expected", 198);
    if (words.size() > 2) return rsErrorRc(words[2] + " found where nothing expected", 198);
    const std::string& w = words[1];
    if (w != "_dta") {
      Datastore* ds = nullptr;
      try {
        ds = session.datastore();
      } catch (const std::exception&) {
        ds = nullptr;
      }
      // `syntax varname`: a unique abbreviation names the variable.
      std::string full = w;
      if (ds != nullptr && !ds->has(w)) {
        bool ambiguous = false;
        std::optional<std::string> hit = resolveVarName(w, session, &ambiguous);
        if (hit) full = *hit;
      }
      if (ds == nullptr || !ds->has(full))
        return rsErrorRc(w + " found where varname or _dta expected", 111);
      noteRenumber(session, full);
      return silent();
    }
    noteRenumber(session, w);
    return silent();
  }

  if (verb == "replace" || verb == "search") {
    return ojson(eval::unsupportedCommand("notes " + verb, std::nullopt, std::nullopt,
                                          "notes_" + verb,
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // `notes varname: text` — the colon may be glued to the name or stand alone.
  const size_t colon = rest.find(':');
  // Without a colon the words are `notes [varlist]`, the listing: a name that
  // is no variable is r(111) "variable NAME not found" (vs-stata bank `notes
  // zznotasub`).
  if (colon == std::string::npos && session.hasData() && words.size() == 1 &&
      words[0] != "_dta" && words[0] != "_all" && strutil::isValidStataIdentifier(words[0])) {
    if (Datastore* nds = session.datastore()) {
      bool prefixOfAny = false;
      for (const auto& nm : nds->names())
        prefixOfAny = prefixOfAny || nm.compare(0, words[0].size(), words[0]) == 0;
      if (!prefixOfAny)
        return ojson(eval::errorBlock("engine_error", "variable " + words[0] + " not found",
                                      std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
  }
  if (colon == std::string::npos)
    return rsSyntax("notes: syntax is `notes[ varname]: text`, `notes list [varlist]` or "
                    "`notes drop _dta|varname`");
  const std::string target = trimOf(rest.substr(0, colon));
  const std::string text = trimOf(rest.substr(colon + 1));
  if (target.empty()) {
    if (text.empty()) return displayBlock("  (no note added)");
    noteAdd(session, "_dta", text);
    return silent();
  }
  if (splitWs(target).size() != 1)
    return rsSyntax("notes: `" + target + "` — one variable name (or `_dta`) before the colon");
  // An empty text after `varname:` is rc 0 and adds nothing (w2ao_label_family
  // lb_note_empty `note h_seq:` rc 0, note count unchanged).
  if (text.empty()) return displayBlock("  (no note added)");
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
