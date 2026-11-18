// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// panel descriptive statistics.
#include "datamgmt/data_combine_internal.h"
#include "piet/handlers/descriptive.h"
#include "piet/inrange_filter.h"
namespace piet::handlers::data_combine_detail {


// BUG-371: `inspect VARNAME` — Stata's quick "what is in this column" readout:
// how many values are negative / zero / positive, how many are integers vs not,
// how many are missing, and how many distinct values there are. It is pure
// counting over one mmap'd column, so it is PIET-native for the same reason
// `isid` is (shelling out would cost a scratch .hbin to answer an arithmetic
// question). Emitted as a `display` text block: the frontend has no inspect
// renderer, and a text block renders TODAY rather than degrading to a JSON echo.

// Stata's %9.0g rendering: 7 significant digits, trailing zeros trimmed, and the
// leading zero dropped for |v| < 1 (Stata writes .4780935, never 0.4780935).
// Checked against eight values in the live StataBE capture
// (internal capture conventions.log): .4780935, 1.674907, 5.263916,
// -.4077221, .424569, 3.912023, .29266, 4.78367 — all exact.
// BUG-1126: the shared %9.0g rule (stata_format.h general), not printf's %.7g,
// whose own small and large sides are not Stata's: xtsum printed 1e-06, 2e-05,
// 1.234568e+08 and 8.582929e-06 where StataBE prints 1.00e-06, .00002,
// 1.23e+08 and 8.58e-06 (and .0000115, -1.69e-21) in the same cells.
std::string stataG(double v) {
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 9, 0);
}


// Sample SD (ddof=1), matching Stata's summarize. Fewer than two values -> missing.
double sampleSdDV(const std::vector<double>& v) {
  if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0;
  for (double q : v) sum += q;
  const double mean = sum / static_cast<double>(v.size());
  double ss = 0;
  for (double q : v) ss += (q - mean) * (q - mean);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}
}
namespace piet::handlers {
using namespace data_combine_detail;


namespace {
// One variable's table and r() set (the observations `ds` holds).
ojson inspectOneVariable(const std::string& name, Datastore* ds, SessionState& session) {
  const int64_t n = ds->nRows();
  // inspect counts a string variable as all missing: nothing to classify, N_unique 0.
  const bool isStr = ds->dtype(name) != "f64";
  const std::vector<double> noValues(isStr ? static_cast<size_t>(n) : 0,
                                     std::numeric_limits<double>::quiet_NaN());
  const double* x = isStr ? noValues.data() : ds->col(name).f64();
  int64_t neg = 0, zero = 0, pos = 0, ints = 0, nonints = 0, miss = 0;
  int64_t negint = 0, posint = 0;
  std::set<double> uniq;
  bool uniqCapped = false;
  for (int64_t i = 0; i < n; ++i) {
    const double v = x[i];
    if (std::isnan(v)) { ++miss; continue; }
    if (v < 0) ++neg; else if (v == 0) ++zero; else ++pos;
    if (v == std::floor(v)) {
      ++ints;
      // BUG-373: Stata splits the integer count by SIGN too (r(N_negint) /
      // r(N_posint)), so track them here rather than deriving them later.
      if (v < 0) ++negint; else if (v > 0) ++posint;
    } else {
      ++nonints;
    }
    // Same 65536 cap `codebook` uses for its unique count — past that the number
    // stops being informative and starts being a memory bill.
    if (!uniqCapped) {
      uniq.insert(v);
      if (uniq.size() > 65536) uniqCapped = true;
    }
  }
  const int64_t total = n - miss;

  // BUG-377: this used to print a single count column with PERCENTAGES. Real
  // Stata prints three COUNT columns — Total / Integers / Nonintegers — with "-"
  // for a zero cell and no percentages anywhere. Captured live
  // (internal capture conventions.log, section Q3) on `inspect mpg`:
  //
  //   mpg:  Mileage (mpg)                             Number of observations
  //   -------------------                    ---------------------------------------
  //                                                Total      Integers   Nonintegers
  //   |      #                     Negative            -             -             -
  //   ...
  //     (21 unique values)
  //
  // The ASCII histogram in Stata's left panel is deliberately NOT reproduced: its
  // binning rule is not derivable from one capture, and a plausible-looking wrong
  // histogram is worse than an absent one. The numbers — which is what a reader
  // acts on — are exact.
  auto cell = [](int64_t k) { return k == 0 ? std::string("-") : std::to_string(k); };
  auto row3 = [&cell](const char* label, int64_t tot, int64_t ints_, int64_t non) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", label, cell(tot).c_str(),
                  cell(ints_).c_str(), cell(non).c_str());
    return std::string(buf);
  };
  // Split each sign bucket by integer-ness so the three columns add up.
  const int64_t negNon = neg - negint, posNon = pos - posint;
  const int64_t zeroInt = zero, zeroNon = 0;  // 0 is an integer
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s:%*sNumber of observations", name.c_str(),
                  static_cast<int>(name.size() < 34 ? 36 - name.size() : 2), "");
    lines.push_back(buf);
    lines.push_back("  ----------------------------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", "", "Total", "Integers",
                  "Nonintegers");
    lines.push_back(buf);
  }
  lines.push_back(row3("Negative", neg, negint, negNon));
  lines.push_back(row3("Zero", zero, zeroInt, zeroNon));
  lines.push_back(row3("Positive", pos, posint, posNon));
  lines.push_back("                           -----------   -----------   -----------");
  lines.push_back(row3("Total", total, ints, nonints));
  {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s", "Missing", cell(miss).c_str());
    lines.push_back(std::string(buf));
    lines.push_back("                           -----------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11lld", "", static_cast<long long>(n));
    lines.push_back(std::string(buf));
    std::snprintf(buf, sizeof(buf), "  (%s unique values)",
                  uniqCapped ? ">65536" : std::to_string(uniq.size()).c_str());
    lines.push_back(std::string(buf));
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // BUG-373: the counts ride alongside the rendered text so the dispatcher can
  // publish them as r(). Stata's inspect is r-class and this one published
  // nothing — measured by the declared-returns sweep the day AFTER it shipped.
  ojson rcls = ojson::object();
  rcls["N"] = static_cast<double>(total);
  rcls["N_neg"] = static_cast<double>(neg);
  rcls["N_0"] = static_cast<double>(zero);
  rcls["N_pos"] = static_cast<double>(pos);
  rcls["N_negint"] = static_cast<double>(negint);
  rcls["N_posint"] = static_cast<double>(posint);
  // BUG-1453 (StataBE 19.5, scratch-laneA/lead/insp/i.do, i2.do): r(N_unique) is
  // the distinct count up to 99 and missing from 100 on (99 -> 99, 100 -> .);
  // r(N_undoc), only for a variable with a value label attached, counts the
  // nonmissing observations whose value the label does not name (a noninteger
  // included: 72 of 117), and is missing otherwise.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  rcls["N_unique"] = (!uniqCapped && uniq.size() <= 99) ? static_cast<double>(uniq.size()) : nan;
  double undoc = nan;
  if (session.varLblname.count(name) != 0) {
    const auto lit = session.valueLabels.find(name);
    int64_t k = 0;
    for (int64_t i = 0; i < n; ++i) {
      const double v = x[i];
      if (std::isnan(v)) continue;
      const bool labelled = v == std::floor(v) && std::fabs(v) < 2147483648.0 &&
                            lit != session.valueLabels.end() &&
                            lit->second.count(static_cast<int>(v)) != 0;
      if (!labelled) ++k;
    }
    undoc = static_cast<double>(k);
  }
  rcls["N_undoc"] = undoc;
  out["inspect"] = rcls;
  return out;
}
}  // namespace

ojson runInspect(const std::vector<std::string>& varlist, const std::optional<std::string>& ifExpr,
                 const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  for (const auto& name : varlist)
    if (!ds->has(name)) return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  const std::vector<std::string> names = varlist.empty() ? ds->names() : varlist;
  // `if` / `in`: the tables describe the selected observations.
  std::unique_ptr<Datastore> selected;
  Datastore* data = ds;
  const bool hasIf = ifExpr.has_value() && !ifExpr->empty();
  if (hasIf || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, session.currentHbin().value_or(ds->path()),
                              hasIf ? ifExpr : std::nullopt, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorMessage(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(""), "inspect");
    if (fr.dataPath != ds->path()) session.noteProjectionScratch(fr.dataPath);
    selected = std::make_unique<Datastore>(fr.dataPath);
    data = selected.get();
  }
  ojson last;
  std::string text;
  for (size_t i = 0; i < names.size(); ++i) {
    ojson one = inspectOneVariable(names[i], data, session);
    if (!one.is_object() || one.value("type", std::string()) == "error") return one;
    if (i) text += "\n\n";
    text += one.value("text", std::string());
    last = std::move(one);
  }
  if (names.empty()) return eval::noDataLoaded();
  last["text"] = text;  // r() is the last variable's
  return last;
}


// Stata's abbrev(name, 8) as xtsum prints a variable's name: a name past 8 characters keeps its
// first characters, `~`, and its tail, the tail being the whole run of trailing digits when the
// name ends in digits (`fatal1517` -> `fat~1517`, `ttl_exp68` -> `ttl_e~68`) and its last
// character otherwise (`mileage_per_gallon` -> `mileage~n`).
static std::string xtsumAbbrev(const std::string& name, size_t width) {
  const size_t chars = cecon::ident::codePointCount(name);
  if (chars <= width) return name;
  size_t tailBytes = 0;
  while (tailBytes < name.size() && std::isdigit(static_cast<unsigned char>(name[name.size() - 1 - tailBytes])))
    ++tailBytes;
  if (tailBytes == 0) {
    size_t last = name.size() - 1;
    while (last > 0 && (static_cast<unsigned char>(name[last]) & 0xC0) == 0x80) --last;
    tailBytes = name.size() - last;
  }
  const std::string tail = name.substr(name.size() - tailBytes);
  const size_t tailChars = cecon::ident::codePointCount(tail);
  const size_t headChars = tailChars + 1 < width ? width - 1 - tailChars : 1;
  return cecon::ident::truncateCodePoints(name, headChars) + "~" + tail;
}

// xtsum.ado's Variable table, one variable at a time, and the r() it leaves for the last one.
// `idOverride` is i(); with none the xtset panel variable. `if` and `in` choose the rows. A
// string variable is not refused: it is summarised as all missing (N = 0, n = 0), the way the
// ado does it.
ojson runXtsum(const std::vector<std::string>& varlistIn, SessionState& session,
               const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
               const std::string& idOverride) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xtsum.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  std::string idvar = idOverride;
  if (idvar.empty()) {
    if (!session.panelvar.has_value() || session.panelvar->empty())
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                    std::nullopt, std::nullopt, "r(459)"));
    idvar = *session.panelvar;
  }
  if (!ds->has(idvar))
    return ojson(eval::errorBlock("engine_error", "variable " + idvar + " not found", std::nullopt,
                                  std::nullopt, std::nullopt, "r(111)"));
  if (ds->dtype(idvar) != "f64")
    return engineErrorBlock("xtsum: panel variable " + idvar + " must be numeric", "bad_input");
  std::vector<std::string> varlist = varlistIn;
  if (varlist.empty()) varlist = ds->names();  // syntax [varlist]: every variable
  for (const auto& v : varlist)
    if (!ds->has(v))
      return ojson(eval::errorBlock("engine_error", "variable " + v + " not found", std::nullopt,
                                    std::nullopt, std::nullopt, "r(111)"));

  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, ds->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorMessage(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(""), "xtsum");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  const int64_t n = ds->nRows();
  const double* id = ds->col(idvar).f64();
  // The ado sorts by the panel and time variables; the last row of each panel is where it
  // reads T_i from. Rows are put in that order (data order within ties).
  std::vector<int64_t> order(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
  const double* tv = nullptr;
  if (session.timevar.has_value() && ds->has(*session.timevar) && ds->dtype(*session.timevar) == "f64")
    tv = ds->col(*session.timevar).f64();
  std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
    const double ia = id[a], ib = id[b];
    if (ia != ib) return ia < ib;
    return tv != nullptr && tv[a] < tv[b];
  });

  std::vector<std::string> lines;
  lines.push_back("Variable         |      Mean   Std. dev.       Min        Max |    Observations");
  lines.push_back("-----------------+--------------------------------------------+----------------");
  ojson stats = ojson::object();
  const double NA = std::numeric_limits<double>::quiet_NaN();
  const auto colPad = [](std::string s, size_t col) {  // _col(col): pad to start at 1-based col
    const size_t have = cecon::ident::codePointCount(s);
    if (have < col - 1) s += std::string(col - 1 - have, ' ');
    return s;
  };
  const auto g9 = [](double v) {
    const std::string t = stataG(v);
    const size_t w = cecon::ident::codePointCount(t);
    return w >= 9 ? t : std::string(9 - w, ' ') + t;
  };
  const auto g8 = [](double v) {
    const std::string t = std::isnan(v) ? std::string(".") : piet::fmt::general(v, 8, 0);
    const size_t w = cecon::ident::codePointCount(t);
    return w >= 8 ? t : std::string(8 - w, ' ') + t;
  };
  const auto f8 = [](int64_t v) {
    const std::string t = std::to_string(v);
    return t.size() >= 8 ? t : std::string(8 - t.size(), ' ') + t;
  };

  for (size_t vi = 0; vi < varlist.size(); ++vi) {
    const std::string& name = varlist[vi];
    const bool isString = ds->dtype(name) == "factor";
    int64_t N = 0, units = 0;
    double gmean = NA, osd = NA, omin = NA, omax = NA, bsd = NA, bmin = NA, bmax = NA;
    double wsd = NA, wmin = NA, wmax = NA, tbar = NA;
    bool balanced = true;
    if (!isString) {
      const double* x = ds->col(name).f64();
      // Per panel, in sorted order: sum, count over rows with x present, and whether the last
      // row of the panel has x present (the ado reads T_i there).
      struct Unit { double sum = 0.0; int64_t cnt = 0; bool lastOk = false; };
      std::map<double, Unit> acc;
      std::vector<double> all;
      for (int64_t k = 0; k < n; ++k) {
        const int64_t i = order[static_cast<size_t>(k)];
        if (std::isnan(id[i])) continue;
        Unit& u = acc[id[i]];
        const bool ok = !std::isnan(x[i]);
        u.lastOk = ok;  // overwritten until the panel's last row
        if (!ok) continue;
        all.push_back(x[i]);
        u.sum += x[i];
        u.cnt += 1;
      }
      N = static_cast<int64_t>(all.size());
      double sum = 0.0;
      for (double q : all) sum += q;
      if (N > 0) {
        gmean = sum / static_cast<double>(N);
        omin = *std::min_element(all.begin(), all.end());
        omax = *std::max_element(all.begin(), all.end());
      }
      osd = N > 1 ? sampleSdDV(all) : NA;
      std::vector<double> means;
      int64_t tMin = std::numeric_limits<int64_t>::max(), tMax = -1;
      for (const auto& kv : acc) {
        if (kv.second.cnt > 0) means.push_back(kv.second.sum / static_cast<double>(kv.second.cnt));
        if (kv.second.lastOk) {
          tMin = std::min(tMin, kv.second.cnt);
          tMax = std::max(tMax, kv.second.cnt);
        }
      }
      balanced = tMax < 0 || tMin == tMax;
      units = static_cast<int64_t>(means.size());
      if (!means.empty()) {
        bmin = *std::min_element(means.begin(), means.end());
        bmax = *std::max_element(means.begin(), means.end());
        bsd = units > 1 ? sampleSdDV(means) : NA;
      }
      std::vector<double> within;
      within.reserve(all.size());
      for (int64_t i = 0; i < n; ++i) {
        if (std::isnan(x[i]) || std::isnan(id[i])) continue;
        const Unit& u = acc[id[i]];
        within.push_back(x[i] - u.sum / static_cast<double>(u.cnt));
      }
      if (!within.empty()) {
        wmin = *std::min_element(within.begin(), within.end()) + gmean;
        wmax = *std::max_element(within.begin(), within.end()) + gmean;
        wsd = within.size() > 1 ? sampleSdDV(within) : NA;
      }
      if (units > 0) tbar = static_cast<double>(N) / static_cast<double>(units);
    }

    // Each line is laid out by column, as the ado's _col() does: the table body to col 63, the
    // Observations column from col 69.
    {
      std::string left = colPad(xtsumAbbrev(name, 8), 10) + "overall | " + g9(gmean) + "  " + g9(osd) + "  " +
                         g9(omin) + "  " + g9(omax) + " |";
      lines.push_back(colPad(left, 69) + "N =" + f8(N));
    }
    {
      std::string left = colPad("", 10) + "between |" + std::string(0, ' ');
      left = colPad(left, 31) + g9(bsd) + "  " + g9(bmin) + "  " + g9(bmax) + " |";
      lines.push_back(colPad(left, 69) + "n =" + f8(units));
    }
    {
      std::string left = colPad("", 10) + "within  |";
      left = colPad(left, 31) + g9(wsd) + "  " + g9(wmin) + "  " + g9(wmax) + " | " +
             (balanced ? "    T" : "T-bar") + " =" + g8(tbar);
      lines.push_back(left);
    }
    if (vi + 1 < varlist.size()) lines.push_back(colPad("", 18) + "|" + std::string(44, ' ') + "|");

    ojson e;
    e["N"] = N;
    e["n_units"] = units;
    e["T_bar"] = tbar;
    e["mean"] = gmean;
    e["sd_overall"] = osd;
    e["min_overall"] = omin;
    e["max_overall"] = omax;
    e["sd_between"] = bsd;
    e["min_between"] = bmin;
    e["max_between"] = bmax;
    e["sd_within"] = wsd;
    e["min_within"] = wmin;
    e["max_within"] = wmax;
    stats[name] = e;
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // The numbers ride along beside the rendered text so a consumer (and the
  // selftest lock) can assert on values rather than on formatting.
  out["xtsum"] = stats;
  out["xtsum_last"] = varlist.back();
  return out;
}


ojson runXttab(const std::vector<std::string>& varlist, SessionState& session,
               const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
               const std::string& idOverride) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xttab.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  std::string idvar = idOverride;
  if (idvar.empty()) {
    if (!session.panelvar.has_value() || session.panelvar->empty())
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                    std::nullopt, std::nullopt, "r(459)"));
    idvar = *session.panelvar;
  }
  if (varlist.size() != 1)
    return ojson(eval::errorBlock("syntax_error", "too many variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(103)"));  // vs-stata bank: `xttab a b`
  const std::string& name = varlist[0];
  if (!ds->has(name)) return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (!ds->has(idvar)) return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return ojson(eval::errorBlock("engine_error", name + " must be numeric", std::nullopt, std::nullopt,
                                  std::nullopt, "r(109)"));  // vs-stata bank: `xttab jail`

  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, ds->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorMessage(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(""), "xttab");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
  const double* id = ds->col(idvar).f64();

  std::map<double, int64_t> overall;                      // value -> rows
  std::map<double, std::set<double>> everUnits;           // value -> units taking it at least once
  std::map<double, std::map<double, int64_t>> perUnitHits;  // value -> unit -> rows with it
  std::map<double, int64_t> unitRows;                     // unit -> its non-missing rows
  std::set<double> allUnits;
  int64_t N = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(x[i]) || std::isnan(id[i])) continue;
    ++N;
    ++overall[x[i]];
    everUnits[x[i]].insert(id[i]);
    ++perUnitHits[x[i]][id[i]];
    ++unitRows[id[i]];
    allUnits.insert(id[i]);
  }
  if (N == 0)
    return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                  std::nullopt, "r(2000)"));
  const int64_t nUnits = static_cast<int64_t>(allUnits.size());

  // xttab.ado's table (StataBE 19.5, bank cells `xttab dry`, `xttab jl`):
  //
  //                     Overall             Between            Within
  //        race |    Freq.  Percent      Freq.  Percent        Percent
  //   ----------+-----------------------------------------------------
  //       White |   20180     70.72      3329     70.66         100.00
  //   ----------+-----------------------------------------------------
  //       Total |   28534    100.00      4711    100.00         100.00
  //                                 (n = 4711)
  //
  // The BETWEEN column counts UNITS. A value-labelled variable shows its label cut to 8
  // characters (`decode, maxlen(8)`), right-aligned in 9; otherwise the value as %9.0g.
  const auto padL = [](const std::string& t, size_t w) {
    const size_t have = cecon::ident::codePointCount(t);
    return have >= w ? t : std::string(w - have, ' ') + t;
  };
  const auto fix = [](double v, int d) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", d, v);
    return std::string(b);
  };
  const std::map<int, std::string>* labels = nullptr;
  {
    const auto lit = session.valueLabels.find(name);
    if (lit != session.valueLabels.end()) labels = &lit->second;
  }
  std::vector<std::string> lines;
  lines.push_back("");
  lines.push_back(std::string(18, ' ') + "Overall" + std::string(13, ' ') + "Between" +
                  std::string(12, ' ') + "Within");
  lines.push_back(padL(xtsumAbbrev(name, 9), 9) + " |    Freq.  Percent" +
                  std::string(6, ' ') + "Freq.  Percent" + std::string(8, ' ') + "Percent");
  lines.push_back(std::string(10, '-') + "+" + std::string(53, '-'));
  ojson rows = ojson::array();
  ojson results = ojson::array();
  double sumUnits = 0.0;
  std::vector<double> unitsPerValue, withinPerValue;
  for (const auto& kv : overall) {
    const double val = kv.first;
    const int64_t freq = kv.second;
    const int64_t units = static_cast<int64_t>(everUnits[val].size());
    // Within% is the AVERAGE over the units that ever take this value of the
    // share of that unit's observations holding it — not freq/N restricted, which
    // is a different (and wrong) number when units have unequal T.
    double withinPct = std::numeric_limits<double>::quiet_NaN();
    if (units > 0) {
      double acc = 0;
      for (const auto& uk : perUnitHits[val])
        acc += static_cast<double>(uk.second) / static_cast<double>(unitRows[uk.first]);
      withinPct = 100.0 * acc / static_cast<double>(units);
    }
    const double ovPct = 100.0 * static_cast<double>(freq) / static_cast<double>(N);
    const double btPct = 100.0 * static_cast<double>(units) / static_cast<double>(nUnits);
    std::string shown;
    if (labels != nullptr && val == std::floor(val) && labels->count(static_cast<int>(val)) != 0) {
      shown = padL(cecon::ident::truncateCodePoints(labels->at(static_cast<int>(val)), 8), 9);
    } else {
      shown = padL(stataG(val), 9);
    }
    lines.push_back(shown + " |" + padL(std::to_string(freq), 8) + padL(fix(ovPct, 2), 10) +
                    padL(std::to_string(units), 10) + padL(fix(btPct, 2), 10) + " " +
                    padL(fix(withinPct, 2), 14));
    ojson r;
    r["value"] = val;
    r["freq"] = freq;
    r["percent"] = ovPct;
    r["units"] = units;
    r["units_percent"] = btPct;
    r["within_percent"] = withinPct;
    rows.push_back(r);
    results.push_back(ojson::array({val, static_cast<double>(freq), ovPct, static_cast<double>(units), btPct,
                                    withinPct}));
    sumUnits += static_cast<double>(units);
    unitsPerValue.push_back(static_cast<double>(units));
    withinPerValue.push_back(withinPct);
  }
  // The Total row: units are counted once per value they take, so the Between percent can pass
  // 100; the Within total is the average of the Within percents weighted by those unit counts.
  double overp = 0.0;
  for (size_t i = 0; i < unitsPerValue.size(); ++i)
    overp += (unitsPerValue[i] / sumUnits) * 100.0 * (withinPerValue[i] / 100.0);
  const double totalBetweenPct = 100.0 * sumUnits / static_cast<double>(nUnits);
  results.push_back(ojson::array({std::numeric_limits<double>::quiet_NaN(), static_cast<double>(N), 100.0,
                                  sumUnits, totalBetweenPct, overp}));
  lines.push_back(std::string(10, '-') + "+" + std::string(53, '-'));
  lines.push_back("    Total |" + padL(std::to_string(N), 8) + padL("100.00", 10) +
                  padL(std::to_string(static_cast<long long>(sumUnits)), 10) +
                  padL(fix(totalBetweenPct, 2), 10) + " " + padL(fix(overp, 2), 14));
  {
    const std::string base = "(n = " + std::to_string(nUnits);
    const size_t col = 40 > base.size() ? 40 - base.size() : 1;
    lines.push_back(std::string(col - 1, ' ') + base + ")");
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  out["xttab"] = rows;
  out["n_obs"] = N;
  out["n_units"] = nUnits;
  out["xttab_results"] = results;
  return out;
}
}
