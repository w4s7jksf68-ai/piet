// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/bcal.h"
// Copyright (C) 2026 M.K. Kim
// Label metadata.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/variables.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

ojson runLabelVariable(const std::string& varname, const std::string& text, SessionState& session) {
  session.setVarLabel(varname, text);
  return ojson::array();  // silent success (Stata-faithful, mirrors label.py)
}

namespace command_detail {
// label_define.py's _refresh_value_labels_for_var: rebuild
// session.valueLabels[varname] (int-keyed) from session.labelDefs[lblname]
// (string-keyed) — called after both `label values` and `label define`
// (define refreshes every var currently attached to that lblname).
void refreshValueLabelsForVar(SessionState& session, const std::string& varname,
                              const std::string& lblname) {
  auto defIt = session.labelDefs.find(lblname);
  if (defIt != session.labelDefs.end() && !defIt->second.empty()) {
    std::map<int, std::string> numeric;
    for (const auto& [codeStr, text] : defIt->second) {
      try {
        numeric[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    session.valueLabels[varname] = numeric;
  } else {
    session.valueLabels.erase(varname);
  }
}
}  // namespace

namespace command_detail {
// LBL-WIDEN: attaching a DEFINED value-label set to a numeric variable widens
// its %[-][0]w.d{g,f,e}[c] display format to the longest text in the set,
// never narrows it, and leaves any other family alone (w2ao_label_family.log
// :369 lb_val_label %8.0g -> %11.0g for "Before 11AM", :687 lb_val_fixed %6.2f
// -> %29.2f, lb_val_long %36.0g -- no cap). An undefined set widens nothing
// (lb_val_undef %8.0g); `nofix` skips it (lb_val_nofix).
// BUG-1136 (FU-1133): a widened format loses its `-` and `0` flags, and one
// already wide enough keeps them (StataBE 19.5, measured 2026-11-03, a
// 17-character label: %-9.0g -> %17.0g, %-12.2f -> %17.2f, %-9.0gc -> %17.0gc,
// %09.2f -> %17.2f; %-20.0g stays %-20.0g).
void widenFormatForSet(SessionState& session, const std::string& varname,
                       const std::string& lblname) {
  auto defIt = session.labelDefs.find(lblname);
  if (defIt == session.labelDefs.end()) return;
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(varname) || ds->dtype(varname) == "factor") return;
  size_t maxLen = 0;
  for (const auto& kv : defIt->second) {
    size_t n = 0;  // UTF-8 characters
    for (unsigned char c : kv.second)
      if ((c & 0xC0) != 0x80) ++n;
    maxLen = std::max(maxLen, n);
  }
  if (maxLen == 0) return;
  const std::vector<VarInfo> card = ds->formatCard(std::vector<std::string>{varname});
  if (card.empty()) return;
  const std::string fmt = session.displayFormat(card.front());
  static const std::regex reFmt(R"(^%(-?)(0?)(\d+)([.,])(\d+)([gfe])(c?)$)");
  std::smatch m;
  if (!std::regex_match(fmt, m, reFmt)) return;
  const size_t w = static_cast<size_t>(std::stoul(m.str(3)));
  if (maxLen <= w) return;
  session.varMeta[varname].format =
      "%" + std::to_string(maxLen) + m.str(4) + m.str(5) + m.str(6) + m.str(7);
}
}  // namespace

ojson runLabelValues(const std::string& varname, const std::optional<std::string>& lblname,
                     SessionState& session, bool nofix) {
  if (!lblname.has_value() || lblname->empty() || *lblname == ".") {
    // `label values VAR` (no lblname) and `label values VAR .` detach.
    session.varLblname.erase(varname);
    session.valueLabels.erase(varname);
    return ojson::array();
  }
  session.varLblname[varname] = *lblname;
  refreshValueLabelsForVar(session, varname, *lblname);
  if (!nofix) widenFormatForSet(session, varname, *lblname);
  return ojson::array();
}

// ---------------------------------------------------------------------------
// format VARLIST %fmt / format %fmt VARLIST (the dispatcher hands both shapes
// here as varlist + fmt) — BS-005/D6: writes session.varMeta[name].format, the
// field applyDtaMeta() and the .hbin header load (seedVarMetaFromLoadedFile)
// also fill and hbinMetaPatch saves; every reader takes it through
// SessionState::displayFormat. An empty fmt lists the current formats.
// Unknown/malformed %fmt, or a family that does not suit the variable's type,
// is an honest reject, never a silent accept (D6) — `display`'s own separate
// inline `%fmt EXPR` mechanism (language/display_number_format.cpp's applyStataFmtLocal) is
// untouched; this is the persistent per-variable attribute.

// %fmt validity ([D] format): the numeric, binary, string, date/time (with an
// optional display mask, accepted unvalidated -- %tcHH:MM, %tmMon_CCYY), legacy
// %d and business-calendar families. Anything else is malformed -> r(120).
bool isValidDisplayFormat(const std::string& fmt) {
  // Numeric ([D] format): %[-][0]w.d{f,g,e}[c], with `,` for the European
  // decimal point; d must be less than w ("%9.12f" is not a format).
  static const std::regex reNumeric(R"(^%-?0?(\d{1,3})(?:[.,](\d{1,3}))?[fge]c?$)");
  static const std::regex reBinary(R"(^%(21x|16H|16L|8H|8L)$)");
  static const std::regex reString(R"(^%[-~]?\d{1,4}s$)");
  // %t{c,C,d,w,m,q,h,y,g} with an optional display mask (%tcHH:MM,
  // %tmMon_CCYY, %tdMon-YY -- the corpus shapes), and the legacy %d date
  // format with its own optional mask (6 corpus papers write `format x %d`).
  static const std::regex reDate(R"(^%-?\d{0,2}t[cCdwmqhyg].*$)");
  static const std::regex reLegacyDate(R"(^%-?d.*$)");
  // AER scope-2 B2-3: `%tbNAME[:detail]`, the business-calendar family. Stata
  // accepts it whether or not the calendar EXISTS (measured) -- and so does
  // this, because the format is a display attribute and the calendar is
  // resolved at RENDER time, where a missing file falls back to %g exactly as
  // the manual's item 5 says. But the calendar-NAME rule binds here as hard as
  // it binds `bcal create` and `bcal load`, and the FORMAT plane is the third
  // surface, not an exception (review 2026-09-09; the earlier comment claimed
  // a blanket acceptance the measurement does not support):
  //   `format x %tbabcdefghij`      rc 0    (10 characters is inside the rule)
  //   `format x %tbabcdefghij:day`  rc 0
  //   `format x %tbelevenchars`     r(120)
  //   `format x %tbelevenchars:day` r(120)
  //   `format x %tbab-cd`           r(120)  ·  `format x %tb9abc`  r(120)
  // (StataBE 19.5, /private/tmp/fix-bcal/st/cap.log keys A_*.) The predicate is
  // bcal::isValidCalName — the ONE place the rule lives — not a fourth copy.
  static const std::regex reBcal(R"(^%-?\d{0,2}tb([A-Za-z_][A-Za-z0-9_]*)(?::.*)?$)");
  std::smatch bm;
  if (std::regex_match(fmt, bm, reBcal)) return bcal::isValidCalName(bm[1].str());
  std::smatch nm;
  if (std::regex_match(fmt, nm, reNumeric))
    return !nm[2].matched || std::stoi(nm[2].str()) < std::stoi(nm[1].str());
  return std::regex_match(fmt, reBinary) || std::regex_match(fmt, reString) ||
         std::regex_match(fmt, reDate) || std::regex_match(fmt, reLegacyDate);
}

// A string %fmt (%#s, %-#s, %~#s) suits only a string variable, and every other
// family only a numeric one.
bool isStringDisplayFormat(const std::string& fmt) {
  static const std::regex reString(R"(^%[-~]?\d{1,4}s$)");
  return std::regex_match(fmt, reString);
}

ojson runFormat(const std::vector<std::string>& varlist, const std::string& fmt,
                SessionState& session) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
  }
  if (fmt.empty()) {
    // `format [varlist]` with no %fmt displays the current formats ([D] format:
    // "format varlist lists the formats for the specified variables"; `format`
    // by itself, every variable). Read through the one resolver.
    const std::vector<VarInfo> card = ds->formatCard(
        varlist.empty() ? std::nullopt : std::optional<std::vector<std::string>>(varlist));
    std::string text = "  Variable name   Display format\n";
    for (const auto& v : card) {
      std::string name = v.name;
      if (name.size() < 16) name.resize(16, ' ');
      text += "  " + name + session.displayFormat(v) + "\n";
    }
    ojson out;
    out["type"] = "display";
    out["text"] = text;
    return out;
  }
  if (fmt[0] != '%' || !isValidDisplayFormat(fmt)) {
    // r(120) is Stata's rc for an invalid %fmt, measured for BOTH the general
    // family and the business-calendar name rule (StataBE 19.5, review
    // 2026-09-09: `format x %zz` r(120), `format x %tbelevenchars` r(120) —
    // cap.log keys A_zz_rc / A_eleven_rc). "invalid_format" is not one of the
    // frozen error CODES, so errorBlock() coerced it to "engine_error" and the
    // refusal reached the user as rc 1; `reason` is the only channel that
    // carries an exact rc (BUG-187), so the rc travels there.
    return engineErrorBlockRc("invalid %format: `" + fmt + "'", 120);
  }
  // A string format on a numeric variable, or a numeric/date format on a
  // string variable, is refused before any variable is touched: a half-applied
  // `format` would leave the varlist in a state no Stata run produces.
  const bool strFmt = isStringDisplayFormat(fmt);
  for (const auto& v : varlist) {
    const bool strVar = ds->dtype(v) == "factor";
    if (strFmt && !strVar)
      return engineErrorBlockRc("format " + fmt + " invalid for numeric variable " + v, 120);
    if (!strFmt && strVar)
      return engineErrorBlockRc("format " + fmt + " invalid for string variable " + v, 120);
  }
  for (const auto& v : varlist) {
    session.varMeta[v].format = fmt;
    session.userFormattedVars.insert(v);
  }
  return ojson::array();  // silent success (Stata-faithful, mirrors `label variable`)
}

// ---------------------------------------------------------------------------
// label define / label list / label dir / label drop — C++ port of
// label_define.py (M3b, ROADMAP M3a's own note: "define/list/dir/drop —
// lower corpus frequency — stubbed for M3b").

namespace command_detail {

// The one Stata name rule (strutil.h -> core/ident.h): `label define تسمية 1 "one"`
// names a value label, as in Stata 14+; at most 32 characters, not bytes.
bool validStataName(const std::string& name) { return strutil::isValidStataIdentifier(name); }

// _find_unquoted_comma: first top-level comma outside quotes/parens.
long findUnquotedComma(const std::string& s) {
  bool inQuote = false;
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (inQuote) {
      if (ch == '"') inQuote = false;
    } else if (ch == '"') {
      inQuote = true;
    } else if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

// _read_string: quoted (until closing ") or unquoted (until whitespace).
std::pair<std::string, size_t> readString(const std::string& s, size_t pos) {
  if (pos >= s.size()) return {"", pos};
  // EM9: a compound-quoted label `"..."' (nesting; inner " kept) -- `99 `"Don't
  // know"'`, a label holding a quote (w2af_macro_text_file.log :1477, :1509).
  if (eval::startsCompound(s, pos)) {
    const size_t end = eval::compoundSpanEnd(s, pos);
    if (end != std::string::npos) return {s.substr(pos + 2, end - pos - 4), end};
  }
  if (s[pos] == '"') {
    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos) return {s.substr(pos + 1), s.size()};
    return {s.substr(pos + 1, end - (pos + 1)), end + 1};
  }
  size_t end = pos;
  while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
  return {s.substr(pos, end - pos), end};
}

}  // namespace

ojson runLabelDefine(const std::string& rawLine, SessionState& session) {
  std::string raw = trim(rawLine);

  bool addOpt = false, modifyOpt = false, replaceOpt = false, nofixOpt = false;
  long commaIdx = findUnquotedComma(raw);
  if (commaIdx != -1) {
    std::string optStr = lower(trim(raw.substr(static_cast<size_t>(commaIdx) + 1)));
    raw = trim(raw.substr(0, static_cast<size_t>(commaIdx)));
    // Options by WORD, each a prefix of its name (`, a` is add:
    // w2ao_label_family.log lb_add_a rc 0); `nofix` keeps the display formats.
    std::istringstream os(optStr);
    for (std::string w; os >> w;) {
      const auto isPrefix = [&w](const char* full) {
        const std::string f(full);
        return !w.empty() && w.size() <= f.size() && f.compare(0, w.size(), w) == 0;
      };
      if (isPrefix("add")) addOpt = true;
      else if (isPrefix("modify")) modifyOpt = true;
      else if (isPrefix("replace")) replaceOpt = true;
      else if (isPrefix("nofix")) nofixOpt = true;
      else
        return engineErrorBlockRc("option " + w + " not allowed", 198);
    }
    if (modifyOpt) addOpt = false;
  }

  static const std::regex reKw(R"(^label\s+define\s+)", std::regex::icase);
  std::smatch km;
  if (!std::regex_search(raw, km, reKw)) {
    return engineErrorBlock("Expected 'label define LBLNAME ...', got: '" + raw + "'");
  }
  std::string rest = raw.substr(static_cast<size_t>(km.length(0)));

  size_t sp = rest.find_first_of(" \t");
  std::string lblname = (sp == std::string::npos) ? rest : rest.substr(0, sp);
  if (lblname.empty()) return engineErrorBlock("label define: missing label name");
  if (!validStataName(lblname)) {
    return engineErrorBlock("label define: invalid label name '" + lblname + "'");
  }
  std::string body = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp));

  // Codes are integers or the extended missing values .a-.z (lb_def_miss
  // `ny -9 "Refused" .a "Not asked" 1 "One"`, rc 0); a code is stored under its
  // text spelling ("-9", ".a").
  //
  // BUG-1435: an integer code was read with std::stoi, whose out_of_range on
  // `label define L 99999999999 "x"` reached the dispatcher as "stoi: out of
  // range" at rc 1. Stata reads the code as a number and labels it only when it
  // is an integer a `long` can hold, -2,147,483,647 through 2,147,483,620:
  // anything else is r(198) "may not label <the number at %12.0g>" (`1.5`,
  // `2147483647`, `-2147483648`, `99999999999` -> "may not label 1.00000e+11",
  // and `.`), while `1e3`, `1.0`, `+5` and `007` are the codes 1000, 1, 5 and 7.
  // A code that is no number (`abc`, `0x10`), a code with no text, and no pair at
  // all are "invalid syntax" r(198). StataBE 19.5, 2026-10-29.
  std::vector<std::pair<std::string, std::string>> mapping;  // preserves insertion order
  size_t pos = 0;
  while (pos < body.size()) {
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) break;

    // The code ends at a blank or where its text's quote opens: `1"a" 2"b"` and
    // `1"a"2"b"` are codes 1 and 2 (StataBE 19.5, rc 0).
    size_t tokEnd = pos;
    while (tokEnd < body.size() && !std::isspace(static_cast<unsigned char>(body[tokEnd])) &&
           body[tokEnd] != '"' && body.compare(tokEnd, 2, "`\"") != 0)
      ++tokEnd;
    std::string code = body.substr(pos, tokEnd - pos);
    static const std::regex reExtMissing(R"(^\.[a-z]$)");
    static const std::regex reNumber(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
    if (code == ".") return engineErrorBlockRc("may not label .", 198);
    if (!std::regex_match(code, reExtMissing)) {
      if (!std::regex_match(code, reNumber)) return engineErrorBlockRc("invalid syntax", 198);
      const double v = std::strtod(code.c_str(), nullptr);
      // 2147483621 is the long Stata stores `.` as: it is accepted, kept under its
      // number (`: label D 2147483621` is its text, `: label D .` is "."), and
      // `label list` shows it as the code `.` (runLabelList).
      if (!(v == std::floor(v) && v >= -2147483647.0 && v <= 2147483621.0))
        return engineErrorBlockRc("may not label " + fmt::general(v, 12, 0), 198);
      code = std::to_string(static_cast<long long>(v));
    }
    pos = tokEnd;

    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return engineErrorBlockRc("invalid syntax", 198);
    auto [text, newPos] = readString(body, pos);
    pos = newPos;
    mapping.emplace_back(code, text);
  }

  if (mapping.empty()) return engineErrorBlockRc("invalid syntax", 198);

  auto existingIt = session.labelDefs.find(lblname);
  bool hasOpts = addOpt || modifyOpt || replaceOpt;
  // BUG-475 (measured StataBE 19.5, B4REC|label_define|<redefine_no_replace>|110|,
  // tests/eval/golden/aer_b4/pins_oracle_r.log:57-61): redefining an
  // EXISTING label with none of add/modify/replace is refused verbatim
  // "label <name> already defined" (log:58), which coarseRc's "already
  // defined" rule (eval/errors.cpp) maps to r(110). Must return BEFORE any
  // mutation or the propagation loop below, so the original mapping survives.
  if (existingIt != session.labelDefs.end() && !hasOpts) {
    return engineErrorBlock("label " + lblname + " already defined");
  }
  // A text "" under modify deletes the code (lb_modify_empty: `: label edu_level
  // 2` reads `2` after `label define edu_level 2 "", modify`).
  if (existingIt == session.labelDefs.end() || replaceOpt) {
    std::map<std::string, std::string> replaced;
    for (const auto& [k, v] : mapping) {
      if (v.empty() && modifyOpt) continue;
      replaced[k] = v;
    }
    session.labelDefs[lblname] = replaced;
  } else if (modifyOpt) {
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) {
      if (v.empty()) merged.erase(k);
      else merged[k] = v;
    }
    session.labelDefs[lblname] = merged;
  } else {
    // add: new codes only; an existing code is Stata's r(180) "invalid attempt
    // to modify label" (w2ao_label_family.log:969 lb_add_exist), nothing changed.
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) {
      if (merged.count(k) != 0) return engineErrorBlockRc("invalid attempt to modify label", 180);
      merged[k] = v;
    }
    session.labelDefs[lblname] = merged;
  }
  session.stampLabelSet(lblname);

  // Propagate to valueLabels for every variable currently attached, and widen
  // their display formats unless nofix (lb_modify %8.0g/%3.0g -> %9.0g/%9.0g,
  // log:1035; lb_modify_nofix keeps them).
  for (const auto& [varname, lname] : session.varLblname) {
    if (lname != lblname) continue;
    refreshValueLabelsForVar(session, varname, lblname);
    if (!nofixOpt) widenFormatForSet(session, varname, lblname);
  }
  return ojson::array();  // Stata silent on success
}

// LBL-LISTDIR: `label list` posts r(k) (codes, extended missing included),
// r(min)/r(max) (the integer codes) and r(hasemiss) of the LAST set it lists;
// bare `label list` walks the sets in `label dir` order (newest first); a name
// that is not defined is r(111) "value label X not found" (label's help;
// w2ao_label_family.log lb_list_one, lb_list_all, :1305 lb_list_absent).
ojson runLabelList(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) lblnames = session.labelSetOrder();
  // The sets named before the first undefined one are listed, then r(111)
  // (w2_data_io.log:126-133 app3_alab_defined).
  std::vector<std::string> errors;
  {
    std::vector<std::string> present;
    for (const auto& name : lblnames) {
      if (session.labelDefs.find(name) == session.labelDefs.end()) {
        errors.push_back(name);
        break;
      }
      present.push_back(name);
    }
    lblnames = present;
  }
  if (lblnames.empty() && !errors.empty())
    return engineErrorBlockRc("value label " + errors.front() + " not found", 111);
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "label list", {"k", "min", "max", "hasemiss"});
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  for (const auto& name : lblnames) {
    const auto& defs = session.labelDefs.at(name);
    lines.push_back(name + ":");
    std::vector<std::pair<int, std::string>> sorted;
    std::vector<std::pair<std::string, std::string>> emiss;
    for (const auto& [codeStr, text] : defs) {
      if (!codeStr.empty() && codeStr[0] == '.') {
        emiss.emplace_back(codeStr, text);
        continue;
      }
      // 2147483621, Stata's long for `.`, lists as the code `.` ahead of .a-.z
      // and counts as a missing code in r(min)/r(max)/r(hasemiss) (StataBE 19.5:
      // `label define D 2147483621 "miss" 1 "one" .a "ea"` lists 1 one / . miss /
      // .a ea with r(min) = r(max) = 1, r(hasemiss) = 1).
      if (codeStr == "2147483621") {
        emiss.emplace(emiss.begin(), ".", text);
        continue;
      }
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%3d", code);
      lines.push_back("           " + std::string(buf) + "  " + text);
    }
    for (const auto& [code, text] : emiss) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%3s", code.c_str());
      lines.push_back("           " + std::string(buf) + "  " + text);
    }
    // r() of this set (the last one listed wins).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    session.eval.rValues.set("k", static_cast<double>(defs.size()));
    session.eval.rValues.set("min", sorted.empty() ? nan : static_cast<double>(sorted.front().first));
    session.eval.rValues.set("max", sorted.empty() ? nan : static_cast<double>(sorted.back().first));
    session.eval.rValues.set("hasemiss", emiss.empty() ? 0.0 : 1.0);
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  if (!errors.empty()) {
    // An undefined name is Stata's r(111) "value label NAME not found", after
    // the sets named before it are listed (w2_data_io.log:126-133
    // app3_alab_defined).
    ojson err = eval::errorBlock("engine_error", "value label " + errors.front() + " not found",
                                 std::nullopt, std::nullopt, std::nullopt, "r(111)");
    if (lines.empty()) return err;
    return ojson::array({block, err});
  }
  return block;
}

// `label dir`: the set names, newest first, and r(names) (label.sthlp; lb_dir
// `zz_lbl keep_lbl negative_lbl grade_drop`).
ojson runLabelDir(SessionState& session) {
  const std::vector<std::string> names = session.labelSetOrder();
  eval::replaceRClass(session.eval);
  std::string joined, spaced;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) {
      joined += "\n";
      spaced += " ";
    }
    joined += names[i];
    spaced += names[i];
  }
  session.eval.rStrings.set("names", spaced);
  eval::declareRPosted(session.eval, "label dir", {"names"});
  ojson block;
  block["type"] = "display";
  block["text"] = names.empty() ? std::string("(no value labels defined)") : joined;
  return block;
}

ojson runLabelbook(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) {
    for (const auto& [name, _] : session.labelDefs) lblnames.push_back(name);
    std::sort(lblnames.begin(), lblnames.end());
  }
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  std::vector<std::string> errors;
  bool first = true;
  for (const auto& name : lblnames) {
    auto it = session.labelDefs.find(name);
    if (it == session.labelDefs.end()) {
      errors.push_back(name);
      continue;
    }
    if (!first) lines.push_back("");
    first = false;
    lines.push_back(std::string(88, '-'));
    lines.push_back("Value label " + name);
    lines.push_back(std::string(88, '-'));
    lines.push_back("  Definition");
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [codeStr, text] : it->second) {
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%6d", code);
      lines.push_back(std::string(buf) + "   " + text);
    }
    // Attached variables: session.varLblname is varname -> lblname (the
    // reverse index of session.labelDefs' lblname -> {code:text}).
    std::vector<std::string> vars;
    for (const auto& [varname, lname] : session.varLblname) {
      if (lname == name) vars.push_back(varname);
    }
    std::sort(vars.begin(), vars.end());
    std::string varsJoined;
    for (size_t i = 0; i < vars.size(); ++i) {
      if (i) varsJoined += " ";
      varsJoined += vars[i];
    }
    lines.push_back("");
    lines.push_back("   Variables:  " + (vars.empty() ? std::string("(none)") : varsJoined));
  }
  if (!errors.empty()) {
    std::string errText;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i) errText += ", ";
      errText += errors[i];
    }
    lines.push_back("warning: " + errText + ": not defined");
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  return block;
}

// `datasignature` — forward to the engine verb of the same name and show the
// string it returns. The engine prints plain text on stdout (JSON only on
// error), so this uses raw runCobra + errorFromRaw like the other status-line
// verbs rather than runCobraJson.
//
// Stata-SHAPED, not Stata-comparable: see the declaration in handlers.h and
// src/data/datasignature.h's Q9 ruling. This wiring deliberately does NOT try
// to reproduce Stata's checksums — it exposes the engine's own contract-
// satisfying fingerprint. BUG-1461: in the user's form, which `datasignature
// confirm` compares against, as Stata's is: blind to row order and a function
// of each variable's storage type (signatureTypesArg).
namespace command_detail {
std::string signatureTypesArg(SessionState& session, const std::vector<std::string>& vars) {
  std::string out;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return out;
  for (const auto& v : vars)
    out += (out.empty() ? "" : ",") + v + "=" + serset::storageType(session, *ds, v);
  return out;
}
}  // namespace

ojson runDatasignature(SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> args = {"datasignature", "--data", *session.currentHbin(), "--unordered",
                                   "--types", signatureTypesArg(session, session.datastore()->names())};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  std::string sig = cr.stdout_text;
  while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r' || sig.back() == ' ')) {
    sig.pop_back();
  }
  ojson block;
  block["type"] = "display";
  block["text"] = sig;
  return block;
}

// BUG-1461: `datasignature set|confirm|clear` -- the signature is kept where
// Stata keeps it, in the dataset's characteristics (_dta[datasignature_si] the
// signature, _dta[datasignature_vl1] the variables it covered, _dta[datasignature_dt]
// when it was set), so it travels with save/use and `char list` shows it. confirm
// recomputes over the recorded variables in their recorded order: a dropped or
// changed one is r(9) "data have changed since ...", while an added variable or
// a new order is noted and allowed (r(k_added)) unless `strict`.
// Measured on StataBE 19.5 (2026-11-03): confirm with nothing set is r(459), a
// second set without `reset` is r(110), an unknown subcommand r(198).
namespace command_detail {
std::optional<std::string> dtaChar(const SessionState& session, const std::string& name) {
  for (const auto& c : session.characteristics)
    if (c.varname == "_dta" && c.charname == name) return c.value;
  return std::nullopt;
}

void setDtaChar(SessionState& session, const std::string& name, const std::string& value) {
  for (auto& c : session.characteristics)
    if (c.varname == "_dta" && c.charname == name) {
      c.value = value;
      return;
    }
  session.characteristics.push_back({"_dta", name, value});
}

// "03nov2026 17:29", the form Stata's notes print the setting time in.
std::string signatureStamp(const std::string& epochSeconds) {
  std::time_t t = 0;
  try {
    t = static_cast<std::time_t>(std::stoll(epochSeconds));
  } catch (...) {
    return epochSeconds;
  }
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  static const char* kMon[] = {"jan", "feb", "mar", "apr", "may", "jun",
                               "jul", "aug", "sep", "oct", "nov", "dec"};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d%s%04d %02d:%02d", tm.tm_mday, kMon[tm.tm_mon],
                tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
  return buf;
}

// `<rows>:<k>:<a>:<b>` of the engine's per-variable signature, and the same
// fields read out of a full `<rows>:<k>(<meta>):<a>:<b>` one: over the same
// variables in the same order the two agree field for field.
std::string valuesPart(const std::string& sig) {
  const size_t open = sig.find('(');
  const size_t close = sig.find(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return sig;
  return sig.substr(0, open) + sig.substr(close + 1);
}
}  // namespace

ojson runDatasignatureSub(const std::string& sub,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          SessionState& session) {
  bool reset = false, strict = false;
  for (const auto& [k, v] : opts) {
    if (k == "reset") reset = true;
    if (k == "strict") strict = true;
  }
  if (sub == "clear") {
    auto& cs = session.characteristics;
    cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [](const DtaCharacteristic& c) {
                              return c.varname == "_dta" &&
                                     c.charname.rfind("datasignature_", 0) == 0;
                            }),
             cs.end());
    ojson b;
    b["type"] = "display";
    b["text"] = "  (data signature cleared)";
    return b;
  }
  if (sub == "set") {
    const bool wasSet = dtaChar(session, "datasignature_si").has_value();
    if (wasSet && !reset)
      return engineErrorBlockRc("data signature already set -- specify option reset", 110);
    ojson sigBlk = runDatasignature(session);
    if (sigBlk.value("type", std::string()) != "display") return sigBlk;
    const std::string sig = sigBlk.value("text", std::string());
    std::string vl;
    for (const auto& n : session.datastore()->names()) vl += (vl.empty() ? "" : " ") + n;
    setDtaChar(session, "datasignature_dt", std::to_string(static_cast<long long>(std::time(nullptr))));
    setDtaChar(session, "datasignature_vl1", vl);
    setDtaChar(session, "datasignature_si", sig);
    session.eval.rStrings.set("datasignature", sig);
    ojson b;
    b["type"] = "display";
    // "reset" only when `reset` replaced one (StataBE 19.5).
    b["text"] = "  " + sig + "       (data signature " + (wasSet ? "reset" : "set") + ")";
    return b;
  }
  // confirm
  const auto si = dtaChar(session, "datasignature_si");
  if (!si.has_value())
    return engineErrorBlockRc("  no data signature was previously set", 459);
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::string since = signatureStamp(dtaChar(session, "datasignature_dt").value_or(""));
  std::vector<std::string> recorded;
  {
    std::istringstream is(dtaChar(session, "datasignature_vl1").value_or(""));
    for (std::string w; is >> w;) recorded.push_back(w);
  }
  const std::vector<std::string>& now = session.datastore()->names();
  bool same = true;
  for (const auto& v : recorded)
    if (std::find(now.begin(), now.end(), v) == now.end()) same = false;
  if (same) {
    std::string csv;
    for (const auto& v : recorded) csv += (csv.empty() ? "" : ",") + v;
    CobraRunResult cr = runCobra({"datasignature", "--data", *session.currentHbin(), "--vars", csv,
                                  "--unordered", "--types", signatureTypesArg(session, recorded)});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string got = cr.stdout_text;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r' || got.back() == ' '))
      got.pop_back();
    same = got == valuesPart(*si);
  }
  if (!same) return engineErrorBlockRc("  data have changed since " + since, 9);
  // Unchanged values; the note names an addition before a reordering, and
  // `strict` turns either into r(9) with the same note and nothing posted.
  const size_t added = now.size() - recorded.size();
  std::vector<std::string> kept;
  for (const auto& v : now)
    if (std::find(recorded.begin(), recorded.end(), v) != recorded.end()) kept.push_back(v);
  std::string note = "  (data unchanged since " + since;
  if (added == 1)
    note += ", except 1 variable has been added";
  else if (added > 1)
    note += ", except " + std::to_string(added) + " variables have been added";
  else if (kept != recorded)
    note += ", but order of variables has changed";
  note += ")";
  if (strict && (added > 0 || kept != recorded)) return engineErrorBlockRc(note, 9);
  session.eval.rValues.set("k_added", static_cast<double>(added));
  session.eval.rStrings.set("datasignature", *si);
  ojson b;
  b["type"] = "display";
  b["text"] = note;
  return b;
}

// LBL-LANGUAGE ([D] label language; w2ao_label_family.log lb_lang_*): languages
// keep their own variable labels and value-label attachments. `new` starts the
// new language empty (`copy`: as a copy of the current one) and makes it
// current; naming a language switches to it; `delete` removes one (the current
// one hands over to the first remaining language: lb_lang_delete); an unknown
// name is r(111) "language X not defined". The bare form posts r(k),
// r(language) and r(languages) (lb_lang_bare `k=1|default|default`).
namespace command_detail {
SessionState::LabelLangStore snapshotLabelLang(SessionState& session) {
  SessionState::LabelLangStore st;
  st.dataLabel = session.dataLabel;
  if (Datastore* ds = session.datastore()) {
    for (const VarInfo& vi : session.varlistWithMeta(ds))
      if (!vi.label.empty()) st.varLabels[vi.name] = vi.label;
  }
  st.varLblname = session.varLblname;
  return st;
}
void loadLabelLang(SessionState& session, const SessionState::LabelLangStore& st) {
  session.dataLabel = st.dataLabel;
  if (Datastore* ds = session.datastore()) {
    for (const auto& name : ds->names()) {
      auto it = st.varLabels.find(name);
      session.setVarLabel(name, it == st.varLabels.end() ? std::string() : it->second);
    }
  }
  session.varLblname = st.varLblname;
  session.valueLabels.clear();
  for (const auto& [var, lbl] : session.varLblname) refreshValueLabelsForVar(session, var, lbl);
}
}  // namespace

ojson runLabelLanguage(const std::vector<std::string>& args,
                       const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       SessionState& session) {
  bool optNew = false, optCopy = false, optDelete = false, optRename = false;
  for (const auto& [k, v] : opts) {
    (void)v;
    if (k == "new") optNew = true;
    else if (k == "copy") optCopy = true;
    else if (k == "delete") optDelete = true;
    else if (k == "rename") optRename = true;
    else return engineErrorBlockRc("option " + k + " not allowed", 198);
  }
  auto& langs = session.labelLangs;
  auto known = [&langs](const std::string& n) {
    return std::find(langs.begin(), langs.end(), n) != langs.end();
  };
  if (args.empty()) {
    eval::replaceRClass(session.eval);
    std::string all;
    for (size_t i = 0; i < langs.size(); ++i) all += (i ? " " : "") + langs[i];
    session.eval.rValues.set("k", static_cast<double>(langs.size()));
    session.eval.rStrings.set("language", session.labelLang);
    session.eval.rStrings.set("languages", all);
    eval::declareRPosted(session.eval, "label language", {"k", "language", "languages"});
    ojson d;
    d["type"] = "display";
    if (langs.size() == 1) {
      d["text"] =
          "Language for variable and value labels\n\n"
          "    In this dataset, value and variable labels have been defined in only one "
          "language:  " + session.labelLang + "\n\n"
          "    To create new language:         . label language <name>, new\n"
          "    To rename current language:     . label language <name>, rename";
    } else {
      std::string text = "Language for variable and value labels\n\n    Available languages:\n";
      for (const auto& l : langs) text += "            " + l + "\n";
      text += "\n    Currently set as:               . label language " + session.labelLang;
      d["text"] = text;
    }
    return d;
  }
  if (args.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
  const std::string& name = args[0];
  if (!validStataName(name)) return engineErrorBlockRc(name + " invalid name", 198);
  if (optDelete) {
    if (!known(name)) return engineErrorBlockRc("language " + name + " not defined", 111);
    if (langs.size() == 1)
      return engineErrorBlockRc("language " + name + " is the only language; it cannot be deleted",
                                198);
    if (name == session.labelLang) {
      const std::string next = langs[0] == name ? langs[1] : langs[0];
      auto it = session.labelLangStash.find(next);
      loadLabelLang(session, it == session.labelLangStash.end() ? SessionState::LabelLangStore{}
                                                                 : it->second);
      session.labelLangStash.erase(next);
      session.labelLang = next;
    }
    session.labelLangStash.erase(name);
    langs.erase(std::find(langs.begin(), langs.end(), name));
    return ojson::array();
  }
  if (optRename) {
    if (known(name)) return engineErrorBlockRc("language " + name + " already defined", 110);
    *std::find(langs.begin(), langs.end(), session.labelLang) = name;
    session.labelLang = name;
    return ojson::array();
  }
  if (optNew) {
    if (known(name)) return engineErrorBlockRc("language " + name + " already defined", 110);
    session.labelLangStash[session.labelLang] = snapshotLabelLang(session);
    if (!optCopy) loadLabelLang(session, SessionState::LabelLangStore{});
    langs.push_back(name);
    session.labelLang = name;
    ojson d;
    d["type"] = "display";
    d["text"] = "(language " + name + " now current language)";
    return d;
  }
  if (name == session.labelLang) {
    ojson d;
    d["type"] = "display";
    d["text"] = "(" + name + " already current language)";
    return d;
  }
  if (!known(name)) return engineErrorBlockRc("language " + name + " not defined", 111);
  session.labelLangStash[session.labelLang] = snapshotLabelLang(session);
  auto it = session.labelLangStash.find(name);
  loadLabelLang(session, it == session.labelLangStash.end() ? SessionState::LabelLangStore{}
                                                             : it->second);
  session.labelLangStash.erase(name);
  session.labelLang = name;
  return ojson::array();
}

// `label drop`: removes the DEFINITION; a variable keeps the attached set's name
// (w2ao_label_family.log lb_drop: `: value label g1` is still grade_drop and
// decode is r(111)). `_all` drops every set; a name that is not defined is
// r(111) "value label X not found" and nothing is dropped (:1373 lb_drop_absent).
ojson runLabelDrop(const std::vector<std::string>& lblnamesIn, SessionState& session) {
  if (lblnamesIn.empty()) {
    return engineErrorBlock("label drop: specify at least one label name");
  }
  std::vector<std::string> lblnames;
  for (const auto& name : lblnamesIn) {
    if (name == "_all") {
      for (const auto& kv : session.labelDefs) lblnames.push_back(kv.first);
      continue;
    }
    if (session.labelDefs.find(name) == session.labelDefs.end())
      return engineErrorBlockRc("value label " + name + " not found", 111);
    lblnames.push_back(name);
  }
  for (const auto& name : lblnames) {
    session.labelDefs.erase(name);
    session.labelSeq.erase(name);
    for (const auto& [varname, lname] : session.varLblname)
      if (lname == name) session.valueLabels.erase(varname);
  }
  return ojson::array();  // Stata silent on success
}

// ---------------------------------------------------------------------------
// preserve / restore — LIFO hbin-file-copy + display-metadata snapshot
// stack (preserve_restore.py's run_preserve/run_restore, ported exactly:
// both return a real data_mutation block with a `preserve_depth` field, NOT
// silent — unlike tempfile/tempname/label).


}  // namespace piet::handlers
