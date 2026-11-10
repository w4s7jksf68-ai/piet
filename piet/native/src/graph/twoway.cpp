// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "piet/handlers/graph.h"
#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "piet/twoway_parse.h"
#include "io/win_compat.h"
#include "graph_support.h"
#include "distribution.h"

namespace piet::eval {
double stataInvttail(double df, double p);
}

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


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



// nextPathG: local copy of the "generate a fresh scratch .hbin path" helper
// every M2+ handler file carries its own copy of (see engine/engine_call_internal.h/
// datamgmt/egen_internal.h's identically-named nextPath — kept file-local
// per this codebase's established per-TU convention rather than factored
// into a shared header).
static std::string nextPathG(const std::string& workdir, const std::string& prefix) {
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


static MaybeOptFromOpts getOptG(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return {true, v.boolValue, ""};
      return {true, false, v.strValue};
    }
  }
  return {false, false, ""};
}


// _finite_mask + boolean-index (+ a stable argsort(x) when `sorted`): returns
// (xs, ys), missing rows dropped. A line or connected plot joins its points in
// data order unless `sort` is given ([G-3] connect_options; BUG-404).
static std::vector<std::pair<double, double>> maskSortXY(const std::vector<double>& y,
                                                   const std::vector<double>& x, bool sorted) {
  std::vector<std::pair<double, double>> pts;  // (x, y)
  for (size_t i = 0; i < y.size(); ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
    pts.emplace_back(x[i], y[i]);
  }
  if (sorted)
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return pts;
}


// _layer_rarea: yhi ylo xvar -> {kind:"rarea", band:[[x,ylo,yhi],...], style, n}.
static ojson layerRarea(Datastore& ds, const std::vector<std::string>& varlist,
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
static ojson layerLineOrConnected(Datastore& ds, const std::vector<std::string>& varlist,
                           const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                           int layerIdx, const std::string& plottype) {
  if (varlist.size() < 2) {
    return ojson{{"_error", plottype + " requires two variables: yvar xvar"}};
  }
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto pts = maskSortXY(numericColumn(ds, yvar), numericColumn(ds, xvar), hasOpt(opts, "sort"));
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
static ojson layerScatter(Datastore& ds, const std::vector<std::string>& varlist,
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


// ---------------------------------------------------------------------------
// serset-twoway FIT: what the lfit and qfit plots and their ci forms compute
// ([G-2] graph twoway lfit, qfit, lfitci, qfitci). `regress y x [x2] [w] if touse`,
// then the x grid `gen x = min + (_n-1)*(max-min)/(points-1)` (c(type), so a
// FLOAT under the default), `predict yhat` (c(type)) at the grid; lfitci/qfitci
// add `predict se, stdp|stdf|stdr` and `lcl/ucl = yhat -/+ invttail(e(df_r),
// (100-level)/200)*se` (each a c(type) store, so yhat and se are rounded BEFORE
// the band arithmetic). A regressor collinear with the constant (or x^2 with
// x) is omitted as regress omits it: constant x fits the mean (w2al
// tf_lfit_const, log:1768; tf_lfitci_const). Weights: aw normalised to sum N,
// fw replicate (N = sum w), pw the aw point estimates with the robust V.
struct FitRequest {
  int degree = 1;  // 1 lfit/lfitci, 2 qfit/qfitci
  bool ci = false;
  int64_t points = 3;
  std::optional<double> rangeMin, rangeMax;
  double level = 95.0;
  std::string stdKind = "stdp";
  std::string wtype;  // "" | aweight | fweight | pweight
  bool genFloat = true;
};

struct FitResult {
  bool ok = false;
  bool noObs = false;
  std::string message;
  std::vector<double> grid, yhat, lcl, ucl;  // stored values (c(type))
  std::vector<double> yhatD;                 // unrounded predictions at the grid
  double b0 = 0.0, b1 = 0.0, b2 = 0.0;
  bool keptX = false, keptX2 = false;
};


static FitResult computeFit(const std::vector<double>& y, const std::vector<double>& x,
                     const std::vector<double>* w, const FitRequest& req) {
  FitResult r;
  const bool hasW = w != nullptr && !req.wtype.empty();
  std::vector<double> ys, xs, ws;
  for (size_t i = 0; i < y.size() && i < x.size(); ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
    double wi = 1.0;
    if (hasW) {
      wi = i < w->size() ? (*w)[i] : kNaN;
      if (!std::isfinite(wi) || wi <= 0.0) continue;
    }
    ys.push_back(y[i]);
    xs.push_back(x[i]);
    ws.push_back(wi);
  }
  const size_t n = ys.size();
  if (n == 0) {
    r.noObs = true;
    r.message = "no observations";
    return r;
  }
  double sumW = 0.0;
  for (double v : ws) sumW += v;
  double nEff = static_cast<double>(n);  // e(N)
  if (hasW && req.wtype == "fweight") {
    nEff = sumW;
  } else if (hasW) {
    // aweight/pweight: normalised to sum to N
    for (double& v : ws) v = v * static_cast<double>(n) / sumW;
  }
  // Candidate columns in Stata's order of precedence: _cons, x, x2.
  const int kAll = req.degree == 2 ? 3 : 2;
  auto colVal = [&](int j, double xv) -> double {
    if (j == 0) return 1.0;
    if (j == 1) return xv;
    return xv * xv;
  };
  // Weighted modified Gram-Schmidt in that order: a column whose residual is
  // (numerically) zero is collinear with the kept ones and omitted.
  std::vector<int> kept;
  std::vector<std::vector<double>> qcols;  // orthonormal (in the w inner product)
  for (int j = 0; j < kAll; ++j) {
    std::vector<double> v(n);
    double ss0 = 0.0;
    for (size_t i = 0; i < n; ++i) {
      v[i] = colVal(j, xs[i]);
      ss0 += ws[i] * v[i] * v[i];
    }
    for (const auto& q : qcols) {
      double dot = 0.0;
      for (size_t i = 0; i < n; ++i) dot += ws[i] * q[i] * v[i];
      for (size_t i = 0; i < n; ++i) v[i] -= dot * q[i];
    }
    double ss = 0.0;
    for (size_t i = 0; i < n; ++i) ss += ws[i] * v[i] * v[i];
    if (!(ss > 1e-13 * ss0) || ss0 == 0.0) continue;
    const double nrm = std::sqrt(ss);
    for (double& e : v) e /= nrm;
    qcols.push_back(std::move(v));
    kept.push_back(j);
  }
  const size_t k = kept.size();
  // Normal equations over the kept columns, solved by Cholesky (k <= 3) after
  // the rank decision above; refined once against the residual.
  std::vector<double> A(k * k, 0.0), bvec(k, 0.0);
  for (size_t i = 0; i < n; ++i) {
    for (size_t a = 0; a < k; ++a) {
      const double va = colVal(kept[a], xs[i]);
      bvec[a] += ws[i] * va * ys[i];
      for (size_t c = 0; c < k; ++c) A[a * k + c] += ws[i] * va * colVal(kept[c], xs[i]);
    }
  }
  // Inverse of A by Gauss-Jordan with partial pivoting.
  std::vector<double> inv(k * k, 0.0), M = A;
  for (size_t a = 0; a < k; ++a) inv[a * k + a] = 1.0;
  for (size_t c = 0; c < k; ++c) {
    size_t piv = c;
    for (size_t rr = c + 1; rr < k; ++rr)
      if (std::fabs(M[rr * k + c]) > std::fabs(M[piv * k + c])) piv = rr;
    if (M[piv * k + c] == 0.0) {
      r.message = "singular fit";
      return r;
    }
    if (piv != c) {
      for (size_t cc = 0; cc < k; ++cc) {
        std::swap(M[c * k + cc], M[piv * k + cc]);
        std::swap(inv[c * k + cc], inv[piv * k + cc]);
      }
    }
    const double d = M[c * k + c];
    for (size_t cc = 0; cc < k; ++cc) {
      M[c * k + cc] /= d;
      inv[c * k + cc] /= d;
    }
    for (size_t rr = 0; rr < k; ++rr) {
      if (rr == c) continue;
      const double f = M[rr * k + c];
      if (f == 0.0) continue;
      for (size_t cc = 0; cc < k; ++cc) {
        M[rr * k + cc] -= f * M[c * k + cc];
        inv[rr * k + cc] -= f * inv[c * k + cc];
      }
    }
  }
  std::vector<double> beta(k, 0.0);
  for (size_t a = 0; a < k; ++a)
    for (size_t c = 0; c < k; ++c) beta[a] += inv[a * k + c] * bvec[c];
  // One step of iterative refinement: beta += inv * X'W(y - X beta).
  {
    std::vector<double> g(k, 0.0);
    for (size_t i = 0; i < n; ++i) {
      double fit = 0.0;
      for (size_t a = 0; a < k; ++a) fit += beta[a] * colVal(kept[a], xs[i]);
      const double e = ys[i] - fit;
      for (size_t a = 0; a < k; ++a) g[a] += ws[i] * colVal(kept[a], xs[i]) * e;
    }
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c) beta[a] += inv[a * k + c] * g[c];
  }
  for (size_t a = 0; a < k; ++a) {
    if (kept[a] == 0) r.b0 = beta[a];
    if (kept[a] == 1) {
      r.b1 = beta[a];
      r.keptX = true;
    }
    if (kept[a] == 2) {
      r.b2 = beta[a];
      r.keptX2 = true;
    }
  }
  // Residual variance and V.
  double rss = 0.0, minX = xs[0], maxX = xs[0];
  std::vector<double> resid(n);
  for (size_t i = 0; i < n; ++i) {
    double fit = 0.0;
    for (size_t a = 0; a < k; ++a) fit += beta[a] * colVal(kept[a], xs[i]);
    resid[i] = ys[i] - fit;
    rss += ws[i] * resid[i] * resid[i];
    minX = std::min(minX, xs[i]);
    maxX = std::max(maxX, xs[i]);
  }
  const double dfr = nEff - static_cast<double>(k);
  const double s2 = dfr > 0 ? rss / dfr : kNaN;
  std::vector<double> V(k * k, 0.0);
  if (hasW && req.wtype == "pweight") {
    // robust: inv * (sum (w e)^2 x x') * inv * N/(N-k)
    std::vector<double> meat(k * k, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double we = ws[i] * resid[i];
      for (size_t a = 0; a < k; ++a)
        for (size_t c = 0; c < k; ++c)
          meat[a * k + c] += we * we * colVal(kept[a], xs[i]) * colVal(kept[c], xs[i]);
    }
    const double adj = dfr > 0 ? static_cast<double>(n) / dfr : kNaN;
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c) {
        double v = 0.0;
        for (size_t p = 0; p < k; ++p)
          for (size_t q = 0; q < k; ++q) v += inv[a * k + p] * meat[p * k + q] * inv[q * k + c];
        V[a * k + c] = v * adj;
      }
  } else {
    for (size_t a = 0; a < k * k; ++a) V[a] = inv[a] * s2;
  }
  // The grid.
  double lo = req.rangeMin.has_value() && std::isfinite(*req.rangeMin) ? *req.rangeMin
                                                                         : viaMacroText(minX);
  double hi = req.rangeMax.has_value() && std::isfinite(*req.rangeMax) ? *req.rangeMax
                                                                         : viaMacroText(maxX);
  const int64_t pts = std::max<int64_t>(req.points, 1);
  for (int64_t i = 0; i < pts; ++i) {
    const double gx = lo + (static_cast<double>(i) * (hi - lo)) / static_cast<double>(pts - 1);
    r.grid.push_back(storeAs(pts == 1 ? kNaN : gx, req.genFloat));
  }
  const double tval = req.ci ? eval::stataInvttail(dfr, (100.0 - req.level) / 200.0) : kNaN;
  for (double g : r.grid) {
    double xb = 0.0;
    std::vector<double> x0(k);
    for (size_t a = 0; a < k; ++a) {
      x0[a] = colVal(kept[a], g);
    }
    // predict's xb: the slope terms, then the constant.
    for (size_t a = 0; a < k; ++a)
      if (kept[a] != 0) xb += beta[a] * x0[a];
    for (size_t a = 0; a < k; ++a)
      if (kept[a] == 0) xb += beta[a];
    r.yhatD.push_back(xb);
    const double yf = storeAs(xb, req.genFloat);
    r.yhat.push_back(yf);
    if (req.ci) {
      double q = 0.0;
      for (size_t a = 0; a < k; ++a)
        for (size_t c = 0; c < k; ++c) q += x0[a] * V[a * k + c] * x0[c];
      double se2 = q;
      if (req.stdKind == "stdf") se2 = q + s2;
      if (req.stdKind == "stdr") se2 = s2 - q;
      const double se = storeAs(se2 >= 0 ? std::sqrt(se2) : kNaN, req.genFloat);
      r.lcl.push_back(storeAs(yf - tval * se, req.genFloat));
      r.ucl.push_back(storeAs(yf + tval * se, req.genFloat));
    }
  }
  r.ok = true;
  return r;
}


// The fit options a layer carries: n(), range(), level(), stdp/stdf/stdr.
// Refusals worded as Stata words them for twoway lfit and lfitci.
struct FitOptsParsed {
  FitRequest req;
  std::optional<ojson> err;
};

static FitOptsParsed parseFitOptions(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& pt, bool genFloat) {
  FitOptsParsed out;
  FitRequest& q = out.req;
  q.degree = (pt == "qfit" || pt == "qfitci") ? 2 : 1;
  q.ci = (pt == "lfitci" || pt == "qfitci");
  q.genFloat = genFloat;
  q.points = q.ci ? 100 : 3;
  if (auto nv = optLast(opts, "n")) {
    const auto d = parseNumberG(*nv);
    if (!d.has_value() || *d != std::floor(*d)) {
      out.err = stataRefusal("option n() incorrectly specified", 198);
      return out;
    }
    q.points = static_cast<int64_t>(*d);
  }
  // qfit: points 3 (the lfit default) becomes 100.
  if (q.degree == 2 && q.points == 3) q.points = 100;
  if (auto rv = optLast(opts, "range")) {
    const auto toks = splitWsG(*rv);
    if (toks.size() != 2) {
      out.err = stataRefusal("option range() incorrectly specified", 198);
      return out;
    }
    auto num = [](const std::string& t) -> std::optional<double> {
      if (t == ".") return kNaN;
      return parseNumberG(t);
    };
    const auto a = num(toks[0]), b = num(toks[1]);
    if (!a.has_value() || !b.has_value()) {
      out.err = stataRefusal("option range() incorrectly specified", 198);
      return out;
    }
    q.rangeMin = *a;
    q.rangeMax = *b;
  }
  if (q.ci) {
    if (auto lv = optLast(opts, "level")) {
      const auto d = parseNumberG(*lv);
      if (!d.has_value() || *d < 10.0 || *d > 99.99) {
        out.err = stataRefusal("level() must be between 10 and 99.99 inclusive", 198);
        return out;
      }
      q.level = *d;
    }
    std::vector<std::string> std;
    for (const char* k : {"stdp", "stdf", "stdr"})
      if (hasOpt(opts, k)) std.push_back(k);
    if (std.size() > 1) {
      out.err = stataRefusal("may not combine " + std[0] + " " + std[1], 198);
      return out;
    }
    if (!std.empty()) q.stdKind = std[0];
  }
  return out;
}


// The fit's serset: `__tmp <xname>` (lfit/qfit) or `__tmp __tmp __tmp <xname>`
// (lfitci/qfitci), every column c(type), sorted by x, always its own serset
// (w2al tf_lfit_n log:1742-1749, tf_lfitci_lvl log:2068-2098).
static serset::Serset fitSerset(const FitResult& f, const std::string& xname, bool ci, bool genFloat) {
  const std::string t = genFloat ? "float" : "double";
  std::vector<size_t> order(f.grid.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&f](size_t a, size_t b) {
    const double x = f.grid[a], y = f.grid[b];
    if (std::isnan(x) || std::isnan(y)) return !std::isnan(x) && std::isnan(y);
    return x < y;
  });
  auto pick = [&order](const std::vector<double>& v) {
    std::vector<double> o;
    for (size_t i : order) o.push_back(i < v.size() ? v[i] : kNaN);
    return o;
  };
  serset::Serset s;
  s.n = static_cast<int64_t>(f.grid.size());
  int ti = 0;
  s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.yhat)));
  if (ci) {
    s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.lcl)));
    s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.ucl)));
  }
  s.cols.push_back(serset::numericColumn(xname, t, pick(f.grid)));
  return s;
}


// BUG-099 + serset-twoway FIT: `lfit|qfit|lfitci|qfitci yvar xvar` -- the fit
// kernel above feeds both the drawn layer and the serset. lfit draws its
// straight line by the grid's two end points (BUG-099's 2-point shape); qfit
// draws every grid point (n() / range() now applied: they were accepted and
// ignored, so the drawn line answered a different question); lfitci/qfitci add
// the confidence band [x, lcl, ucl]. `coef` keeps BUG-099's {cons, slope} /
// {cons, b1, b2}. A refusal comes back under "_refusal" (an error block).
static ojson layerFit(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx, const std::string& pt,
               bool genFloat, const std::vector<double>* w, const std::string& wtype,
               std::optional<serset::Serset>* sersetOut) {
  const auto& varlist = spec.varlist;
  if (varlist.size() < 2) return ojson{{"_error", pt + " requires two variables: yvar xvar"}};
  if (varlist.size() > 2) {
    return ojson{{"_refusal", stataRefusal("too many variables specified", 103)}};
  }
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  FitOptsParsed fo = parseFitOptions(spec.opts, pt, genFloat);
  if (fo.err) return ojson{{"_refusal", *fo.err}};
  fo.req.wtype = wtype;
  const FitResult f = computeFit(numericColumn(ds, yvar), numericColumn(ds, xvar), w, fo.req);
  if (!f.ok) return ojson{{"_error", pt + ": " + f.message}};
  const bool ci = fo.req.ci;
  ojson pointsJ = ojson::array();
  int64_t nUsed = 0;
  {
    // the number of observations the fit used (BUG-099's `n`)
    auto yv = numericColumn(ds, yvar), xv = numericColumn(ds, xvar);
    for (size_t i = 0; i < yv.size(); ++i)
      if (std::isfinite(yv[i]) && std::isfinite(xv[i])) ++nUsed;
  }
  if (pt == "lfit" && !f.grid.empty()) {
    pointsJ.push_back(ojson::array({f.grid.front(), f.yhatD.front()}));
    pointsJ.push_back(ojson::array({f.grid.back(), f.yhatD.back()}));
  } else {
    for (size_t i = 0; i < f.grid.size(); ++i) {
      if (std::isnan(f.grid[i]) || std::isnan(f.yhatD[i])) continue;
      pointsJ.push_back(ojson::array({f.grid[i], f.yhatD[i]}));
    }
  }
  ojson out;
  out["kind"] = pt;
  out["points"] = pointsJ;
  if (ci) {
    ojson band = ojson::array();
    for (size_t i = 0; i < f.grid.size(); ++i) {
      if (std::isnan(f.grid[i]) || std::isnan(f.lcl[i]) || std::isnan(f.ucl[i])) continue;
      band.push_back(ojson::array({f.grid[i], f.lcl[i], f.ucl[i]}));
    }
    out["band"] = band;
    out["level"] = fo.req.level;
  }
  if (fo.req.degree == 1) {
    out["coef"] = ojson{{"cons", f.b0}, {"slope", f.b1}};
  } else {
    out["coef"] = ojson{{"cons", f.b0}, {"b1", f.b1}, {"b2", f.b2}};
  }
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, pt);
  out["n"] = nUsed;
  if (sersetOut != nullptr) *sersetOut = fitSerset(f, xvar, ci, genFloat);
  return out;
}


// serset-twoway RNG: the range plots (rcap, rcapsym, rspike, rline,
// rconnected, rscatter: `y1 y2 x`) and the base plots (area, spike, dropline,
// bar: `y x`, drawn to base(), default 0). The variables are taken in the order
// written; the serset comes from the generic data builder (layerDataPlotSpec),
// so nothing plot-specific reaches it (w2al tf_rcap_079y `y xaxis lo hi`
// log:2279, tf_area_nosort log:2621). base() must be a number: Stata's three
// lines and r(4026) (tf_r_area_base, log:5347-5353).
static ojson layerRangeOrBase(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx,
                       const std::string& pt) {
  static const std::set<std::string> kRange = {"rcap", "rcapsym", "rspike", "rline", "rconnected",
                                               "rscatter"};
  const bool isRange = kRange.count(pt) != 0;
  const size_t need = isRange ? 3 : 2;
  const auto& vl = spec.varlist;
  if (vl.size() < need) {
    std::string listed;
    for (const auto& v : vl) listed += (listed.empty() ? "" : " ") + v;
    return ojson{{"_refusal", stataRefusal(pt + " requires " + std::to_string(need) + " variables: " + listed, 102)}};
  }
  if (isRange && vl.size() > 3) {
    return ojson{{"_refusal", stataRefusal("too many variables specified", 103)}};
  }
  double base = 0.0;
  if (!isRange) {
    if (auto b = optLast(spec.opts, "base")) {
      const std::string bt = trimG(*b);
      const auto d = parseNumberG(bt);
      if (!d.has_value()) {
        return ojson{{"_refusal", stataRefusal("option base() incorrectly specified, " + bt +
                                                   " is not a number\n" + bt + " not found\nrvalue invalid",
                                               4026)}};
      }
      base = *d;
    }
  }
  ojson out;
  out["kind"] = pt;
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, isRange ? "rarea" : pt);
  if (isRange) {
    const auto a = numericColumn(ds, vl[0]), b = numericColumn(ds, vl[1]), x = numericColumn(ds, vl[2]);
    ojson segs = ojson::array();
    for (size_t i = 0; i < x.size(); ++i) {
      if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || !std::isfinite(x[i])) continue;
      segs.push_back(ojson::array({x[i], a[i], b[i]}));
    }
    out["n"] = static_cast<int64_t>(segs.size());
    out["segments"] = segs;
  } else {
    // one y (area/spike/dropline/bar `y x`); a further y would be another series
    const auto y = numericColumn(ds, vl[0]), x = numericColumn(ds, vl[vl.size() - 1]);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < x.size(); ++i) {
      if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
      pts.emplace_back(x[i], y[i]);
    }
    if (hasOpt(spec.opts, "sort")) {
      std::stable_sort(pts.begin(), pts.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
    }
    ojson pj = ojson::array();
    for (const auto& [px, py] : pts) pj.push_back(ojson::array({px, py}));
    out["points"] = pj;
    out["base"] = base;
    out["n"] = static_cast<int64_t>(pts.size());
  }
  return out;
}


// serset-twoway FN: `twoway function [[y] =] f(x)` ([G-2] graph twoway function):
// n() points (default 300) over range() (default 0
// 1; two numbers, sorted, or a numeric variable whose min and max over the plot's
// if-sample are used), x_1 = min and x_i = min + delta*(i-1) in double, y =
// f(x) in double with the expression evaluated by the row evaluator over a frame
// holding only the grid column (so a dataset variable named x cannot leak in);
// missing where f is undefined. Serset `__tmp __tmp` = (y, x) double double,
// sorted by x, its own serset (w2al tf_fn_miss log:2919-2928, tf_fn_rangevar
// :2948-2952). No expression: `expression required`, r(100) (tf_r_fn_empty); an
// unknown function: `error in expression: <exp>`, r(133) (tf_r_fn_badfn).
static ojson layerFunction(SessionState& session, Datastore& ds, const twoway::LayerSpec& spec, int layerIdx,
                    std::optional<serset::Serset>* sersetOut) {
  std::string expr = trimG(spec.rawArgs);
  if (expr.empty()) return ojson{{"_refusal", stataRefusal("expression required", 100)}};
  // [[y] =] expression
  {
    static const std::regex reLhs(R"(^([A-Za-z_][A-Za-z0-9_]*)?\s*=\s*(.*)$)");
    std::smatch m;
    // `y = f`, `= f`; not `==` and not a name followed by an operator
    if (std::regex_match(expr, m, reLhs) && !(m.str(2).rfind("=", 0) == 0)) {
      expr = trimG(m.str(2));
    }
  }
  if (expr.empty()) return ojson{{"_refusal", stataRefusal("nothing found where expression expected", 111)}};
  const auto& opts = spec.opts;
  int64_t n = 300;
  if (auto nv = optLast(opts, "n")) {
    const auto d = parseNumberG(*nv);
    if (!d.has_value() || *d != std::floor(*d)) {
      return ojson{{"_refusal", stataRefusal("option n() incorrectly specified", 198)}};
    }
    n = static_cast<int64_t>(*d);
  }
  if (n < 1) return ojson{{"_refusal", stataRefusal("option n() requires a positive integer", 198)}};
  double lo = 0.0, hi = 1.0;
  if (auto rv = optLast(opts, "range")) {
    const auto toks = splitWsG(*rv);
    if (toks.size() == 1 && ds.has(toks[0])) {
      const auto col = numericColumn(ds, toks[0]);
      double mn = kNaN, mx = kNaN;
      for (double v : col) {
        if (std::isnan(v)) continue;
        if (std::isnan(mn) || v < mn) mn = v;
        if (std::isnan(mx) || v > mx) mx = v;
      }
      if (std::isnan(mn)) return ojson{{"_refusal", stataRefusal("no observations", 2000)}};
      lo = mn;
      hi = mx;
    } else {
      const auto a = toks.size() == 2 ? parseNumberG(toks[0]) : std::nullopt;
      const auto b = toks.size() == 2 ? parseNumberG(toks[1]) : std::nullopt;
      if (!a.has_value() || !b.has_value()) {
        return ojson{{"_refusal", stataRefusal("option range() requires two numbers", 198)}};
      }
      lo = std::min(*a, *b);
      hi = std::max(*a, *b);
    }
  }
  const double delta = n == 1 ? kNaN : (hi - lo) / static_cast<double>(n - 1);
  std::vector<double> grid(static_cast<size_t>(n));
  grid[0] = lo;
  for (int64_t i = 1; i < n; ++i) grid[static_cast<size_t>(i)] = lo + delta * static_cast<double>(i);
  // dropline(numlist): every value inside range()
  if (auto dl = optLast(opts, "dropline")) {
    for (const auto& t : splitWsG(*dl)) {
      const auto d = parseNumberG(t);
      if (!d.has_value() || *d < lo || *d > hi) {
        return ojson{{"_refusal", stataRefusal("some droplines not within range() option", 198)}};
      }
    }
  }
  std::string xname = "x";
  if (auto sc = optLast(opts, "subchar")) {
    if (!trimG(*sc).empty()) xname = trimG(*sc);
  }
  // Each grid point is substituted for the bare name x (a same-named function
  // call is spared) and the expression evaluated by the macro layer's evaluator,
  // which already answers Stata's missing for an out-of-domain argument (BUG-374(3):
  // sqrt(-1) is `.`).
  std::vector<double> y(static_cast<size_t>(n), kNaN);
  for (int64_t i = 0; i < n; ++i) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "(%.17g)", grid[static_cast<size_t>(i)]);
    const std::string e = eval::substituteBareName(expr, xname, buf);
    std::string raised;
    bool isStr = false;
    const std::optional<std::string> v = eval::evalArith(e, &session.eval, &raised, &isStr);
    if (!v.has_value() || isStr) {
      int rc = isStr ? 109 : 198;
      if (!raised.empty()) {
        static const std::regex reRc(R"(r\((\d+)\))");
        std::smatch m;
        if (std::regex_search(raised, m, reRc)) {
          rc = std::atoi(m.str(1).c_str());
        } else {
          rc = 133;  // an undefined name: Stata's `unknown function` for a call
        }
      }
      return ojson{{"_refusal", stataRefusal("error in expression: " + expr, rc)}};
    }
    const std::string t = trimG(*v);
    if (t.empty() || t[0] == '.') {
      y[static_cast<size_t>(i)] = kNaN;
    } else {
      y[static_cast<size_t>(i)] = std::strtod(t.c_str(), nullptr);
    }
  }
  ojson pts = ojson::array();
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::isnan(grid[i]) || std::isnan(y[i])) continue;
    pts.push_back(ojson::array({grid[i], y[i]}));
  }
  ojson out;
  out["kind"] = "line";
  out["subkind"] = "function";
  out["points"] = pts;
  out["style"] = twoway::layerStyle(opts, layerIdx, "line");
  out["n"] = static_cast<int64_t>(pts.size());
  if (sersetOut != nullptr) {
    serset::Serset sf;
    sf.n = n;
    std::vector<size_t> order(static_cast<size_t>(n));
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&grid](size_t a, size_t b) {
      if (std::isnan(grid[a]) || std::isnan(grid[b])) return !std::isnan(grid[a]) && std::isnan(grid[b]);
      return grid[a] < grid[b];
    });
    std::vector<double> ys, xs;
    for (size_t i : order) {
      ys.push_back(y[i]);
      xs.push_back(grid[i]);
    }
    sf.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(ys)));
    sf.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(xs)));
    *sersetOut = std::move(sf);
  }
  return out;
}


// serset-twoway SCI: `scatteri y x [(#)] ["label"] ...` ([G-2] graph twoway
// scatteri): one point per y x pair, values stored
// as floats; a `(#)` clock position (0..12) and/or a quoted label after a pair.
// Serset y x float float, plus a byte position column when any point has one
// and a str# label column when any point has one; its own serset, in the order
// written (w2al tf_sci_008 log:3098-3100, tf_sci_lab log:3137-3139). recast(line)
// / connect(l) only change the drawing.
// serset-twoway HIST-5: `pci|pcarrowi y0 x0 y1 x1 [(#)] ["label"] ...` take the
// same path with four numbers per point ([G-2] graph twoway pci): serset
// y0 x0 y1 x1 float, then the position byte and
// the label str# when any point has them (w2aq hk_h_040addplot_s2 log:2040-2051,
// hk_h_064addplot_s2/_s3 log:2079-2107).
static ojson layerScatteri(const twoway::LayerSpec& spec, int layerIdx, std::optional<serset::Serset>* sersetOut) {
  const std::string ptype = strutil::lower(spec.plottype);
  const size_t groupSize = (ptype == "pci" || ptype == "pcarrowi") ? 4 : 2;
  // Tokens: a quoted string ("..." or `"..."'), a (...) group, or a blank-free word.
  std::vector<std::pair<char, std::string>> toks;  // kind: 'q' quoted, 'p' paren, 'w' word
  const std::string& s = spec.rawArgs;
  size_t i = 0;
  while (i < s.size()) {
    if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    if (s[i] == '"') {
      const size_t e = s.find('"', i + 1);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('q', s.substr(i + 1, end - i - 1));
      i = end + 1;
    } else if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      const size_t e = s.find("\"'", i + 2);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('q', s.substr(i + 2, end - i - 2));
      i = end + 2;
    } else if (s[i] == '(') {
      const size_t e = s.find(')', i + 1);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('p', trimG(s.substr(i + 1, end - i - 1)));
      i = end + 1;
    } else {
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      toks.emplace_back('w', s.substr(i, j - i));
      i = j;
    }
  }
  auto num = [](const std::string& t) -> std::optional<double> {
    if (t == ".") return kNaN;
    return parseNumberG(t);
  };
  std::vector<double> ys, xs, ybs, xbs, ps;
  std::vector<std::string> labels;
  bool anyPos = false, anyLbl = false;
  size_t k = 0;
  while (k < toks.size()) {
    if (k + groupSize > toks.size()) {
      return ojson{{"_refusal", stataRefusal("invalid point, " + toks[k].second, 198)}};
    }
    std::vector<double> vals;
    for (size_t q = 0; q < groupSize; ++q) {
      const auto v = num(toks[k + q].second);
      if (toks[k + q].first != 'w' || !v.has_value()) {
        return ojson{{"_refusal", stataRefusal("invalid point, " + toks[k + q].second, 198)}};
      }
      vals.push_back(*v);
    }
    const std::optional<double> yv = vals[0], xv = vals[1];
    if (groupSize == 4) {
      ybs.push_back(serset::toFloat(vals[2]));
      xbs.push_back(serset::toFloat(vals[3]));
    }
    k += groupSize;
    double pos = kNaN;
    std::string lbl;
    if (k < toks.size() && toks[k].first == 'p') {
      const auto pv = parseNumberG(toks[k].second);
      if (!pv.has_value() || *pv < 0 || *pv > 12 || *pv != std::floor(*pv)) {
        return ojson{{"_refusal", stataRefusal("invalid position, " + toks[k].second, 198)}};
      }
      pos = *pv;
      anyPos = true;
      ++k;
    }
    if (k < toks.size() && toks[k].first == 'q') {
      lbl = toks[k].second;
      anyLbl = true;
      ++k;
    }
    ys.push_back(serset::toFloat(*yv));
    xs.push_back(serset::toFloat(*xv));
    ps.push_back(pos);
    labels.push_back(lbl);
  }
  bool asLine = false;
  if (auto rc = optLast(spec.opts, "recast")) asLine = strutil::lower(trimG(*rc)) == "line";
  if (auto cn = optLast(spec.opts, "connect")) {
    const std::string c = strutil::lower(trimG(*cn));
    if (!c.empty() && c[0] == 'l') asLine = true;
  }
  ojson pts = ojson::array();
  for (size_t j = 0; j < ys.size(); ++j) {
    if (std::isnan(ys[j]) || std::isnan(xs[j])) continue;
    pts.push_back(ojson::array({xs[j], ys[j]}));
  }
  ojson out;
  if (groupSize == 4) {
    // drawn as segments (x0, y0) -> (x1, y1)
    ojson pc = ojson::array();
    for (size_t j = 0; j < ys.size(); ++j) pc.push_back(ojson::array({xs[j], ys[j], xbs[j], ybs[j]}));
    out["kind"] = ptype;
    out["pc"] = pc;
  } else {
    out["kind"] = asLine ? "line" : "scatter";
    out["subkind"] = "scatteri";
    out["points"] = pts;
  }
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, asLine || groupSize == 4 ? "line" : "scatter");
  out["n"] = static_cast<int64_t>(ys.size());
  if (sersetOut != nullptr) {
    serset::Serset si;
    si.n = static_cast<int64_t>(ys.size());
    int t = 0;
    si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", ys));
    si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", xs));
    if (groupSize == 4) {
      si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", ybs));
      si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", xbs));
    }
    if (anyPos) si.cols.push_back(serset::numericColumn(serset::tempName(t++), "byte", ps));
    if (anyLbl) si.cols.push_back(serset::stringColumn(serset::tempName(t++), labels));
    *sersetOut = std::move(si);
  }
  return out;
}


// The rows of a one-variable plot layer that enter its estimate: the variable
// nonmissing and, with a layer weight, the weight nonmissing and nonzero (a row
// past the end of the weight column counts as a missing weight).
struct OneVarSample {
  std::vector<double> v, w;
};


static OneVarSample oneVarLayerSample(Datastore& ds, const std::string& var, const std::vector<double>* w) {
  OneVarSample out;
  const std::vector<double> col = numericColumn(ds, var);
  for (size_t i = 0; i < col.size(); ++i) {
    if (std::isnan(col[i])) continue;
    if (w == nullptr) {
      out.v.push_back(col[i]);
      continue;
    }
    const double wi = i < w->size() ? (*w)[i] : kNaN;
    if (std::isnan(wi) || wi == 0.0) continue;
    out.v.push_back(col[i]);
    out.w.push_back(wi);
  }
  return out;
}


// A layer refusal, in the shape runTwowayLayers unwraps.
static ojson layerRefusal(const std::string& message, int rc) { return ojson{{"_refusal", stataRefusal(message, rc)}}; }


// serset-twoway HKL: the `twoway histogram var` layer -- the binning rule over
// the layer sample, last occurrence of each option. start() is passed only
// when typed, so the closing row appears only then (w2al tf_hist_082 3 rows,
// tf_hist_085 `(.,0,-5)`).
static ojson layerHistogram(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx, bool genFloat,
                     int64_t nFull, const std::vector<double>* w, const std::string& wtype,
                     std::optional<serset::Serset>* sersetOut) {
  (void)wtype;  // runTwowayLayers has already refused anything but fweights
  if (spec.varlist.empty()) return layerRefusal("varlist required", 100);
  if (spec.varlist.size() > 1) return layerRefusal("too many variables specified", 103);
  const std::string& var = spec.varlist.front();
  const HistOpts ho = parseHistOpts(spec.opts, var, genFloat, /*firstWins=*/false);
  if (ho.err) return ojson{{"_refusal", *ho.err}};
  const OneVarSample smp = oneVarLayerSample(ds, var, w);
  const HistGenResult g = histogramGen(smp.v, w != nullptr ? &smp.w : nullptr, ho.spec, nFull, true);
  if (g.rc != 0) return layerRefusal(g.message, g.rc);
  ojson layer;
  layer["kind"] = "histogram";
  layer["bars"] = barList(g);
  layer["y_kind"] = ho.spec.type;
  layer["start"] = g.start;
  layer["width"] = g.width;
  layer["bin"] = g.bin;
  layer["style"] = twoway::layerStyle(spec.opts, layerIdx, "histogram");
  layer["n"] = static_cast<int64_t>(smp.v.size());
  if (sersetOut != nullptr) *sersetOut = barSerset(g, genFloat);
  return layer;
}


// serset-twoway HKL: the `twoway kdensity var` layer ([G-2] graph twoway
// kdensity) -- n() points (default 300) accumulated across range() (default
// the sample's min and max, not widened by the bandwidth: tf_kd_n), the
// kdensity estimate at them times area().
static ojson layerKdensity(SessionState& session, Datastore& ds, const twoway::LayerSpec& spec,
                    int layerIdx, const std::vector<double>* w, const std::string& wtype,
                    std::optional<serset::Serset>* sersetOut) {
  if (spec.varlist.empty()) return layerRefusal("varlist required", 100);
  if (spec.varlist.size() > 1) return layerRefusal("too many variables specified", 103);
  const std::string& var = spec.varlist.front();
  const auto& opts = spec.opts;

  int64_t points = 300;
  if (const auto text = optLast(opts, "n")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value() || std::floor(*d) != *d) return layerRefusal("option n() not allowed", 198);  // probe tknf
    points = static_cast<int64_t>(*d);
  }
  const BandwidthChoice bw = chooseBandwidth(opts, "options width() and bwidth() may not be combined");
  if (bw.err) return ojson{{"_refusal", *bw.err}};
  double area = 1.0;
  if (const auto text = optLast(opts, "area")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value()) return layerRefusal("option area() incorrectly specified", 198);
    area = *d;
  }
  if (points < 1) return layerRefusal("option n() requires a positive integer", 198);
  if (area <= 0) return layerRefusal("option area() expects a positive number", 198);
  const KernelChoice kc = chooseKernel(opts, /*strictWords=*/false);
  if (kc.err) return ojson{{"_refusal", *kc.err}};

  const OneVarSample smp = oneVarLayerSample(ds, var, w);
  // The grid's ends.
  double lo = kNaN, hi = kNaN;
  auto widenTo = [&](double x) {
    if (std::isnan(x)) return;
    if (std::isnan(lo) || x < lo) lo = x;
    if (std::isnan(hi) || x > hi) hi = x;
  };
  const std::optional<std::string> rangeText = optLast(opts, "range");
  const std::vector<std::string> rangeWords = rangeText.has_value() ? splitWsG(*rangeText) : std::vector<std::string>{};
  if (rangeWords.size() == 1 && ds.has(rangeWords.front())) {
    // range(varname): that variable's extent over the rows where the plotted
    // variable is nonmissing (probe tkrv).
    const std::vector<double> rangeCol = numericColumn(ds, rangeWords.front());
    const std::vector<double> plotted = numericColumn(ds, var);
    for (size_t i = 0; i < rangeCol.size() && i < plotted.size(); ++i) {
      if (!std::isnan(plotted[i])) widenTo(rangeCol[i]);
    }
  } else if (!rangeWords.empty()) {
    // range(# #), in either order (probe tkrr); one or three numbers are
    // r(122) / r(123) as in Stata (probes tkr1, tkr3).
    std::vector<double> ends;
    for (const std::string& word : rangeWords) {
      const std::optional<double> d = parseNumberG(word);
      if (!d.has_value()) return layerRefusal("option range() requires two numbers", 198);
      ends.push_back(*d);
    }
    if (ends.size() < 2) return layerRefusal("option range() requires two numbers", 122);
    if (ends.size() > 2) return layerRefusal("option range() requires two numbers", 123);
    widenTo(ends[0]);
    widenTo(ends[1]);
  } else {
    for (double x : smp.v) widenTo(x);
  }

  const std::vector<double> grid = accumulatedGrid(lo, hi, points);
  KdensitySpec ks;
  ks.kernel = kc.name;
  ks.width = bw.h;
  ks.wtype = wtype;
  Datastore* full = session.datastore();
  const int64_t nFull = full != nullptr ? full->nRows() : ds.nRows();
  const KdensityResult k = kdensityEstimate(smp.v, smp.w, ks, nFull, &grid);
  std::vector<double> curve(grid.size(), kNaN);
  for (size_t i = 0; i < curve.size() && i < k.density.size(); ++i) curve[i] = k.density[i] * area;

  ojson pts = ojson::array();
  for (size_t i = 0; i < grid.size(); ++i) {
    if (!std::isnan(grid[i]) && !std::isnan(curve[i])) pts.push_back(ojson::array({grid[i], curve[i]}));
  }
  ojson layer;
  layer["kind"] = "kdensity";
  layer["points"] = pts;
  layer["kernel"] = kc.name;
  layer["bwidth"] = k.bwidth;
  layer["style"] = twoway::layerStyle(opts, layerIdx, "line");
  layer["n"] = static_cast<int64_t>(smp.v.size());
  if (sersetOut != nullptr && full != nullptr) *sersetOut = curveWithVariableSerset(session, *full, var, curve, grid);
  return layer;
}


// _parse_scale_range: extract (lo, hi) from `xscale(range(LO HI) ...)`.
static std::optional<std::pair<double, double>> parseScaleRange(const MaybeOptFromOpts& scaleOpt) {
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
static ojson reflines(const MaybeOptFromOpts& raw) {
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


// ---------------------------------------------------------------------------
// legend() content parser (BUG-796..799, FU-112/FU-113).
//
// The text of `legend(...)` used to be searched by regexes that ran over the
// RAW string, quotes included -- so `order(1 "a" 4 "All ages (pooled)")` was cut
// at the first `)` inside the label, a label containing `off`, `size(` or
// `cols(` bound to the wrong suboption, and `order(2 1)` (no text) vanished.
// There was no reusable top-level splitter in src/ or include/ (the only
// `splitTopLevel` is internal to tables/table_text.cpp), so this one is
// written here. It splits ONCE into top-level suboptions with a quote-aware
// (plain "..." and compound `"..."') and paren-balanced scan; every consumer
// below reads from that list.
// ---------------------------------------------------------------------------

struct LegendSub {
  std::string name;  // lower-cased suboption token; empty for a bare quoted string
  std::string arg;   // text between the matching parens, verbatim; empty when none
  bool hasParens = false;
};


// Returns the index one past the quoted string that starts at s[i] (a plain
// `"` or a compound `"`), or npos when s[i] does not start one. An unterminated
// quote consumes the rest of the text.
static size_t skipLegendQuote(const std::string& s, size_t i) {
  if (i >= s.size()) return std::string::npos;
  if (s[i] == '"') {
    size_t j = s.find('"', i + 1);
    return j == std::string::npos ? s.size() : j + 1;
  }
  if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
    int depth = 1;
    size_t j = i + 2;
    while (j < s.size() && depth > 0) {
      if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
        ++depth;
        j += 2;
      } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
        --depth;
        j += 2;
      } else {
        ++j;
      }
    }
    return j;
  }
  return std::string::npos;
}


// The text of a quoted string (the span returned by skipLegendQuote) without
// its delimiters.
static std::string legendQuoteBody(const std::string& s, size_t i, size_t end) {
  if (s[i] == '"') {
    size_t hi = (end > i + 1 && s[end - 1] == '"') ? end - 1 : end;
    return s.substr(i + 1, hi - (i + 1));
  }
  size_t hi = (end >= i + 4 && s[end - 2] == '"' && s[end - 1] == '\'') ? end - 2 : end;
  return s.substr(i + 2, hi - (i + 2));
}


static std::vector<LegendSub> splitLegendSubopts(const std::string& text) {
  std::vector<LegendSub> subs;
  size_t i = 0;
  const size_t n = text.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; };
  while (i < n) {
    if (isSp(text[i])) { ++i; continue; }
    LegendSub sub;
    size_t q = skipLegendQuote(text, i);
    if (q != std::string::npos) {  // a free-standing quoted string
      i = q;
      subs.push_back(sub);
      continue;
    }
    size_t j = i;
    while (j < n && !isSp(text[j]) && text[j] != '(' && text[j] != ')' && text[j] != '"' && text[j] != '`') ++j;
    if (j == i) { ++i; continue; }  // a stray `)` / quote char: step over it
    sub.name = strutil::lower(text.substr(i, j - i));
    i = j;
    if (i < n && text[i] == '(') {
      sub.hasParens = true;
      int depth = 1;
      size_t k = i + 1;
      while (k < n && depth > 0) {
        size_t qq = skipLegendQuote(text, k);
        if (qq != std::string::npos) { k = qq; continue; }
        if (text[k] == '(') ++depth;
        else if (text[k] == ')') --depth;
        ++k;
      }
      size_t close = (depth == 0) ? k - 1 : n;  // unbalanced: take the rest
      sub.arg = text.substr(i + 1, close - (i + 1));
      i = (depth == 0) ? k : n;
    }
    subs.push_back(sub);
  }
  return subs;
}


// One order() entry: a 1-based plot index and its key text, or a `-` entry:
// text inserted with no key ([G-3] legend_options orderinfo `{#|-} ["text"
// ["text" ...]]`). Each quoted string is one line of the text.
struct LegendOrderEntry {
  bool dash = false;
  int64_t index = 0;
  bool hasText = false;
  std::string text;  // the quoted strings, one per line, joined with "\n"
};


static std::vector<LegendOrderEntry> parseLegendOrder(const std::string& inner) {
  std::vector<LegendOrderEntry> entries;
  size_t i = 0;
  const size_t n = inner.size();
  while (i < n) {
    char c = inner[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
    size_t q = skipLegendQuote(inner, i);
    if (q != std::string::npos) {
      if (!entries.empty()) {
        LegendOrderEntry& e = entries.back();
        std::string body = legendQuoteBody(inner, i, q);
        // A `-` entry's "" inserts nothing (" " is a blank line), as Stata's
        // help says of order(1 - " " "Predicted" 2 3).
        if (!(e.dash && body.empty())) {
          if (e.hasText) e.text += "\n";
          e.text += body;
          e.hasText = true;
        }
      }
      i = q;
      continue;
    }
    if (c >= '0' && c <= '9') {
      size_t j = i;
      while (j < n && inner[j] >= '0' && inner[j] <= '9') ++j;
      LegendOrderEntry e;
      try {
        e.index = std::stoll(inner.substr(i, j - i));
      } catch (...) {
        e.index = 0;
      }
      entries.push_back(e);
      i = j;
      continue;
    }
    if (c == '-') {
      LegendOrderEntry e;
      e.dash = true;
      entries.push_back(e);
      ++i;
      continue;
    }
    ++i;  // anything else (`|`, stray text) is not an entry
  }
  return entries;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


// The legend block -- order, cols (rows when given), size, off -- from the text
// of legend(...); one top-level split (quote-aware, paren-balanced), nothing
// searched in the raw text: `off` is a standalone top-level token, so "Payoff"
// or `size(off)` inside a label or another suboption is not it. (Also called
// from cdfplot.cpp's cdfplot block.) order() lists the keys as
// [plot index (0-based), text]: a bare index shows keyText(index), and one
// keyText has no plot for is dropped. A `-` entry is text with no key, [-1,
// text] (BUG-1311). Each quoted string is a line of the text, joined with "\n"
// (BUG-1311). cols() and rows() are also spelled by any prefix down to c() and
// r() ([G-3] legend_options `c:ols(#)`, `r:ows(#)`; BUG-1310). *hasOrder says
// whether order() was given.
ojson legendBlock(const std::string& legendText,
                  const std::function<std::optional<std::string>(int64_t)>& keyText, bool* hasOrder) {
  ojson legendOrder = ojson::array();
  bool legendOff = false;
  ojson legendCols = nullptr, legendRows = nullptr, legendSize = nullptr;
  bool sawOrder = false, sawCols = false, sawSize = false;
  auto prefixOf = [](const std::string& name, const std::string& full) {
    return !name.empty() && name.size() <= full.size() && full.compare(0, name.size(), name) == 0;
  };
  for (const auto& sub : splitLegendSubopts(legendText)) {
    if (sub.name == "off" && !sub.hasParens) {
      legendOff = true;
    } else if (sub.name == "order" && sub.hasParens && !sawOrder) {
      sawOrder = true;
      for (const auto& e : parseLegendOrder(sub.arg)) {
        if (e.dash) {
          if (e.hasText) legendOrder.push_back(ojson::array({-1, e.text}));
          continue;
        }
        if (e.index < 1) continue;
        std::string label;
        if (e.hasText) {
          label = e.text;
        } else {
          const std::optional<std::string> k = keyText(e.index - 1);
          if (!k.has_value()) continue;  // no such plot: ignored
          label = *k;
        }
        legendOrder.push_back(ojson::array({e.index - 1, label}));
      }
    } else if ((prefixOf(sub.name, "cols") || prefixOf(sub.name, "rows")) && sub.hasParens && !sawCols) {
      std::string a = strutil::trim(sub.arg);
      if (!a.empty() && std::all_of(a.begin(), a.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        sawCols = true;
        (sub.name[0] == 'c' ? legendCols : legendRows) = std::stoi(a.substr(0, 9));
      }
    } else if (sub.name == "size" && sub.hasParens && !sawSize) {
      std::string a = strutil::trim(sub.arg);
      if (!a.empty()) {
        sawSize = true;
        legendSize = a;
      }
    }
  }
  ojson legend;
  legend["order"] = legendOrder;
  legend["cols"] = legendCols;
  if (!legendRows.is_null()) legend["rows"] = legendRows;
  legend["size"] = legendSize;
  legend["off"] = legendOff;
  if (hasOrder != nullptr) *hasOrder = sawOrder;
  return legend;
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;




// serset-twoway SS: a data plot's serset contribution (piet/serset.h rule (a)):
// its varlist in role order, then the mlabel() variable when the plot does not
// already hold it (w2al tf_sc_mlabel `y x s`, tf_tw_graph077 mlabel(y) -> `xaxis
// y`), over the layer's sample in data order. Plots drawn with `sort` get their
// own serset sorted by x, the last role variable.
static std::optional<serset::PlotSpec> layerDataPlotSpec(SessionState& session, Datastore& ds,
                                                  const twoway::LayerSpec& spec,
                                                  const std::string& pt) {
  static const std::set<std::string> kDataPlotTypes = {
      "scatter", "line",   "connected", "rarea", "area",   "rcap",    "rcapsym", "rspike",
      "rline",   "rbar",   "rscatter",  "rconnected", "bar", "spike", "dropline", "dot"};
  if (kDataPlotTypes.count(pt) == 0) return std::nullopt;
  std::vector<std::string> vars = spec.varlist;
  const size_t roleCount = vars.size();
  // serset-twoway WT: the weight variable follows the plot's variables (w2al
  // tf_tw_072_s1 `y x w` byte, log:325).
  const std::string wvar = trimG(spec.weightExp);
  if (!spec.weightType.empty() && !wvar.empty() && ds.has(wvar) &&
      std::find(vars.begin(), vars.end(), wvar) == vars.end()) {
    vars.push_back(wvar);
  }
  if (auto ml = optLast(spec.opts, "mlabel")) {
    const std::string mv = trimG(*ml);
    if (!mv.empty() && ds.has(mv) && std::find(vars.begin(), vars.end(), mv) == vars.end()) {
      vars.push_back(mv);
    }
  }
  std::vector<int64_t> rows(static_cast<size_t>(ds.nRows()));
  for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<int64_t>(i);
  serset::PlotSpec p;
  for (const auto& v : vars) p.cols.push_back(serset::variableColumn(session, ds, v, rows));
  p.key = (spec.ifExpr.has_value() ? trimG(*spec.ifExpr) : std::string()) + "\x1f" +
          (spec.inRange.has_value() ? trimG(*spec.inRange) : std::string()) + "\x1f" +
          spec.weightType + "=" + wvar;
  p.sorted = hasOpt(spec.opts, "sort");
  p.sortCol = roleCount > 0 ? static_cast<int>(roleCount) - 1 : -1;
  return p;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


ojson runTwowayLayers(Datastore* dsPtr, SessionState& session,
                      const std::vector<twoway::LayerSpec>& layers,
                      const std::vector<std::pair<std::string, eval::OptValue>>& globalOpts,
                      const std::optional<std::string>& globalIf,
                      std::vector<serset::PlotSpec>* sersetPlots,
                      const std::optional<std::string>& globalIn) {
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
      "scatter", "line",  "connected", "lfit",   "qfit",    "lfitci",     "qfitci",
      "rarea",   "histogram", "kdensity", "rcap", "rcapsym", "rspike", "rline",
      "rconnected", "rscatter", "area", "spike", "dropline", "bar", "function", "scatteri",
      "pci", "pcarrowi"};
  for (const auto& spec : layers) {
    std::string pt = spec.plottype;
    std::transform(pt.begin(), pt.end(), pt.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (pt == "hist") pt = "histogram";
    // serset-twoway REF: a word that is no Stata twoway plot type at all is
    // r(198) `<word> is not a twoway plot type` (w2al tf_r_badtype,
    // log:5165-5172); a real Stata type PIET lacks keeps the honest r(199).
    static const std::set<std::string> kStataTwowayTypes = {
        "scatter", "line", "connected", "scatteri", "area", "bar", "spike", "dropline", "dot",
        "rarea", "rbar", "rspike", "rcap", "rcapsym", "rscatter", "rline", "rconnected",
        "pcspike", "pccapsym", "pcarrow", "pcbarrow", "pcscatter", "pci", "pcarrowi", "tsline",
        "tsrline", "contour", "contourline", "heatmap", "mband", "mspline", "lowess", "lpoly",
        "lpolyci", "lfit", "qfit", "fpfit", "lfitci", "qfitci", "fpfitci", "function",
        "histogram", "kdensity"};
    if (kSupportedLayerTypes.count(pt) == 0 && kStataTwowayTypes.count(pt) == 0) {
      return stataRefusal(pt + " is not a twoway plot type", 198);
    }
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

  // serset-twoway REF: every plot's variables must exist (r(111) `variable
  // <v> not found`, the whole statement fails: w2al tf_r_novar, log:5180-5187)
  // and be numeric (r(109) `string variables not allowed in varlist;` / `<v> is
  // a string variable`: tf_r_sc_strx, tf_r_lfit_str, log:5315-5339). A layer
  // whose variable was missing used to be dropped and the rest drawn at rc 0.
  for (const auto& spec : layers) {
    // serset-twoway WT: the weights each plot type takes, else r(101) `<type>s
    // not allowed` (w2al tf_r_iw / tf_r_hist_pw / tf_r_kd_pw, log:5208-5245),
    // and the weight variable must exist (tf_r_wtvar `nosuchwt not found`).
    if (!spec.weightType.empty()) {
      std::string pt = strutil::lower(spec.plottype);
      if (pt == "hist") pt = "histogram";
      std::set<std::string> legal = {"aweight", "fweight", "pweight"};
      if (pt == "histogram") legal = {"fweight"};
      if (pt == "kdensity") legal = {"fweight", "aweight"};
      if (legal.count(spec.weightType) == 0) return stataRefusal(spec.weightType + "s not allowed", 101);
      const std::string wv = trimG(spec.weightExp);
      if (!dsPtr->has(wv)) return stataRefusal(wv + " not found", 111);
    }
    {
      const std::string ptl = strutil::lower(spec.plottype);
      if (ptl == "function" || ptl == "scatteri" || ptl == "pci" || ptl == "pcarrowi") continue;
    }
    for (const auto& v : spec.varlist) {
      if (!dsPtr->has(v)) return stataRefusal("variable " + v + " not found", 111);
      if (dsPtr->dtype(v) == "factor") {
        return stataRefusal("string variables not allowed in varlist;\n" + v + " is a string variable",
                            109);
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

    // serset-twoway IN: the layer's `in` and the global `in` select by position
    // in the full dataset and intersect each other and both ifs (w2al tf_sc_in
    // 8 rows log:770, tf_sc_ifin 9/4 rows :798-808, tf_q_globalin :4864-4868,
    // tf_q_both :4926-4934).
    std::optional<std::string> effectiveIn;
    {
      const bool li = spec.inRange.has_value() && !spec.inRange->empty();
      const bool gi = globalIn.has_value() && !globalIn->empty();
      if (li || gi) {
        const int64_t nAll = dsPtr->nRows();
        long long a = 1, b = nAll;
        for (const auto* r : {li ? &*spec.inRange : nullptr, gi ? &*globalIn : nullptr}) {
          if (r == nullptr) continue;
          const InRangeBounds bb = parseInRangeBounds(*r, nAll);
          if (!bb.ok) {
            InRangeFilterResult fr;
            fr.ok = false;
            fr.refusal = bb.refusal;
            return inRangeRejectError(fr, *r, "twoway");
          }
          a = std::max(a, bb.a);
          b = std::min(b, bb.b);
        }
        if (a > b) {
          // an empty intersection: no observation is plotted
          effectiveIf = std::string("0");
        } else {
          effectiveIn = std::to_string(a) + "/" + std::to_string(b);
        }
      }
    }

    Datastore* layerDs = dsPtr;
    std::string tmpPath;
    std::unique_ptr<Datastore> tmpDs;
    bool filterFailed = false;
    if (effectiveIn.has_value()) {
      try {
        InRangeFilterResult fr = applyIfAndInFilter(session, dsPtr->path(), effectiveIf, effectiveIn);
        if (!fr.ok) return inRangeRejectError(fr, *effectiveIn, "twoway");
        tmpDs = std::make_unique<Datastore>(fr.dataPath);
        layerDs = tmpDs.get();
        if (fr.dataPath != dsPtr->path()) cecon::native_unlink(fr.dataPath);
      } catch (const std::exception& e) {
        layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " + e.what());
        filterFailed = true;
      }
    } else if (effectiveIf.has_value()) {
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
    if (pt == "hist") pt = "histogram";
    // serset-twoway WT: the layer's weight over the layer sample.
    std::vector<double> layerW;
    const std::vector<double>* layerWp = nullptr;
    if (!spec.weightType.empty() && layerDs->has(trimG(spec.weightExp))) {
      layerW = numericColumn(*layerDs, trimG(spec.weightExp));
      layerWp = &layerW;
    }
    ojson lyr;
    if (pt == "histogram" || pt == "kdensity") {
      // serset-twoway HKL: the twoway histogram/kdensity plot types (their own
      // serset each, never shared).
      std::optional<serset::Serset> oneSet;
      Datastore* fullDs = session.datastore();
      const int64_t nFull = fullDs != nullptr ? fullDs->nRows() : layerDs->nRows();
      if (pt == "histogram") {
        lyr = layerHistogram(*layerDs, spec, static_cast<int>(idx), session.defaultStorageType != "double",
                             nFull, layerWp, spec.weightType, &oneSet);
      } else {
        lyr = layerKdensity(session, *layerDs, spec, static_cast<int>(idx), layerWp, spec.weightType,
                            &oneSet);
      }
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && oneSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*oneSet);
        sersetPlots->push_back(std::move(ps));
      }
    } else if (pt == "function" || pt == "scatteri" || pt == "pci" || pt == "pcarrowi") {
      // serset-twoway FN / SCI: their own serset each, never shared.
      std::optional<serset::Serset> oneSet;
      lyr = pt == "function" ? layerFunction(session, *layerDs, spec, static_cast<int>(idx), &oneSet)
                             : layerScatteri(spec, static_cast<int>(idx), &oneSet);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && oneSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*oneSet);
        sersetPlots->push_back(std::move(ps));
      }
    } else if (pt == "rcap" || pt == "rcapsym" || pt == "rspike" || pt == "rline" ||
               pt == "rconnected" || pt == "rscatter" || pt == "area" || pt == "spike" ||
               pt == "dropline" || pt == "bar") {
      lyr = layerRangeOrBase(*layerDs, spec, static_cast<int>(idx), pt);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
    } else if (pt == "rarea") {
      lyr = layerRarea(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else if (pt == "lfit" || pt == "qfit" || pt == "lfitci" || pt == "qfitci") {
      // BUG-099: was aliased straight into layerLineOrConnected -- a raw
      // connect-the-dots polyline through the DATA points, never an actual
      // fit. serset-twoway FIT: one kernel (computeFit) feeds the drawn layer
      // and the fit's own serset (see layerFit above).
      std::optional<serset::Serset> fitSet;
      lyr = layerFit(*layerDs, spec, static_cast<int>(idx), pt,
                     session.defaultStorageType != "double", layerWp, spec.weightType, &fitSet);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && fitSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*fitSet);
        sersetPlots->push_back(std::move(ps));
      }
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

    if (sersetPlots != nullptr) {
      if (auto ps = layerDataPlotSpec(session, *layerDs, spec, pt)) sersetPlots->push_back(std::move(*ps));
    }

    std::string lkind = lyr.value("kind", std::string());
    if (lkind == "rarea" || lkind == "lfitci" || lkind == "qfitci") {
      for (const auto& row : lyr.value("band", ojson::array())) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allY.push_back(row[2].get<double>());
      }
    }
    if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
        lkind == "qfit" || lkind == "lfitci" || lkind == "qfitci" || lkind == "kdensity") {
      for (const auto& pt2 : lyr.value("points", ojson::array())) {
        allX.push_back(pt2[0].get<double>());
        allY.push_back(pt2[1].get<double>());
      }
    }
    if (lyr.contains("pc")) {
      for (const auto& row : lyr["pc"]) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allX.push_back(row[2].get<double>());
        allY.push_back(row[3].get<double>());
      }
    }
    if (lyr.contains("segments")) {
      for (const auto& row : lyr["segments"]) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allY.push_back(row[2].get<double>());
      }
    }
    if (lyr.contains("base") && lyr.contains("points")) {
      for (const auto& pt2 : lyr["points"]) {
        allX.push_back(pt2[0].get<double>());
        allY.push_back(pt2[1].get<double>());
      }
      allY.push_back(lyr.value("base", 0.0));
    }
    if (lkind == "histogram") {
      for (const auto& b : lyr.value("bars", ojson::array())) {
        allX.push_back(b.value("x0", 0.0));
        allX.push_back(b.value("x1", 0.0));
        allY.push_back(0.0);
        allY.push_back(b.value("y", 0.0));
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
  MaybeOptFromOpts legendRaw = getOptG(globalOpts, "legend");
  bool labelsSynced = false;
  // A bare index keeps the key the legend shows for that plot without order():
  // the plot's y-variable label (or name); the fitted-line plot types read
  // "Fitted values", as in Stata.
  auto keyText = [&](int64_t idx) -> std::optional<std::string> {
    if (idx < 0 || static_cast<size_t>(idx) >= layers.size()) return std::nullopt;
    const twoway::LayerSpec& ls = layers[static_cast<size_t>(idx)];
    std::string pt = strutil::lower(ls.plottype);
    if (pt == "lfit" || pt == "qfit") return std::string("Fitted values");
    if (ls.varlist.empty()) return std::string();
    // eval.varLabels is a projection refreshed only on demand (see
    // tables/table_esttab.cpp's esttab `label` note); pay for it once, and
    // only when a bare index actually needs a default.
    if (!labelsSynced) {
      session.syncEvalVarLabels();
      labelsSynced = true;
    }
    std::string label = session.eval.varLabels.get(ls.varlist[0], std::string());
    return label.empty() ? ls.varlist[0] : label;
  };
  out["legend"] = legendBlock(legendRaw.present && !legendRaw.isTrue ? legendRaw.str : std::string(), keyText,
                              nullptr);

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

}  // namespace piet::handlers
