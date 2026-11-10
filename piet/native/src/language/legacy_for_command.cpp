// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include "loops_internal.h"

namespace piet::loops {

using namespace loops_detail;

// Legacy `for` command (out of date since Stata 8, still accepted by Stata).
// Behaviour: every rule below is
// measured StataBE 19.5 behaviour (the manuals no longer document `for`).
namespace {

ojson forSyntaxError(const std::string& msg) {
  return ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                std::string("r(198)")));
}

ojson forRcError(const std::string& msg, int rc) {
  return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool forIsBlank(char c) { return c == ' ' || c == '\t'; }

bool forIsWordByte(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c >= 0x80; }

// Stand-in for the '(' of a bare r(name)/e(name) while the line's macros are
// expanded: those are expressions each generated command evaluates when it
// runs, not macros of the `for` line.
constexpr char kForParenHold = '\x1d';

// Expands the line's macros once, as Stata does for any command line, leaving
// bare r()/e() expressions alone. Compound quotes come back as typed.
std::optional<std::string> forExpandLine(const std::string& line, SessionState& session,
                                         ojson& err) {
  std::string held = line;
  int macroDepth = 0;
  int compound = 0;
  for (size_t i = 0; i < held.size(); ++i) {
    const char c = held[i];
    const char nx = i + 1 < held.size() ? held[i + 1] : '\0';
    if (c == '`' && nx == '"') { ++compound; ++i; continue; }
    if (c == '"' && nx == '\'' && compound > 0) { --compound; ++i; continue; }
    if (c == '`') { ++macroDepth; continue; }
    if (c == '\'' && macroDepth > 0) { --macroDepth; continue; }
    if (macroDepth == 0 && (c == 'r' || c == 'e') && nx == '(' &&
        (i == 0 || !forIsWordByte(static_cast<unsigned char>(held[i - 1])))) {
      held[i + 1] = kForParenHold;
      ++i;
    }
  }
  std::string out = held;
  if (eval::hasMacroRefs(held)) {
    eval::ExpandResult er = eval::expandMacros(held, session.eval);
    if (er.isError) {
      err = er.error;
      return std::nullopt;
    }
    out = er.typed.empty() ? er.line : er.typed;
  }
  std::replace(out.begin(), out.end(), kForParenHold, '(');
  return out;
}

// End (exclusive) of the compound-quoted span opening at `i` (s[i..i+1] is
// `" ); nesting counts. Runs to the end of `s` when unterminated.
size_t forCompoundEnd(const std::string& s, size_t i) {
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
      ++depth;
      j += 2;
    } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
      --depth;
      j += 2;
      if (depth == 0) return j;
    } else {
      ++j;
    }
  }
  return s.size();
}

// A blank-separated word of the header: `text` is the word as written,
// `value` the element it stands for (outer quotes removed), `quoted` whether
// it was a quoted string.
struct ForWord {
  std::string text;
  std::string value;
  bool quoted = false;
};

std::vector<ForWord> forHeaderWords(const std::string& s) {
  std::vector<ForWord> words;
  size_t i = 0;
  while (i < s.size()) {
    if (forIsBlank(s[i])) { ++i; continue; }
    ForWord w;
    size_t end;
    if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      end = forCompoundEnd(s, i);
      w.quoted = true;
      const size_t inner = end - i >= 4 ? end - i - 4 : 0;
      w.value = s.substr(i + 2, inner);
    } else if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      end = close == std::string::npos ? s.size() : close + 1;
      w.quoted = true;
      w.value = s.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
    } else {
      end = i;
      while (end < s.size() && !forIsBlank(s[end])) ++end;
      w.value = s.substr(i, end - i);
    }
    w.text = s.substr(i, end - i);
    words.push_back(std::move(w));
    i = end;
  }
  return words;
}

// One command of the body rebuilt the way `for` stores it: blank runs become
// one blank, a word opening with `"` runs to the next `"` and is rewritten as a
// compound-quoted string, a compound-quoted word is kept, and anything else
// runs to the next blank.
std::string forRebuildCommand(const std::string& s) {
  std::string out;
  size_t i = 0;
  auto add = [&out](const std::string& w) {
    if (!out.empty()) out += ' ';
    out += w;
  };
  while (i < s.size()) {
    if (forIsBlank(s[i])) { ++i; continue; }
    if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      const size_t end = forCompoundEnd(s, i);
      add(s.substr(i, end - i));
      i = end;
    } else if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      const std::string inner =
          s.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
      add("`\"" + inner + "\"'");
      i = close == std::string::npos ? s.size() : close + 1;
    } else {
      size_t end = i;
      while (end < s.size() && !forIsBlank(s[end])) ++end;
      add(s.substr(i, end - i));
      i = end;
    }
  }
  return out;
}

// Every non-overlapping occurrence of `from`, left to right, becomes `to`; the
// inserted text is not rescanned.
std::string forReplaceAll(const std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string out;
  size_t pos = 0;
  for (size_t hit = s.find(from); hit != std::string::npos; hit = s.find(from, pos)) {
    out.append(s, pos, hit - pos);
    out += to;
    pos = hit + from.size();
  }
  out.append(s, pos, std::string::npos);
  return out;
}

// `word` names `full` when it is a prefix of it at least `minLen` long
// (case-sensitive).
bool forAbbrev(const std::string& word, const std::string& full, size_t minLen) {
  return word.size() >= minLen && word.size() <= full.size() &&
         full.compare(0, word.size(), word) == 0;
}

struct ForOptions {
  bool dryrun = false;
  bool noheader = false;
  bool nostop = false;
};

std::optional<ojson> forParseOptions(const std::string& text, ForOptions& opts) {
  std::string own = text;
  std::string afterSecondComma;
  bool secondComma = false;
  const long long c2 = eval::findUnquotedComma(text);
  if (c2 >= 0) {
    own = text.substr(0, static_cast<size_t>(c2));
    afterSecondComma = text.substr(static_cast<size_t>(c2) + 1);
    secondComma = true;
  }
  for (const ForWord& w : forHeaderWords(own)) {
    const std::string& word = w.text;
    const size_t paren = word.find('(');
    if (paren != std::string::npos) {
      return forSyntaxError("option " + word.substr(0, paren) + "() not allowed");
    }
    if (forAbbrev(word, "dryrun", 1)) opts.dryrun = true;
    else if (forAbbrev(word, "noheader", 3)) opts.noheader = true;
    else if (forAbbrev(word, "nostop", 3)) opts.nostop = true;
    else if (forAbbrev(word, "pause", 1)) {
      // Pauses only under `pause on`; nothing to do here.
    } else {
      return forSyntaxError("option " + word + " not allowed");
    }
  }
  if (secondComma) {
    const std::vector<ForWord> rest = forHeaderWords(afterSecondComma);
    if (rest.empty()) return forSyntaxError("invalid syntax");
    return forSyntaxError("invalid '" + rest.front().text + "' ");
  }
  return std::nullopt;
}

// Elements of one `newlist` list: a numbered range stubA-stubB expands to the
// names in between; every name must be valid and not yet a variable.
std::optional<ojson> forNewlistElements(const std::vector<ForWord>& words, SessionState& session,
                                        std::vector<std::string>& out) {
  std::vector<std::string> existing;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& v : *card) existing.push_back(v.name);
  } else if (Datastore* ds = session.datastore()) {
    existing = ds->names();
  }
  auto splitNumbered = [](const std::string& name, std::string& stub, std::string& digits) {
    size_t k = name.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(name[k - 1])) != 0) --k;
    stub = name.substr(0, k);
    digits = name.substr(k);
    return !stub.empty() && !digits.empty();
  };
  std::vector<std::string> names;
  for (const ForWord& w : words) {
    const std::string& tok = w.text;
    const size_t dash = tok.find('-');
    if (dash != std::string::npos && dash > 0 && dash + 1 < tok.size() &&
        tok.find('-', dash + 1) == std::string::npos) {
      const std::string lo = tok.substr(0, dash), hi = tok.substr(dash + 1);
      std::string s1, d1, s2, d2;
      if (strutil::isValidStataIdentifier(lo) && strutil::isValidStataIdentifier(hi) &&
          splitNumbered(lo, s1, d1) && splitNumbered(hi, s2, d2) && s1 == s2 &&
          d1.size() < 10 && d2.size() < 10) {
        const long n1 = std::stol(d1), n2 = std::stol(d2);
        const long step = n1 <= n2 ? 1 : -1;
        for (long n = n1;; n += step) {
          names.push_back(s1 + std::to_string(n));
          if (n == n2) break;
        }
        continue;
      }
    }
    names.push_back(tok);
  }
  for (const std::string& n : names) {
    if (!strutil::isValidStataIdentifier(n) || n.size() > 32) {
      return forSyntaxError(n + " invalid name");
    }
    if (std::find(existing.begin(), existing.end(), n) != existing.end()) {
      return forRcError("variable " + n + " already defined", 110);
    }
  }
  out = std::move(names);
  return std::nullopt;
}

// Elements of one `varlist` list: existing variables, wildcards and ranges
// expanded as in any varlist.
std::optional<ojson> forVarlistElements(const std::vector<ForWord>& words, SessionState& session,
                                        std::vector<std::string>& out) {
  if (words.empty()) {
    out.clear();
    return std::nullopt;
  }
  std::string spec;
  for (const ForWord& w : words) {
    if (!spec.empty()) spec += ' ';
    spec += w.text;
  }
  VarlistExpandResult ex = expandVarlistImpl(spec, session);
  if (ex.isError) {
    // A range whose end is not a variable: name the missing end.
    std::vector<std::string> known;
    if (Datastore* ds = session.datastore()) known = ds->names();
    for (const ForWord& w : words) {
      const size_t dash = w.text.find('-');
      if (dash == std::string::npos) continue;
      for (const std::string& end : {w.text.substr(0, dash), w.text.substr(dash + 1)}) {
        if (!end.empty() && std::find(known.begin(), known.end(), end) == known.end())
          return forRcError("variable " + end + " not found", 111);
      }
    }
    if (eval::coarseRc(ex.error) == "111") return ex.error;
    return forRcError(ex.error.value("message", std::string("invalid varlist")), 111);
  }
  if (auto bad = checkKnownVarTokens(ex.tokens, session)) return bad;
  out = std::move(ex.tokens);
  return std::nullopt;
}

struct ForList {
  std::string name;
  std::vector<std::string> elements;
};

}  // namespace

bool isLegacyForLine(const std::string& line) {
  const std::string t = trim(line);
  if (t.compare(0, 3, "for") != 0) return false;
  return t.size() == 3 || forIsBlank(t[3]) || t[3] == ':';
}

ojson runLegacyFor(const std::string& rawLine, SessionState& session,
                   const DispatchFn& dispatchFn) {
  ojson out = ojson::array();
  try {
    // 1. The line's macros, once, before `for` reads it.
    ojson expandErr;
    const std::optional<std::string> expanded = forExpandLine(trim(rawLine), session, expandErr);
    if (!expanded) {
      out.push_back(expandErr);
      return out;
    }
    const std::string rest = expanded->size() > 3 ? expanded->substr(3) : std::string();

    // 2. Header : body, at the first colon outside quotes.
    const std::vector<bool> qmask = eval::quotedSpanMask(rest);
    size_t colon = std::string::npos;
    for (size_t i = 0; i < rest.size(); ++i) {
      if (rest[i] == ':' && !qmask[i]) {
        colon = i;
        break;
      }
    }
    if (colon == std::string::npos || trim(rest.substr(0, colon)).empty()) {
      out.push_back(forSyntaxError("invalid syntax"));
      return out;
    }
    const std::string header = rest.substr(0, colon);
    const std::string body = rest.substr(colon + 1);

    // 3. Options.
    std::string listText = header;
    ForOptions opts;
    const long long comma = eval::findUnquotedComma(header);
    if (comma >= 0) {
      listText = header.substr(0, static_cast<size_t>(comma));
      if (auto bad = forParseOptions(header.substr(static_cast<size_t>(comma) + 1), opts)) {
        out.push_back(*bad);
        return out;
      }
    }

    // 4. Lists: segments separated by a `\` that stands as its own word.
    std::vector<std::vector<ForWord>> segments(1);
    for (ForWord& w : forHeaderWords(listText)) {
      if (!w.quoted && w.text == "\\") {
        segments.emplace_back();
        continue;
      }
      segments.back().push_back(std::move(w));
    }
    segments.erase(std::remove_if(segments.begin(), segments.end(),
                                  [](const std::vector<ForWord>& s) { return s.empty(); }),
                   segments.end());
    static const char* const kDefaultNames[] = {"X", "Y", "Z", "A", "B", "C", "D", "E", "F"};
    if (segments.size() > 9) {
      out.push_back(forSyntaxError("too many lists"));
      return out;
    }
    std::vector<ForList> lists;
    for (size_t k = 0; k < segments.size(); ++k) {
      const std::vector<ForWord>& seg = segments[k];
      ForList list;
      size_t typeAt = 0;
      if (seg.size() >= 2 && !seg[1].quoted && seg[1].text == "in") {
        if (seg.size() < 3) {
          out.push_back(forSyntaxError("invalid syntax"));
          return out;
        }
        list.name = seg[0].text;
        typeAt = 2;
      } else {
        list.name = kDefaultNames[k];
      }
      const std::string& type = seg[typeAt].text;
      const std::vector<ForWord> elems(seg.begin() + static_cast<long>(typeAt) + 1, seg.end());
      if (forAbbrev(type, "varlist", 3)) {
        if (auto bad = forVarlistElements(elems, session, list.elements)) {
          out.push_back(*bad);
          return out;
        }
      } else if (forAbbrev(type, "numlist", 3)) {
        if (!elems.empty()) {
          std::string spec;
          for (const ForWord& w : elems) spec += w.text + " ";
          NumlistResult nl = parseNumlist(spec);
          if (nl.isError) {
            out.push_back(forRcError("invalid numlist", 121));
            return out;
          }
          list.elements = std::move(nl.tokens);
        }
      } else if (forAbbrev(type, "newlist", 3)) {
        if (auto bad = forNewlistElements(elems, session, list.elements)) {
          out.push_back(*bad);
          return out;
        }
      } else if (type == "any") {
        for (const ForWord& w : elems) list.elements.push_back(w.value);
      } else {
        out.push_back(forSyntaxError("\"" + type + "\" invalid list type"));
        return out;
      }
      lists.push_back(std::move(list));
    }
    for (const ForList& l : lists) {
      if (l.elements.size() != lists.front().elements.size()) {
        out.push_back(forSyntaxError("lists have unequal number of elements"));
        return out;
      }
    }

    // 5. Every placeholder must occur in the commands; each list's first
    // element stands in for its placeholder before the next list is looked for.
    {
      std::string probe = body;
      for (const ForList& l : lists) {
        if (probe.find(l.name) == std::string::npos) {
          out.push_back(forSyntaxError(l.name + " not in command"));
          return out;
        }
        if (!l.elements.empty()) probe = forReplaceAll(probe, l.name, l.elements.front());
      }
    }

    // 6. Commands: split at every `\` outside quotes, then rebuilt.
    std::vector<std::string> commands;
    {
      const std::vector<bool> bmask = eval::quotedSpanMask(body);
      size_t from = 0;
      for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || (body[i] == '\\' && !bmask[i])) {
          commands.push_back(forRebuildCommand(body.substr(from, i - from)));
          from = i + 1;
        }
      }
    }

    // 7. Iterate. The generated commands run in a local scope of their own,
    // as the body of any program does: a `local` they set is gone afterwards
    // (StataBE: `local i 0` then `for num 1 2 3: local i = `i' + X` leaves i
    // at 0, while the same with a global leaves it at 3).
    const size_t n = lists.empty() ? 0 : lists.front().elements.size();
    doexec::PrivateLocalScope bodyScope(session);
    for (size_t it = 0; it < n; ++it) {
      for (const std::string& cmdTemplate : commands) {
        std::string cmd = cmdTemplate;
        for (const ForList& l : lists) cmd = forReplaceAll(cmd, l.name, l.elements[it]);
        if (!opts.noheader) {
          ojson hdr;
          hdr["type"] = "display";
          hdr["text"] = cmd.empty() ? std::string("-> ") : "->  " + cmd;
          out.push_back(std::move(hdr));
        }
        if (opts.dryrun || cmd.empty()) continue;
        ojson res;
        try {
          res = dispatchFn(cmd, session);
        } catch (const std::exception& e) {
          res = ojson(eval::engineError(e.what()));
        }
        if (!res.is_array()) res = ojson::array({res});
        // A failure is the command's first uncaptured error block. Without
        // nostop it goes out as is and the loop ends there; with nostop it is
        // shown as captured (an `exit N` halt has no message to show), then
        // `->  r(N); `, and the loop goes on.
        static const std::regex reExitHalt(R"(^exit(\s+-?\d+)?$)");
        std::optional<std::string> failedRc;
        for (const auto& b : res) {
          const std::string t = b.value("type", std::string());
          if (t == "echo") continue;
          if (!failedRc && t == "error" && !b.value("captured", false)) {
            failedRc = eval::coarseRc(b);
            if (!opts.nostop) {
              out.push_back(b);
              return out;
            }
            if (std::regex_match(b.value("message", std::string()), reExitHalt)) continue;
            ojson shown = b;
            shown["captured"] = true;
            out.push_back(std::move(shown));
            continue;
          }
          out.push_back(b);
        }
        if (!failedRc) continue;
        ojson rcLine;
        rcLine["type"] = "display";
        rcLine["text"] = "->  r(" + *failedRc + "); ";
        out.push_back(std::move(rcLine));
      }
    }
  } catch (const std::exception& e) {
    out.push_back(ojson(eval::engineError(std::string("for: ") + e.what())));
  } catch (...) {
    out.push_back(ojson(eval::engineError("for: internal error")));
  }
  return out;
}

}  // namespace piet::loops
