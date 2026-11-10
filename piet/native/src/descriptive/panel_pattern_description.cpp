// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtdescribe -- panel participation patterns and the T_i distribution (variable-metadata
// family; shared helpers in datamgmt/variable_metadata_support.h).
//
//   xtdescribe  the T_i distribution's r() is `summarize T_i, detail` -- the SAME
//             summarizeCoreStats every `summarize, detail` runs, over a one-column scratch
//             frame, and captured through eval::captureRFromBlock; the qualifier goes
//             through applyIfAndInFilter; every number printed goes through
//             eval::applyStataFmt.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers/descriptive.h"
#include "piet/handlers/time_panel.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "datamgmt/variable_metadata_support.h"

namespace {

// xtdescribe: `i()`/`t()` override the xtset variables (the pre-xtset
// spelling); not read here.
const piet::eval::OptVocab& kVocabXtdescribe = piet::eval::registerOptVocab(
    {"xtdescribe"}, {{"patterns", "width"}, {"i", "t"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

// ===========================================================================
// xtdescribe
// ===========================================================================
//
// Implements xtdescribe per its help file; identified against StataBE probes
// (w2ax_t1_official.log xtdes_*, mx4.log XD|*):
//   * not xtset -> r(459) "must specify panelvar and timevar; use xtset"; a panel but no
//     time variable -> r(459) "must specify timevar; use xtset" (xtdes_notset/notime);
//   * the sample is if/in and a non-missing panel id; fewer than two distinct non-missing
//     times in it -> r(459) "<t> must have multiple distinct nonmissing values" (XD|ifnone,
//     XD|onet);
//   * T_i is each panel's observation count in the sample (a missing time counts, XD|nonuniq),
//     and r() is `summarize T_i, detail` over the panels (xtdes_basic r(N)=10 r(sum)=51
//     r(p50)=5.5 -- printed %6.0f as 6);
//   * the header: id range, n, the time range in the time variable's display format
//     (XD|tq `2000q1, 2000q2, ..., 2002q2`), T = distinct times (a missing time is one
//     more, XD|nonuniq T 6), Delta, Span = periods from first to last;
//   * width(2) is noted ("width must be a positive integer greater than 2") and used, binning
//     two periods a column; below 2 is refused (review p2 xd_w2);
//   * patterns(#) (default 9, 0 = no table, >= 1000 = all, < 0 r(198)): the times are put
//     on the delta grid, one column per period from the first to the last (several periods
//     per column only when that exceeds width(#), with a `*` note); a panel's pattern is its
//     count per column ('.', the digit, X above 9); patterns are listed by frequency, ties in
//     ascending order of the pattern itself ('.' before '1': xtdes_basic `..1111` before
//     `11.111`), the rest pooled as "(other patterns)", then the total line where a column
//     any panel reaches is X.
namespace {

// `display %w.df v`: the format engine's text (eval::applyStataFmt), right-aligned in the
// format's width as display prints it.
std::string fmtNum(double v, const char* fmt) {
  std::string s = eval::applyStataFmt(v, fmt);
  const int w = std::atoi(fmt + 1);
  if (w > 0 && s.size() < static_cast<size_t>(w)) s.insert(0, static_cast<size_t>(w) - s.size(), ' ');
  return s;
}

// `summarize name, detail` over `values`, through the summarize computation itself: the
// values become a one-column scratch frame (cobra convert) and summarizeCoreStats reads
// it. Returns the summarize block (or an error block).
ojson summarizeDetailOf(const std::vector<double>& values, const std::string& name,
                        SessionState& session) {
  const std::string base = mintScratchPath(session.workdir(), "t1xtdes");
  const std::string csv = base + ".csv";
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) return ojson(eval::engineError("xtdescribe: cannot stage the T_i column", "data_io"));
    f << name << "\n";
    for (double v : values) f << (std::isnan(v) ? std::string() : eval::pythonFloatRepr(v)) << "\n";
    f.flush();
    if (!f) {
      dropScratch(csv);
      return ojson(eval::engineError("xtdescribe: cannot stage the T_i column", "data_io"));
    }
  }
  CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", base, "--delim", ","});
  dropScratch(csv);
  if (!cr.ok) {
    dropScratch(base);
    return ojson(eval::engineError("xtdescribe: could not build the T_i frame -- " +
                                       strutil::trim(cr.stderr_text.empty() ? cr.error : cr.stderr_text),
                                   "data_io"));
  }
  ojson block;
  try {
    Datastore tmp(base);
    SessionState::StatelessRead stateless(session);
    block = summarizeCoreStats(tmp, std::vector<std::string>{name}, /*detail=*/true);
  } catch (const std::exception& e) {
    block = ojson(eval::engineError(std::string("xtdescribe: ") + e.what(), "data_io"));
  }
  dropScratch(base);
  return block;
}

// Stata orders patterns by (-freq, the pattern's columns ascending).
struct Pattern {
  std::vector<long long> cols;
  long long freq = 0;
};

}  // namespace

ojson runXtdescribe(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXtdescribe.known,
                                            kVocabXtdescribe.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  ojson perr;
  auto patterns = intOption(parsed, "patterns", 9, &perr);
  if (!patterns) return perr;
  auto width = intOption(parsed, "width", 100, &perr);
  if (!width) return perr;
  if (*patterns < 0) return rcError("invalid syntax", 198);  // XD|patneg
  const bool allPatterns = *patterns >= 1000;
  // width(2) is reported and then used (review p2 xd_w2: the note, then a table binned two
  // periods a column). Below 2 the column width divides by zero in Stata itself; refused.
  if (*patterns != 0 && *width < 2)
    return rcError("width must be a positive integer greater than 2", 198);
  const bool widthNote = *patterns != 0 && *width == 2;

  // From the panel check on, r() is replaced (review p2 xd_rnotset, p9 xd_r_notime: a
  // 459 leaves r(N) empty behind an earlier summarize).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "xtdescribe", {});
  const bool havePanel = session.panelvar.has_value() && !session.panelvar->empty();
  const bool haveTime = session.timevar.has_value() && !session.timevar->empty();
  if (!havePanel && !haveTime)
    return rcError("must specify panelvar and timevar; use xtset", 459);
  if (!havePanel) return rcError("must specify panelvar; use xtset", 459);
  if (!haveTime) return rcError("must specify timevar; use xtset", 459);
  const std::string ivar = *session.panelvar;
  const std::string tvar = *session.timevar;
  const double delta = session.delta.value_or(1.0);

  // tmin over the WHOLE data (xtset's r(tmin)): the delta grid's origin.
  double tminAll = NAN;
  std::string tfmt = "%9.0g";
  {
    Datastore* full = session.datastore();
    if (full == nullptr) return ojson(eval::noDataLoaded());
    if (!full->has(ivar)) return rcError("variable " + ivar + " not found", 111);
    if (!full->has(tvar)) return rcError("variable " + tvar + " not found", 111);
    if (full->dtype(ivar) != "f64" || full->dtype(tvar) != "f64")
      return rcError("xtdescribe: the panel and time variables must be numeric", 109);
    const double* t = full->col(tvar).f64();
    for (int64_t i = 0; i < full->nRows(); ++i)
      if (!std::isnan(t[i]) && (std::isnan(tminAll) || t[i] < tminAll)) tminAll = t[i];
    const std::vector<VarInfo> card = full->formatCard(std::vector<std::string>{tvar});
    if (!card.empty()) tfmt = session.displayFormat(card.front());
  }

  ColumnScope scope(session, {ivar, tvar}, parsed.ifExpr, parsed.inRange, "xtdescribe");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();
  const int64_t n = ds.nRows();
  const double* idc = ds.col(ivar).f64();
  const double* tc = ds.col(tvar).f64();

  // The sample: a non-missing panel id. tg = the time on the delta grid.
  struct Obs {
    double id, t, tg;
  };
  std::vector<Obs> obs;
  obs.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(idc[i])) continue;
    const double tg = std::isnan(tc[i]) ? tc[i] : std::trunc((tc[i] - tminAll) / delta);
    obs.push_back({idc[i], tc[i], tg});
  }

  // `tab t`: fewer than two distinct non-missing times is r(459), with tab's r(N)/r(r)
  // left behind as Stata leaves them (XD|ifnone r(N) 0, XD|onet r(N) 5).
  std::set<double> distinctT;
  long long nT = 0;
  for (const auto& o : obs)
    if (!std::isnan(o.tg)) {
      distinctT.insert(o.tg);
      ++nT;
    }
  if (distinctT.size() < 2) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(nT));
    session.eval.rValues.set("r", static_cast<double>(distinctT.size()));
    eval::declareRPosted(session.eval, "xtdescribe", {"N", "r"});
    return rcError(tvar + " must have multiple distinct nonmissing values", 459);
  }

  // Times: T counts the distinct values the sorted time column carries, a missing value
  // (of each kind) one more (XD|nonuniq).
  std::set<int> missKinds;
  for (const auto& o : obs)
    if (std::isnan(o.t)) missKinds.insert(cecon::missing_kind(o.t));
  const long long T = static_cast<long long>(distinctT.size() + missKinds.size());
  const double tv0 = *distinctT.begin(), tv1 = *distinctT.rbegin();
  const double tval2 = *std::next(distinctT.begin());
  const long long span = static_cast<long long>(tv1 - tv0) + 1;
  // The real-time edges for the header.
  double realFirst = NAN, realSecond = NAN, realLast = NAN;
  for (const auto& o : obs) {
    if (std::isnan(o.tg)) continue;
    if (o.tg == tv0 && (std::isnan(realFirst) || o.t < realFirst)) realFirst = o.t;
    if (o.tg == tval2 && (std::isnan(realSecond) || o.t < realSecond)) realSecond = o.t;
    if (std::isnan(realLast) || o.t > realLast) realLast = o.t;
  }
  double dtv = INFINITY;
  for (auto it = std::next(distinctT.begin()); it != distinctT.end(); ++it)
    dtv = std::min(dtv, *it - *std::prev(it));
  if (*patterns != 0 && static_cast<long long>(std::trunc((tv1 - tv0) / dtv)) + 1 > *width)
    dtv = std::trunc((tv1 - tv0) / static_cast<double>(*width - 1));

  // Panels, ascending id; T_i and uniqueness of (id, t).
  std::map<double, std::vector<double>> byPanel;  // id -> grid times (NaN kept)
  for (const auto& o : obs) byPanel[o.id].push_back(o.tg);
  bool unique = true;
  for (const auto& [id, ts] : byPanel) {
    std::vector<double> s = ts;
    std::sort(s.begin(), s.end(), [](double a, double b) { return cecon::stata_cmp(a, b) < 0; });
    for (size_t k = 1; k < s.size(); ++k)
      if (cecon::stata_cmp(s[k], s[k - 1]) == 0) unique = false;
  }
  const long long nPanels = static_cast<long long>(byPanel.size());
  const double val1 = byPanel.begin()->first, valn = byPanel.rbegin()->first;
  const double val2 = nPanels > 1 ? std::next(byPanel.begin())->first : NAN;
  std::vector<double> ti;
  ti.reserve(byPanel.size());
  for (const auto& kv : byPanel) ti.push_back(static_cast<double>(kv.second.size()));

  // r(): summarize T_i, detail -- the summarize computation itself.
  ojson summ = summarizeDetailOf(ti, "T_i", session);
  if (isErrorBlock(summ)) return summ;
  eval::replaceRClass(session.eval);  // summarize replaces ALL of r(), macros included
  eval::captureRFromBlock(summ, session.eval);
  {
    std::vector<std::string> names = {"N", "mean", "min", "max", "sum_w", "sum", "Var", "sd",
                                      "skewness", "kurtosis", "p1", "p5", "p10", "p25",
                                      "p50", "p75", "p90", "p95", "p99"};
    // A name summarize posts whose value is undefined here (one panel: sd, Var,
    // skewness, kurtosis) is posted missing, as summarize posts it (XD|onepanel `.`).
    for (const auto& nm : names)
      if (session.eval.rValues.find(nm) == nullptr)
        session.eval.rValues.set(nm, eval::kStataMissingSentinel);
    eval::declareRPosted(session.eval, "xtdescribe", names);
  }
  auto rv = [&](const char* k) {
    const double* v = session.eval.rValues.find(k);
    return v == nullptr ? NAN : *v;
  };

  // ---- the header ----------------------------------------------------------
  std::vector<std::string> lines;
  if (widthNote) lines.push_back("width must be a positive integer greater than 2");
  lines.emplace_back();  // the leading `di`
  auto label8 = [](const std::string& name) {
    const long long skipN = 8 - static_cast<long long>(name.size());
    return std::string(static_cast<size_t>(std::max(0LL, skipN)), ' ') + name + ":  ";
  };
  {
    std::string l = label8(ivar) + displayNumber(val1, session) + ", " +
                    displayNumber(val2, session) + ", ..., " + displayNumber(valn, session);
    l = padTo(l, 61) + "n =" + fmtNum(static_cast<double>(nPanels), "%11.0f");
    lines.push_back(l);
  }
  {
    // xtdescribe's PrettyPrintT: up to three lines of the time range.
    auto fmtT = [&](double v) { return strutil::trim(eval::applyStataFmt(v, tfmt)); };
    std::string line1 = fmtT(realFirst) + ",", line2, line3;
    const std::string per2 = fmtT(realSecond);
    int lincnt = 1;
    if (line1.size() + per2.size() < 46) {
      line1 += " " + per2 + ", ...,";
    } else {
      line2 = per2 + ", ...,";
      lincnt = 2;
    }
    const std::string pern = fmtT(realLast);
    std::string& cur = lincnt == 1 ? line1 : line2;
    if (cur.size() + pern.size() < 50) {
      cur += " " + pern;
    } else if (lincnt == 1) {
      line2 = pern;
    } else {
      line3 = pern;
    }
    std::string l = padTo(label8(tvar) + line1, 61) + "T =" + fmtNum(static_cast<double>(T), "%11.0f");
    lines.push_back(l);
    if (!line2.empty()) lines.push_back(std::string(11, ' ') + line2);
    if (!line3.empty()) lines.push_back(std::string(11, ' ') + line3);
  }
  lines.push_back(std::string(11, ' ') + "Delta(" + tvar + ") = " + tsDeltaText(delta, tfmt));
  lines.push_back(std::string(11, ' ') + "Span(" + tvar + ")  = " + std::to_string(span) + " periods");
  lines.push_back(std::string(11, ' ') + "(" + ivar + "*" + tvar +
                  (unique ? " uniquely identifies each observation)"
                          : " does not uniquely identify observations)"));
  lines.emplace_back();
  lines.push_back("Distribution of T_i:   min      5%     25%       50%       75%     95%     max");
  lines.push_back(std::string(20, ' ') + fmtNum(rv("min"), "%6.0f") + "  " + fmtNum(rv("p5"), "%6.0f") +
                  "  " + fmtNum(rv("p25"), "%6.0f") + "    " + fmtNum(rv("p50"), "%6.0f") + "    " +
                  fmtNum(rv("p75"), "%6.0f") + "  " + fmtNum(rv("p95"), "%6.0f") + "  " +
                  fmtNum(rv("max"), "%6.0f"));
  if (*patterns == 0) return displayBlock(joinWords(lines, "\n"));

  // ---- the pattern table ---------------------------------------------------
  std::vector<std::pair<double, double>> bins;  // (l, u]: l < t <= u
  for (double l = tv0 - 0.5; l < tv1; l += dtv) bins.emplace_back(l, l + dtv);
  std::map<std::vector<long long>, long long> freq;
  for (const auto& [id, ts] : byPanel) {
    std::vector<long long> c(bins.size(), 0);
    for (double t : ts) {
      if (std::isnan(t)) continue;
      for (size_t b = 0; b < bins.size(); ++b)
        if (bins[b].first < t && t <= bins[b].second) ++c[b];
    }
    ++freq[c];
  }
  std::vector<Pattern> pats;
  for (const auto& [c, f] : freq) pats.push_back({c, f});
  std::stable_sort(pats.begin(), pats.end(), [](const Pattern& a, const Pattern& b) {
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.cols < b.cols;
  });
  const bool binned = dtv != 1.0;
  const std::string patsMark = binned ? "*" : "";
  const long long dup = std::max<long long>(
      static_cast<long long>(std::trunc((tv1 - tv0) / dtv + 1.5)),
      static_cast<long long>(std::string("Pattern" + patsMark).size()));
  const std::string rule = " " + std::string(27, '-') + "+--" + std::string(static_cast<size_t>(dup), '-');
  lines.emplace_back();
  lines.push_back("     Freq.  Percent    Cum. |  Pattern" + patsMark);
  lines.push_back(rule);
  const double total = static_cast<double>(nPanels);
  const size_t shownN = allPatterns ? pats.size()
                                    : std::min(pats.size(), static_cast<size_t>(*patterns));
  long long cum = 0;
  for (size_t i = 0; i < shownN; ++i) {
    cum += pats[i].freq;
    std::string l = fmtNum(static_cast<double>(pats[i].freq), "%9.0f") +
                    fmtNum(100.0 * static_cast<double>(pats[i].freq) / total, "%10.2f") +
                    fmtNum(100.0 * static_cast<double>(cum) / total, "%8.2f") + " |  ";
    for (long long c : pats[i].cols) l += c == 0 ? "." : (c <= 9 ? std::to_string(c) : "X");
    lines.push_back(l);
  }
  if (shownN < pats.size()) {
    const double rest = total - static_cast<double>(cum);
    lines.push_back(fmtNum(rest, "%9.0f") + fmtNum(100.0 * rest / total, "%10.2f") +
                    fmtNum(100.0, "%8.2f") + " | (other patterns)");
  }
  lines.push_back(rule);
  {
    std::string l = fmtNum(total, "%9.0f") + fmtNum(100.0, "%10.2f") + std::string(8, ' ') + " |  ";
    for (size_t b = 0; b < bins.size(); ++b) {
      bool any = false;
      for (const auto& p : pats) any = any || p.cols[b] != 0;
      l += any ? "X" : ".";
    }
    lines.push_back(l);
  }
  if (binned) {
    // {c BT} prints as `-` in text output (review p2 xd_binned), and the final `di` is then a
    // blank line of its own; unbinned, that `di` only ends the totals line.
    lines.push_back(" " + std::string(30 + static_cast<size_t>(dup), '-'));
    lines.push_back(" *Each column represents " + std::to_string(static_cast<long long>(dtv)) +
                    " periods.");
    lines.emplace_back();
  }
  return displayBlock(joinWords(lines, "\n"));
}

}  // namespace piet::handlers
