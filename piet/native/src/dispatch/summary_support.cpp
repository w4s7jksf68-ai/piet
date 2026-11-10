// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "summary_support.h"
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {


// tabstat's Stats subprogram, shared by the tabstat arm and the by-prefix driver
// (BUG-1520): each word is `syntax [, n MEan sd Variance SUm COunt MIn MAx Range
// SKewness Kurtosis SDMean SEMean p1 ... p99 iqr q MEDian CV]` (capitals = the
// minimum abbreviation), lower-cased; anything else is "unknown statistic",
// r(198) (sx_tabstat_ref_stat) -- that word is returned. The engine gets the
// canonical keys; the row names are the ado's.
std::optional<std::string> parseTabstatStats(const std::optional<std::string>& statisticsVal,
                                             std::vector<std::string>& statKeys,
                                             std::vector<std::string>& statRows) {
  struct TsStat { const char* full; size_t minLen; const char* key; const char* row; };
  static const TsStat kTsStats[] = {
      {"n", 1, "n", "N"},           {"mean", 2, "mean", "Mean"},
      {"sd", 2, "sd", "SD"},        {"variance", 1, "variance", "Variance"},
      {"sum", 2, "sum", "Sum"},     {"count", 2, "n", "N"},
      {"min", 2, "min", "Min"},     {"max", 2, "max", "Max"},
      {"range", 1, "range", "Range"}, {"skewness", 2, "skewness", "Skewness"},
      {"kurtosis", 1, "kurtosis", "Kurtosis"}, {"sdmean", 3, "semean", "se(mean)"},
      {"semean", 3, "semean", "se(mean)"}, {"p1", 2, "p1", "p1"},
      {"p5", 2, "p5", "p5"},        {"p10", 3, "p10", "p10"},
      {"p25", 3, "p25", "p25"},     {"p50", 3, "p50", "p50"},
      {"p75", 3, "p75", "p75"},     {"p90", 3, "p90", "p90"},
      {"p95", 3, "p95", "p95"},     {"p99", 3, "p99", "p99"},
      {"iqr", 3, "iqr", "IQR"},     {"median", 3, "p50", "p50"},
      {"cv", 2, "cv", "CV"}};
  std::string list = statisticsVal.has_value() ? *statisticsVal : std::string("mean");
  std::string flat;
  for (char ch : list) flat += (ch == '"' || ch == ',') ? ' ' : ch;
  std::istringstream iss(flat);
  std::string w;
  while (iss >> w) {
    std::string lw = w;
    for (auto& ch : lw) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lw == "q") {
      for (const char* p : {"p25", "p50", "p75"}) {
        statKeys.push_back(p);
        statRows.push_back(p);
      }
      continue;
    }
    const TsStat* hit = nullptr;
    for (const auto& t : kTsStats) {
      const std::string full = t.full;
      if (lw.size() >= t.minLen && lw.size() <= full.size() && full.compare(0, lw.size(), lw) == 0) {
        hit = &t;
        break;
      }
    }
    if (hit == nullptr) return w;
    statKeys.push_back(hit->key);
    statRows.push_back(hit->row);
  }
  if (statKeys.empty()) {
    statKeys.push_back("mean");
    statRows.push_back("Mean");
  }
  return std::nullopt;
}


// One of tabstat's saved matrices (r(Stat#), r(StatTotal)): a row per statistic,
// a column per variable, from a block's per-variable stats object.
eval::EvalMatrix tabstatStatMatrix(const ojson& statsObj, const std::vector<std::string>& statKeys,
                                   const std::vector<std::string>& statRows,
                                   const std::vector<std::string>& colVars) {
  eval::EvalMatrix m;
  m.rownames = statRows;
  m.colnames = colVars;
  for (const auto& sk : statKeys) {
    std::vector<double> row;
    for (const auto& vn : colVars) {
      double cell = std::numeric_limits<double>::quiet_NaN();
      if (statsObj.contains(vn) && statsObj[vn].is_object() && statsObj[vn].contains(sk) &&
          statsObj[vn][sk].is_number())
        cell = statsObj[vn][sk].get<double>();
      row.push_back(cell);
    }
    m.data.push_back(std::move(row));
  }
  return m;
}


// ---- tabstat's text table ------------------------------------------------------
// Layout measured on StataBE at linesize 80; widths count display characters.

constexpr size_t kTabstatLineWidth = 80;


std::string tsPadLeft(const std::string& text, size_t width) {
  const size_t have = eval::utf8Len(text);
  return have >= width ? text : std::string(width - have, ' ') + text;
}

std::string tsPadRight(const std::string& text, size_t width) {
  const size_t have = eval::utf8Len(text);
  return have >= width ? text : text + std::string(width - have, ' ');
}

std::string tsClip(const std::string& text, size_t width) {
  return eval::utf8Len(text) <= width ? text : eval::utf8Slice(text, 0, width);
}


// Stata's name abbreviation: a run of digits ending the name survives when room
// is left for at least one leading character.
std::string tsShortName(const std::string& name, size_t room) {
  const size_t length = eval::utf8Len(name);
  if (length <= room) return name;
  size_t digits = 0;
  while (digits < name.size() &&
         std::isdigit(static_cast<unsigned char>(name[name.size() - 1 - digits])))
    ++digits;
  if (digits > 0 && room >= digits + 2)
    return eval::utf8Slice(name, 0, room - 1 - digits) + "~" + name.substr(name.size() - digits);
  return eval::utf8Slice(name, 0, room - 2) + "~" + eval::utf8Slice(name, length - 1, length);
}


bool tsIsLeftJustified(const std::string& fmt) { return fmt.size() > 1 && fmt[1] == '-'; }


bool tsIsTimeFormat(const std::string& fmt) {
  const size_t at = tsIsLeftJustified(fmt) ? 2 : 1;
  return at < fmt.size() && (fmt[at] == 't' || fmt[at] == 'd');
}


// The width a format asks for; the %t family states none, so its printed text
// decides.
size_t tsFormatWidth(const std::string& fmt) {
  size_t at = 1;
  while (at < fmt.size() && (fmt[at] == '-' || fmt[at] == '0' || fmt[at] == '+')) ++at;
  size_t width = 0, used = 0;
  for (; at < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[at])); ++at, ++used)
    width = width * 10 + static_cast<size_t>(fmt[at] - '0');
  return used > 0 ? width : eval::utf8Len(eval::applyStataFmt(0.0, fmt));
}


// A number under `fmt`, keeping the format's own padding.
std::string tsFormatted(double value, const std::string& fmt, size_t width) {
  const std::string text = eval::applyStataFmt(value, fmt);
  return tsIsLeftJustified(fmt) ? tsPadRight(text, width) : tsPadLeft(text, width);
}


std::optional<VarInfo> tsVarCard(SessionState& session, const std::string& name) {
  if (const std::vector<VarInfo>* card = session.residentReadCard())
    for (const VarInfo& info : *card)
      if (info.name == name) return info;
  Datastore* frame = session.datastore();
  if (frame == nullptr || !frame->has(name)) return std::nullopt;
  return frame->varinfo(name);
}


std::string tsDeclaredFormat(SessionState& session, const std::string& name) {
  const std::optional<VarInfo> info = tsVarCard(session, name);
  return info.has_value() ? session.displayFormat(*info) : std::string("%9.0g");
}


size_t tsClampedWidth(const eval::OptValue& given, const char* option, size_t fallback,
                      std::string& notes) {
  char* stop = nullptr;
  const std::string text = strutil::trim(given.strValue);
  const double asked = std::strtod(text.c_str(), &stop);
  if (text.empty() || *stop != '\0') return fallback;
  if (asked >= 8.0 && asked <= 32.0) return static_cast<size_t>(asked);
  const size_t bound = asked < 8.0 ? 8 : 32;
  notes += std::string("(option ") + option + "() outside valid range 8..32; " +
           std::to_string(bound) + " assumed)\n";
  return bound;
}


// False when `refusal` was set.
bool tsReadLookOptions(const eval::ParsedCommand& parsed, TabstatLookOptions& look,
                       std::optional<ojson>& refusal) {
  const auto refuseWith = [&refusal](const std::string& message, const char* rc) {
    refusal = ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt,
                                     std::nullopt, std::string(rc)));
    return false;
  };
  const auto isOn = [](const eval::OptValue& v) { return !v.isBool || v.boolValue; };
  std::optional<std::string> typedFormat;
  for (const auto& [key, value] : parsed.opts) {
    if (key == "format") {
      if (value.isBool) {
        look.ownFormats = look.ownFormats || value.boolValue;
      } else if (const std::string typed = strutil::trim(value.strValue); !typed.empty()) {
        typedFormat = typed;
      }
    } else if (key == "longstub") {
      look.longStubAsked = isOn(value);
    } else if (key == "noseparator") {
      look.separators = !isOn(value);
    } else if (key == "labelwidth" && !value.isBool) {
      look.labelWidth = tsClampedWidth(value, "labelwidth", look.labelWidth, look.notes);
    } else if (key == "varwidth" && !value.isBool) {
      look.varWidth = tsClampedWidth(value, "varwidth", look.varWidth, look.notes);
      look.varWidthGiven = true;
    }
  }
  if (look.ownFormats && typedFormat.has_value())
    return refuseWith("options format and format() may not be specified together", "r(198)");
  if (typedFormat.has_value()) {
    if (!handlers::isValidDisplayFormat(*typedFormat))
      return refuseWith("invalid %fmt in option format(): " + *typedFormat, "r(120)");
    look.commonFormat = *typedFormat;
  }
  return true;
}


TabstatCellStyle tsCellStyleFor(SessionState& session, const TabstatLookOptions& look,
                                const std::string& var) {
  TabstatCellStyle style;
  if (!look.commonFormat.empty()) {
    style.width = tsFormatWidth(look.commonFormat);
    // A string format still sets the width, but a number prints in the variable's
    // own format.
    style.fmt = handlers::isStringDisplayFormat(look.commonFormat) ? tsDeclaredFormat(session, var)
                                                                   : look.commonFormat;
    return style;
  }
  style.fmt = look.ownFormats ? tsDeclaredFormat(session, var) : std::string("%9.0g");
  style.width = tsFormatWidth(style.fmt);
  return style;
}


// Group texts come from the value label when one applies, then from a 18-digit
// general rendering; a date-like by() variable prints through its own format and
// is allowed to overflow the column.
TabstatByColumn tsLayOutByColumn(SessionState& session, const std::string& byName,
                                 const ojson& groups, size_t labelWidth, bool totalRow) {
  TabstatByColumn column;
  column.headerName = byName;
  const size_t nameWidth = eval::utf8Len(byName);
  const std::optional<VarInfo> card = tsVarCard(session, byName);
  const bool isString = card.has_value() && card->dtype == "factor";
  const auto plainNumber = [](double v) {
    return strutil::trim(eval::applyStataFmt(v, "%18.0g"));
  };
  const auto withTotalFloor = [totalRow](size_t width) {
    return totalRow ? std::max<size_t>(width, 6) : width;
  };

  if (isString) {
    size_t declared = 0;
    const auto meta = session.varMeta.find(byName);
    const std::string storage =
        meta != session.varMeta.end() ? meta->second.stataType.value_or("") : std::string();
    if (storage == "strL" || storage == "strl") {
      declared = 33;
    } else if (storage.rfind("str", 0) == 0 && storage.size() > 3) {
      declared = static_cast<size_t>(std::strtoul(storage.c_str() + 3, nullptr, 10));
    } else if (groups.is_array()) {
      for (const auto& g : groups)
        if (g.contains("value") && g["value"].is_string())
          declared = std::max(declared, eval::utf8Len(g["value"].get<std::string>()));
    }
    column.width = withTotalFloor(std::max(nameWidth, std::min(labelWidth, declared)));
    column.groupText = [](const ojson& v) { return v.is_string() ? v.get<std::string>() : std::string(); };
    return column;
  }

  const auto labelSet = session.valueLabels.find(byName);
  if (labelSet != session.valueLabels.end() && !labelSet->second.empty()) {
    size_t longest = 0;
    for (const auto& [code, text] : labelSet->second) longest = std::max(longest, eval::utf8Len(text));
    column.width = withTotalFloor(std::max(nameWidth, std::min(labelWidth, longest)));
    const std::map<int, std::string>* labels = &labelSet->second;
    column.groupText = [labels, plainNumber](const ojson& v) -> std::string {
      if (v.is_string()) return v.get<std::string>();
      const double d = v.is_number() ? v.get<double>() : std::numeric_limits<double>::quiet_NaN();
      if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 2147483647.0) {
        const auto hit = labels->find(static_cast<int>(d));
        if (hit != labels->end()) return hit->second;
      }
      return plainNumber(d);
    };
    return column;
  }

  const std::string ownFormat = tsDeclaredFormat(session, byName);
  if (tsIsTimeFormat(ownFormat)) {
    column.width = 9;
    column.headerName = tsShortName(byName, 9);
    column.clipGroups = false;
    // %tm and %tw print at width 7 whatever the value's own length (` 2020m2`).
    const size_t fullWidth = ownFormat == "%tm" || ownFormat == "%tw" ? 7 : 0;
    column.groupText = [ownFormat, fullWidth](const ojson& v) {
      const double cell = v.is_number() ? v.get<double>() : std::nan("");
      return tsPadLeft(eval::applyStataFmt(cell, ownFormat), fullWidth);
    };
    return column;
  }
  column.width = std::max<size_t>(8, nameWidth);
  column.groupText = [plainNumber](const ojson& v) -> std::string {
    if (v.is_string()) return v.get<std::string>();
    return plainNumber(v.is_number() ? v.get<double>() : std::nan(""));
  };
  return column;
}


// tabstat's text table, as a `log using` transcript records it in place of the
// structured block (SessionState::pendingTabstatLogText). Returns the table text;
// a refused display option sets `refusal` to the error block and returns "".
std::string renderTabstatTable(SessionState& session, const eval::ParsedCommand& parsed,
                               const ojson& tsBlock, const std::optional<std::string>& byVal,
                               const std::optional<std::string>& columnsVal, bool noTotal,
                               const std::vector<std::string>& statKeys,
                               const std::vector<std::string>& statRows,
                               std::optional<ojson>& refusal) {
  TabstatLookOptions look;
  if (!tsReadLookOptions(parsed, look, refusal)) return std::string();

  std::vector<std::string> vars;
  if (tsBlock.contains("vars") && tsBlock["vars"].is_array())
    for (const auto& name : tsBlock["vars"])
      if (name.is_string()) vars.push_back(name.get<std::string>());

  const bool byMode = byVal.has_value() && !byVal->empty();
  if (!byMode && noTotal) return look.notes + "nothing to display\n";

  // Which way the table reads: statistics across, or variables across.
  bool statsAcross = vars.size() < 2;
  if (columnsVal.has_value() && !columnsVal->empty())
    statsAcross = !(std::string("variables").compare(0, columnsVal->size(), *columnsVal) == 0);

  std::vector<TabstatCellStyle> styles;
  size_t cellWidth = 9;
  for (const std::string& var : vars) {
    styles.push_back(tsCellStyleFor(session, look, var));
    cellWidth = std::max(cellWidth, styles.back().width);
  }
  cellWidth += 1;

  const ojson emptyList = ojson::array();
  const ojson& groups =
      tsBlock.contains("groups") && tsBlock["groups"].is_array() ? tsBlock["groups"] : emptyList;
  const bool longStub = byMode && (look.longStubAsked || look.varWidthGiven);
  TabstatByColumn byColumn;
  if (byMode) byColumn = tsLayOutByColumn(session, *byVal, groups, look.labelWidth, !noTotal);

  const std::string rowHeading = statsAcross ? "Variable" : "Stats";
  const size_t rowNameWidth = statsAcross ? look.varWidth : 8;
  const auto rowNameAt = [&](size_t row) {
    return statsAcross ? tsShortName(vars[row], look.varWidth) : statRows[row];
  };
  const size_t rowCount = statsAcross ? vars.size() : statRows.size();
  const size_t columnCount = statsAcross ? statRows.size() : vars.size();

  // Left part of a line, for the heading or a body row. `groupLabel` is empty on a
  // group's later rows; `overflowOk` lets a date-like group text run past the column.
  const auto bodyStub = [&](const std::string& groupLabel, size_t row, bool overflowOk) {
    if (!byMode) return tsPadLeft(rowNameAt(row), rowNameWidth);
    const std::string shown =
        overflowOk ? groupLabel : tsClip(groupLabel, byColumn.width);
    if (!longStub) return tsPadLeft(shown, byColumn.width);
    return tsPadRight(shown, byColumn.width) + " " + tsPadLeft(rowNameAt(row), rowNameWidth);
  };
  std::string headStub;
  if (!byMode)
    headStub = tsPadLeft(rowHeading, rowNameWidth);
  else if (!longStub)
    headStub = tsPadLeft(byColumn.headerName, byColumn.width);
  else
    headStub = tsPadRight(byColumn.headerName, byColumn.width) + " " + tsPadLeft(rowHeading, rowNameWidth);
  const size_t stubWidth = eval::utf8Len(headStub);

  const auto statAt = [&](const ojson& statsObj, size_t row, size_t col) {
    const std::string& var = statsAcross ? vars[row] : vars[col];
    const std::string& key = statsAcross ? statKeys[col] : statKeys[row];
    if (statsObj.is_object() && statsObj.contains(var) && statsObj[var].is_object() &&
        statsObj[var].contains(key) && statsObj[var][key].is_number())
      return statsObj[var][key].get<double>();
    return std::numeric_limits<double>::quiet_NaN();
  };
  const auto cellText = [&](const ojson& statsObj, size_t row, size_t col) {
    const double value = statAt(statsObj, row, col);
    if (std::isnan(value)) return tsPadLeft(".", cellWidth);
    const TabstatCellStyle& style = styles[statsAcross ? row : col];
    return tsPadLeft(tsFormatted(value, style.fmt, style.width), cellWidth);
  };

  const ojson* totalStats =
      !noTotal && tsBlock.contains("total") && tsBlock["total"].is_object() ? &tsBlock["total"] : nullptr;

  std::string text = look.notes;
  text += "\n";
  if (byMode && !longStub) {
    const auto joined = [](const std::vector<std::string>& words, const char* between) {
      std::string line;
      for (size_t i = 0; i < words.size(); ++i) line += (i == 0 ? "" : between) + words[i];
      return line;
    };
    text += statsAcross ? "Summary for variables: " + joined(vars, " ")
                        : "Summary statistics: " + joined(statRows, ", ");
    text += "\nGroup variable: " + *byVal + " ";
    const auto meta = session.varMeta.find(*byVal);
    if (meta != session.varMeta.end() && meta->second.label.has_value() && !meta->second.label->empty())
      text += "(" + *meta->second.label + ")";
    text += "\n\n";
  }

  const size_t usable = kTabstatLineWidth - std::min(kTabstatLineWidth, stubWidth + 2);
  const size_t perBlock = std::max<size_t>(1, usable / cellWidth);
  for (size_t from = 0; from < columnCount || from == 0; from += perBlock) {
    if (from > 0) text += "\n";
    const size_t to = std::min(columnCount, from + perBlock);
    const size_t bandWidth = cellWidth * (to - from);
    const std::string rule = std::string(stubWidth + 1, '-') + "+" + std::string(bandWidth, '-') + "\n";

    text += headStub + " |";
    for (size_t col = from; col < to; ++col)
      text += tsPadLeft(statsAcross ? statRows[col] : tsShortName(vars[col], 8), cellWidth);
    text += "\n" + rule;

    const auto emitRows = [&](const std::string& label, const ojson& statsObj, bool overflowOk) {
      for (size_t row = 0; row < rowCount; ++row) {
        text += bodyStub(row == 0 ? label : std::string(), row, overflowOk) + " |";
        for (size_t col = from; col < to; ++col) text += cellText(statsObj, row, col);
        text += "\n";
      }
    };
    if (!byMode) {
      if (totalStats != nullptr) emitRows(std::string(), *totalStats, false);
    } else {
      const bool ruleBetween = rowCount > 1 && look.separators;
      for (size_t g = 0; g < groups.size(); ++g) {
        if (g > 0 && ruleBetween) text += rule;
        const ojson& group = groups[g];
        const std::string label =
            group.contains("value") ? byColumn.groupText(group["value"]) : std::string();
        emitRows(label, group.contains("stats") ? group["stats"] : emptyList, !byColumn.clipGroups);
      }
      if (totalStats != nullptr) {
        text += rule;
        emitRows("Total", *totalStats, false);
      }
    }
    text += std::string(stubWidth + 2 + bandWidth, '-') + "\n";
    if (columnCount == 0) break;
  }
  return text;
}


// ---------------------------------------------------------------------------
// B4-W3b: `tabulate VAR, generate(stub)`.
//
// Returns a null json on success, or the error block to emit. The dummies are
// built by dispatching the user's own `generate` (BUG-1012: it was
// piet::handlers::runGen, which skips the string row evaluator) rather than a
// private column writer, so the storage plane, the resident-frame opt-in and
// the identifier validation are the ones already in service and cannot drift
// from them.
//
// BUG-1012 (StataBE 19.5, w2as_summary_stats_family):
//  - the sample: a row outside `if`/`in` gets missing in every dummy, as a row
//    whose source is missing does (sx_tab_g_q, sx_tab_generate: 0 before);
//  - with `missing` the missing rows are a category of their own: that dummy is
//    1 there and every other dummy 0 (sx_tab_gen_m);
//  - a string variable works (sx_tab_g, sx_tab_gen_m: "" is its missing); it
//    failed in the engine with rc 1 before;
//  - the label is `var==` + the value label when the level has one (sx_tab_gen,
//    sx_tabulate_generate: `g==alpha`, `d==no`), the string itself for a string
//    variable (`s==a`), else the value as %11.4f (`z==     3.0000`,
//    `aw==     0.5000`; the label read back through `local : variable label`).
nlohmann::ordered_json runTabulateGenerate(const std::string& stub,
                                           const std::vector<std::string>& varlist,
                                           const std::vector<std::string>& rawRows,
                                           SessionState& session,
                                           const std::optional<std::string>& ifExpr,
                                           const std::optional<std::string>& inRange,
                                           bool includeMissing) {
  using ojson = nlohmann::ordered_json;
  if (varlist.size() != 1) {
    // Stata's `generate()` is a ONE-WAY tabulate option; the two-way syntax
    // does not carry it. Refusing by name rather than inventing a meaning for
    // the cross-tabulated case.
    return ojson(eval::errorBlock(
        "unsupported_option",
        "tabulate: generate() is a one-way option — it makes one indicator per level of a "
        "single variable, and a two-way table has no single level list to make them from."));
  }
  if (!strutil::isValidStataIdentifier(stub)) {
    return ojson(eval::errorBlock(
        "syntax_error",
        "tabulate: invalid variable name `" + stub + "' — " + strutil::stataIdentifierRuleText()));
  }
  if (rawRows.empty()) {
    return ojson(eval::errorBlock("bad_input",
                                  "tabulate: generate() has no levels to make indicators from."));
  }
  // Stata's own guard, and it fires BEFORE any column is written: a collision
  // half-way through would leave the dataset holding stub1..stub3 of a set of
  // five, which is worse than the error.
  Datastore* ds = session.datastore();
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    bool exists = false;
    if (const auto* card = session.shadowVarlistIfCurrent()) {
      for (const auto& info : *card)
        if (info.name == nm) exists = true;
    } else if (ds != nullptr) {
      exists = ds->has(nm);
    }
    if (exists) {
      return ojson(eval::errorBlock("bad_input", "variable " + nm + " already defined"));
    }
  }

  const std::string& srcVar = varlist[0];
  const bool srcIsString = ds != nullptr && ds->has(srcVar) && ds->dtype(srcVar) == "factor";
  // The sample: the `if`/`in` qualifier as typed, on each generate.
  std::string qual;
  if (ifExpr.has_value() && !strutil::trim(*ifExpr).empty()) qual += " if " + *ifExpr;
  if (inRange.has_value() && !strutil::trim(*inRange).empty()) qual += " in " + *inRange;
  const std::map<int, std::string>* valueLabels = nullptr;
  if (!srcIsString) {
    auto it = session.valueLabels.find(srcVar);
    if (it != session.valueLabels.end()) valueLabels = &it->second;
  }
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    const std::string& lvl = rawRows[i];
    // A numeric level key is the value's round-trip rendering, or `.`/`.a` for a
    // missing category (runTabulate with `missing`); a string level is the text.
    const bool missingLevel = !srcIsString && !lvl.empty() && lvl[0] == '.' &&
                              (lvl.size() == 1 || (lvl.size() == 2 && std::islower(
                                                       static_cast<unsigned char>(lvl[1]))));
    std::string escaped;
    for (char ch : lvl) {
      if (ch == '"' || ch == '\\') escaped += '\\';
      escaped += ch;
    }
    const std::string cmp = srcIsString ? (srcVar + "==\"" + escaped + "\"") : (srcVar + "==" + lvl);
    // The missing rule (oracle pins_oracle_g.log:170-176): without `missing`,
    // a missing source gives a missing dummy, never 0.
    const std::string missTest = srcIsString ? (srcVar + "==\"\"") : ("missing(" + srcVar + ")");
    const std::string rhs = includeMissing ? cmp : ("cond(" + missTest + ", ., " + cmp + ")");
    // Through the user's own `generate` (a string comparison takes the string
    // row evaluator there, `in` its _n path); runGen alone sent `s==""` to the
    // engine, which compares a string's missing code unequal to "".
    ojson made = dispatch("quietly generate byte " + nm + " = " + rhs + qual, session);
    if (made.is_array())
      for (const auto& b : made)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
    // Stata makes these `byte`, formats them %8.0g, and labels them `var==`
    // followed by the level's value label, the string, or the value as %11.4f.
    // BUG-1012: the width is 11, read back through `local : variable label`
    // (w2as sx_tab_g_q_v1 `z==     3.0000`, sx_tab_generate_v1
    // `aw==     0.5000`). pins_shapes.log:36 shows `rep78== 1.0000` because
    // `describe` prints the label with its runs of blanks collapsed; the
    // earlier `%7.4f` reading of that line was wrong.
    session.varMeta[nm].stataType = "byte";
    std::string shown = lvl;
    if (!srcIsString) {
      double v = 0;
      bool haveValue = false;
      try {
        v = std::stod(lvl);
        haveValue = !missingLevel;
      } catch (...) {
      }
      const double rv = std::nearbyint(v);
      std::map<int, std::string>::const_iterator vl;
      if (haveValue && valueLabels != nullptr && v == rv && std::fabs(v) < 2147483648.0 &&
          (vl = valueLabels->find(static_cast<int>(rv))) != valueLabels->end()) {
        shown = vl->second;
      } else {
        // BUG-1152: %11.4f as Stata renders it, so a value too wide for it goes
        // to e-notation in the same 11 columns (`z==-1.2346e+05`, where printf
        // wrote 12); applyStataFmt is string()'s rendering, so the field's
        // leading blanks are put back. A missing category: an extended `.a`
        // takes those 11 columns, the system `.` only 7 (StataBE 19.5:
        // `z==         .a` and `z==      .`, for a %9.0g byte and a %20.0g
        // double alike).
        if (haveValue) {
          shown = eval::applyStataFmt(v, "%11.4f");
          if (shown.size() < 11) shown.insert(0, 11 - shown.size(), ' ');
        } else {
          char buf[64];
          std::snprintf(buf, sizeof(buf), lvl == "." ? "%7s" : "%11s", lvl.c_str());
          shown = buf;
        }
      }
    }
    session.setVarLabel(nm, variableLabelText(srcVar + "==" + shown));
  }
  return ojson();  // null => no error
}

DescribeShape describeDatasetShape(SessionState& session) {
  std::vector<std::pair<std::string, bool>> vars;  // name, string-typed
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& v : *card) vars.emplace_back(v.name, v.dtype == "factor");
  } else if (Datastore* ds = session.datastore()) {
    for (const auto& n : ds->names()) vars.emplace_back(n, ds->dtype(n) == "factor");
  }
  DescribeShape shape;
  shape.k = vars.size();
  double width = 0.0;
  for (const auto& [name, isString] : vars) {
    const auto meta = session.varMeta.find(name);
    const std::string t = meta != session.varMeta.end() && meta->second.stataType.has_value()
                              ? *meta->second.stataType
                              : (isString ? "str#" : "float");
    if (t == "byte") width += 1;
    else if (t == "int") width += 2;
    else if (t == "long" || t == "float") width += 4;
    else if (t == "double" || t == "strL") width += 8;
    else if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
             t.find_first_not_of("0123456789", 3) == std::string::npos)
      width += std::stod(t.substr(3));
    else return shape;
  }
  shape.width = width;
  return shape;
}


// The r() a summarize block leaves behind, shared by the plain `summarize` arm and
// the by-prefixed driver (which captures only the LAST group's block, as Stata does).
void captureSummarizeR(const ojson& summBlock, bool meanonly, bool detail,
                              SessionState& session) {
  eval::captureRFromBlock(summBlock, session.eval);
  // BUG-476: `meanonly` leaves r(sd) (and everything captureRFromBlock
  // DERIVES from sd — r(Var), computed as sd*sd when sd was present but
  // Var was not; macros.cpp's captureRFromBlock, "Var = sd^2" branch —
  // and r(median), the p50 alias) Stata-missing. ONLY r(sd) is oracle-
  // measured (BUG-476.do's own header: "do not add an arm for it
  // without capturing it first" for r(min)/r(max)); the percentile/
  // skewness/kurtosis keys are pruned here too because they are NEVER
  // computed under Stata's real meanonly (undisputed — meanonly skips
  // the whole variance/percentile pass), not because a cell pins them.
  // NOT erased: Stata's own convention (and the BUG-476 probe's `scalar
  // b476sd = r(sd)` / `assert b476sd >= .`, which must run to rc 0, not
  // error) is that a numeric r()-result the last command did not compute
  // reads back as MISSING, not "not found" — real Stata's `display
  // r(sd)` after `summarize ..., meanonly` succeeds and shows `.`
  // (oracle_r.log:71's `%21x r(sd)` = `+1.0000000000000X+3ff`, exactly
  // 2^1023 = kStataMissingSentinel). Erasing the key instead would make
  // PIET's own command-line r()-read path answer the honest-but-wrong
  // "r(sd) is not set" error (macros.cpp's unsetRAsMissing guard, the
  // FIX-RESTIMATE precedent) — so these are SET to the sentinel, not
  // removed. The RENDERED block (summBlock itself) is untouched — only
  // the r() store captureRFromBlock just built is overwritten, so
  // freeze/summarize.json and every other summarize fixture's displayed
  // table stays byte-identical.
  if (meanonly && summBlock.value("type", std::string()) != "error") {
    for (const char* k : {"sd", "Var", "p1", "p5", "p10", "p25", "p50", "p75",
                           "p90", "p95", "p99", "skewness", "kurtosis"}) {
      session.eval.rValues.set(k, eval::kStataMissingSentinel);
    }
  }
  // The names Stata posts for this invocation (summarize's help; meanonly
  // as well): an absent one is a named r(111), any other absent name `.`
  // (RULING.md C.1, D.3.2).
  if (summBlock.value("type", std::string()) != "error") {
    // BUG-1511: on an empty sample (a string variable too) Stata posts r(N),
    // r(sum_w) and r(sum) and nothing else, with or without detail or meanonly
    // (StataBE 19.5, scratch-laneA2/agents/rprog/x1511.do: `return list` lists
    // those three, in that order; r(mean), r(p50) then read `.` like any name the
    // command does not post). The block's detail percentiles and moments on no
    // rows (0) are not Stata's, nor the meanonly missings set above.
    const double* nObs = session.eval.rValues.find("N");
    if (nObs != nullptr && *nObs == 0.0) {
      eval::OrderedFloatDict posted;  // `return list` lists the newest first
      for (const char* k : {"sum", "sum_w", "N"})
        if (const double* v = session.eval.rValues.find(k)) posted.set(k, *v);
      session.eval.rValues = std::move(posted);
      eval::declareRPosted(session.eval, "summarize", {"N", "sum_w", "sum"});
      return;
    }
    std::vector<std::string> stataNames = {"N", "mean", "min", "max", "sum_w", "sum"};
    if (!meanonly) stataNames.insert(stataNames.end(), {"Var", "sd"});
    if (detail)
      stataNames.insert(stataNames.end(), {"skewness", "kurtosis", "p1", "p5", "p10", "p25",
                                           "p50", "p75", "p90", "p95", "p99"});
    // A name Stata posts but whose value is undefined here (N 1: Var, sd,
    // skewness, kurtosis) is posted as missing, as Stata posts it (BUG-1013; w2as
    // sx_sum_d_single). The block carries those as null, which captureRFromBlock
    // skips, so they were refused as unposted.
    for (const auto& n : stataNames)
      if (session.eval.rValues.find(n) == nullptr)
        session.eval.rValues.set(n, eval::kStataMissingSentinel);
    eval::declareRPosted(session.eval, "summarize", std::move(stataNames));
  }
}
}  // namespace piet::dispatch_detail
