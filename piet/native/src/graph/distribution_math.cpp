// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "piet/handlers/graph.h"
#include "graph_support.h"
#include "distribution.h"

namespace piet::eval {
std::string stataMacroRealText(double d);
}

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


// Total weight and the two extremes of a binning sample (unit weights when no
// weight vector is given).
struct BinExtent {
  double total = 0.0;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
};


static BinExtent binExtent(const std::vector<double>& v, const std::vector<double>* w) {
  BinExtent e;
  for (size_t i = 0; i < v.size(); ++i) {
    e.total += (w != nullptr) ? (*w)[i] : 1.0;
    if (v[i] < e.lo) e.lo = v[i];
    if (v[i] > e.hi) e.hi = v[i];
  }
  return e;
}


// [R] histogram's default number of bins, k = min{sqrt(N), 10 ln(N)/ln(10)},
// truncated and at least 1. The log term is formed from natural logs as the
// manual writes it: StataBE gives 29 bins at N = 1000 and 59 at N = 1e6, where
// log10(N) would give 30 and 60 (measured on StataBE: bins1000, bins1e6fw).
static int64_t defaultBinCount(double n) {
  const double viaRoot = std::sqrt(n);
  const double viaLog = 10.0 * std::log(n) / std::log(10.0);
  const auto k = static_cast<int64_t>(std::min(viaRoot, viaLog));
  return k < 1 ? 1 : k;
}


// The discrete bar width: the smallest positive step between neighbouring
// sorted values. Under c(type) float the values and the step are float
// quantities, so non-integer data give a float width (hk_h_discnonint_tw:
// .33333325 = float(7/3) - 2). No positive step means width 1.
static double smallestStep(const std::vector<double>& v, bool asFloat) {
  std::vector<double> sorted(v);
  std::sort(sorted.begin(), sorted.end());
  double step = std::numeric_limits<double>::infinity();
  for (size_t i = 1; i < sorted.size(); ++i) {
    const double gap = storeAs(storeAs(sorted[i], asFloat) - storeAs(sorted[i - 1], asFloat), asFloat);
    if (gap > 0.0 && gap < step) step = gap;
  }
  return std::isfinite(step) ? step : 1.0;
}


// r(area): the factor that turns a density into the chosen height unit.
static double heightUnitArea(const std::string& type, double total, double width) {
  if (type == "frequency") return total * width;
  if (type == "fraction") return width;
  if (type == "percent") return 100.0 * width;
  return 1.0;
}


// A bar's height from its stored count: density bars integrate to 1, fraction
// heights sum to 1, percent heights to 100, frequency is the count itself.
static double barHeight(const std::string& type, double count, double total, double width) {
  if (type == "density") return count / (total * width);
  if (type == "fraction") return count / total;
  if (type == "percent") return 100.0 * count / total;
  return count;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


HistGenResult histogramGen(const std::vector<double>& v, const std::vector<double>* w,
                           const HistGenSpec& spec, int64_t nDataRows, bool wantGenerate) {
  HistGenResult res;
  res.type = spec.type;
  const bool asFloat = spec.genFloat;
  const BinExtent ext = binExtent(v, w);

  if (v.empty()) {
    // What Stata posts for an empty sample (hk_thg_empty_r): N and bin are 1,
    // the rest 0; the start() check is not reached.
    res.empty = true;
    res.area = res.max = res.min = res.start = res.width = 0.0;
    res.bin = 1;
    res.N = 1.0;
    res.nx = 1;
    return res;
  }

  // A constant variable without width() gets bars of width 1.
  std::optional<double> fixedWidth = spec.width;
  if (!fixedWidth.has_value() && ext.lo == ext.hi) fixedWidth = 1.0;

  // start() may sit at or below the minimum, or above it by a relative
  // difference of at most 1e-5 (probe startnear). It moves the lower edge of a
  // continuous histogram; for a discrete one it only changes r(start).
  double lowerEdge = ext.lo;
  double shownStart = ext.lo;
  if (spec.start.has_value()) {
    const double requested = *spec.start;
    if (ext.lo < requested && reldifG(ext.lo, requested) > 1e-5) {
      res.rc = 198;
      res.message = "option start() may not be larger than minimum of " + spec.varname;
      return res;
    }
    res.userStart = true;
    shownStart = requested;
    if (!spec.discrete) lowerEdge = requested;
  }

  double barWidth = 1.0;
  int64_t nBars = 1;
  if (spec.discrete) {
    // Bars centred on the values: the edge sits half a width below the minimum.
    barWidth = fixedWidth.has_value() ? *fixedWidth : smallestStep(v, asFloat);
    lowerEdge = ext.lo - barWidth / 2.0;
    nBars = static_cast<int64_t>((ext.hi - lowerEdge) / barWidth + 0.5);
  } else if (fixedWidth.has_value()) {
    barWidth = *fixedWidth;
    nBars = std::max<int64_t>(1, static_cast<int64_t>(std::ceil((ext.hi - lowerEdge) / barWidth)));
  } else {
    nBars = spec.bins.has_value() ? *spec.bins : defaultBinCount(ext.total);
    barWidth = (ext.hi - lowerEdge) / static_cast<double>(nBars);
  }

  res.area = heightUnitArea(spec.type, ext.total, barWidth);
  res.min = lowerEdge;
  res.max = lowerEdge + barWidth * static_cast<double>(nBars);
  res.start = shownStart;
  res.width = barWidth;
  res.bin = nBars;
  res.N = ext.total;
  if (!wantGenerate) return res;

  // Bin index floor((x - L)/h): bins are closed on the left, except that a
  // value on the top edge belongs to the last bin (probe edge) and anything
  // below the lower edge to the first.
  const double lastIndex = static_cast<double>(nBars);
  std::map<double, double> weightInBin;
  for (size_t i = 0; i < v.size(); ++i) {
    double idx = storeAs(std::floor((v[i] - lowerEdge) / barWidth), asFloat);
    if (idx < 0.0) idx = 0.0;
    if (idx == lastIndex) idx = lastIndex - 1.0;
    weightInBin[idx] += (w != nullptr) ? (*w)[i] : 1.0;
  }
  const double halfWidth = barWidth / 2.0;
  for (const auto& [idx, cnt] : weightInBin) {
    res.binIndex.push_back(idx);
    res.count.push_back(cnt);
    res.centre.push_back(storeAs(lowerEdge + halfWidth + barWidth * idx, asFloat));
    res.height.push_back(storeAs(barHeight(spec.type, storeAs(cnt, asFloat), ext.total, barWidth), asFloat));
  }
  // With a user start(), one more centre row holds the start itself when the
  // dataset has room for it (hk_thg_start: row 6 = (., -4)).
  if (res.userStart && static_cast<int64_t>(res.centre.size()) < nDataRows) {
    res.centre.push_back(storeAs(shownStart, asFloat));
    res.closingRow = true;
  }
  res.nx = static_cast<int64_t>(res.centre.size());
  return res;
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;




// The eight kernels of [R] kdensity, Methods and formulas. Each is zero
// outside its support; the support edge itself is outside, except parzen's.
using KernelFn = double (*)(double);


static double kernelEpanechnikov(double z) {
  static const double root5 = std::sqrt(5.0);
  if (std::fabs(z) >= root5) return 0.0;
  return 0.75 * (1.0 - z * z / 5.0) / root5;
}

static double kernelEpan2(double z) { return std::fabs(z) >= 1.0 ? 0.0 : 0.75 * (1.0 - z * z); }

static double kernelBiweight(double z) {
  if (std::fabs(z) >= 1.0) return 0.0;
  const double u = 1.0 - z * z;
  return (15.0 / 16.0) * u * u;
}

static double kernelCosine(double z) { return std::fabs(z) >= 0.5 ? 0.0 : 1.0 + std::cos(2.0 * M_PI * z); }

static double kernelGaussian(double z) {
  static const double root2pi = std::sqrt(2.0 * M_PI);
  return std::exp(-0.5 * z * z) / root2pi;
}

static double kernelParzen(double z) {
  const double a = std::fabs(z);
  if (a > 1.0) return 0.0;
  if (a > 0.5) {
    const double u = 1.0 - a;
    return 8.0 * u * u * u / 3.0;
  }
  return 4.0 / 3.0 - 8.0 * z * z + 8.0 * a * a * a;
}

static double kernelRectangle(double z) { return std::fabs(z) >= 1.0 ? 0.0 : 0.5; }

static double kernelTriangle(double z) {
  const double a = std::fabs(z);
  return a >= 1.0 ? 0.0 : 1.0 - a;
}

static double kernelUnknown(double) { return kNaN; }


struct KernelEntry {
  const char* name;
  size_t shortest;  // the minimum abbreviation kdensity accepts for kernel()
  KernelFn fn;
};


// Alphabetical. No accepted abbreviation fits two kernels.
static const std::array<KernelEntry, 8>& kernelTable() {
  static const std::array<KernelEntry, 8> table = {{{"biweight", 2, kernelBiweight},
                                                    {"cosine", 3, kernelCosine},
                                                    {"epan2", 5, kernelEpan2},
                                                    {"epanechnikov", 2, kernelEpanechnikov},
                                                    {"gaussian", 3, kernelGaussian},
                                                    {"parzen", 3, kernelParzen},
                                                    {"rectangle", 3, kernelRectangle},
                                                    {"triangle", 3, kernelTriangle}}};
  return table;
}


static KernelFn kernelByFullName(const std::string& name) {
  for (const KernelEntry& k : kernelTable()) {
    if (name == k.name) return k.fn;
  }
  return kernelUnknown;
}


// True for the full name of one of the eight kernels.
bool isKernelName(const std::string& word) {
  return std::any_of(kernelTable().begin(), kernelTable().end(),
                     [&word](const KernelEntry& e) { return word == e.name; });
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


double kdeKernel(const std::string& kernel, double z) { return kernelByFullName(kernel)(z); }


std::optional<std::string> kdensityKernelName(const std::string& typed) {
  for (const KernelEntry& k : kernelTable()) {
    const std::string full(k.name);
    const bool longEnough = typed.size() >= k.shortest;
    const bool isPrefix = typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
    if (longEnough && isPrefix) return full;
  }
  return std::nullopt;
}


// f(p) = sum_i w_i K((p - x_i)/h) / (q h), [R] kdensity Methods and formulas:
// q is the weight total (the observation count unweighted; aweights need no
// rescaling, it cancels), and 1 for iweights (hk_kd_iw). The sum runs in data
// order.
std::vector<double> kdensityAt(const std::vector<double>& x, const std::vector<double>& w,
                               const std::string& wtype, const std::string& kernel, double h,
                               const std::vector<double>& at) {
  std::vector<double> dens(at.size(), kNaN);
  if (x.empty() || std::isnan(h)) return dens;
  const bool weighted = !w.empty();
  const KernelFn K = kernelByFullName(kernel);
  double q = 0.0;
  for (size_t i = 0; i < x.size(); ++i) q += weighted ? w[i] : 1.0;
  const double divisor = (wtype == "iweight") ? h : h * q;
  for (size_t j = 0; j < at.size(); ++j) {
    const double p = at[j];
    if (std::isnan(p)) continue;
    if (h == 0.0) {  // hk_kd_const: a zero bandwidth estimates 0 at every point
      dens[j] = 0.0;
      continue;
    }
    double acc = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
      const double kv = K((p - x[i]) / h);
      acc += weighted ? w[i] * kv : kv;
    }
    dens[j] = acc / divisor;
  }
  return dens;
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;




// A double as Stata's macro text carries it (eval::stataMacroRealText), read
// back as a double.
double viaMacroText(double d) {
  if (std::isnan(d)) return d;
  return std::strtod(eval::stataMacroRealText(d).c_str(), nullptr);
}


// A running sum with Neumaier's correction term. Summarize's mean and variance
// (and so the default bandwidth) agree with the exactly rounded sums on every
// oracle bandwidth; a plain left-to-right sum is an ulp off on some.
class AccurateSum {
 public:
  void add(double term) {
    const double next = total_ + term;
    if (std::fabs(total_) >= std::fabs(term)) {
      carry_ += (total_ - next) + term;
    } else {
      carry_ += (term - next) + total_;
    }
    total_ = next;
  }
  double value() const { return total_ + carry_; }

 private:
  double total_ = 0.0;
  double carry_ = 0.0;
};


// One (value, weight) observation of a summarize sample.
struct WeightedObs {
  double x;
  double w;
};


// Summarize's p-th percentile of `obs` sorted by x ([R] summarize, Methods and
// formulas): with cumulative weights W_(i) and P = W p/100, the first x_(i)
// whose W_(i) exceeds P; on an exact tie the midpoint of x_(i) and x_(i+1).
static double weightedPercentile(const std::vector<WeightedObs>& obs, double totalWeight, double p) {
  if (obs.empty()) return kNaN;
  const double target = totalWeight * p / 100.0;
  double running = 0.0;
  for (size_t i = 0; i < obs.size(); ++i) {
    running += obs[i].w;
    if (running > target) return obs[i].x;
    if (running == target) {
      return (i + 1 < obs.size()) ? (obs[i].x + obs[i + 1].x) / 2.0 : obs[i].x;
    }
  }
  return obs.back().x;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


// kdensity's default bandwidth, [R] kdensity Methods and formulas:
// h = 0.9 m / N^(1/5) with m = min(sd, IQR/1.349) (m = sd when that is not
// positive). The moments and quartiles are summarize's with the weights;
// iweights do not enter the bandwidth. Stata carries m and h as macro text,
// which decides the last bit (W2AQ (10)); h is formed as 0.9 (m / N^0.2).
double kdensityDefaultWidth(const std::vector<double>& x, const std::vector<double>& w,
                            const std::string& wtype, double* outMin, double* outMax,
                            double* outMean, double* outSd) {
  const bool useWeights = !w.empty() && wtype != "iweight";
  auto rawWeight = [&](size_t i) { return useWeights ? w[i] : 1.0; };

  double lo = kNaN, hi = kNaN;
  AccurateSum rawTotal;
  for (size_t i = 0; i < x.size(); ++i) {
    rawTotal.add(rawWeight(i));
    if (std::isnan(lo) || x[i] < lo) lo = x[i];
    if (std::isnan(hi) || x[i] > hi) hi = x[i];
  }
  *outMin = lo;
  *outMax = hi;
  if (x.empty()) {
    *outMean = *outSd = kNaN;
    return kNaN;
  }

  const double nObs = static_cast<double>(x.size());
  // aweights are rescaled to sum to the number of observations.
  const double rescale = (useWeights && wtype == "aweight") ? nObs / rawTotal.value() : 1.0;
  std::vector<WeightedObs> obs(x.size());
  AccurateSum weightTotal, weightedX;
  for (size_t i = 0; i < x.size(); ++i) {
    obs[i] = {x[i], rawWeight(i) * rescale};
    weightTotal.add(obs[i].w);
    weightedX.add(obs[i].w * obs[i].x);
  }
  const double W = weightTotal.value();
  const double mean = weightedX.value() / W;
  AccurateSum squares;
  for (const WeightedObs& o : obs) {
    const double dev = o.x - mean;
    squares.add(o.w * dev * dev);
  }
  // Summarize's N: the fweight total, else the observation count.
  const double N = (useWeights && wtype == "fweight") ? rawTotal.value() : nObs;
  const double sd = N > 1.0 ? std::sqrt(squares.value() / (N - 1.0)) : kNaN;
  *outMean = mean;
  *outSd = sd;

  std::stable_sort(obs.begin(), obs.end(), [](const WeightedObs& a, const WeightedObs& b) { return a.x < b.x; });
  const double spreadByIqr =
      (weightedPercentile(obs, W, 75) - weightedPercentile(obs, W, 25)) / 1.349;
  // min() over the two spreads ignores a missing sd.
  double m = viaMacroText(std::isnan(sd) ? spreadByIqr : std::min(sd, spreadByIqr));
  if (m <= 0.0) m = viaMacroText(sd);
  return viaMacroText(0.9 * (m / std::pow(N, 0.2)));
}


KdensityResult kdensityEstimate(const std::vector<double>& x, const std::vector<double>& w,
                                const KdensitySpec& spec, int64_t nDataRows,
                                const std::vector<double>* at) {
  KdensityResult res;
  res.kernel = spec.kernel;
  double lo = kNaN, hi = kNaN;
  const double defaultH = kdensityDefaultWidth(x, w, spec.wtype, &lo, &hi, &res.mean, &res.sd);
  const double h = spec.width != 0.0 ? spec.width : defaultH;
  res.bwidth = h;

  if (at != nullptr) {
    // Evaluate at the given points (one per data row, missing ones kept);
    // r(n) counts the nonmissing ones but is at least 1 (probe kdatmiss).
    res.grid = *at;
    const auto present = std::count_if(at->begin(), at->end(), [](double a) { return !std::isnan(a); });
    res.n = std::max<int64_t>(1, static_cast<int64_t>(present));
  } else {
    // n() of 1 or less means the default 50; more points than data rows are
    // cut to _N with a note (probes kdn0, kdnneg, kdn20).
    int64_t points = spec.n > 1 ? spec.n : 50;
    if (points > nDataRows) {
      points = nDataRows;
      res.nCapped = true;
    }
    res.n = points;
    // Equally spaced from min - h to max + h.
    const double first = lo - h;
    const double step = ((hi - lo) + 2.0 * h) / static_cast<double>(points - 1);
    res.grid.resize(static_cast<size_t>(points));
    for (int64_t i = 0; i < points; ++i) res.grid[static_cast<size_t>(i)] = first + static_cast<double>(i) * step;
  }
  res.density = kdensityAt(x, w, spec.wtype, spec.kernel, h, res.grid);

  // r(scale) = 1 / (n * mean density); missing when there is no density or
  // the mean is 0 (hk_kd_const, hk_kd_empty). StataBE's value is the one an
  // accurately summed mean gives (probes kdn20, kdatpart).
  AccurateSum densSum;
  int64_t counted = 0;
  for (double d : res.density) {
    if (std::isnan(d)) continue;
    densSum.add(d);
    ++counted;
  }
  res.scale = kNaN;
  if (counted > 0) {
    const double meanDensity = densSum.value() / static_cast<double>(counted);
    if (meanDensity != 0.0) res.scale = 1.0 / (static_cast<double>(res.n) * meanDensity);
  }
  return res;
}

}  // namespace piet::handlers
