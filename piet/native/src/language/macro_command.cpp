// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
//   macro     session.eval.globals/locals (the OrderedDicts the macro layer owns);
//             `define` goes through eval::tryCaptureDefinition -- the very call
//             the dispatcher makes for a `global`/`local` line -- so a defined
//             macro is stored by the one definition path, quotes and `=` included.
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
#include "piet/handlers/session_commands.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "command_reply_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

namespace {

// ===========================================================================
// macro
// ===========================================================================
//
// Implements `macro drop|define|shift|list|dir` per macro's help file;
// identified against StataBE probes (w2ax_t1_official.log macro_* arms and the
// lane's envp1/envp2/envp3 probes).
//
// WHICH NAMES ARE NOT THE USER'S. The macro store also holds PIET's own state,
// and `macro drop _all` / `macro dir` must neither erase nor show it:
//   * a global whose name starts with `__piet` (texdoc's open path, the
//     frmttable stores -- tables/table_options.cpp);
//   * the local `_rc` (the dispatcher keeps the return code there);
//   * any name that is not an identifier (program.cpp's " __piet_progclass"
//     marker, the reserved \x01 keys).
// And one class Stata itself keeps: `macro drop _all` leaves every global
// whose name starts with `S_` (envp3: `global S_mine mine`, `macro drop _all`,
// `$S_mine` is still `mine`; envp2: S_ADO/S_OS survive while F1..F8 go).
bool internalMacroName(const std::string& n, bool isLocal) {
  if (n.empty()) return true;
  const unsigned char c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalnum(c0) || c0 == '_')) return true;
  if (n.rfind("__piet", 0) == 0) return true;
  if (isLocal && n == "_rc") return true;
  return false;
}

// A global's name: Stata's identifier rule, at most 32 characters.
bool validGlobalName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  const unsigned char c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

// A local's name (after the `_` that marks it): identifier characters, digits
// first allowed (`_1` is the positional local 1), at most 31 characters.
bool validLocalName(const std::string& n) {
  if (n.empty() || n.size() > 31) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

bool validPattern(const std::string& p) {
  if (p.empty()) return false;
  for (unsigned char c : p)
    if (!(std::isalnum(c) || c == '_' || c == '*' || c == '?' || c == '~')) return false;
  return true;
}

// `macro list` / `macro dir` line: "NAME:" padded to column 16, then the
// value; a label of 16 or more characters puts the value on the next line,
// indented 16 (envp1: `abcdefghijklmn: x14`, `abcdefghijklmno:` then
// 16 blanks + `x15`; oracle macro_list `t1LST:          hello world`).
std::string listLine(const std::string& label, const std::string& value) {
  const std::string head = label + ":";
  if (head.size() <= 15) return head + std::string(16 - head.size(), ' ') + value;
  return head + "\n" + std::string(16, ' ') + value;
}

ojson macroDrop(const std::vector<std::string>& toks, SessionState& session) {
  if (toks.empty()) return envError("invalid syntax", 198);
  // Validate every name before touching the store, so a bad name late in the
  // list does not leave the earlier ones half-dropped.
  for (const auto& t : toks) {
    if (t == "_all") continue;
    const bool isLocal = t.size() > 1 && t[0] == '_';
    const std::string bare = isLocal ? t.substr(1) : t;
    const bool wild = bare.find_first_of("*?~") != std::string::npos;
    const bool ok = wild ? validPattern(bare)
                         : (isLocal ? validLocalName(bare) : validGlobalName(bare));
    if (!ok) return envError(t + " invalid name", 198);
  }
  auto dropMatching = [&](eval::OrderedDict& store, const std::string& pattern, bool isLocal) {
    std::vector<std::string> names;
    for (const auto& kv : store.items())
      if (!internalMacroName(kv.first, isLocal) &&
          strutil::globMatch(pattern == "~" ? "*" : pattern, kv.first))
        names.push_back(kv.first);
    for (const auto& n : names) store.erase(n);
  };
  for (const auto& t : toks) {
    if (t == "_all") {
      // envp2/envp3 + oracle macro_drop_all: every user global (but `S_*`)
      // AND every local of the current scope. KNOWN (review m3): inside a
      // foreach body that includes the loop's own macro, which can end PIET's
      // loop early where Stata's runs on.
      std::vector<std::string> g;
      for (const auto& kv : session.eval.globals.items())
        if (!internalMacroName(kv.first, false) && kv.first.rfind("S_", 0) != 0)
          g.push_back(kv.first);
      for (const auto& n : g) session.eval.globals.erase(n);
      dropMatching(session.eval.locals, "*", true);
      continue;
    }
    const bool isLocal = t.size() > 1 && t[0] == '_';
    const std::string bare = isLocal ? t.substr(1) : t;
    eval::OrderedDict& store = isLocal ? session.eval.locals : session.eval.globals;
    if (bare.find_first_of("*?~") != std::string::npos) {
      dropMatching(store, bare, isLocal);
    } else if (!internalMacroName(bare, isLocal)) {
      store.erase(bare);  // a name that is not there is silent, rc 0 (macro_drop_missing)
    }
  }
  return ojson::array();
}

// `macro define NAME rest` is `global NAME rest`, and `macro define _NAME rest`
// is `local NAME rest` (envp1 mdef_local: `3 + 4` stored as text; mdef_eq:
// `= 3 + 4` evaluates to 7). The rewritten line goes through the dispatcher's
// own definition path, so quoting, compound quotes and `=` behave exactly as
// they do for `global`/`local` (macro_define: `"x y"` stores x y).
ojson macroDefine(const std::string& restRaw, SessionState& session, const std::string& line) {
  const std::string rest = strutil::trim(restRaw);
  if (rest.empty()) return envError("invalid syntax", 198);
  size_t sp = rest.find_first_of(" \t");
  const std::string name = sp == std::string::npos ? rest : rest.substr(0, sp);
  const std::string value = sp == std::string::npos ? std::string() : rest.substr(sp);
  const bool isLocal = name.size() > 1 && name[0] == '_';
  const std::string bare = isLocal ? name.substr(1) : name;
  if (!(isLocal ? validLocalName(bare) : validGlobalName(bare)))
    return envError(name + " invalid name", 198);
  const std::string defLine = (isLocal ? "local " : "global ") + bare + value;
  eval::MacroFnError mfe;
  if (!eval::tryCaptureDefinition(substituteCRefs(defLine, session), session.eval, &mfe))
    return envError("invalid syntax", 198);
  if (mfe.failed())
    return ojson(eval::errorBlock("engine_error", mfe.message, std::nullopt, std::nullopt,
                                  strutil::trim(line), "r(" + mfe.rc + ")"));
  return ojson::array();
}

// `macro shift [#]`: the numbered locals `1`, `2`, ... move down by # (default
// 1); the top # become unset and `0` is left as it was (oracle macro_shift
// `|q|r||p q r|`, macro_shift2 `|r|s||`; envp1 ms_big: shift 5 of three
// arguments clears all three). Outside a program the do-file's numbered locals
// are the ones shifted -- usually none, rc 0 (macro_shift_outside).
ojson macroShift(const std::vector<std::string>& args, SessionState& session) {
  long long k = 1;
  if (args.size() > 1) return envError("invalid syntax", 198);  // envp1 ms_two
  if (args.size() == 1) {
    const std::string& a = args[0];
    if (a.empty() || !std::all_of(a.begin(), a.end(), [](unsigned char c) { return std::isdigit(c); }))
      return envError("invalid syntax", 198);  // envp1 ms_x, ms_neg
    try {
      k = std::stoll(a);
    } catch (const std::exception&) {
      return envError("invalid syntax", 198);
    }
  }
  if (k == 0) return ojson::array();  // envp1 ms_zero
  long long top = 0;
  for (const auto& kv : session.eval.locals.items()) {
    const std::string& n = kv.first;
    if (n.empty() || n[0] == '0' || n.size() > 9) continue;
    if (!std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
    top = std::max(top, std::stoll(n));
  }
  for (long long i = 1; i <= top; ++i) {
    const std::string from = std::to_string(i + k);
    const std::string* v = (i + k <= top) ? session.eval.locals.find(from) : nullptr;
    if (v != nullptr) {
      const std::string copy = *v;
      session.eval.locals.set(std::to_string(i), copy);
    } else {
      session.eval.locals.erase(std::to_string(i));
    }
  }
  return ojson::array();
}

// `macro list [names | _all]` and `macro dir [names]` (one command: envp1
// mdir_extra, `macro dir extra` answers as `macro list extra`). Bare/_all:
// globals newest first, then locals newest first as `_name` (envp3). Stata
// keeps no empty macro, so an empty value is "not found" (envp1 ml_empty).
ojson macroList(const std::vector<std::string>& toks, SessionState& session) {
  ojson out = ojson::array();
  std::vector<std::string> lines;
  const bool all = toks.empty() || (toks.size() == 1 && toks[0] == "_all");
  if (all) {
    const auto& g = session.eval.globals.items();
    for (size_t i = g.size(); i-- > 0;)
      if (!internalMacroName(g[i].first, false) && !g[i].second.empty())
        lines.push_back(listLine(g[i].first, g[i].second));
    const auto& l = session.eval.locals.items();
    for (size_t i = l.size(); i-- > 0;)
      if (!internalMacroName(l[i].first, true) && !l[i].second.empty())
        lines.push_back(listLine("_" + l[i].first, l[i].second));
  } else {
    for (const auto& t : toks) {
      const bool isLocal = t.size() > 1 && t[0] == '_';
      const std::string bare = isLocal ? t.substr(1) : t;
      if (!(isLocal ? validLocalName(bare) : validGlobalName(bare))) {
        if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
        out.push_back(envError(t + " invalid name", 198));
        return out;
      }
      const std::string* v = internalMacroName(bare, isLocal)
                                 ? nullptr
                                 : (isLocal ? session.eval.locals.find(bare)
                                            : session.eval.globals.find(bare));
      if (v == nullptr || v->empty()) {
        // macro_list_missing: `global macro $t1nosuch not found`; envp1
        // ml_noloc: "local macro `nosuchloc' not found". Both r(111).
        if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
        out.push_back(envError(isLocal ? "local macro `" + bare + "' not found"
                                       : "global macro $" + bare + " not found",
                               111));
        return out;
      }
      lines.push_back(listLine(t, *v));
    }
  }
  if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
  return out;
}

}  // namespace

// The raw-line pre-pass (dispatcher.cpp, both sites). capture/quietly/noisily
// are already stripped. `macro` is in the tokenizer's unsupported_macro net, so
// every `macro ...` line is consumed here -- the expanded site included -- and
// the net never sees one. On the raw site a line that still carries a macro
// reference is left alone (false) and answered at the expanded site, so
// `macro drop $fh` drops the macro $fh NAMES; the test is eval::hasMacroRefs,
// the very gate the dispatcher's own expansion step uses.
bool tryMacroVerbIntercept(const std::string& line, SessionState& session, const ojson& echo,
                           ojson& out, bool expanded) {
  const size_t h = line.find_first_not_of(" \t");
  if (h == std::string::npos) return false;
  // Memchr-class gate before any allocation: the command word starts "ma".
  if (line.size() < h + 2 || std::tolower(static_cast<unsigned char>(line[h])) != 'm' ||
      std::tolower(static_cast<unsigned char>(line[h + 1])) != 'a')
    return false;
  size_t e = line.find_first_of(" \t", h);
  const std::string word = strutil::lower(line.substr(h, e == std::string::npos ? std::string::npos : e - h));
  // ma:cro (macro's help file): ma, mac, macr, macro.
  if (word != "ma" && word != "mac" && word != "macr" && word != "macro") return false;
  if (!expanded && eval::hasMacroRefs(line)) return false;

  const std::string rest = e == std::string::npos ? std::string() : line.substr(e);
  const size_t sh = rest.find_first_not_of(" \t");
  std::string sub, subRest;
  if (sh != std::string::npos) {
    const size_t se = rest.find_first_of(" \t", sh);
    sub = rest.substr(sh, se == std::string::npos ? std::string::npos : se - sh);
    subRest = se == std::string::npos ? std::string() : rest.substr(se);
  }
  const std::vector<std::string> args = splitWs(subRest);

  ojson r;
  if (sub == "drop") {
    r = macroDrop(args, session);
  } else if (sub.size() >= 2 && std::string("define").compare(0, sub.size(), sub) == 0) {
    // de:fine -- `macro de`/`def`/`defi` are rc 0 in StataBE (review m1 m5/m5b,
    // m2 b4); a single `d` is r(198) (m2 b3/b5), and `dr`/`dro` are not drop.
    r = macroDefine(subRest, session, line);
  } else if (sub == "s" || sub == "sh" || sub == "shi" || sub == "shif" || sub == "shift") {
    r = macroShift(args, session);
  } else if (sub == "l" || sub == "li" || sub == "lis" || sub == "list" || sub == "di" ||
             sub == "dir") {
    r = macroList(args, session);
  } else {
    // macro_badsub (`macro bogus t1K1`) and a bare `macro`: invalid syntax.
    r = envError("invalid syntax", 198);
  }
  if (r.is_array() && r.empty()) {
    out = ojson::array();
    return true;
  }
  out = ojson::array({echo});
  if (r.is_array()) {
    for (const auto& b : r) out.push_back(b);
  } else {
    out.push_back(r);
  }
  return true;
}

}  // namespace piet::handlers
