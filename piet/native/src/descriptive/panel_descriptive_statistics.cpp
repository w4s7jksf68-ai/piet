// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// panel descriptive statistics.
#include "datamgmt/data_combine_internal.h"
#include "piet/handlers/descriptive.h"
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


ojson runInspect(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (varlist.size() != 1)
    return engineErrorBlock("inspect: one variable at a time in this build (`inspect varname`)",
                            "syntax_error");
  const std::string& name = varlist[0];
  if (!ds->has(name))
    return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock(
        "inspect: " + name + " is a string variable; inspect reports the numeric composition of "
        "a numeric variable.",
        "bad_input");

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
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


ojson runXtsum(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xtsum.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  const std::string& idvar = *session.panelvar;
  if (!ds->has(idvar))
    return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(idvar) != "f64")
    return engineErrorBlock("xtsum: panel variable " + idvar + " must be numeric", "bad_input");
  if (varlist.empty())
    return engineErrorBlock("xtsum: variable list required", "syntax_error");
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    if (ds->dtype(v) != "f64")
      return engineErrorBlock("xtsum: " + v + " is a string variable", "bad_input");
  }

  const int64_t n = ds->nRows();
  const double* id = ds->col(idvar).f64();

  // BUG-377: Stata's real layout, captured live on nlswork
  // (internal capture conventions.log, section Q4):
  //
  //   Variable         |      Mean   Std. dev.       Min        Max |    Observations
  //   -----------------+--------------------------------------------+----------------
  //   ln_wage  overall |  1.674907   .4780935          0   5.263916 |     N =   28534
  //            between |              .424569          0   3.912023 |     n =    4711
  //            within  |               .29266  -.4077221    4.78367 | T-bar = 6.05689
  //
  // The `|` rules and the N=/n=/T-bar= labels in the Observations column are the
  // shape; the previous rendering used a plain Obs column and no rules.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | %15s", "Variable", "Mean",
                  "Std. dev.", "Min", "Max", "Observations");
    lines.push_back(buf);
    lines.push_back("-----------------+--------------------------------------------+----------------");
  }
  ojson stats = ojson::object();

  for (const auto& name : varlist) {
    const double* x = ds->col(name).f64();
    // Unit means, in first-appearance order; a unit with no non-missing value of
    // THIS variable contributes nothing (Stata's n counts units that have one).
    std::map<double, std::pair<double, int64_t>> acc;  // id -> (sum, count)
    std::vector<double> all;
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      all.push_back(x[i]);
      auto& a = acc[id[i]];
      a.first += x[i];
      a.second += 1;
    }
    const int64_t N = static_cast<int64_t>(all.size());
    const int64_t units = static_cast<int64_t>(acc.size());
    const double NA = std::numeric_limits<double>::quiet_NaN();
    double gmean = NA, omin = NA, omax = NA;
    if (N > 0) {
      double sum = 0;
      omin = all[0];
      omax = all[0];
      for (double q : all) { sum += q; omin = std::min(omin, q); omax = std::max(omax, q); }
      gmean = sum / static_cast<double>(N);
    }
    std::vector<double> means;
    means.reserve(acc.size());
    for (const auto& kv : acc) means.push_back(kv.second.first / static_cast<double>(kv.second.second));
    double bmin = NA, bmax = NA;
    if (!means.empty()) {
      bmin = *std::min_element(means.begin(), means.end());
      bmax = *std::max_element(means.begin(), means.end());
    }
    // Within: x_it - x̄_i + x̄. Stata reports the min/max of THAT quantity, which
    // is why they can look odd next to the raw range — they are deviations
    // re-centred on the grand mean, not observations.
    std::vector<double> within;
    within.reserve(all.size());
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      const auto& a = acc[id[i]];
      within.push_back(x[i] - a.first / static_cast<double>(a.second) + gmean);
    }
    double wmin = NA, wmax = NA;
    if (!within.empty()) {
      wmin = *std::min_element(within.begin(), within.end());
      wmax = *std::max_element(within.begin(), within.end());
    }
    const double osd = sampleSdDV(all), bsd = sampleSdDV(means), wsd = sampleSdDV(within);
    const double tbar = units > 0 ? static_cast<double>(N) / static_cast<double>(units) : NA;

    auto num = [](double v) { return stataG(v); };
    char buf[240];
    // Stata puts the variable name and the "overall" tag in one 16-wide stub.
    std::string stub = name + "  overall";
    // Widths count CHARACTERS (StataBE, scratch-r150/uni-v/p4.do), not bytes.
    stub = cecon::ident::truncateCodePoints(stub, 16);
    stub += std::string(16 - cecon::ident::codePointCount(stub), ' ');
    std::snprintf(buf, sizeof(buf), "%s | %9s %10s %10s %10s |   N = %9lld", stub.c_str(),
                  num(gmean).c_str(), num(osd).c_str(), num(omin).c_str(), num(omax).c_str(),
                  static_cast<long long>(N));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s |   n = %9lld", "         between",
                  "", num(bsd).c_str(), num(bmin).c_str(), num(bmax).c_str(),
                  static_cast<long long>(units));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | T-bar = %7s", "         within ",
                  "", num(wsd).c_str(), num(wmin).c_str(), num(wmax).c_str(), num(tbar).c_str());
    lines.push_back(buf);

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
  return out;
}


ojson runXttab(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xttab.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (varlist.size() != 1)
    return engineErrorBlock("xttab: one variable at a time (`xttab varname`)", "syntax_error");
  const std::string& name = varlist[0];
  const std::string& idvar = *session.panelvar;
  if (!ds->has(name)) return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (!ds->has(idvar)) return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock("xttab: " + name + " is a string variable", "bad_input");

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
  const int64_t nUnits = static_cast<int64_t>(allUnits.size());

  // BUG-377: Stata's real layout, captured live on nlswork (section Q4):
  //
  //                     Overall             Between            Within
  //        race |    Freq.  Percent      Freq.  Percent        Percent
  //   ----------+-----------------------------------------------------
  //       White |   20180     70.72      3329     70.66         100.00
  //   ----------+-----------------------------------------------------
  //       Total |   28534    100.00      4711    100.00         100.00
  //                                 (n = 4711)
  //
  // Note the BETWEEN column is headed "Freq." like the overall one — it is a
  // count of UNITS, and the previous rendering called it "Units", which reads as
  // a different quantity.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%*s%-19s%-19s%s", 12, "", "Overall", "Between", "Within");
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%10s | %8s %8s  %9s %8s %14s", name.c_str(), "Freq.",
                  "Percent", "Freq.", "Percent", "Percent");
    lines.push_back(buf);
    lines.push_back("-----------+-----------------------------------------------------");
  }
  ojson rows = ojson::array();
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
    const double ovPct = N > 0 ? 100.0 * static_cast<double>(freq) / static_cast<double>(N) : 0.0;
    const double btPct =
        nUnits > 0 ? 100.0 * static_cast<double>(units) / static_cast<double>(nUnits) : 0.0;
    char buf[220];
    char valbuf[32];
    std::snprintf(valbuf, sizeof(valbuf), "%g", val);
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f %14.2f", valbuf,
                  static_cast<long long>(freq), ovPct, static_cast<long long>(units), btPct,
                  withinPct);
    lines.push_back(buf);
    ojson r;
    r["value"] = val;
    r["freq"] = freq;
    r["percent"] = ovPct;
    r["units"] = units;
    r["units_percent"] = btPct;
    r["within_percent"] = withinPct;
    rows.push_back(r);
  }
  {
    char buf[220];
    lines.push_back("-----------+-----------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f", "Total",
                  static_cast<long long>(N), 100.0, static_cast<long long>(nUnits), 100.0);
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%*s(n = %lld)", 30, "", static_cast<long long>(nUnits));
    lines.push_back(buf);
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  out["xttab"] = rows;
  out["n_obs"] = N;
  out["n_units"] = nUnits;
  return out;
}
}
