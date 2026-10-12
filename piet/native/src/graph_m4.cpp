// M4a item 4: graph JSON surface — C++ port of piet/backend/piet/graph.py's
// run_scatter/run_histogram/run_kdensity/run_line/run_xtline/run_tsline,
// and dispatcher.py's _run_marginsplot. Block STRUCTURE/JSON only for the
// live console — the frontend renders SVG from these blocks client-side
// (graph.js) exactly as before the M4b transport swap (Doctrine §3, ported
// verbatim from graph.py's own module docstring).
//
// M4b addition (below the M4a marginsplot section): `graph save`/`graph
// combine`/`graph export` — the SVG *structural* builder here is a direct
// C++ port of graph_export.py's `_build_svg*` family (same geometry
// constants/tick math), used for `graph export FILE.svg` (server-side file
// write, as Stata's `graph export` always is) and reused by `graph_combine`
// blocks. `.png`/`.pdf` stay a documented `piet_gap` (matplotlib-identical
// rendering is out of reach without bundling matplotlib).
#include "piet/handlers.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/twoway_opts.h"
#include "piet/twoway_parse.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson engineErrorBlockG(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// _numeric_column: f64 columns as-is (NaN = missing); factor columns as
// their integer codes cast to double, with code<0 (missing) -> NaN.
std::vector<double> numericColumn(Datastore& ds, const std::string& name) {
  const cecon::Column& c = ds.col(name);
  int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n));
  if (ds.dtype(name) == "f64") {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  } else {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      out[static_cast<size_t>(i)] = codes[i] < 0 ? std::nan("") : static_cast<double>(codes[i]);
    }
  }
  return out;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}

std::optional<bool> optBool(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                             const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return v.boolValue;
      return true;  // a valued option is still "truthy" for a boolean check
    }
  }
  return std::nullopt;
}

// BS-052 (BETA E): `by(groupvar)` real Stata faceting (multiple small-
// multiple panels split by a grouping variable) has no renderer anywhere in
// PIET -- previously every graph command simply never read the `by` key, so
// `scatter y x, by(foreign)` silently rendered ONE ungrouped panel and threw
// the grouping away with no signal at all. Honest-reject it by name instead
// (Doctrine (1): an explicit gap, never a silently different answer) --
// shared by every bare graph-command entry point AND runTwowayLayers' global
// options below, since `by()` is always a whole-graph option, never a
// per-layer one.
std::optional<ojson> rejectByFacet(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts) {
    if (k == "by") {
      return ojson(eval::unsupportedOption(
          "by()",
          "Graph faceting (splitting into multiple panels by a grouping variable) is not yet "
          "supported in PIET -- run a separate graph command per group instead."));
    }
  }
  return std::nullopt;
}

// U1 (BS-008) + U2 (BS-009): title()/subtitle()/xtitle()/ytitle()/note()
// text extraction. Stata's grammar is `title("text"[, suboptions])` --
// tokenizer.cpp's parseOpts (the shared quote/paren-aware option grammar)
// captures the WHOLE parenthesised content verbatim, so e.g. opts["title"]
// is literally `"text", suboptions` today: the unstripped quote characters
// AND any glued sub-option both reach the graph block and thence the
// exported SVG (BS-008 evidence: "corrupts ~100% of idiomatic quoted
// titles"). This is the ONE shared consumer (BS-099: "single title/axis/
// legend/name/note consumer") every title/subtitle/xtitle/ytitle/note
// callsite below routes through instead of raw optStr().
std::string dequoteTitleText(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t");
  s = s.substr(a, b - a + 1);

  if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
    char q = s.front();
    size_t close = s.find(q, 1);
    if (close != std::string::npos) return s.substr(1, close - 1);
    // Unterminated quote (parseOpts is paren-balanced, not quote-aware) --
    // fall through to the bare-token handling below.
  }
  // Bare/unquoted value (or unterminated-quote fallback): still strip a
  // glued `, suboption(...)` tail at the first top-level comma so e.g.
  // `title(Wages, size(large))` doesn't leak `size(large)` into the text.
  size_t comma = s.find(',');
  if (comma != std::string::npos) s = s.substr(0, comma);
  a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// title/subtitle/xtitle/ytitle/note all share the exact same dequote +
// glued-suboption-strip rule -- one accessor for all five keys.
std::optional<std::string> optTitleStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::string& key) {
  auto raw = optStr(opts, key);
  if (!raw.has_value()) return std::nullopt;
  return dequoteTitleText(*raw);
}

// Stata's name(X, replace)/name(X): take the first comma/whitespace-
// delimited token as the registered name, discarding any glued suboption
// (e.g. `replace` -- BS-057). Mirrors dispatcher.cpp's identical twoway-
// path name() split (this codebase's per-TU convention: the same small
// parsing rule copied per call site rather than factored into a shared
// header -- see e.g. nextPathG's identical per-file duplication note above).
std::string firstNameToken(const std::string& raw) {
  static const std::regex reNameSplit(R"([,\s]+)");
  std::sregex_token_iterator it(raw.begin(), raw.end(), reNameSplit, -1), endIt;
  return (it != endIt) ? it->str() : raw;
}

}  // namespace

// ===========================================================================
// scatter

ojson runScatter(Datastore& ds, const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("scatter requires two variables: `scatter yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;

  // BS-050 (BETA E): Stata's `scatter y1 y2 ... x` puts the SHARED x
  // variable LAST -- every token before it is an independent y-series
  // plotted against that same x (previously only varlist[0]/varlist[1] were
  // ever read, so `scatter y1 y2 x` silently rendered y1-vs-y2 and dropped
  // the real x entirely). varlist.size()==2 is the single-series case and
  // keeps the EXACT pre-existing flat JSON shape (points/x_var/y_var)
  // untouched below -- only 3+ tokens engage the new "series" array, so no
  // existing consumer (buildSvgScatter's legacy branch, BS-010's selftest,
  // the not-yet-updated frontend) sees a shape change for the common case.
  std::string xvar = varlist.back();
  std::vector<std::string> yvars(varlist.begin(), varlist.end() - 1);
  for (const auto& yv : yvars) {
    if (!ds.has(yv)) return engineErrorBlockG("variable " + yv + " not found");
  }
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto x = numericColumn(ds, xvar);
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nTotal = 0;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "scatter";

  if (yvars.size() == 1) {
    auto y = numericColumn(ds, yvars[0]);
    ojson points = ojson::array();
    for (size_t i = 0; i < y.size(); ++i) {
      if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
      double xv = x[i], yv = y[i];
      points.push_back(ojson::array({xv, yv}));
      if (first) {
        xMin = xMax = xv;
        yMin = yMax = yv;
        first = false;
      } else {
        xMin = std::min(xMin, xv);
        xMax = std::max(xMax, xv);
        yMin = std::min(yMin, yv);
        yMax = std::max(yMax, yv);
      }
      ++nTotal;
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];
    out["points"] = points;
    out["n"] = nTotal;
  } else {
    ojson seriesArr = ojson::array();
    for (size_t si = 0; si < yvars.size(); ++si) {
      auto y = numericColumn(ds, yvars[si]);
      ojson points = ojson::array();
      int64_t nSeries = 0;
      for (size_t i = 0; i < y.size(); ++i) {
        if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
        double xv = x[i], yv = y[i];
        points.push_back(ojson::array({xv, yv}));
        if (first) {
          xMin = xMax = xv;
          yMin = yMax = yv;
          first = false;
        } else {
          xMin = std::min(xMin, xv);
          xMax = std::max(xMax, xv);
          yMin = std::min(yMin, yv);
          yMax = std::max(yMax, yv);
        }
        ++nSeries;
        ++nTotal;
      }
      ojson s;
      s["y_var"] = yvars[si];
      s["points"] = points;
      s["style"] = twoway::layerStyle(opts, static_cast<int>(si), "scatter");
      s["n"] = nSeries;
      seriesArr.push_back(s);
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];  // first series, for any single-y-shaped reader
    out["y_vars"] = yvars;    // full typed-order list
    out["series"] = seriesArr;
    // Backward-compat flat "points": the union of every series, so a
    // consumer that only reads the flat field (an as-yet-unupdated
    // renderer) still degrades gracefully to "every point plotted" instead
    // of an empty graph.
    ojson flatPoints = ojson::array();
    for (const auto& s : seriesArr) {
      for (const auto& p : s["points"]) flatPoints.push_back(p);
    }
    out["points"] = flatPoints;
    out["n"] = nTotal;
  }

  std::string joinedY;
  for (size_t i = 0; i < yvars.size(); ++i) {
    if (i) joinedY += " ";
    joinedY += yvars[i];
  }
  out["title"] = optTitleStr(opts, "title").value_or(joinedY + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvars[0]);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// histogram

namespace {

int defaultBins(int64_t n) {
  if (n <= 1) return 1;
  double k = std::min(std::sqrt(static_cast<double>(n)), 10.0 * std::log(static_cast<double>(n)) / std::log(10.0));
  return std::max(1, static_cast<int>(std::llround(k)));
}

}  // namespace

namespace {

// BUG-178 (P2-PREP): the fitted-normal overlay, `, normal`.
//
// This computation used to live INLINE at the bottom of runKdensity and
// nowhere else, so `histogram y, normal` — the far more common Stata
// spelling — parsed the option, read it nowhere, and returned rc 0 with the
// curve silently missing. Extracted here so both plots build the same curve
// from the same three lines of arithmetic; runKdensity's numbers are
// unchanged by the move (same mean, same n-1 divisor, same pdf form, same
// grid).
//
// `scale` multiplies the pdf so the curve lands in whatever y units the host
// plot already uses. kdensity's y-axis IS density, so it passes 1.0;
// histogram passes the factor that maps a density onto its own y_kind
// (frequency/fraction/percent) — otherwise the curve would be drawn flat
// against bars three orders of magnitude taller.
struct NormalCurveFit {
  bool ok = false;  // false when sd is not positive (degenerate data)
  double mean = 0.0;
  double sd = 0.0;
  ojson points = ojson::array();  // [[x, y], ...] — same shape as `points`
};

NormalCurveFit fitNormalCurve(const std::vector<double>& x, const std::vector<double>& grid,
                              double scale) {
  NormalCurveFit fit;
  const auto n = static_cast<int64_t>(x.size());
  if (n < 2) return fit;
  double mu = 0.0;
  for (double v : x) mu += v;
  mu /= static_cast<double>(n);
  double ss = 0.0;
  for (double v : x) ss += (v - mu) * (v - mu);
  double sigma = std::sqrt(ss / static_cast<double>(n - 1));
  if (!(sigma > 0)) return fit;
  const double kSqrt2Pi = std::sqrt(2.0 * M_PI);
  for (double gx : grid) {
    double z = (gx - mu) / sigma;
    double gy = std::exp(-0.5 * z * z) / (kSqrt2Pi * sigma);
    fit.points.push_back(ojson::array({gx, gy * scale}));
  }
  fit.ok = true;
  fit.mean = mu;
  fit.sd = sigma;
  return fit;
}

// Evenly spaced evaluation points for a normal overlay drawn across [lo, hi].
// Same linspace evaluation order runHistogram/runKdensity already use for
// their own grids (step computed once, endpoint forced), so the curve's x
// values line up exactly with the plot's x range.
std::vector<double> linspaceGrid(double lo, double hi, int n) {
  std::vector<double> g(static_cast<size_t>(std::max(n, 2)));
  double step = (g.size() > 1) ? (hi - lo) / static_cast<double>(g.size() - 1) : 0.0;
  for (size_t i = 0; i < g.size(); ++i) g[i] = lo + step * static_cast<double>(i);
  g.back() = hi;
  return g;
}

// Point count for the histogram overlay. The curve is cosmetic (a smooth
// reference shape, not a number anyone reads off), so this is a rendering
// resolution, not an estimate: 100 is enough to look smooth at any plot
// width and small enough not to bloat the block.
constexpr int kNormalCurvePoints = 100;

}  // namespace

ojson runHistogram(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("histogram requires one variable: `histogram xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string xvar = varlist[0];
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto raw = numericColumn(ds, xvar);
  std::vector<double> x;
  x.reserve(raw.size());
  for (double v : raw) {
    if (std::isfinite(v)) x.push_back(v);
  }
  int64_t n = static_cast<int64_t>(x.size());
  if (n == 0) return engineErrorBlockG("no non-missing observations for " + xvar);

  double xmin = *std::min_element(x.begin(), x.end());
  double xmax = *std::max_element(x.begin(), x.end());

  std::optional<double> widthOpt;
  std::optional<int> binOpt;
  auto w = optStr(opts, "width");
  if (w.has_value()) {
    try {
      widthOpt = std::stod(*w);
    } catch (...) {
      return engineErrorBlockG("histogram width(#) must be numeric", "syntax_error");
    }
    if (*widthOpt <= 0) return engineErrorBlockG("histogram width(#) must be positive", "syntax_error");
  }
  auto b = optStr(opts, "bin");
  if (!widthOpt.has_value() && b.has_value()) {
    try {
      size_t pos;
      binOpt = std::stoi(*b, &pos);
    } catch (...) {
      return engineErrorBlockG("histogram bin(#) must be an integer", "syntax_error");
    }
    if (*binOpt < 1) return engineErrorBlockG("histogram bin(#) must be >= 1", "syntax_error");
  }

  std::vector<double> edges;
  std::vector<int64_t> counts;

  if (xmax <= xmin) {
    edges = {xmin - 0.5, xmin + 0.5};
    counts = {n};
  } else if (widthOpt.has_value()) {
    double width = *widthOpt;
    double span = xmax - xmin;
    int nbins = std::max(1, static_cast<int>(std::ceil(span / width - 1e-9)));
    for (int i = 0; i <= nbins; ++i) edges.push_back(xmin + i * width);
    counts.assign(static_cast<size_t>(nbins), 0);
    for (double v : x) {
      int idx = static_cast<int>(std::floor((v - xmin) / width + 1e-12));
      if (idx < 0) idx = 0;
      if (idx >= nbins) idx = nbins - 1;
      counts[static_cast<size_t>(idx)]++;
    }
  } else {
    int nbins = binOpt.value_or(defaultBins(n));
    double span = xmax - xmin;
    // np.linspace's ACTUAL internal formula computes step=delta/n once, then
    // start + step*i — NOT start + delta*i/n (these diverge in the last ULP;
    // verified against the frozen corpus's histogram-of-y cell). np.histogram
    // (bins=int, range=(lo,hi)) builds its edges via np.linspace internally,
    // so matching this exact evaluation order is required for byte-identical
    // edges.
    double step = span / nbins;
    for (int i = 0; i <= nbins; ++i) edges.push_back(xmin + step * i);
    counts.assign(static_cast<size_t>(nbins), 0);
    for (double v : x) {
      int idx = static_cast<int>(std::floor((v - xmin) / span * nbins));
      if (idx < 0) idx = 0;
      if (idx >= nbins) idx = nbins - 1;
      counts[static_cast<size_t>(idx)]++;
    }
  }

  double binW = edges.size() >= 2 ? (edges[1] - edges[0]) : 1.0;

  std::string yKind;
  std::vector<double> ys;
  bool freqOpt = optBool(opts, "freq").value_or(false) || optBool(opts, "frequency").value_or(false);
  bool fracOpt = optBool(opts, "fraction").value_or(false);
  bool pctOpt = optBool(opts, "percent").value_or(false);
  if (freqOpt) {
    yKind = "frequency";
    for (int64_t c : counts) ys.push_back(static_cast<double>(c));
  } else if (fracOpt) {
    yKind = "fraction";
    for (int64_t c : counts) ys.push_back(static_cast<double>(c) / static_cast<double>(n));
  } else if (pctOpt) {
    yKind = "percent";
    for (int64_t c : counts) ys.push_back(100.0 * static_cast<double>(c) / static_cast<double>(n));
  } else {
    yKind = "density";
    for (int64_t c : counts) {
      ys.push_back(binW > 0 ? static_cast<double>(c) / (static_cast<double>(n) * binW) : 0.0);
    }
  }

  ojson bars = ojson::array();
  for (size_t i = 0; i < counts.size(); ++i) {
    ojson b2;
    b2["x0"] = edges[i];
    b2["x1"] = edges[i + 1];
    b2["count"] = counts[i];
    b2["y"] = ys[i];
    bars.push_back(b2);
  }

  std::string yLabelDefault = yKind;
  if (!yLabelDefault.empty()) yLabelDefault[0] = static_cast<char>(std::toupper(yLabelDefault[0]));

  ojson out;
  out["type"] = "graph";
  out["kind"] = "histogram";
  out["title"] = optTitleStr(opts, "title").value_or("Histogram of " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yLabelDefault);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_var"] = xvar;
  out["y_kind"] = yKind;
  out["n"] = n;
  out["bins"] = bars;
  out["x_min"] = xmin;
  out["x_max"] = xmax;
  out["y_min"] = 0.0;
  out["y_max"] = ys.empty() ? 0.0 : *std::max_element(ys.begin(), ys.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)

  // BUG-178 (P2-PREP): `histogram y, normal`. The option was parsed by the
  // shared grammar and then read NOWHERE in this function — rc 0, no curve,
  // no diagnostic: exactly the silently-ignored-option class. ADD-only, so
  // every existing key above is untouched and no pinned histogram block
  // changes shape unless `, normal` was actually asked for.
  //
  // Scaled onto this plot's own y-axis (see fitNormalCurve): the bars are
  // density / fraction / percent / frequency depending on y_kind, and a raw
  // pdf would be invisible against a frequency axis.
  //
  // KEY NAME: `normal_points`, deliberately the SAME key runKdensity emits
  // (plans/piet/CONTRACT.md publishes `normal_points?` under the kdensity
  // overlay). Same concept, same name — an earlier draft of this fix called
  // it `normal_curve`, which would have left one product with two names for
  // one thing. The two emissions agree on everything a consumer keys off:
  // the shape ([[x, y], ...] pairs), the meaning (a fitted normal density),
  // and the y-units convention (each curve is expressed in ITS OWN host
  // plot's y-axis units, which for kdensity is already density, hence its
  // scale of 1.0). They differ only in the x-grid each inherits from its
  // host plot — kdensity reuses the KDE's own bandwidth-padded evaluation
  // grid, histogram spans its own bin edges at kNormalCurvePoints — which
  // is the same kind of difference as the two plots having different x
  // ranges in the first place, not a difference in what the key means.
  if (optBool(opts, "normal").value_or(false)) {
    double scale = 1.0;
    if (yKind == "frequency") {
      scale = binW * static_cast<double>(n);
    } else if (yKind == "fraction") {
      scale = binW;
    } else if (yKind == "percent") {
      scale = binW * 100.0;
    }
    double lo = edges.empty() ? xmin : edges.front();
    double hi = edges.empty() ? xmax : edges.back();
    NormalCurveFit fit = fitNormalCurve(x, linspaceGrid(lo, hi, kNormalCurvePoints), scale);
    if (fit.ok) {
      out["normal_points"] = fit.points;
      out["normal_mean"] = fit.mean;
      out["normal_sd"] = fit.sd;
      // The curve can legitimately peak above the tallest bar; without this
      // the frontend's y-scale would clip it.
      double curveMax = 0.0;
      for (const auto& p : fit.points) curveMax = std::max(curveMax, p[1].get<double>());
      out["y_max"] = std::max(out["y_max"].get<double>(), curveMax);
    }
  }
  return out;
}

// ===========================================================================
// kdensity

namespace {

// numpy's pairwise (cascade) summation algorithm — reproduced EXACTLY
// (blocksize=128, 8-way-unrolled naive base case, recursive halving aligned
// to 8), same as tabulate.cpp's identical helper (duplicated per this
// codebase's established one-copy-per-file convention for small ported
// helpers). np.std/np.mean use this internally; a naive sequential
// accumulator diverges in the last ULP for N in the hundreds — enough to
// break the byte-identical debug_all stream-replay gate.
double pairwiseSumG(const double* arr, size_t n) {
  if (n <= 8) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += arr[i];
    return s;
  }
  if (n <= 128) {
    double r[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t i = 0;
    while (i + 8 <= n) {
      for (int k = 0; k < 8; ++k) r[static_cast<size_t>(k)] += arr[i + static_cast<size_t>(k)];
      i += 8;
    }
    double s = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    while (i < n) {
      s += arr[i];
      ++i;
    }
    return s;
  }
  size_t half = n / 2;
  half -= half % 8;
  return pairwiseSumG(arr, half) + pairwiseSumG(arr + half, n - half);
}

double pairwiseSumG(const std::vector<double>& v) { return pairwiseSumG(v.data(), v.size()); }

double kdeBwidthSilverman(const std::vector<double>& x) {
  size_t n = x.size();
  if (n < 2) return 1.0;
  double mean = pairwiseSumG(x) / static_cast<double>(n);
  std::vector<double> sq(n);
  for (size_t i = 0; i < n; ++i) {
    double d = x[i] - mean;
    sq[i] = d * d;
  }
  double ss = pairwiseSumG(sq);
  double sd = std::sqrt(ss / static_cast<double>(n - 1));

  std::vector<double> sorted = x;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double p) -> double {
    double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    int64_t lo = static_cast<int64_t>(std::floor(idx));
    int64_t hi = static_cast<int64_t>(std::ceil(idx));
    if (lo == hi) return sorted[static_cast<size_t>(lo)];
    double frac = idx - static_cast<double>(lo);
    return sorted[static_cast<size_t>(lo)] * (1.0 - frac) + sorted[static_cast<size_t>(hi)] * frac;
  };
  double q75 = pct(75), q25 = pct(25);
  double iqr = q75 - q25;
  // BS-049 (BETA E): Stata's documented default bandwidth divides the IQR
  // by 1.349 (the interquartile range of a standard normal, N(0,1), is
  // 1.3489... — this is the "convert IQR to an equivalent sigma" constant),
  // not 1.34. On this batch's own oracle probe (fatalities.csv `income`,
  // n=336) sd < iqr/either-constant, so min() picks sd regardless and the
  // two constants coincidentally produce the SAME pinned bwidth=633.4973 —
  // but the constant is still wrong for any dataset where iqr/1.34 is the
  // binding term, so fixed to match Stata's documented formula exactly.
  double a = iqr > 0 ? std::min(sd, iqr / 1.349) : sd;
  if (a <= 0) a = sd > 0 ? sd : 1.0;
  return 0.9 * a * std::pow(static_cast<double>(n), -0.2);
}

// `kernel.sum(axis=1)` in _kde_gaussian is a pairwise sum over N (here 950)
// data points per grid point — reuse the pairwiseSumG helper defined above
// (a naive sequential accumulator diverges in the last ULP, enough to break
// the byte-identical stream gate).
std::vector<double> kdeGaussian(const std::vector<double>& x, const std::vector<double>& grid, double h) {
  const double kSqrt2Pi = std::sqrt(2.0 * M_PI);
  std::vector<double> out(grid.size(), 0.0);
  std::vector<double> kernelRow(x.size());
  for (size_t gi = 0; gi < grid.size(); ++gi) {
    for (size_t xi = 0; xi < x.size(); ++xi) {
      double u = (grid[gi] - x[xi]) / h;
      kernelRow[xi] = std::exp(-0.5 * u * u) / kSqrt2Pi;
    }
    double s = pairwiseSumG(kernelRow.data(), kernelRow.size());
    out[gi] = s / (static_cast<double>(x.size()) * h);
  }
  return out;
}

// BS-049 (BETA E): Stata's DEFAULT kdensity kernel is Epanechnikov, not
// Gaussian (`kdensity` previously always computed Gaussian regardless of
// what -- if anything -- `kernel()` said, a silent-wrong: the option was
// parsed by the shared grammar and then never read again). Stata's
// Epanechnikov is its own normalized parameterization (see [R] kdensity
// Methods and formulas): K(u) = 0.75*(1 - u^2/5)/sqrt(5) for |u| < sqrt(5),
// 0 otherwise -- NOT the textbook-common 0.75*(1-u^2) unit-support form.
// Verified bit-for-bit against the live-Stata oracle's `kdensity income,
// n(50)` (kx/kd list rows 1/5 and 46/50): e-oracle-stata.log.
std::vector<double> kdeEpanechnikov(const std::vector<double>& x, const std::vector<double>& grid, double h) {
  const double kSqrt5 = std::sqrt(5.0);
  std::vector<double> out(grid.size(), 0.0);
  std::vector<double> kernelRow(x.size());
  for (size_t gi = 0; gi < grid.size(); ++gi) {
    for (size_t xi = 0; xi < x.size(); ++xi) {
      double u = (grid[gi] - x[xi]) / h;
      kernelRow[xi] = (std::fabs(u) < kSqrt5) ? (0.75 * (1.0 - (u * u) / 5.0) / kSqrt5) : 0.0;
    }
    double s = pairwiseSumG(kernelRow.data(), kernelRow.size());
    out[gi] = s / (static_cast<double>(x.size()) * h);
  }
  return out;
}

}  // namespace

ojson runKdensity(Datastore& ds, const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("kdensity requires one variable: `kdensity xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string xvar = varlist[0];
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  // BS-049 (BETA E): `kernel()` was parsed by the shared option grammar and
  // then never read anywhere -- ALWAYS computed Gaussian regardless. Default
  // (unset) is Stata's own default, Epanechnikov; `kernel(gaussian)` is the
  // other oracle-pinned spelling; anything else is an honest reject naming
  // the two supported kernels rather than a silent substitution.
  std::string kernelName = "epanechnikov";
  auto kernelOpt = optStr(opts, "kernel");
  if (kernelOpt.has_value()) {
    std::string k = *kernelOpt;
    size_t a = k.find_first_not_of(" \t");
    size_t b = k.find_last_not_of(" \t");
    k = (a == std::string::npos) ? std::string() : k.substr(a, b - a + 1);
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
    if (k == "epanechnikov" || k == "epan" || k == "epan2") {
      kernelName = "epanechnikov";
    } else if (k == "gaussian" || k == "gauss" || k == "normal") {
      kernelName = "gaussian";
    } else {
      return engineErrorBlockG(
          "kdensity kernel(" + *kernelOpt + "): only kernel(epanechnikov) (the default) and "
          "kernel(gaussian) are supported in this version of PIET.",
          "unsupported_option");
    }
  }

  auto raw = numericColumn(ds, xvar);
  std::vector<double> x;
  x.reserve(raw.size());
  for (double v : raw) {
    if (std::isfinite(v)) x.push_back(v);
  }
  int64_t nObs = static_cast<int64_t>(x.size());
  if (nObs == 0) return engineErrorBlockG("no non-missing observations for " + xvar);
  if (nObs < 2) {
    return engineErrorBlockG("kdensity requires at least 2 observations (found " +
                             std::to_string(nObs) + ")");
  }

  double h;
  auto bwOpt = optStr(opts, "bwidth");
  if (bwOpt.has_value()) {
    try {
      h = std::stod(*bwOpt);
    } catch (...) {
      return engineErrorBlockG("kdensity bwidth(#) must be numeric", "syntax_error");
    }
    if (h <= 0) return engineErrorBlockG("kdensity bwidth(#) must be positive", "syntax_error");
  } else {
    h = kdeBwidthSilverman(x);
  }

  int nGrid = 512;
  auto nOpt = optStr(opts, "n");
  if (nOpt.has_value()) {
    try {
      nGrid = std::stoi(*nOpt);
    } catch (...) {
      return engineErrorBlockG("kdensity n(#) must be an integer", "syntax_error");
    }
    if (nGrid < 2) return engineErrorBlockG("kdensity n(#) must be >= 2", "syntax_error");
  } else {
    // Item 6 (User ruling 2026-09-09) — UNMEASURED beyond this ONE cell: the
    // only in-repo Stata capture of kdensity's grid size under generate()
    // is tests/eval/golden/aer_b4/private_oracle_p.log:64-71 (one
    // 74-observation fixture, `B4VAL|kx_N|50|kx_mean|...`), giving
    // n=min(N,50)=50 on that fixture. The GENERAL claim "Stata's kdensity
    // default n is min(N,50)" is NOT captured beyond this one cell — do
    // not generalize past it; a future capture may narrow or widen this.
    // Gated STRICTLY on generate()-present-and-n()-absent so the THREE
    // freeze fixtures byte-pinning the bare `kdensity y` 512-point form
    // (freeze/kdensity.json, freeze/margins.json, freeze/marginsplot.json)
    // stay untouched — nObs >= 2 is already guaranteed above.
    bool wantsGenerate = false;
    for (const auto& [k, v] : opts) {
      if ((k == "gen" || k == "generate") && !v.isBool) wantsGenerate = true;
    }
    if (wantsGenerate) nGrid = static_cast<int>(std::min<int64_t>(nObs, 50));
  }

  double xmin = *std::min_element(x.begin(), x.end());
  double xmax = *std::max_element(x.begin(), x.end());
  // BS-049 (BETA E): the grid was padded by 3*bwidth on each side; the live
  // oracle (`kdensity income, n(50)` -> `summarize kx`: min=8880.2644,
  // max=22826.952 against income's own min=9513.76172/max=22193.45508 and
  // bwidth=633.4973) shows Stata pads by EXACTLY 1*bwidth on each side
  // (9513.76172 - 633.4973 = 8880.2644; 22193.45508 + 633.4973 =
  // 22826.952) — fixed to match; this also explains why the grid's own x
  // VALUES (not just the kernel shape) never matched Stata even before the
  // kernel/bandwidth fixes above.
  double lo = xmin - h, hi = xmax + h;
  // np.linspace(lo, hi, n_grid): step = delta/(n-1) computed ONCE, then
  // start + step*i, with the LAST point forced to exactly `hi` (matches
  // np.linspace's endpoint=True override — see run_histogram's identical
  // note on why this evaluation order matters bit-for-bit).
  std::vector<double> grid(static_cast<size_t>(nGrid));
  {
    double step = (nGrid > 1) ? (hi - lo) / (nGrid - 1) : 0.0;
    for (int i = 0; i < nGrid; ++i) grid[static_cast<size_t>(i)] = lo + step * i;
    if (nGrid > 1) grid[static_cast<size_t>(nGrid - 1)] = hi;
  }
  auto density = (kernelName == "gaussian") ? kdeGaussian(x, grid, h) : kdeEpanechnikov(x, grid, h);
  for (auto& d : density) d = std::max(d, 0.0);

  ojson points = ojson::array();
  for (size_t i = 0; i < grid.size(); ++i) points.push_back(ojson::array({grid[i], density[i]}));

  ojson out;
  out["type"] = "graph";
  out["kind"] = "line";
  out["title"] = optTitleStr(opts, "title").value_or("Kernel density estimate of " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(std::string("Density"));
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_var"] = xvar;
  out["n"] = nObs;
  out["bwidth"] = h;
  out["n_grid"] = nGrid;
  out["kernel"] = kernelName;
  out["points"] = points;
  out["x_min"] = lo;
  out["x_max"] = hi;
  out["y_min"] = 0.0;
  out["y_max"] = density.empty() ? 0.0 : *std::max_element(density.begin(), density.end());
  out["subkind"] = "kdensity";
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)

  // BUG-178 (P2-PREP): body moved verbatim into fitNormalCurve() above so
  // `histogram y, normal` can build the SAME curve — kdensity's y-axis is
  // already a density, hence scale = 1.0, and every emitted number here is
  // bit-for-bit what the inline version produced (x.size() == nObs by
  // construction: `x` is the finite-filtered column and nObs is its size).
  // Key name kept as `normal_points` — it is the spelling plans/piet/
  // CONTRACT.md already documents for kdensity, and renaming it would break
  // a published contract key for no gain.
  if (optBool(opts, "normal").value_or(false)) {
    NormalCurveFit fit = fitNormalCurve(x, grid, 1.0);
    if (fit.ok) {
      out["normal_points"] = fit.points;
      out["normal_mean"] = fit.mean;
      out["normal_sd"] = fit.sd;
    }
  }
  return out;
}

// ===========================================================================
// line (also serves bare `twoway line yvar xvar [if ...]`)

ojson runGraphLine(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("line requires two variables: `line yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0], xvar = varlist[1];
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto y = numericColumn(ds, yvar);
  auto x = numericColumn(ds, xvar);

  std::vector<std::pair<double, double>> pairs;
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::isfinite(y[i]) && std::isfinite(x[i])) pairs.emplace_back(x[i], y[i]);
  }
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });

  ojson points = ojson::array();
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  for (const auto& [xv, yv] : pairs) {
    points.push_back(ojson::array({xv, yv}));
    if (first) {
      xMin = xMax = xv;
      yMin = yMax = yv;
      first = false;
    } else {
      xMin = std::min(xMin, xv);
      xMax = std::max(xMax, xv);
      yMin = std::min(yMin, yv);
      yMax = std::max(yMax, yv);
    }
  }
  int64_t n = static_cast<int64_t>(pairs.size());

  ojson out;
  out["type"] = "graph";
  out["kind"] = "line";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_var"] = xvar;
  out["y_var"] = yvar;
  out["n"] = n;
  out["points"] = points;
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// xtline

ojson runXtline(Datastore& ds, const std::vector<std::string>& varlist,
                const std::optional<std::string>& panelvar, const std::optional<std::string>& timevar,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("xtline requires a variable: `xtline yvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0];
  if (!panelvar.has_value() || panelvar->empty()) {
    return engineErrorBlockG("xtline: panel variable not set. Run `xtset ivar [tvar]` first.",
                             "syntax_error");
  }
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(*panelvar)) return engineErrorBlockG("panel variable " + *panelvar + " not found");
  bool haveTime = timevar.has_value() && !timevar->empty();
  if (haveTime && !ds.has(*timevar)) {
    return engineErrorBlockG("time variable " + *timevar + " not found");
  }

  auto yArr = numericColumn(ds, yvar);
  auto idArr = numericColumn(ds, *panelvar);
  std::vector<double> tArr;
  if (haveTime) {
    tArr = numericColumn(ds, *timevar);
  }
  // When no time variable is set, the x-axis is a per-panel-local row index
  // (0, 1, 2, ... within each panel), NOT a global running index across all
  // panels — matching the Python oracle (graph.py run_xtline: np.arange over
  // panel_mask.sum(), computed fresh per panel). The local counter is
  // assigned below, inside the per-uid loop, in dataset row order.

  std::set<double> uniqueIds;
  for (double v : idArr) {
    if (std::isfinite(v)) uniqueIds.insert(v);
  }

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (double uid : uniqueIds) {
    std::vector<std::pair<double, double>> pts;
    double localIdx = 0.0;
    for (size_t i = 0; i < idArr.size(); ++i) {
      if (idArr[i] != uid) continue;
      double tv = haveTime ? tArr[i] : localIdx;
      localIdx += 1.0;
      if (!std::isfinite(yArr[i]) || !std::isfinite(tv)) continue;
      pts.emplace_back(tv, yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["id"] = uid;
    std::string label;
    if (uid == std::floor(uid)) {
      label = std::to_string(static_cast<long long>(uid));
    } else {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", uid);
      label = buf;
    }
    s["label"] = label;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  if (seriesArr.empty()) {
    return engineErrorBlockG("no non-missing observations for xtline " + yvar);
  }

  std::string xLabelDefault = haveTime ? *timevar : "obs";
  ojson out;
  out["type"] = "graph";
  out["kind"] = "xtline";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " by " + *panelvar + " over " +
                                                      (haveTime ? *timevar : "obs"));
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xLabelDefault);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["y_var"] = yvar;
  out["panel_var"] = *panelvar;
  out["time_var"] = haveTime ? *timevar : "";
  out["n_panels"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  // BUG-179 (P2-PREP): `xtline y, overlay` — Stata's option for drawing every
  // panel on ONE set of axes instead of a grid of small multiples. It was
  // parsed by the shared option grammar and then read nowhere here, so the
  // request vanished silently (rc 0, ungrouped default layout).
  //
  // This is a RENDERING choice, and rendering is client-side in this
  // architecture — the kernel ships plot DATA (piet/frontend/js/graph.js's
  // own top-of-file note). The data side is what this file owns, so the flag
  // is published here; piet/frontend/js/graph.js has no consumer for it yet
  // and renderXtline still draws its single overlaid axis regardless.
  // Reported as such rather than silently half-done: after this change the
  // option is *reachable*, not yet *honored end-to-end*.
  //
  // Emitted ONLY when actually requested, not as an unconditional `false`.
  // An optional key is this contract's established idiom for exactly this
  // (plans/piet/CONTRACT.md already spells kdensity's own extras
  // `subkind?`/`normal_points?`), a missing key and `false` are the same
  // falsy value to the JS consumer, and the conditional form keeps every
  // xtline block that did NOT ask for overlay byte-identical — so the
  // frozen tests/kitchensink/freeze/xtline.json capture does not move.
  if (optBool(opts, "overlay").value_or(false)) out["overlay"] = true;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// tsline

ojson runTsline(Datastore& ds, const std::vector<std::string>& varlist,
               const std::optional<std::string>& timevar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("tsline requires at least one variable: `tsline yvar [yvar2 ...]`.",
                             "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  if (!timevar.has_value() || timevar->empty()) {
    return engineErrorBlockG("tsline: time variable not set. Run `tsset tvar` or `xtset ivar tvar` first.",
                             "syntax_error");
  }
  if (!ds.has(*timevar)) return engineErrorBlockG("time variable " + *timevar + " not found");
  for (const auto& v : varlist) {
    if (!ds.has(v)) return engineErrorBlockG("variable " + v + " not found");
  }

  auto tArr = numericColumn(ds, *timevar);

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (const auto& yvar : varlist) {
    auto yArr = numericColumn(ds, yvar);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < tArr.size(); ++i) {
      if (!std::isfinite(tArr[i]) || !std::isfinite(yArr[i])) continue;
      pts.emplace_back(tArr[i], yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["var"] = yvar;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  std::string yLabelDefault = varlist.size() == 1 ? varlist[0] : "value";
  std::string titleDefault;
  if (varlist.size() == 1) {
    titleDefault = varlist[0] + " over " + *timevar;
  } else {
    std::string joined;
    for (size_t i = 0; i < varlist.size(); ++i) {
      if (i) joined += ", ";
      joined += varlist[i];
    }
    titleDefault = joined + " over " + *timevar;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "tsline";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(*timevar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yLabelDefault);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["time_var"] = *timevar;
  ojson varsJ = ojson::array();
  for (const auto& v : varlist) varsJ.push_back(v);
  out["vars"] = varsJ;
  out["n_series"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// twoway (parenthesised multi-layer form) — C++ port of graph.py's
// run_twoway_layers() + its per-plottype layer producers (_layer_rarea/
// _layer_line_or_connected/_layer_scatter). This handles the parenthesised
// `twoway (plottype ...) (plottype ...) ...` form. The bare single-plot
// twoway form (no parens) for scatter/line/kdensity/histogram stays on the
// existing runScatter/runGraphLine/etc path (dispatcher.cpp); the bare
// twoway-SPECIFIC plot types (rarea + the range-plot family) route back
// INTO this function as a single synthesised layer (BUG-129/BUG-130), so
// rarea reaches the band builder and unsupported types hit the same
// reject-before-output guard below.

namespace {

// nextPathG: local copy of the "generate a fresh scratch .hbin path" helper
// every M2+ handler file carries its own copy of (see handlers.cpp/
// handlers_m3.cpp/egen.cpp's identically-named nextPath — kept file-local
// per this codebase's established per-TU convention rather than factored
// into a shared header).
std::string nextPathG(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// A parsed opt value in the shape Python's `dict.get('key')` would see:
// unset (nullopt-equivalent), a bare-flag True, or a string. Local
// equivalent of twoway_opts.cpp's (anonymous-namespace, not header-exposed)
// MaybeOpt — used here only for the twoway BLOCK-level options (xscale/
// yscale/xline/yline/legend/...), a distinct concern from twoway_opts.cpp's
// per-LAYER style options.
struct MaybeOptFromOpts {
  bool present = false;
  bool isTrue = false;
  std::string str;
};

MaybeOptFromOpts getOptG(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return {true, v.boolValue, ""};
      return {true, false, v.strValue};
    }
  }
  return {false, false, ""};
}

// _finite_mask + boolean-index + argsort(x) composed into one pass: returns
// (xs, ys) sorted by x, missing rows dropped. Mirrors _layer_line_or_connected/
// _layer_scatter's numpy pipeline (argsort is stable, matching np.argsort's
// default 'stable'-for-ties-preserving-input-order behavior relied on there).
std::vector<std::pair<double, double>> maskSortXY(const std::vector<double>& y,
                                                   const std::vector<double>& x) {
  std::vector<std::pair<double, double>> pts;  // (x, y)
  for (size_t i = 0; i < y.size(); ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
    pts.emplace_back(x[i], y[i]);
  }
  std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return pts;
}

// _layer_rarea: yhi ylo xvar -> {kind:"rarea", band:[[x,ylo,yhi],...], style, n}.
ojson layerRarea(Datastore& ds, const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 3) return ojson{{"_error", "rarea requires three variables: yhi ylo xvar"}};
  const std::string &yhiVar = varlist[0], &yloVar = varlist[1], &xvar = varlist[2];
  for (const auto& v : {yhiVar, yloVar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto yhiArr = numericColumn(ds, yhiVar);
  auto yloArr = numericColumn(ds, yloVar);
  auto xArr = numericColumn(ds, xvar);
  std::vector<std::tuple<double, double, double>> rows;  // (x, ylo, yhi)
  for (size_t i = 0; i < xArr.size(); ++i) {
    if (!std::isfinite(yhiArr[i]) || !std::isfinite(yloArr[i]) || !std::isfinite(xArr[i])) continue;
    rows.emplace_back(xArr[i], yloArr[i], yhiArr[i]);
  }
  std::stable_sort(rows.begin(), rows.end(),
                    [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
  ojson band = ojson::array();
  for (const auto& [x, ylo, yhi] : rows) band.push_back(ojson::array({x, ylo, yhi}));
  ojson out;
  out["kind"] = "rarea";
  out["band"] = band;
  out["style"] = twoway::layerStyle(opts, layerIdx, "rarea");
  out["n"] = static_cast<int64_t>(rows.size());
  return out;
}

// _layer_line_or_connected: yvar xvar -> {kind:plottype, points:[[x,y],...], style, n}.
ojson layerLineOrConnected(Datastore& ds, const std::vector<std::string>& varlist,
                           const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                           int layerIdx, const std::string& plottype) {
  if (varlist.size() < 2) {
    return ojson{{"_error", plottype + " requires two variables: yvar xvar"}};
  }
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto pts = maskSortXY(numericColumn(ds, yvar), numericColumn(ds, xvar));
  ojson pointsJ = ojson::array();
  for (const auto& [x, y] : pts) pointsJ.push_back(ojson::array({x, y}));
  ojson out;
  out["kind"] = plottype;
  out["points"] = pointsJ;
  out["style"] = twoway::layerStyle(opts, layerIdx, plottype);
  out["n"] = static_cast<int64_t>(pts.size());
  return out;
}

// _layer_scatter: yvar xvar -> {kind:"scatter", points:[[x,y],...] (unsorted), style, n}.
ojson layerScatter(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 2) return ojson{{"_error", "scatter requires two variables: yvar xvar"}};
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto yArr = numericColumn(ds, yvar);
  auto xArr = numericColumn(ds, xvar);
  ojson pointsJ = ojson::array();
  int64_t n = 0;
  for (size_t i = 0; i < yArr.size(); ++i) {
    if (!std::isfinite(yArr[i]) || !std::isfinite(xArr[i])) continue;
    pointsJ.push_back(ojson::array({xArr[i], yArr[i]}));
    ++n;
  }
  ojson out;
  out["kind"] = "scatter";
  out["points"] = pointsJ;
  out["style"] = twoway::layerStyle(opts, layerIdx, "scatter");
  out["n"] = n;
  return out;
}

// BUG-099: closed-form 2-variable OLS -- y = a + b*x, b = cov(x,y)/var(x),
// a = ybar - b*xbar (the (n-1) denominators of cov/var cancel, so summing
// unnormalized cross-products is equivalent and avoids a spurious n==1 edge
// case). Deterministic, dependency-free, no cobra round-trip needed for a
// 2-coefficient fit -- verified bit-for-bit against the live-Stata oracle
// (`regress income beertax` on fatalities.csv: b_cons=14842.18,
// b_beertax=-1874.3005; yhat_at_xmin=14761.003, yhat_at_xmax=9742.6517 --
// e-oracle-stata.log). Returns nullopt if x has zero variance (undefined
// slope), mirroring ols3's identical singular-input guard below.
std::optional<std::pair<double, double>> ols2(const std::vector<std::pair<double, double>>& xy) {
  size_t n = xy.size();
  if (n < 2) return std::nullopt;
  double sx = 0.0, sy = 0.0;
  for (const auto& [x, y] : xy) {
    sx += x;
    sy += y;
  }
  double mx = sx / static_cast<double>(n), my = sy / static_cast<double>(n);
  double sxx = 0.0, sxy = 0.0;
  for (const auto& [x, y] : xy) {
    double dx = x - mx;
    sxx += dx * dx;
    sxy += dx * (y - my);
  }
  if (sxx <= 0.0) return std::nullopt;
  double b = sxy / sxx;
  double a = my - b * mx;
  return std::make_pair(a, b);
}

// Gaussian elimination with partial pivoting for a 3x3 system Ax=b (A/b
// modified in place). Returns nullopt if the system is (numerically)
// singular -- e.g. fewer than 3 distinct x values.
std::optional<std::array<double, 3>> solve3x3(double A[3][3], double b[3]) {
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    double best = std::fabs(A[col][col]);
    for (int r = col + 1; r < 3; ++r) {
      if (std::fabs(A[r][col]) > best) {
        best = std::fabs(A[r][col]);
        piv = r;
      }
    }
    if (best < 1e-9) return std::nullopt;
    if (piv != col) {
      std::swap(A[piv], A[col]);
      std::swap(b[piv], b[col]);
    }
    for (int r = col + 1; r < 3; ++r) {
      double f = A[r][col] / A[col][col];
      for (int c = col; c < 3; ++c) A[r][c] -= f * A[col][c];
      b[r] -= f * b[col];
    }
  }
  std::array<double, 3> x{};
  for (int r = 2; r >= 0; --r) {
    double s = b[r];
    for (int c = r + 1; c < 3; ++c) s -= A[r][c] * x[static_cast<size_t>(c)];
    x[static_cast<size_t>(r)] = s / A[r][r];
  }
  return x;
}

// BUG-099: closed-form quadratic fit -- y = a + b1*x + b2*x^2, via the
// 3-coefficient normal equations (X'X)coef = X'y built directly from power
// sums (Sum(x^0..x^4), Sum(x^0..x^2 * y)) rather than an explicit design
// matrix. Verified bit-for-bit against the live-Stata oracle (`regress
// income beertax beertax2`: b_cons=15931.426, b1=-5823.7769/b2=1909.7359 --
// e-oracle-stata.log's "Eb qfit coefficients" section).
std::optional<std::array<double, 3>> ols3(const std::vector<std::pair<double, double>>& xy) {
  size_t n = xy.size();
  if (n < 3) return std::nullopt;
  double s0 = static_cast<double>(n), s1 = 0, s2 = 0, s3 = 0, s4 = 0, sy = 0, sxy = 0, sx2y = 0;
  for (const auto& [x, y] : xy) {
    double x2 = x * x;
    s1 += x;
    s2 += x2;
    s3 += x2 * x;
    s4 += x2 * x2;
    sy += y;
    sxy += x * y;
    sx2y += x2 * y;
  }
  double A[3][3] = {{s0, s1, s2}, {s1, s2, s3}, {s2, s3, s4}};
  double B[3] = {sy, sxy, sx2y};
  return solve3x3(A, B);
}

// BUG-099: `lfit yvar xvar` -- real OLS-fitted straight line, rendered as
// the 2-point line [ (xmin, yhat(xmin)), (xmax, yhat(xmax)) ] over the
// layer's OWN data range (post if-filter, matching Stata's `regress`+
// `summarize x` both running on the SAME estimation sample). Kind is
// "lfit", NOT "line" -- the frontend styling lane distinguishes it -- but
// the `points` array shape is identical to layerLineOrConnected's, so any
// renderer that doesn't yet know about "lfit" specifically still degrades
// gracefully to drawing a 2-point line.
ojson layerLfit(Datastore& ds, const std::vector<std::string>& varlist,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 2) return ojson{{"_error", "lfit requires two variables: yvar xvar"}};
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto pts = maskSortXY(numericColumn(ds, yvar), numericColumn(ds, xvar));  // sorted by x
  if (pts.size() < 2) return ojson{{"_error", "lfit requires at least 2 non-missing observations"}};
  auto fit = ols2(pts);
  if (!fit.has_value()) {
    return ojson{{"_error", "lfit: " + xvar + " has zero variance (all values equal) -- slope is undefined"}};
  }
  auto [a, b] = *fit;
  double xmin = pts.front().first, xmax = pts.back().first;
  ojson pointsJ = ojson::array();
  pointsJ.push_back(ojson::array({xmin, a + b * xmin}));
  pointsJ.push_back(ojson::array({xmax, a + b * xmax}));
  ojson out;
  out["kind"] = "lfit";
  out["points"] = pointsJ;
  out["coef"] = ojson{{"cons", a}, {"slope", b}};
  out["style"] = twoway::layerStyle(opts, layerIdx, "lfit");
  out["n"] = static_cast<int64_t>(pts.size());
  return out;
}

// BUG-099: `qfit yvar xvar` -- real OLS-fitted quadratic curve, sampled at
// n=100 evenly-spaced points across the layer's own x-range (Stata itself
// samples the fitted curve over the data's range rather than plotting the
// raw observations; n=100 is this port's own choice, undocumented by
// Stata, noted here since it's not oracle-pinned). Kind "qfit"; `points`
// shape matches layerLineOrConnected's for the same graceful-degradation
// reason as lfit above.
ojson layerQfit(Datastore& ds, const std::vector<std::string>& varlist,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 2) return ojson{{"_error", "qfit requires two variables: yvar xvar"}};
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto pts = maskSortXY(numericColumn(ds, yvar), numericColumn(ds, xvar));  // sorted by x
  if (pts.size() < 3) return ojson{{"_error", "qfit requires at least 3 non-missing observations"}};
  auto fit = ols3(pts);
  if (!fit.has_value()) {
    return ojson{{"_error", "qfit: " + xvar + " has insufficient variation for a quadratic fit"}};
  }
  const auto& coef = *fit;  // {a, b1, b2}
  double xmin = pts.front().first, xmax = pts.back().first;
  constexpr int kQfitN = 100;
  ojson pointsJ = ojson::array();
  double step = (xmax > xmin) ? (xmax - xmin) / (kQfitN - 1) : 0.0;
  for (int i = 0; i < kQfitN; ++i) {
    double x = (i == kQfitN - 1) ? xmax : xmin + step * i;
    double y = coef[0] + coef[1] * x + coef[2] * x * x;
    pointsJ.push_back(ojson::array({x, y}));
  }
  ojson out;
  out["kind"] = "qfit";
  out["points"] = pointsJ;
  out["coef"] = ojson{{"cons", coef[0]}, {"b1", coef[1]}, {"b2", coef[2]}};
  out["style"] = twoway::layerStyle(opts, layerIdx, "qfit");
  out["n"] = static_cast<int64_t>(pts.size());
  return out;
}

// _parse_scale_range: extract (lo, hi) from `xscale(range(LO HI) ...)`.
std::optional<std::pair<double, double>> parseScaleRange(const MaybeOptFromOpts& scaleOpt) {
  if (!scaleOpt.present || scaleOpt.isTrue) return std::nullopt;
  static const std::regex reRange(R"(range\(\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*\))");
  std::smatch m;
  if (std::regex_search(scaleOpt.str, m, reRange)) {
    try {
      return std::make_pair(std::stod(m[1].str()), std::stod(m[2].str()));
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// _reflines: split a `xline(0 5 10)`-style option into a list of doubles.
ojson reflines(const MaybeOptFromOpts& raw) {
  ojson out = ojson::array();
  if (!raw.present || raw.isTrue) return out;
  std::string s = raw.str;
  size_t a = 0, b = s.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (a < b && isSp(s[a])) ++a;
  while (b > a && isSp(s[b - 1])) --b;
  s = s.substr(a, b - a);
  static const std::regex reSplit(R"([,\s]+)");
  std::sregex_token_iterator it(s.begin(), s.end(), reSplit, -1), end;
  for (; it != end; ++it) {
    std::string tok = it->str();
    if (tok.empty()) continue;
    try {
      size_t pos = 0;
      double v = std::stod(tok, &pos);
      if (pos == tok.size()) out.push_back(v);
    } catch (...) {
    }
  }
  return out;
}

}  // namespace

ojson runTwowayLayers(Datastore* dsPtr, SessionState& session,
                      const std::vector<twoway::LayerSpec>& layers,
                      const std::vector<std::pair<std::string, eval::OptValue>>& globalOpts,
                      const std::optional<std::string>& globalIf) {
  if (dsPtr == nullptr) return ojson(eval::noDataLoaded());
  if (layers.empty()) return engineErrorBlockG("twoway: no layers specified", "syntax_error");
  if (auto err = rejectByFacet(globalOpts)) return *err;  // BS-052: by() is whole-graph, never per-layer

  // BUG-129 (graph-honesty): an unsupported/unrecognised twoway plot type is a
  // reject-BEFORE-output error — the WHOLE command fails, no partial plot,
  // exactly as the B.0.3 option contract rejects an unknown option before any
  // result is emitted. Pre-scan every layer's plot type up front so a lone
  // `(rcap ...)` no longer renders an empty framed plot that "succeeds" (rc=0)
  // and a `(scatter ...)(lfitci ...)` no longer draws the scatter with the
  // requested CI band silently missing (Doctrine ① — "answers a different
  // question"). Supported layer plot types = the six with real builders below.
  static const std::set<std::string> kSupportedLayerTypes = {
      "scatter", "line", "connected", "lfit", "qfit", "rarea"};
  for (const auto& spec : layers) {
    std::string pt = spec.plottype;
    std::transform(pt.begin(), pt.end(), pt.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (kSupportedLayerTypes.count(pt) == 0) {
      return engineErrorBlockG(
          "twoway: plot type `" + pt +
              "' is not supported in this version of PIET — supported plot types: "
              "scatter line connected lfit qfit rarea.",
          "unsupported_command");
    }
  }

  // AER scope-2 W4 -- THE UNKNOWN-OPTION NET FOR THE MULTI-LAYER PATH.
  //
  // Measured on main before this landed: `twoway (scatter y x) (line y x),
  // xlabel(1 2) bogusopt(1)` was rc 0. The whole parenthesised/`||` route had no
  // net at all, on either plane, so a typed option name was accepted, dropped
  // and never mentioned -- the cardinal defect class, because the user gets a
  // plot and never learns the option did nothing. The single-plot arm
  // (dispatcher.cpp) has had a net since BUG-336; this is its twin.
  //
  // POSITION IS LOAD-BEARING, and it is why this sits here rather than at the
  // dispatcher's `merged`: the supported-plot-type pre-scan above owns a reject
  // about the COMMAND, and it must win. `twoway (rcap hi lo x, lstyle(ci))` has
  // to answer "plot type `rcap' is not supported", not "option `lstyle' not
  // allowed" -- the same ordering rule the bare arm's comment states, and the
  // same one four self-test legs already lock for `graph bar y, over(x)`.
  //
  // The vocabulary is read back from the registry rather than spelled again
  // here: `twoway_layer` is registered in dispatcher.cpp beside the plot-type
  // heads (`connected`, `lfit`, `rarea`, ...), which is the one copy of the set.
  // A non-command token in the registry has precedent (`mediate:tvar`).
  // rejectByFacet above already answered for `by()`, so a by() never reaches
  // this net and keeps its specific message.
  {
    const eval::OptVocab* lv = eval::findOptVocab("twoway_layer");
    if (lv != nullptr) {
      if (auto err = eval::rejectUnknownOptions(globalOpts, lv->known, lv->unimplemented)) {
        return ojson(*err);
      }
      for (const auto& spec : layers) {
        if (auto err = eval::rejectUnknownOptions(spec.opts, lv->known, lv->unimplemented)) {
          return ojson(*err);
        }
      }
    }
  }

  ojson renderedLayers = ojson::array();
  std::vector<double> allX, allY;
  std::vector<std::string> layerErrors;

  for (size_t idx = 0; idx < layers.size(); ++idx) {
    const twoway::LayerSpec& spec = layers[idx];

    std::optional<std::string> effectiveIf;
    if (globalIf.has_value() && !globalIf->empty() && spec.ifExpr.has_value() && !spec.ifExpr->empty()) {
      effectiveIf = "(" + *globalIf + ") & (" + *spec.ifExpr + ")";
    } else if (globalIf.has_value() && !globalIf->empty()) {
      effectiveIf = globalIf;
    } else if (spec.ifExpr.has_value() && !spec.ifExpr->empty()) {
      effectiveIf = spec.ifExpr;
    }

    Datastore* layerDs = dsPtr;
    std::string tmpPath;
    std::unique_ptr<Datastore> tmpDs;
    bool filterFailed = false;
    if (effectiveIf.has_value()) {
      tmpPath = nextPathG(session.workdir(), "twowayif");
      std::vector<std::string> args = {"eval", "--data", dsPtr->path(), "--keepif", *effectiveIf,
                                        "--out", tmpPath};
      try {
        runCobraJson(args);
        tmpDs = std::make_unique<Datastore>(tmpPath);
        layerDs = tmpDs.get();
      } catch (const std::exception& e) {
        layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " + e.what());
        filterFailed = true;
      }
      if (!tmpPath.empty()) cecon::native_unlink(tmpPath);
    }
    if (filterFailed) continue;

    std::string pt = spec.plottype;
    std::transform(pt.begin(), pt.end(), pt.begin(), [](unsigned char c) { return std::tolower(c); });
    ojson lyr;
    if (pt == "rarea") {
      lyr = layerRarea(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else if (pt == "lfit") {
      // BUG-099: was aliased straight into layerLineOrConnected -- a raw
      // connect-the-dots polyline through the DATA points, never an actual
      // fit. Now a real closed-form OLS line (see layerLfit above).
      lyr = layerLfit(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else if (pt == "qfit") {
      // BUG-099: same aliasing bug, quadratic case (see layerQfit above).
      lyr = layerQfit(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else if (pt == "line") {
      lyr = layerLineOrConnected(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx), "line");
    } else if (pt == "connected") {
      lyr = layerLineOrConnected(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx), "connected");
    } else if (pt == "scatter") {
      lyr = layerScatter(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else {
      // Unreachable: the reject-before-output pre-scan at the top of
      // runTwowayLayers already failed the whole command for any plot type
      // outside the supported set (BUG-129). Defensive guard only.
      return engineErrorBlockG("twoway: internal — unhandled plot type `" + pt + "'.",
                               "engine_error");
    }

    if (lyr.contains("_error")) {
      layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " +
                            lyr["_error"].get<std::string>());
      continue;
    }

    std::string lkind = lyr.value("kind", std::string());
    if (lkind == "rarea") {
      for (const auto& row : lyr.value("band", ojson::array())) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allY.push_back(row[2].get<double>());
      }
    } else if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
               lkind == "qfit") {
      for (const auto& pt2 : lyr.value("points", ojson::array())) {
        allX.push_back(pt2[0].get<double>());
        allY.push_back(pt2[1].get<double>());
      }
    }

    renderedLayers.push_back(lyr);
  }

  bool anyDataLayer = false;
  for (const auto& l : renderedLayers) {
    if (l.value("kind", std::string()) != "unsupported") {
      anyDataLayer = true;
      break;
    }
  }
  if (!anyDataLayer && !layerErrors.empty()) {
    std::string joined;
    for (size_t i = 0; i < layerErrors.size(); ++i) {
      if (i) joined += "; ";
      joined += layerErrors[i];
    }
    return engineErrorBlockG("twoway: all layers failed. " + joined, "engine_error");
  }

  double xMin, xMax, yMin, yMax;
  if (allX.empty()) {
    // No plottable points in any layer (e.g. save_empty_plot's all-missing
    // y) — Stata still draws the framed plot; use the explicit scale
    // range() if given, else a unit default. Never error.
    MaybeOptFromOpts xrOpt = getOptG(globalOpts, "xscale");
    MaybeOptFromOpts yrOpt = getOptG(globalOpts, "yscale");
    auto xr = parseScaleRange(xrOpt);
    auto yr = parseScaleRange(yrOpt);
    std::tie(xMin, xMax) = xr.value_or(std::make_pair(0.0, 1.0));
    std::tie(yMin, yMax) = yr.value_or(std::make_pair(0.0, 1.0));
  } else {
    xMin = *std::min_element(allX.begin(), allX.end());
    xMax = *std::max_element(allX.begin(), allX.end());
    yMin = *std::min_element(allY.begin(), allY.end());
    yMax = *std::max_element(allY.begin(), allY.end());
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "twoway";
  out["title"] = optTitleStr(globalOpts, "title").value_or(std::string());
  out["subtitle"] = optTitleStr(globalOpts, "subtitle").value_or(std::string());
  out["x_label"] = optTitleStr(globalOpts, "xtitle").value_or(std::string());
  out["y_label"] = optTitleStr(globalOpts, "ytitle").value_or(std::string());
  out["note"] = optTitleStr(globalOpts, "note").value_or(std::string());
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["layers"] = renderedLayers;

  ojson axes;
  axes["xline"] = reflines(getOptG(globalOpts, "xline"));
  axes["yline"] = reflines(getOptG(globalOpts, "yline"));
  {
    MaybeOptFromOpts xs = getOptG(globalOpts, "xscale");
    std::string xsLower = xs.present && !xs.isTrue ? xs.str : "";
    std::transform(xsLower.begin(), xsLower.end(), xsLower.begin(), [](unsigned char c) { return std::tolower(c); });
    axes["xscale_noline"] = xsLower.find("noline") != std::string::npos;
  }
  {
    MaybeOptFromOpts ys = getOptG(globalOpts, "yscale");
    std::string ysLower = ys.present && !ys.isTrue ? ys.str : "";
    std::transform(ysLower.begin(), ysLower.end(), ysLower.begin(), [](unsigned char c) { return std::tolower(c); });
    axes["yscale_noline"] = ysLower.find("noline") != std::string::npos;
  }
  {
    MaybeOptFromOpts xl = getOptG(globalOpts, "xlabel");
    std::string xlLower = xl.present && !xl.isTrue ? xl.str : "";
    std::transform(xlLower.begin(), xlLower.end(), xlLower.begin(), [](unsigned char c) { return std::tolower(c); });
    // strip() then compare, mirroring the oracle's `.strip().lower() == "none"`.
    size_t a = xlLower.find_first_not_of(" \t");
    size_t b = xlLower.find_last_not_of(" \t");
    std::string trimmed = (a == std::string::npos) ? std::string() : xlLower.substr(a, b - a + 1);
    axes["xlabel_none"] = (trimmed == "none");
  }
  {
    MaybeOptFromOpts yl = getOptG(globalOpts, "ylabel");
    std::string ylLower = yl.present && !yl.isTrue ? yl.str : "";
    std::transform(ylLower.begin(), ylLower.end(), ylLower.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t a = ylLower.find_first_not_of(" \t");
    size_t b = ylLower.find_last_not_of(" \t");
    std::string trimmed = (a == std::string::npos) ? std::string() : ylLower.substr(a, b - a + 1);
    axes["ylabel_none"] = (trimmed == "none");
  }
  out["axes"] = axes;

  // Legend: legend(order(2 "pooled" 4 "h1") cols(1) size(vsmall)) / legend(off).
  ojson legendOrder = ojson::array();
  bool legendOff = false;
  ojson legendCols = nullptr;
  ojson legendSize = nullptr;
  MaybeOptFromOpts legendRaw = getOptG(globalOpts, "legend");
  if (legendRaw.present && !legendRaw.isTrue) {
    std::string legS = legendRaw.str;
    std::string legLower = legS;
    std::transform(legLower.begin(), legLower.end(), legLower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (legLower.find("off") != std::string::npos) legendOff = true;
    static const std::regex reOrder(R"(order\(([^)]+)\))");
    std::smatch mOrder;
    if (std::regex_search(legS, mOrder, reOrder)) {
      static const std::regex rePair(R"re((\d+)\s+"([^"]+)")re");
      std::string inner = mOrder[1].str();
      auto begin = std::sregex_iterator(inner.begin(), inner.end(), rePair);
      auto end = std::sregex_iterator();
      for (auto it = begin; it != end; ++it) {
        int idxS = std::stoi((*it)[1].str());
        legendOrder.push_back(ojson::array({idxS - 1, (*it)[2].str()}));
      }
    }
    static const std::regex reCols(R"(cols\((\d+)\))");
    std::smatch mCols;
    if (std::regex_search(legS, mCols, reCols)) legendCols = std::stoi(mCols[1].str());
    static const std::regex reSize(R"(size\(([^)]+)\))");
    std::smatch mSize;
    if (std::regex_search(legS, mSize, reSize)) {
      std::string sz = mSize[1].str();
      size_t a = sz.find_first_not_of(" \t");
      size_t b = sz.find_last_not_of(" \t");
      legendSize = (a == std::string::npos) ? std::string() : sz.substr(a, b - a + 1);
    }
  }
  ojson legend;
  legend["order"] = legendOrder;
  legend["cols"] = legendCols;
  legend["size"] = legendSize;
  legend["off"] = legendOff;
  out["legend"] = legend;

  out["name"] = nullptr;  // filled by the dispatcher's name() capture
  int64_t nObs = 0;
  for (const auto& l : renderedLayers) nObs += l.value("n", 0);
  out["n_obs"] = nObs;
  // BS-047 (BETA A4): session.graphicsOff now genuinely tracks `set graphics
  // on|off` (dispatcher.cpp's `set` arm) — previously always false since
  // SessionState had no such field (see the superseded comment this
  // replaces).
  out["suppressed"] = session.graphicsOff;
  return out;
}

// ===========================================================================
// marginsplot

ojson runMarginsplot(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const double kZ975 = 1.959963984540054;
  if (!session.lastMargins.has_value()) {
    return engineErrorBlockG("marginsplot: no margins result in memory. Run `margins` first.");
  }
  const ojson& raw = *session.lastMargins;
  ojson coefs = raw.value("coefs", ojson::array());

  if (!session.lastMarginsDydx.empty()) {
    std::set<std::string> dydxSet(session.lastMarginsDydx.begin(), session.lastMarginsDydx.end());
    ojson filtered = ojson::array();
    for (const auto& c : coefs) {
      if (c.contains("name") && dydxSet.count(c["name"].get<std::string>())) filtered.push_back(c);
    }
    coefs = filtered;
  }
  if (coefs.empty()) {
    return engineErrorBlockG("marginsplot: margins result has no estimates to plot.");
  }

  std::string model = raw.value("model", std::string());
  int64_t nObs = raw.contains("n_obs") ? raw["n_obs"].get<int64_t>() : 0;

  ojson points = ojson::array();
  std::vector<double> allLo, allHi;
  for (const auto& c : coefs) {
    std::string name = c.value("name", std::string("?"));
    ojson bVal = c.contains("dydx") ? c["dydx"] : (c.contains("b") ? c["b"] : ojson(nullptr));
    ojson seVal = c.contains("se") ? c["se"] : ojson(nullptr);
    if (bVal.is_null() || seVal.is_null()) continue;
    double b, se;
    try {
      b = bVal.get<double>();
      se = seVal.get<double>();
    } catch (...) {
      continue;
    }
    double ciLo = b - kZ975 * se, ciHi = b + kZ975 * se;
    ojson p;
    p["name"] = name;
    p["b"] = b;
    p["se"] = se;
    p["ci_lo"] = ciLo;
    p["ci_hi"] = ciHi;
    points.push_back(p);
    allLo.push_back(ciLo);
    allHi.push_back(ciHi);
  }
  if (points.empty()) {
    return engineErrorBlockG("marginsplot: cannot build plot — no numeric AME estimates found.");
  }

  double yMin = *std::min_element(allLo.begin(), allLo.end());
  double yMax = *std::max_element(allHi.begin(), allHi.end());
  double span = (yMax != yMin) ? (yMax - yMin) : 0.1;
  yMin -= 0.1 * span;
  yMax += 0.1 * span;
  yMin = std::min(yMin, 0.0);
  yMax = std::max(yMax, 0.0);

  std::string modelLabel = model.empty() ? "" : (" (" + model + ")");
  std::string titleDefault = "Average Marginal Effects" + modelLabel;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "marginsplot";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["y_label"] = "Average marginal effect (dydx)";
  out["x_label"] = "";
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["n"] = nObs;
  out["model"] = model;
  out["points"] = points;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011, mirrors BS-010)
  return out;
}

// ===========================================================================
// M4b: graph save / combine / export — C++ port of graph_combine.py +
// graph_export.py. SVG geometry constants and tick/scale helpers below
// MUST match graph_export.py's `_W/_H/_MT/_MR/_MB/_ML`/`_nice_ticks`/
// `_fmt_tick`/`_padded_range`/`_sx`/`_sy` byte-for-byte (same source of
// truth graph.js's frontend renderer already uses) — this is the
// "SVG structural" parity the M4 milestone gate gauges debug_all against.

namespace {

bool fileExistsG(const std::string& path) { return cecon::native_exists(path); }

// BS-007 (D3): the on-disk read-back half of runGraphSave's (below)
// {"_magic":"PIETGPH1","block":<block>} envelope -- a graph SPEC-JSON
// serializer+loader pair, NEVER a raster/frozen-pixel format (D3 hard-stop:
// growing this beyond serialize+load is out of scope). Returns nullopt for
// any of: file missing/unreadable, not valid JSON, wrong/missing magic, or
// no "block" key -- callers turn that into an honest reject, never a crash.
std::optional<nlohmann::ordered_json> loadGphFile(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return std::nullopt;
  // Whole-file slurp: the same `ostringstream << rdbuf()` idiom used
  // throughout this codebase (doexec.cpp/handlers.cpp/main.cpp/routes.cpp/
  // settings.cpp) rather than istreambuf_iterator, for consistency.
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  nlohmann::ordered_json doc;
  try {
    doc = nlohmann::ordered_json::parse(text);
  } catch (...) {
    return std::nullopt;
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1" || !doc.contains("block")) {
    return std::nullopt;
  }
  return doc["block"];
}

std::string svgEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

// SVG geometry constants (graph_export.py's _W/_H/_MT/_MR/_MB/_ML).
constexpr double kSvgW = 520.0, kSvgH = 360.0;
constexpr double kSvgMT = 34.0, kSvgMR = 18.0, kSvgMB = 48.0, kSvgML = 62.0;
constexpr double kSvgPlotW = kSvgW - kSvgML - kSvgMR;   // 440
constexpr double kSvgPlotH = kSvgH - kSvgMT - kSvgMB;   // 278

std::vector<double> niceTicksG(double lo, double hi, int target = 5) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0};
  if (lo == hi) return {lo};
  double span = hi - lo;
  double raw = span / target;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  double norm = raw / mag;
  double step;
  if (norm < 1.5) step = 1;
  else if (norm < 3) step = 2;
  else if (norm < 7) step = 5;
  else step = 10;
  step *= mag;
  double start = std::ceil(lo / step) * step;
  std::vector<double> ticks;
  for (double t = start; t <= hi + step * 1e-9; t += step) {
    ticks.push_back(std::abs(t) < step * 1e-9 ? 0.0 : t);
  }
  if (ticks.empty()) ticks = {lo, hi};
  return ticks;
}

std::string trimTrailingZeros(std::string s) {
  if (s.find('.') == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string fmtTickG(double v) {
  if (v == 0) return "0";
  double a = std::abs(v);
  char buf[64];
  if (a >= 1e6) {
    double f = v / 1e6;
    std::snprintf(buf, sizeof(buf), a >= 1e7 ? "%.0fM" : "%.1fM", f);
    return buf;
  }
  if (a >= 1e4) {
    std::snprintf(buf, sizeof(buf), "%.0fk", v / 1e3);
    return buf;
  }
  if (v == std::floor(v)) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 100) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 1) {
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return trimTrailingZeros(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.2g", v);
  return trimTrailingZeros(buf);
}

std::pair<double, double> paddedRangeG(double lo, double hi, double padLo = 0.04, double padHi = 0.04) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0, 1.0};
  if (lo == hi) return {lo - 0.5, hi + 0.5};
  double span = hi - lo;
  return {lo - span * padLo, hi + span * padHi};
}

double svgSx(double x, double x0, double x1, double plotW = kSvgPlotW) {
  return kSvgML + ((x - x0) / (x1 - x0)) * plotW;
}

// Conservative advance-width estimate (SVG has no font metrics here): em
// fractions per character class, one em per non-ASCII code point.
double svgTextWidth(const std::string& s, double fontSize) {
  double em = 0.0;
  for (unsigned char c : s) {
    if (c >= 0x80) {
      if (c >= 0xC0) em += 1.0;
    } else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') {
      em += 0.9;
    } else if (c >= 'A' && c <= 'Z') {
      em += 0.72;
    } else if (c == ' ' || c == 'i' || c == 'j' || c == 'l' || c == 't' || c == 'f' || c == 'r' || c == 'I' ||
               c == '.' || c == ',' || c == ':' || c == ';' || c == '|' || c == '\'' || c == '!') {
      em += 0.4;
    } else {
      em += 0.6;
    }
  }
  return em * fontSize;
}
double svgSy(double y, double y0, double y1) {
  return kSvgMT + kSvgPlotH - ((y - y0) / (y1 - y0)) * kSvgPlotH;
}

char fbuf[64];
std::string fmtNum(double v, const char* spec = "%.1f") {
  std::snprintf(fbuf, sizeof(fbuf), spec, v);
  return fbuf;
}

void svgFrameAndAxes(std::vector<std::string>& lines, std::pair<double, double> xr,
                      std::pair<double, double> yr, const std::string& xLabel,
                      const std::string& yLabel, bool suppressXTickLabels = false,
                      bool suppressYTickLabels = false, double plotW = kSvgPlotW) {
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  lines.push_back("  <rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" + fmtNum(kSvgMT, "%.0f") +
                   "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" + fmtNum(kSvgPlotH, "%.0f") +
                   "\" fill=\"none\" stroke=\"#aaa\" stroke-width=\"1\"/>");

  for (double t : niceTicksG(x0, x1)) {
    double sx = svgSx(t, x0, x1, plotW);
    if (sx < kSvgML - 0.5 || sx > kSvgML + plotW + 0.5) continue;
    double ty = kSvgMT + kSvgPlotH;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(ty, "%.0f") + "\" x2=\"" +
                     fmtNum(sx) + "\" y2=\"" + fmtNum(ty + 5, "%.0f") +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // marginsplot (BS-011) suppresses the numeric x tick text -- it places
    // its own categorical term-name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressXTickLabels:true}) for the identical
    // reason). The tick MARK still draws either way.
    if (!suppressXTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(ty + 18, "%.0f") +
                       "\" text-anchor=\"middle\" font-size=\"10\" fill=\"#444\">" +
                       svgEscape(fmtTickG(t)) + "</text>");
    }
  }

  for (double t : niceTicksG(y0, y1)) {
    double sy = svgSy(t, y0, y1);
    if (sy < kSvgMT - 0.5 || sy > kSvgMT + kSvgPlotH + 0.5) continue;
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML - 5, "%.0f") + "\" y1=\"" + fmtNum(sy) +
                     "\" x2=\"" + fmtNum(kSvgML, "%.0f") + "\" y2=\"" + fmtNum(sy) +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // coefplot (BS-011) suppresses the numeric y tick text -- it places its
    // own per-coefficient name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressYTickLabels:true})).
    if (!suppressYTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                       "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(fmtTickG(t)) +
                       "</text>");
    }
  }

  double midX = kSvgML + plotW / 2;
  lines.push_back("  <text x=\"" + fmtNum(midX) + "\" y=\"" + fmtNum(kSvgH - 10, "%.0f") +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\">" + svgEscape(xLabel) +
                   "</text>");
  double midY = kSvgMT + kSvgPlotH / 2;
  lines.push_back("  <text x=\"14\" y=\"" + fmtNum(midY) +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\" transform=\"rotate(-90 14 " +
                   fmtNum(midY) + ")\">" + svgEscape(yLabel) + "</text>");
}

void svgTitle(std::vector<std::string>& lines, const std::string& title, double width = kSvgW) {
  lines.push_back("  <text x=\"" + fmtNum(width / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                   "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
}

double jnum(const ojson& v, double dflt = 0.0) {
  if (v.is_null()) return dflt;
  try {
    return v.get<double>();
  } catch (...) {
    return dflt;
  }
}

// jnum's "read a possibly-absent/possibly-null object KEY" counterpart --
// used by buildSvgMarginsplot/buildSvgCoefplot below (BS-011) where a
// coefplot row's ci_lo/ci_hi may be JSON null (no CI for that coefficient;
// see eststo_esttab.cpp's runCoefplot, which stores `ojson(nullptr)` for a
// NaN bound) -- `obj[key]` alone would still be a legal (null) ojson, but
// jnum() only guards a value ALREADY in hand, not a missing key.
double jnumOr(const ojson& obj, const char* key, double dflt) {
  if (!obj.contains(key) || obj[key].is_null()) return dflt;
  return jnum(obj[key], dflt);
}

// BS-055 (BETA E): shared multi-series renderer -- xtline (`series:
// [{id,label,points}]`) and tsline (`series: [{var,points}]`) both carry
// their data in a top-level `series` array, NOT the flat `points` key every
// OTHER kind uses. buildSvgDispatch had no "xtline"/"tsline" branch at all,
// so both silently fell through to buildSvgScatter's DEFAULT arm, which
// reads a "points" key neither block shape has -- frame/axes/title render
// (svgFrameAndAxes doesn't care about kind), zero data (an empty `pts`
// array), exactly the bug's own description. Also reused by BS-050's
// multi-y `scatter y1 y2 x` -- see buildSvgScatter's delegation below.
// `asMarkers=true` draws small circles (scatter-style); false draws
// connected polylines (xtline/tsline-style) -- matching each kind's own
// natural rendering.
std::string buildSvgSeriesPlot(const ojson& block, bool asMarkers) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  lines.push_back("  <clipPath id=\"sc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#sc)\">");
  ojson series = block.value("series", ojson::array());
  for (size_t si = 0; si < series.size(); ++si) {
    ojson pts = series[si].value("points", ojson::array());
    if (pts.empty()) continue;
    std::string color;
    if (series[si].contains("style") && series[si]["style"].contains("color") &&
        series[si]["style"]["color"].is_string()) {
      color = series[si]["style"]["color"].get<std::string>();
    } else {
      color = twoway::kSeriesColors[si % twoway::kSeriesColors.size()];
    }
    if (!asMarkers) {
      std::string d;
      for (size_t i = 0; i < pts.size(); ++i) {
        double sx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
        double sy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
        d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
      }
      if (!d.empty()) d.pop_back();
      lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                       "\" stroke-width=\"1.5\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>");
    } else {
      double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
      for (const auto& xy : pts) {
        double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
        double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
        lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                         fmtNum(r) + "\" fill=\"" + color + "\" opacity=\"0.8\"/>");
      }
    }
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgScatter(const ojson& block) {
  // BS-050: multi-y `scatter y1 y2 x` populates a "series" array (2+
  // entries) alongside the legacy flat "points" -- delegate to the shared
  // multi-series renderer (marker/dot style) so every y-series actually
  // gets its own distinguishable color instead of all points collapsing
  // into one flat #1f77b4 blob. The single-y case (no "series" key, the
  // overwhelmingly common path) is UNCHANGED below.
  if (block.contains("series") && block["series"].is_array() && !block["series"].empty()) {
    return buildSvgSeriesPlot(block, /*asMarkers=*/true);
  }
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
  lines.push_back("  <clipPath id=\"gc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#gc)\">");
  for (const auto& xy : pts) {
    double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
    double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
    lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                     fmtNum(r) + "\" fill=\"#1f77b4\" opacity=\"0.8\"/>");
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgHistogram(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))), 0.02, 0.02);
  double yMaxRaw = jnum(block.value("y_max", ojson(0.0)));
  double yMax = yMaxRaw > 0 ? yMaxRaw : 1.0;
  std::pair<double, double> yr = {0.0, yMax * 1.08};
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  double baseY = svgSy(0, yr.first, yr.second);
  for (const auto& b : block.value("bins", ojson::array())) {
    double bx0 = svgSx(jnum(b["x0"]), xr.first, xr.second);
    double bx1 = svgSx(jnum(b["x1"]), xr.first, xr.second);
    double ytop = svgSy(jnum(b["y"]), yr.first, yr.second);
    double w = std::max(0.5, bx1 - bx0 - 0.6);
    double h = std::max(0.0, baseY - ytop);
    lines.push_back("  <rect x=\"" + fmtNum(bx0 + 0.3) + "\" y=\"" + fmtNum(ytop) + "\" width=\"" +
                     fmtNum(w) + "\" height=\"" + fmtNum(h) +
                     "\" fill=\"#1f77b4\" stroke=\"white\" stroke-width=\"0.5\" opacity=\"0.85\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgLine(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  if (!pts.empty()) {
    std::string d;
    for (size_t i = 0; i < pts.size(); ++i) {
      double cx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
      double cy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
      d += (i == 0 ? "M" : "L") + fmtNum(cx, "%.2f") + " " + fmtNum(cy, "%.2f") + " ";
    }
    if (!d.empty()) d.pop_back();
    lines.push_back("  <path d=\"" + d + "\" fill=\"none\" stroke=\"#1f77b4\" stroke-width=\"1.5\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgTwoway(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;
  // A right-hand legend needs room for its widest rendered label: shrink the
  // plot rect so label text ends inside the viewBox; only if the plot would
  // fall below kMinPlotW widen the viewBox instead.
  ojson layers = block.value("layers", ojson::array());
  ojson legend = block.value("legend", ojson::object());
  constexpr double kLegendGap = 4, kLegendText = 13, kLegendPad = 4, kLegendFont = 9, kMinPlotW = 160;
  double legendLabelW = 0.0;
  bool hasLegend = false;
  if (!legend.value("off", false)) {
    for (const auto& pair : legend.value("order", ojson::array())) {
      int layerIdx = pair[0].get<int>();
      if (layerIdx < 0 || static_cast<size_t>(layerIdx) >= layers.size()) continue;
      std::string label = pair[1].is_string() ? pair[1].get<std::string>() : pair[1].dump();
      legendLabelW = std::max(legendLabelW, svgTextWidth(label, kLegendFont));
      hasLegend = true;
    }
  }
  double plotW = kSvgPlotW, viewW = kSvgW;
  if (hasLegend) {
    double legendW = kLegendGap + kLegendText + legendLabelW + kLegendPad;
    plotW = std::min(kSvgPlotW, kSvgW - kSvgML - legendW);
    if (plotW < kMinPlotW) {
      plotW = kMinPlotW;
      viewW = std::ceil(kSvgML + plotW + legendW);
    }
  }
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(viewW, "%.0f") +
                                     " 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()), viewW);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  false, false, plotW);

  lines.push_back("  <clipPath id=\"tw-clip\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");

  ojson axes = block.value("axes", ojson::object());
  for (const auto& yv : axes.value("yline", ojson::array())) {
    double syRef = svgSy(jnum(yv), y0, y1);
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(syRef) + "\" x2=\"" +
                     fmtNum(kSvgML + plotW, "%.0f") + "\" y2=\"" + fmtNum(syRef) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }
  for (const auto& xv : axes.value("xline", ojson::array())) {
    double sxRef = svgSx(jnum(xv), x0, x1, plotW);
    lines.push_back("  <line x1=\"" + fmtNum(sxRef) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(sxRef) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }

  lines.push_back("  <g clip-path=\"url(#tw-clip)\">");
  for (const auto& layer : layers) {
    std::string lkind = layer.value("kind", std::string("line"));
    ojson style = layer.value("style", ojson::object());

    if (lkind == "rarea") {
      ojson band = layer.value("band", ojson::array());
      if (band.size() >= 2) {
        std::string color = style.value("fcolor", std::string("#aec7e8"));
        double opacity = jnum(style.value("opacity", ojson(0.3)), 0.3);
        std::string lcolor = style.value("lcolor", std::string("none"));
        double lwidth = jnum(style.value("lwidth", ojson(0.0)), 0.0);
        std::string fwd, rev;
        for (size_t i = 0; i < band.size(); ++i) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][2]), y0, y1);
          fwd += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        for (size_t i = band.size(); i-- > 0;) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][1]), y0, y1);
          rev += "L" + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        std::string d = fwd + rev + "Z";
        std::string strokeAttr = (lwidth > 0 && lcolor != "none")
                                      ? ("stroke=\"" + lcolor + "\" stroke-width=\"" + fmtNum(lwidth) + "\"")
                                      : "stroke=\"none\"";
        lines.push_back("    <path d=\"" + d + "\" fill=\"" + color + "\" fill-opacity=\"" +
                         fmtNum(opacity, "%.3g") + "\" " + strokeAttr + "/>");
      }
    } else if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
               lkind == "qfit") {
      // BUG-099: lfit/qfit's `points` shape is identical to line's (a
      // 2-point line / a 100-point sampled curve, both plain [x,y] pairs),
      // so they render through this SAME polyline branch -- no marker dots
      // (the `lkind == "scatter" || lkind == "connected"` guard below
      // deliberately excludes them, matching Stata: a fit line never plots
      // marker symbols).
      ojson pts = layer.value("points", ojson::array());
      if (pts.empty()) continue;
      std::string color = style.value("color", std::string("#1f77b4"));
      double lwidth = jnum(style.value("lwidth", ojson(1.5)), 1.5);
      std::string lpattern = style.contains("lpattern") && !style["lpattern"].is_null()
                                  ? style["lpattern"].get<std::string>()
                                  : std::string();
      std::string msymbol = style.contains("msymbol") && !style["msymbol"].is_null()
                                 ? style["msymbol"].get<std::string>()
                                 : std::string();
      double msize = jnum(style.value("msize", ojson(2.4)), 2.4);
      bool hollow = style.value("hollow", false);

      if ((lkind == "line" || lkind == "connected" || lkind == "lfit" || lkind == "qfit") && lwidth > 0) {
        std::string d;
        for (size_t i = 0; i < pts.size(); ++i) {
          double sx = svgSx(jnum(pts[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(pts[i][1]), y0, y1);
          d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        if (!d.empty()) d.pop_back();
        std::string dashAttr = lpattern.empty() ? "" : (" stroke-dasharray=\"" + lpattern + "\"");
        lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                         "\" stroke-width=\"" + fmtNum(lwidth) +
                         "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"" + dashAttr + "/>");
      }
      if ((lkind == "scatter" || lkind == "connected") && !msymbol.empty() && msymbol != "none") {
        std::string fill = hollow ? "none" : color;
        double sw = hollow ? lwidth : 0.0;
        for (const auto& pt : pts) {
          double cx = svgSx(jnum(pt[0]), x0, x1, plotW);
          double cy = svgSy(jnum(pt[1]), y0, y1);
          lines.push_back("    <circle cx=\"" + fmtNum(cx, "%.2f") + "\" cy=\"" + fmtNum(cy, "%.2f") +
                           "\" r=\"" + fmtNum(msize) + "\" fill=\"" + fill + "\" stroke=\"" + color +
                           "\" stroke-width=\"" + fmtNum(sw) + "\"/>");
        }
      }
    }
  }
  lines.push_back("  </g>");

  if (!legend.value("off", false)) {
    ojson order = legend.value("order", ojson::array());
    if (!order.empty()) {
      double lx = kSvgML + plotW + kLegendGap;
      int li = 0;
      for (const auto& pair : order) {
        int layerIdx = pair[0].get<int>();
        std::string label = pair[1].is_string() ? pair[1].get<std::string>() : pair[1].dump();
        double ly = kSvgMT + 14 + li * 14;
        ++li;
        if (layerIdx < 0 || static_cast<size_t>(layerIdx) >= layers.size()) continue;
        ojson lst = layers[static_cast<size_t>(layerIdx)].value("style", ojson::object());
        std::string color = lst.value("color", lst.value("fcolor", std::string("#1f77b4")));
        lines.push_back("  <line x1=\"" + fmtNum(lx, "%.0f") + "\" y1=\"" + fmtNum(ly - 3) + "\" x2=\"" +
                         fmtNum(lx + 10, "%.0f") + "\" y2=\"" + fmtNum(ly - 3) + "\" stroke=\"" + color +
                         "\" stroke-width=\"1.5\"/>");
        lines.push_back("  <text x=\"" + fmtNum(lx + kLegendText, "%.0f") + "\" y=\"" + fmtNum(ly) +
                         "\" text-anchor=\"start\" font-size=\"9\" fill=\"#333\">" + svgEscape(label) +
                         "</text>");
      }
    }
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

// BS-011 (BETA B-graph-B): coefplot/marginsplot SVG export builders. Neither
// kind had a native (headless `--do`) SVG renderer before -- unreachable
// until now because session.lastGraph was never populated for these two
// kinds (see runCoefplot's/runMarginsplot's dispatcher call sites), so
// `graph export`/`graph save`+`graph combine` after a coefplot/marginsplot
// always hit the "no graph in memory" error and buildSvgDispatch's kind
// table never needed an entry for them. Wiring lastGraph WITHOUT also
// adding these two branches would have swapped that honest error for a
// crash/garbage render (buildSvgScatter indexing `points[i][0]` on a block
// whose points/models are shaped completely differently) -- so the two go
// together. Geometry mirrors piet/frontend/js/graph.js's renderMarginsplot/
// renderCoefplot exactly (categorical axis + whisker-and-dot layout); the
// color palette matches THIS file's existing scatter/line/twoway builders
// (#1f77b4 points, #888 dashed reference line) rather than graph.js's CSS
// theme tokens -- the native SVG export has always been a fixed, theme-
// independent palette (see buildSvgScatter et al. above), never the live
// console's --graph-accent-* variables.

std::string buildSvgMarginsplot(const ojson& block) {
  ojson pts = block.value("points", ojson::array());
  int n = static_cast<int>(pts.size());

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  // Categorical x axis: one evenly-spaced slot per AME term (1..n); numeric
  // x tick text suppressed -- the term-name labels below take its place
  // (graph.js's identical suppressXTickLabels rationale).
  auto xr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))), 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  /*suppressXTickLabels=*/true, /*suppressYTickLabels=*/false);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zy = svgSy(0.0, y0, y1);
  if (zy >= kSvgMT - 0.5 && zy <= kSvgMT + kSvgPlotH + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(zy) + "\" x2=\"" +
                     fmtNum(kSvgML + kSvgPlotW, "%.0f") + "\" y2=\"" + fmtNum(zy) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  double labelY = kSvgMT + kSvgPlotH + 18;
  bool rotate = n > 4;
  for (int i = 0; i < n; ++i) {
    double sx = svgSx(i + 1.0, x0, x1);
    std::string name = pts[static_cast<size_t>(i)].value("name", std::string("?"));
    std::string transformAttr =
        rotate ? (" transform=\"rotate(-35 " + fmtNum(sx) + " " + fmtNum(labelY, "%.0f") + ")\"") : "";
    lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(labelY, "%.0f") + "\" text-anchor=\"" +
                     (rotate ? "end" : "middle") + "\" font-size=\"10\" fill=\"#444\"" + transformAttr + ">" +
                     svgEscape(name) + "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const ojson& d = pts[static_cast<size_t>(i)];
    double sx = svgSx(i + 1.0, x0, x1);
    double b = jnumOr(d, "b", 0.0);
    double ciLo = jnumOr(d, "ci_lo", b);
    double ciHi = jnumOr(d, "ci_hi", b);
    double syLo = svgSy(ciLo, y0, y1), syHi = svgSy(ciHi, y0, y1), syB = svgSy(b, y0, y1);
    double capLen = 4;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" + fmtNum(sx) +
                     "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syLo) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syHi) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <circle cx=\"" + fmtNum(sx) + "\" cy=\"" + fmtNum(syB) + "\" r=\"3\" fill=\"#1f77b4\"/>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgCoefplot(const ojson& block) {
  struct Row {
    std::string label;
    double b;
    bool hasCI;
    double ciLo, ciHi;
  };
  std::vector<Row> rows;
  ojson models = block.value("models", ojson::array());
  bool multi = models.size() > 1;
  for (const auto& m : models) {
    std::string mname = m.value("name", std::string());
    for (const auto& c : m.value("coefs", ojson::array())) {
      std::string cname = c.value("name", std::string());
      bool hasCI = c.contains("ci_lo") && !c["ci_lo"].is_null() && c.contains("ci_hi") && !c["ci_hi"].is_null();
      rows.push_back({multi ? (mname + ":" + cname) : cname, jnumOr(c, "b", std::nan("")), hasCI,
                      hasCI ? jnumOr(c, "ci_lo", 0.0) : 0.0, hasCI ? jnumOr(c, "ci_hi", 0.0) : 0.0});
    }
  }

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  int n = static_cast<int>(rows.size());

  // X: coefficient value, always widened to include 0 so the zero reference
  // line is in frame (mirrors renderCoefplot's `Math.min(0, xlo)`); falls
  // back to deriving a range from the rows themselves only if the block's
  // own x_min/x_max are missing/non-finite (runCoefplot always sets both,
  // so this fallback is defensive, not the normal path).
  double xlo = jnum(block.value("x_min", ojson(std::nan(""))), std::nan(""));
  double xhi = jnum(block.value("x_max", ojson(std::nan(""))), std::nan(""));
  if (!std::isfinite(xlo) || !std::isfinite(xhi)) {
    bool any = false;
    xlo = xhi = 0.0;
    for (const auto& r : rows) {
      std::vector<double> vs = {r.b};
      if (r.hasCI) { vs.push_back(r.ciLo); vs.push_back(r.ciHi); }
      for (double v : vs) {
        if (!std::isfinite(v)) continue;
        if (!any) { xlo = xhi = v; any = true; } else { xlo = std::min(xlo, v); xhi = std::max(xhi, v); }
      }
    }
  }
  xlo = std::min(xlo, 0.0);
  xhi = std::max(xhi, 0.0);
  auto xr = paddedRangeG(xlo, xhi, 0.05, 0.05);
  auto yr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string("Coefficient estimate")), std::string(),
                  /*suppressXTickLabels=*/false, /*suppressYTickLabels=*/true);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zx = svgSx(0.0, x0, x1);
  if (zx >= kSvgML - 0.5 && zx <= kSvgML + kSvgPlotW + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(zx) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(zx) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  for (int i = 0; i < n; ++i) {
    double sy = svgSy(n - i, y0, y1);
    lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                     "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(rows[static_cast<size_t>(i)].label) +
                     "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const Row& r = rows[static_cast<size_t>(i)];
    double sy = svgSy(n - i, y0, y1);
    if (r.hasCI) {
      double sxLo = svgSx(r.ciLo, x0, x1), sxHi = svgSx(r.ciHi, x0, x1);
      double capLen = 4;
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxLo) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxHi) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    }
    if (std::isfinite(r.b)) {
      double sxB = svgSx(r.b, x0, x1);
      lines.push_back("  <circle cx=\"" + fmtNum(sxB) + "\" cy=\"" + fmtNum(sy) + "\" r=\"3\" fill=\"#1f77b4\"/>");
    }
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgDispatch(const ojson& block);  // fwd decl for combine's recursive call

std::string buildSvgCombine(const ojson& block) {
  ojson grid = block.value("grid", ojson::object());
  int rows = std::max(1, grid.value("rows", 1));
  int cols = std::max(1, grid.value("cols", 1));
  double totalW = cols * kSvgW, totalH = rows * kSvgH;

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(totalW, "%.0f") + " " +
                                     fmtNum(totalH, "%.0f") + "\" xmlns=\"http://www.w3.org/2000/svg\">"};

  // graphregion_color is always present in the block dict but may be JSON null
  // (graph_combine.py: unset graphregion(color(...)) opt leaves it None). A
  // null-but-present key makes nlohmann::json::value()'s default inapplicable
  // (defaults only cover an ABSENT key), so read it null-safely here — matching
  // the oracle's `if bg and bg != "none"` falsy-check (None/empty ⇒ no rect
  // painted, i.e. no default color substitution when unspecified).
  ojson bgJ = block.value("graphregion_color", ojson());
  std::string bg = bgJ.is_string() ? bgJ.get<std::string>() : std::string();
  if (!bg.empty() && bg != "none") {
    lines.push_back("  <rect x=\"0\" y=\"0\" width=\"" + fmtNum(totalW, "%.0f") + "\" height=\"" +
                     fmtNum(totalH, "%.0f") + "\" fill=\"" + bg + "\"/>");
  }

  std::string title = block.value("title", std::string());
  if (!title.empty()) {
    lines.push_back("  <text x=\"" + fmtNum(totalW / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                     "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
  }

  ojson panels = block.value("panels", ojson::array());
  for (size_t idx = 0; idx < panels.size(); ++idx) {
    int row = static_cast<int>(idx) / cols;
    int col = static_cast<int>(idx) % cols;
    if (row >= rows) break;
    double tx = col * kSvgW, ty = row * kSvgH;
    std::string subSvg;
    try {
      subSvg = buildSvgDispatch(panels[idx]);
    } catch (...) {
      subSvg = "<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"
                "<text x=\"10\" y=\"20\" font-size=\"10\" fill=\"red\">render error</text></svg>";
    }
    size_t bodyStart = subSvg.find('>');
    size_t bodyEnd = subSvg.rfind("</svg>");
    std::string inner = (bodyStart != std::string::npos && bodyEnd != std::string::npos && bodyEnd > bodyStart)
                             ? subSvg.substr(bodyStart + 1, bodyEnd - bodyStart - 1)
                             : std::string();
    // A panel widened past one cell (long legend labels) is scaled to fit its cell.
    std::string scaleAttr;
    double subW = kSvgW;
    size_t vb = subSvg.find("viewBox=\"0 0 ");
    if (vb != std::string::npos) subW = std::atof(subSvg.c_str() + vb + 13);
    if (subW > kSvgW + 0.5) scaleAttr = " scale(" + fmtNum(kSvgW / subW, "%.4f") + ")";
    lines.push_back("  <g transform=\"translate(" + fmtNum(tx, "%.0f") + "," + fmtNum(ty, "%.0f") + ")" +
                     scaleAttr + "\">");
    lines.push_back(inner);
    lines.push_back("  </g>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgDispatch(const ojson& block) {
  std::string kind = block.value("kind", std::string("scatter"));
  if (kind == "histogram") return buildSvgHistogram(block);
  if (kind == "line") return buildSvgLine(block);
  // BS-055: xtline/tsline previously had NO branch here at all -- both fell
  // through to buildSvgScatter, which reads a top-level "points" key
  // neither block shape carries (their data lives in "series"). Result:
  // frame/axes/title rendered fine (kind-agnostic), zero data ever drawn.
  if (kind == "xtline" || kind == "tsline") return buildSvgSeriesPlot(block, /*asMarkers=*/false);
  if (kind == "twoway") return buildSvgTwoway(block);
  if (kind == "graph_combine") return buildSvgCombine(block);
  if (kind == "marginsplot") return buildSvgMarginsplot(block);
  if (kind == "coefplot") return buildSvgCoefplot(block);
  return buildSvgScatter(block);
}

}  // namespace

std::string buildGraphSvg(const ojson& block) { return buildSvgDispatch(block); }

ojson runGraphSave(const std::optional<std::string>& name, const std::optional<std::string>& filepath,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  bool replace = optBool(opts, "replace").value_or(false);

  const ojson* blk = nullptr;
  if (name.has_value() && !name->empty()) {
    auto it = session.namedGraphs.find(*name);
    if (it != session.namedGraphs.end()) blk = &it->second;
  }
  if (blk == nullptr && session.lastGraph.has_value()) blk = &*session.lastGraph;
  if (blk == nullptr) {
    return engineErrorBlockG("graph save: no graph in memory. Run a graph command first.");
  }

  std::string regName = name.value_or(std::string());
  if (regName.empty() && filepath.has_value()) {
    // Basename-only derivation — resolveUserPath's cwd-joined result still
    // ends in the same last path component a bare dequote would have kept,
    // so this reuses the canonical resolver instead of a second dequote
    // fork just to strip quotes.
    std::string fp = resolveUserPath(session, *filepath);
    size_t slash = fp.find_last_of('/');
    std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    regName = dot == std::string::npos ? base : base.substr(0, dot);
  }
  ojson toSave = *blk;
  if (!regName.empty()) session.namedGraphs[regName] = toSave;

  if (filepath.has_value()) {
    std::string fp = resolveUserPath(session, *filepath);
    if (fileExistsG(fp) && !replace) {
      return engineErrorBlockG("graph save: file already exists: " + fp +
                                ". Use `, replace` to overwrite.");
    }
    ojson doc;
    doc["_magic"] = "PIETGPH1";
    doc["block"] = toSave;
    std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
    if (!f) return engineErrorBlockG("graph save: could not write file: " + fp);
    std::string text = doc.dump();
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = "(graph saved" + (regName.empty() ? std::string() : (": " + regName)) + ")";
  return out;
}

ojson runGraphCombine(const std::vector<std::string>& names,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  ojson panels = ojson::array();
  for (const auto& raw : names) {
    // BS-007 (D3): each combine token resolves as EITHER a bare in-memory
    // graph NAME (session.namedGraphs -- the pre-existing behavior) OR a
    // quoted on-disk `.gph` spec-JSON path. Real-world need: analysis.do's
    // loop-then-combine idiom `graph save` (above)s each iteration's graph
    // to a DISTINCT tempfile while reusing the SAME in-memory name every
    // iteration (a Stata `tempfile` local re-bound per loop pass), so by
    // the time `graph combine` runs, the in-memory name holds only the
    // LAST iteration's graph -- only the on-disk .gph files carry every
    // iteration's distinct graph (analysis.do:1533's `graph combine
    // `graph_list'` where `graph_list` accumulates quoted tempfile paths,
    // not names -- BS-007 evidence). dispatcher.cpp's varlist tokenizer is
    // NOT quote-aware (eval/tokenizer.cpp's splitWs is a bare `>>` split),
    // so a token can arrive as e.g. `"a.gph"` with the quote characters
    // still embedded -- one-layer dequote first (same single-pair-strip
    // rule as handlers.cpp's dequoteUserPath, replicated here per this
    // TU's established per-file small-helper convention -- see
    // firstNameToken above) before either lookup.
    std::string tok = raw;
    {
      size_t a = tok.find_first_not_of(" \t");
      size_t b = tok.find_last_not_of(" \t");
      tok = (a == std::string::npos) ? std::string() : tok.substr(a, b - a + 1);
      if (tok.size() >= 2 && ((tok.front() == '"' && tok.back() == '"') ||
                              (tok.front() == '\'' && tok.back() == '\''))) {
        tok = tok.substr(1, tok.size() - 2);
      }
    }

    auto it = session.namedGraphs.find(tok);
    if (it != session.namedGraphs.end()) {
      panels.push_back(it->second);
      continue;
    }

    std::string fp = resolveUserPath(session, tok);
    auto loaded = loadGphFile(fp);
    if (!loaded.has_value()) {
      return engineErrorBlockG("graph combine: no graph named '" + tok +
                                "' and no readable .gph file at '" + fp +
                                "' (use `graph save` first).");
    }
    panels.push_back(*loaded);
  }
  if (panels.empty()) {
    return engineErrorBlockG("graph combine: no graph names given.");
  }

  int n = static_cast<int>(panels.size());
  int cols = 0, rows = 0;
  auto colsOpt = optStr(opts, "cols");
  auto rowsOpt = optStr(opts, "rows");
  if (colsOpt.has_value()) {
    try { cols = std::stoi(*colsOpt); } catch (...) { cols = 0; }
  }
  if (rowsOpt.has_value()) {
    try { rows = std::stoi(*rowsOpt); } catch (...) { rows = 0; }
  }
  if (cols <= 0 && rows <= 0) {
    cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  } else if (cols <= 0) {
    cols = static_cast<int>(std::ceil(static_cast<double>(n) / rows));
  } else if (rows <= 0) {
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "graph_combine";
  out["title"] = optTitleStr(opts, "title").value_or(std::string());
  out["subtitle"] = optTitleStr(opts, "subtitle").value_or(std::string());
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["panels"] = panels;
  ojson grid;
  grid["rows"] = rows;
  grid["cols"] = cols;
  out["grid"] = grid;
  out["imargin"] = optStr(opts, "imargin").has_value() ? ojson(*optStr(opts, "imargin")) : ojson(nullptr);
  out["graphregion_color"] = optStr(opts, "graphregion").has_value() ? ojson(*optStr(opts, "graphregion"))
                                                                       : ojson(nullptr);
  out["n_panels"] = n;
  // BS-047 (BETA A4): see runTwowayLayers' identical comment above.
  out["suppressed"] = session.graphicsOff;

  session.lastGraph = out;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value() && !nameOpt->empty()) {
    // BS-057: `name(X, replace)` -- strip the glued `, replace` (or any
    // other suboption) the same way the bare/twoway name() capture does, so
    // the stored key is the bare name "X", not the literal "X, replace"
    // (previously baked verbatim into session.namedGraphs' key).
    std::string nm = firstNameToken(*nameOpt);
    if (!nm.empty()) {
      session.namedGraphs[nm] = out;
      out["name"] = nm;
    } else {
      out["name"] = nullptr;
    }
  } else {
    out["name"] = nullptr;
  }
  return out;
}

ojson runGraphExport(const std::string& filepathArg,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  std::string filepath = resolveUserPath(session, filepathArg);

  // W:P8 (BUG-169): `name()` was parsed and then never read, so
  // `graph export FILE, name(X)` ALWAYS wrote session.lastGraph — the current
  // graph, not X. Exporting a name that does not exist returned rc 0 and a
  // file byte-identical to exporting a name that does, which is the cardinal
  // silent-wrong-answer shape: the P8 target's own
  // `graph export "_runs/p8_graph.svg", name(p8all)` would have shipped the
  // last histogram in place of the three-panel combine with every pin green.
  // Stata answers an unknown name with "could not find Graph window", r(693).
  const ojson* blk = nullptr;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value()) {
    std::string want = *nameOpt;
    size_t a = want.find_first_not_of(" \t");
    size_t b = want.find_last_not_of(" \t");
    want = (a == std::string::npos) ? std::string() : want.substr(a, b - a + 1);
    if (!want.empty()) {
      auto it = session.namedGraphs.find(want);
      if (it == session.namedGraphs.end()) {
        return engineErrorBlockG("graph export: could not find Graph window " + want);
      }
      blk = &it->second;
    }
  }
  if (blk == nullptr && !session.lastGraph.has_value()) {
    return engineErrorBlockG(
        "graph export: no graph in memory. Run a graph command first (e.g. `scatter price mpg`).");
  }
  const ojson& exportBlock = blk != nullptr ? *blk : *session.lastGraph;

  // BUG-103: `dot` used to be `filepath.find_last_of('.')` over the WHOLE
  // path, not just the basename. Under a TMPDIR/workdir containing a dot in
  // some directory component (e.g. a scratch dir literally named
  // `.tmp-scratch`), that scan finds the directory's dot instead of a real
  // filename extension whenever the filename itself is extensionless --
  // `/…/.tmp-scratch/noext_plot` has its ONLY '.' inside `.tmp-scratch`.
  // Two consequences, both silent: (1) extFromName above would read back a
  // directory-path fragment instead of "" ; (2) far worse, the as(png)/pdf
  // fallback path below (`filepath.substr(0, dot) + ".svg"`) TRUNCATES THE
  // PATH at that same wrong dot, producing a garbage sibling path outside
  // the intended directory -- the write silently lands nowhere the caller
  // (or the BS-012 selftest leg) ever looks, i.e. "writes nothing" under a
  // dotted TMPDIR while working fine under `/tmp` (which has no dot in a
  // typical path). Fix: only recognise a '.' that appears AFTER the last
  // path separator (i.e. within the basename) as a real extension.
#ifdef _WIN32
  size_t baseStart = filepath.find_last_of("/\\");
#else
  size_t baseStart = filepath.find_last_of('/');
#endif
  baseStart = (baseStart == std::string::npos) ? 0 : baseStart + 1;
  size_t dot = filepath.find_last_of('.');
  if (dot != std::string::npos && dot < baseStart) dot = std::string::npos;
  std::string extFromName = dot == std::string::npos ? std::string() : filepath.substr(dot + 1);
  std::transform(extFromName.begin(), extFromName.end(), extFromName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // BS-012 (D8): `as(fmt)` names the export format explicitly and wins over
  // the filename's own extension when given (Stata's `graph export FILE,
  // as(fmt)` semantics) -- previously parsed into `opts` by the shared
  // option grammar and then silently never read again.
  std::string fmt;
  auto asOpt = optStr(opts, "as");
  if (asOpt.has_value()) {
    fmt = *asOpt;
    size_t a = fmt.find_first_not_of(" \t");
    size_t b = fmt.find_last_not_of(" \t");
    fmt = (a == std::string::npos) ? std::string() : fmt.substr(a, b - a + 1);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
  } else {
    fmt = extFromName;
  }

  if (fmt != "png" && fmt != "pdf" && fmt != "svg") {
    return eval::engineError(
        "graph export: unsupported format '" + fmt + "'. Use a .png/.pdf/.svg filename, or `, as(png|pdf|svg)`.",
        "syntax_error");
  }

  bool replace = optBool(opts, "replace").value_or(false);
  if (fileExistsG(filepath) && !replace) {
    return engineErrorBlockG("graph export: file already exists: " + filepath +
                              "\nUse `graph export FILE, replace` to overwrite.");
  }

#ifdef _WIN32
  // Absolute Windows paths use '\\'; find_last_of('/') would miss it and treat
  // the whole path as the parent, so `graph export C:\dir\plot.svg` wrongly
  // reports "directory does not exist". Match either separator. POSIX unchanged.
  auto psep = filepath.find_last_of("/\\");
  std::string parent = (psep == std::string::npos) ? std::string() : filepath.substr(0, psep);
#else
  std::string parent = filepath.substr(0, filepath.find_last_of('/'));
#endif
  if (!parent.empty() && !cecon::native_exists(parent)) {
    return engineErrorBlockG("graph export: directory does not exist: " + parent);
  }

  if (fmt == "svg") {
    std::string svg = buildGraphSvg(exportBlock);
    std::ofstream f(cecon::to_native_path(filepath), std::ios::binary);
    if (!f) return engineErrorBlockG("graph export: could not write file: " + filepath);
    f.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    f.close();
    long sizeBytes = static_cast<long>(svg.size());
    ojson out;
    out["type"] = "display";
    out["text"] = "(graph saved: " + filepath + ", " + std::to_string(sizeBytes) + " bytes)";
    return out;
  }

  // BS-012 (D8): fmt is png/pdf. Real rasterization is the packaged app's
  // OWN webview doing an offscreen render -- meaningless for a headless
  // `--do` run, which never has a webview to ask, and out of scope for
  // this stint regardless (deferred post-beta). PRE-AUTHORIZED FALLBACK:
  // write an HONEST .svg file -- NEVER the SVG byte-stream saved under the
  // requested .png/.pdf name, which would be a silent format lie -- to the
  // `.png`/`.pdf` path's basename with `.svg` substituted (or `.svg`
  // appended, if the given path had no extension to substitute), plus an
  // explicit one-line "rasterizer pending" warning naming the substitution
  // (Doctrine ① requires an explicit signal, never a silently different
  // answer). This is the same fallback shape BUG-022 originally
  // established (SVG-with-warning over a hard stop, so a real research
  // .do hardcoding `.png` — analysis.do does, uncaptured — still reaches
  // its terminal `exit`); the wording below refreshes the framing from
  // "matplotlib not bundled" to D8's "rasterizer pending" now that the
  // longer-term plan is a native webview rasterizer, not matplotlib.
  std::string svgPath = (dot == std::string::npos) ? (filepath + ".svg") : (filepath.substr(0, dot) + ".svg");
  bool svgReplace = replace || !fileExistsG(svgPath);
  if (fileExistsG(svgPath) && !svgReplace) {
    return engineErrorBlockG("graph export: file already exists: " + svgPath +
                              "\nUse `graph export FILE, replace` to overwrite.");
  }
  std::string svg = buildGraphSvg(exportBlock);
  std::ofstream f(cecon::to_native_path(svgPath), std::ios::binary);
  if (!f) return engineErrorBlockG("graph export: could not write file: " + svgPath);
  f.write(svg.data(), static_cast<std::streamsize>(svg.size()));
  f.close();
  long sizeBytes = static_cast<long>(svg.size());
  ojson out;
  out["type"] = "display";
  out["text"] = "(note: ." + fmt + " rasterizer pending -- wrote " + svgPath +
                " instead)\n(graph saved: " + svgPath + ", " + std::to_string(sizeBytes) + " bytes)";
  return out;
}

// ===========================================================================
// L2-FIX-B (BUG-066 / COUNCIL-20260710 C3-F5): LIVE-VIEW point decimation.
//
// The graph pipeline shipped and rendered one JSON+SVG node per data point,
// unbounded -- a 108K/1M-point scatter is 108K/1M DOM nodes plus a
// multi-megabyte block copied across the native->frontend bridge, which the
// webview cannot absorb (BUG-066). C3-F5's ruling: density-preserving
// decimation for the LIVE VIEW ONLY with an honest "(showing N of M points)"
// caption; `graph export` keeps ALL points (the golden-pinned SVG builder =
// the REPIN seam) and so does everything else that reads session.lastGraph /
// namedGraphs (graph export/save/combine).
//
// The caller (dispatcher's graph blocks.push_back seams) invokes this AFTER
// the FULL block has already been deep-copied into session.lastGraph (and
// namedGraphs), so thinning the console-bound copy here shrinks ONLY the live
// view -- the export path is untouched, byte-for-byte, no re-pin.
//
// Method: an order-preserving proportional stride (keep every ceil(n/target)-th
// point, always anchoring the last one). Proportional subsampling preserves the
// cloud's relative DENSITY by construction (a region with k times more points
// keeps k times more samples), and order-preservation keeps connected-line /
// tsline shapes intact -- so one primitive serves scatter and line kinds alike.
// The axis ranges (x_min..y_max) are computed by the handler from the FULL data
// and left untouched, so the frame still spans the true extent even when a
// corner point is sampled out.
namespace {
// Thin a [[x,y],...] array to <= `target` entries in place (order-preserving,
// last point always retained). Returns the resulting (shown) count.
size_t strideThinPoints(ojson& arr, size_t target) {
  if (!arr.is_array()) return 0;
  const size_t n = arr.size();
  if (target == 0 || n <= target) return n;
  const size_t step = (n + target - 1) / target;  // ceil(n/target), always >= 2
  ojson kept = ojson::array();
  for (size_t i = 0; i < n; i += step) kept.push_back(arr[i]);
  if ((n - 1) % step != 0) kept.push_back(arr[n - 1]);  // anchor the last point
  arr = std::move(kept);
  return arr.size();
}
}  // namespace

void decimateGraphBlockForView(ojson& block) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;

  static constexpr size_t kMaxViewPoints = 5000;  // DOM/bridge budget per graph
  size_t total = 0, shown = 0;
  auto thinCounted = [&](ojson& pts) {
    if (!pts.is_array()) return;
    total += pts.size();
    shown += strideThinPoints(pts, kMaxViewPoints);
  };

  if (block.contains("layers") && block["layers"].is_array()) {
    // twoway: real data lives in each rendered layer's own points array.
    for (auto& lyr : block["layers"])
      if (lyr.is_object() && lyr.contains("points")) thinCounted(lyr["points"]);
  } else if (block.contains("series") && block["series"].is_array()) {
    // multi-y scatter / xtline / tsline: data lives in series[].points.
    for (auto& s : block["series"])
      if (s.is_object() && s.contains("points")) thinCounted(s["points"]);
    // The flat top-level "points" is native's backward-compat UNION of the
    // series; thin it too so the bridge copy shrinks, but do NOT count it --
    // that would double-count the series points.
    if (block.contains("points") && block["points"].is_array())
      strideThinPoints(block["points"], kMaxViewPoints);
  } else if (block.contains("points") && block["points"].is_array()) {
    // single scatter / line / connected.
    thinCounted(block["points"]);
  }

  if (shown < total) {
    block["n_shown_points"] = shown;
    block["n_total_points"] = total;
  }
}

}  // namespace piet::handlers
