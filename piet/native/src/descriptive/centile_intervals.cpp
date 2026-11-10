// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// centile: centiles with binomial or normal-theory confidence intervals.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "estimation/statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// centile: the centile and its confidence interval, [R] centile (Methods and
// formulas: the default binomial-interpolated interval, the exact binomial
// interval `cci`, and the two normal-theory forms `normal` and `meansd`). The
// behaviour is as measured on StataBE 19.5.
namespace {

enum class CentileMethod { Interpolated, Exact, Normal, MeanSd };

struct CentileRequest {
  std::vector<double> percents;  // as typed, duplicates and order kept
  double level = 95.0;
  CentileMethod method = CentileMethod::Interpolated;
};

struct CentileRow {
  double centile = std::numeric_limits<double>::quiet_NaN();
  double lower = std::numeric_limits<double>::quiet_NaN();
  double upper = std::numeric_limits<double>::quiet_NaN();
  bool held = false;  // a limit was held at the sample minimum or maximum
};

// A number in Stata's numlist spelling: `5`, `.25`, `2.50`, `1e1`, `-3`.
bool centileNumber(const std::string& tok, double* v) {
  static const std::regex kNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  if (!std::regex_match(tok, kNum)) return false;
  *v = std::strtod(tok.c_str(), nullptr);
  return std::isfinite(*v);
}

bool centileMissingLiteral(const std::string& tok) {
  return tok == "." || (tok.size() == 2 && tok[0] == '.' && tok[1] >= 'a' && tok[1] <= 'z');
}

// A plain decimal of up to 15 significant digits with no exponent, no leading
// zero before the point and no trailing zeros (`.25`, `33.3333`, `.00001`): the
// text r(centiles) and the header's level carry.
std::string centilePlainText(double v) {
  if (v == 0.0) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.14e", std::fabs(v));
  const std::string s(buf);
  const size_t e = s.find('e');
  const int exp10 = std::atoi(s.c_str() + e + 1);
  std::string digits = s.substr(0, 1) + s.substr(2, e - 2);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const long point = static_cast<long>(exp10) + 1;  // digits before the point
  std::string out;
  if (point <= 0) {
    out = "." + std::string(static_cast<size_t>(-point), '0') + digits;
  } else if (static_cast<size_t>(point) >= digits.size()) {
    out = digits + std::string(static_cast<size_t>(point) - digits.size(), '0');
  } else {
    out = digits.substr(0, static_cast<size_t>(point)) + "." +
          digits.substr(static_cast<size_t>(point));
  }
  return v < 0 ? "-" + out : out;
}

// The centile() numlist: numbers separated by blanks or commas, and the range
// forms a(s)b, a[s]b, a/b and `x y to z` (step y - x). Each element must lie in
// [0, 100]. Refusals carry Stata's rc: 121 malformed, 127 missing, 125 range.
std::optional<ojson> parseCentileList(const std::string& text, std::vector<double>* out) {
  std::string spaced = text;
  for (char& ch : spaced)
    if (ch == ',') ch = ' ';
  std::vector<std::string> toks;
  {
    std::istringstream iss(spaced);
    std::string t;
    while (iss >> t) toks.push_back(t);
  }
  const auto invalid = [] {
    return aerErrorReason("centile() invalid -- invalid numlist", 121);
  };
  std::vector<double> vals;
  bool sawMissing = false;
  int plainRun = 0;  // single numbers seen since the last range
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& t = toks[i];
    if (t == "to" || t == ":") {
      double end = 0.0;
      if (plainRun < 2 || i + 1 >= toks.size() || !centileNumber(toks[i + 1], &end))
        return invalid();
      const double from = vals[vals.size() - 1];
      const double step = from - vals[vals.size() - 2];
      if (step == 0.0) return invalid();
      char spec[128];
      std::snprintf(spec, sizeof(spec), "%.17g(%.17g)%s", from, step, toks[i + 1].c_str());
      std::vector<std::string> els;
      if (!loops::expandNumlist(spec, &els) || els.empty()) return invalid();
      for (size_t k = 1; k < els.size(); ++k) vals.push_back(std::strtod(els[k].c_str(), nullptr));
      ++i;
      plainRun = 0;
      continue;
    }
    if (t.find_first_of("([/") != std::string::npos) {
      std::string r = t;
      for (char& ch : r) {
        if (ch == '[') ch = '(';
        else if (ch == ']') ch = ')';
      }
      std::vector<std::string> els;
      if (!loops::expandNumlist(r, &els) || els.empty()) return invalid();
      for (const auto& el : els) vals.push_back(std::strtod(el.c_str(), nullptr));
      plainRun = 0;
      continue;
    }
    if (centileMissingLiteral(t)) {
      sawMissing = true;
      vals.push_back(std::numeric_limits<double>::quiet_NaN());
      plainRun = 0;
      continue;
    }
    double v = 0.0;
    if (!centileNumber(t, &v)) return invalid();
    vals.push_back(v);
    ++plainRun;
  }
  if (sawMissing)
    return aerErrorReason("centile() invalid -- invalid numlist has missing values", 127);
  for (double v : vals) {
    if (!(v >= 0.0 && v <= 100.0))
      return aerErrorReason(
          "centile() invalid -- invalid numlist has elements outside of allowed range", 125);
  }
  *out = std::move(vals);
  return std::nullopt;
}

// Option spellings, shortest first: Centile() CCi Normal Meansd Level() and the
// default's own name NOCCI. A bare `c` is centile(); cci needs `cc`.
std::string centileOptionName(const std::string& key) {
  const auto abbrevOf = [&key](const char* full, size_t minLen) {
    const std::string f(full);
    return key.size() >= minLen && key.size() <= f.size() && f.compare(0, key.size(), key) == 0;
  };
  if (abbrevOf("centile", 1)) return "centile";
  if (abbrevOf("cci", 2)) return "cci";
  if (key == "nocci") return "nocci";
  if (abbrevOf("normal", 1)) return "normal";
  if (abbrevOf("meansd", 1)) return "meansd";
  if (abbrevOf("level", 1)) return "level";
  return std::string();
}

std::optional<ojson> parseCentileOptions(const eval::ParsedCommand& parsed,
                                         const SessionState& session, CentileRequest* req) {
  std::vector<std::pair<std::string, eval::OptValue>> canon;
  canon.reserve(parsed.opts.size());
  for (const auto& [key, val] : parsed.opts) {
    const std::string name = centileOptionName(key);
    eval::OptValue v = val;
    if (name.empty()) v.exactOnly = true;
    canon.emplace_back(name.empty() ? key : name, v);
  }
  static const std::unordered_set<std::string> kKnown = {"centile", "cci",   "nocci",
                                                         "normal",  "meansd", "level"};
  if (auto e = eval::rejectUnknownOptions(canon, kKnown)) return ojson(*e);

  bool wantExact = false, wantNormal = false, wantMeanSd = false;
  std::optional<std::string> centileText, levelText;
  for (size_t i = 0; i < canon.size(); ++i) {
    const std::string& name = canon[i].first;
    const eval::OptValue& v = canon[i].second;
    const std::string& typed = parsed.opts[i].first;
    const bool valued = name == "centile" || name == "level";
    if (valued && v.isBool)
      return aerErrorReason("option " + typed + "() incorrectly specified", 198);
    if (!valued && !v.isBool)
      return aerErrorReason("option " + typed + "() incorrectly specified", 198);
    if (name == "centile") {
      if (centileText) return aerErrorReason("option " + typed + "() not allowed", 198);
      centileText = v.strValue;
    } else if (name == "level") {
      if (levelText) return aerErrorReason("option " + typed + "() not allowed", 198);
      levelText = v.strValue;
    } else if (name == "cci") {
      wantExact = true;
    } else if (name == "normal") {
      wantNormal = true;
    } else if (name == "meansd") {
      wantMeanSd = true;
    }
  }
  // meansd outranks normal, which outranks cci.
  req->method = wantMeanSd   ? CentileMethod::MeanSd
                : wantNormal ? CentileMethod::Normal
                : wantExact  ? CentileMethod::Exact
                             : CentileMethod::Interpolated;

  req->percents.clear();
  if (centileText) {
    if (auto e = parseCentileList(*centileText, &req->percents)) return e;
  }
  if (req->percents.empty()) req->percents.push_back(50.0);

  req->level = static_cast<double>(session.level);
  if (levelText) {
    std::string lt = *levelText;
    while (!lt.empty() && std::isspace(static_cast<unsigned char>(lt.back()))) lt.pop_back();
    while (!lt.empty() && std::isspace(static_cast<unsigned char>(lt.front()))) lt.erase(0, 1);
    double lv = 0.0;
    if (!centileNumber(lt, &lv)) {
      return aerErrorReason(
          (lt.empty() ? std::string("nothing") : lt) + " found where number expected", 198);
    }
    if (!(lv >= 10.0 && lv <= 99.99))
      return aerErrorReason("level() must be between 10 and 99.99 inclusive", 198);
    req->level = lv;
  }
  return std::nullopt;
}

// Cumulative binomial probabilities for S ~ Binomial(n, p):
//   below(i) = Pr(S <= i)       (F_i in [R] centile; F_-1 = 0, F_n = 1)
//   above(i) = Pr(S >= i + 1)   (1 - F_i, formed without the subtraction)
// For n up to kDirect the probability masses are summed from each end, which is
// exact for the small cases whose comparisons with alpha are ties (n = 3, p = .5,
// alpha = .125); beyond it the regularized incomplete beta gives each tail.
class BinomialTails {
 public:
  BinomialTails(long long n, double p) : n_(n), p_(p) {
    if (n_ > kDirect) return;
    const double q = 1.0 - p_;
    std::vector<double> mass(static_cast<size_t>(n_) + 1);
    double coef = 1.0;  // C(n, k)
    for (long long k = 0; k <= n_; ++k) {
      if (k > 0) coef = coef * static_cast<double>(n_ - k + 1) / static_cast<double>(k);
      mass[static_cast<size_t>(k)] = coef * std::pow(p_, static_cast<double>(k)) *
                                     std::pow(q, static_cast<double>(n_ - k));
    }
    below_.assign(static_cast<size_t>(n_) + 1, 0.0);
    above_.assign(static_cast<size_t>(n_) + 1, 0.0);
    double run = 0.0;
    for (long long k = 0; k <= n_; ++k) {
      run += mass[static_cast<size_t>(k)];
      below_[static_cast<size_t>(k)] = run;
    }
    run = 0.0;
    for (long long k = n_; k >= 1; --k) {
      run += mass[static_cast<size_t>(k)];
      above_[static_cast<size_t>(k - 1)] = run;  // Pr(S >= k) = above(k - 1)
    }
  }

  double below(long long i) const {
    if (i < 0) return 0.0;
    if (i >= n_) return 1.0;
    if (n_ <= kDirect) return below_[static_cast<size_t>(i)];
    return cecon::betaitail(static_cast<double>(i + 1), static_cast<double>(n_ - i), p_);
  }
  double above(long long i) const {
    if (i < 0) return 1.0;
    if (i >= n_) return 0.0;
    if (n_ <= kDirect) return above_[static_cast<size_t>(i)];
    return cecon::betai(static_cast<double>(i + 1), static_cast<double>(n_ - i), p_);
  }

 private:
  static constexpr long long kDirect = 300;
  long long n_;
  double p_;
  std::vector<double> below_, above_;
};

// x_i of the sorted sample, 1-based, with x_0 = x_1 and x_{n+1} = x_n: a limit
// never leaves [min, max].
double orderStat(const std::vector<double>& x, long long i) {
  if (i < 1) return x.front();
  if (i > static_cast<long long>(x.size())) return x.back();
  return x[static_cast<size_t>(i - 1)];
}

// [R] centile, Methods and formulas, "Default case": R = (n+1)p, the centile
// interpolates between x_r and x_{r+1} at the fraction f = R - int(R).
double interpolatedCentile(const std::vector<double>& x, double p) {
  const double R = static_cast<double>(x.size() + 1) * p;
  const double r = std::floor(R);
  const double f = R - r;
  const long long ri = static_cast<long long>(r);
  const double lo = orderStat(x, ri), hi = orderStat(x, ri + 1);
  return lo + f * (hi - lo);
}

CentileRow centileFor(const std::vector<double>& x, double mean, double sd, double pct,
                      const CentileRequest& req) {
  CentileRow row;
  const long long n = static_cast<long long>(x.size());
  const double p = pct / 100.0;
  const double alpha = (100.0 - req.level) / 200.0;
  const double nan = std::numeric_limits<double>::quiet_NaN();

  if (req.method == CentileMethod::MeanSd) {
    // c = mean + z_p * s; se = s * sqrt(1/n + z_p^2 / (2n - 2)).
    const double zp = cecon::stata_invnormal(p);
    if (n < 2 || std::isnan(zp) || std::isnan(sd)) return row;
    const double zc = cecon::stata_invnormal(1.0 - alpha);
    const double nd = static_cast<double>(n);
    row.centile = mean + zp * sd;
    const double se = sd * std::sqrt(1.0 / nd + zp * zp / (2.0 * nd - 2.0));
    row.lower = row.centile - zc * se;
    row.upper = row.centile + zc * se;
    return row;
  }

  row.centile = interpolatedCentile(x, p);

  if (req.method == CentileMethod::Normal) {
    // Kendall and Stuart: se = sqrt(p(1-p)/n) / f(c), f the normal density with
    // the sample mean and standard deviation.
    if (n < 2 || std::isnan(sd) || !(sd > 0.0)) {
      row.lower = row.upper = nan;
      return row;
    }
    const double dens = cecon::stata_normalden(row.centile, mean, sd);
    const double se = std::sqrt(p * (1.0 - p) / static_cast<double>(n)) / dens;
    const double zc = cecon::stata_invnormal(1.0 - alpha);
    row.lower = row.centile - zc * se;
    row.upper = row.centile + zc * se;
    if (!std::isfinite(row.lower) || !std::isfinite(row.upper)) row.lower = row.upper = nan;
    return row;
  }

  // Binomial limits. t: F_t <= alpha < F_{t+1}, t in -1..n-1.
  //                  u: 1 - F_u <= alpha < 1 - F_{u-1}, u in 0..n.
  const BinomialTails tails(n, p);
  long long lo = -1, hi = n;  // below(lo) <= alpha < below(hi)
  while (hi - lo > 1) {
    const long long mid = lo + (hi - lo) / 2;
    if (tails.below(mid) <= alpha) lo = mid;
    else hi = mid;
  }
  const long long t = lo;
  long long ulo = -1, uhi = n;  // above(ulo) > alpha >= above(uhi)
  while (uhi - ulo > 1) {
    const long long mid = ulo + (uhi - ulo) / 2;
    if (tails.above(mid) <= alpha) uhi = mid;
    else ulo = mid;
  }
  const long long u = uhi;

  if (req.method == CentileMethod::Exact) {
    row.lower = orderStat(x, t + 1);
    row.upper = orderStat(x, u + 1);
  } else {
    const double Ft = tails.below(t), Ft1 = tails.below(t + 1);
    const double g = (alpha - Ft) / (Ft1 - Ft);
    const double a1 = orderStat(x, t + 1), a2 = orderStat(x, t + 2);
    row.lower = a1 + g * (a2 - a1);
    const double Tu = tails.above(u), Tu1 = tails.above(u - 1);
    const double h = (alpha - Tu) / (Tu1 - Tu);
    const double b1 = orderStat(x, u + 1), b0 = orderStat(x, u);
    row.upper = b1 - h * (b1 - b0);
  }
  row.held = (t == -1) || (u == n);
  return row;
}

// A value cell: %.7g right-aligned in `width`, `.` for a missing value.
std::string centileCell(double v, int width) {
  char buf[64];
  if (std::isnan(v)) {
    std::snprintf(buf, sizeof(buf), "%*s", width, ".");
  } else {
    std::snprintf(buf, sizeof(buf), "%*.7g", width, v);
  }
  return buf;
}

}  // namespace

ojson runCentile(const eval::ParsedCommand& parsed, SessionState& session) {
  CentileRequest req;
  if (auto e = parseCentileOptions(parsed, session, &req)) return *e;

  // The variables: the varlist as typed (wildcards, ranges, abbreviations), or
  // every variable in dataset order.
  std::vector<std::string> vars;
  if (!parsed.varlist.empty()) {
    std::string spec;
    for (const auto& v : parsed.varlist) spec += (spec.empty() ? "" : " ") + v;
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    vars = std::move(ex.tokens);
    if (!session.hasData()) {
      return aerErrorReason("variable " + (vars.empty() ? spec : vars.front()) + " not found",
                            111);
    }
    if (auto e = loops::checkKnownVarlist(vars, session)) return *e;
  } else if (session.hasData()) {
    if (const auto* card = session.residentReadCard()) {
      for (const auto& vi : *card) vars.push_back(vi.name);
    } else if (Datastore* ds = session.datastore()) {
      vars = ds->names();
    }
  }

  struct Printed {
    std::string name;
    long long n = 0;
    std::vector<CentileRow> rows;
  };
  std::vector<Printed> results;
  if (!vars.empty()) {
    ColumnScope scope(session, parsed, vars, "centile");
    if (scope.failed()) return scope.error();
    Datastore& ds = *scope.get();
    for (const auto& name : vars) {
      if (!ds.has(name)) return aerErrorReason("variable " + name + " not found", 111);
      if (ds.dtype(name) == "factor") continue;  // a string variable is skipped
      std::vector<double> col;
      std::string err;
      if (!numericColumn(ds, name, &col, &err)) return aerError("centile: " + err);
      std::vector<double> x;
      x.reserve(col.size());
      for (double v : col)
        if (std::fabs(v) < eval::kStataMissingSentinel) x.push_back(v);
      std::sort(x.begin(), x.end());
      Printed pr;
      pr.name = name;
      pr.n = static_cast<long long>(x.size());
      if (!x.empty()) {
        cecon::ExactSum sum;
        for (double v : x) sum.add(v);
        const double mean = sum.value() / static_cast<double>(x.size());
        double sd = std::numeric_limits<double>::quiet_NaN();
        if (x.size() > 1) {
          cecon::ExactSqDev dev(mean);
          for (double v : x) dev.add(v);
          sd = std::sqrt(dev.value() / static_cast<double>(x.size() - 1));
        }
        for (double pct : req.percents) pr.rows.push_back(centileFor(x, mean, sd, pct, req));
      }
      results.push_back(std::move(pr));
    }
  }

  // r(): replaced wholesale. Posting order makes `return list` read n_cent, N,
  // then ub_k lb_k c_k ... ub_1 lb_1 c_1.
  eval::replaceRClass(session.eval);
  const size_t k = req.percents.size();
  std::vector<std::string> posted = {"N", "n_cent", "centiles"};
  const auto rval = [](double v) { return std::isnan(v) ? eval::kStataMissingSentinel : v; };
  if (!results.empty()) {
    const Printed& last = results.back();
    for (size_t j = 0; j < k; ++j) {
      const std::string s = std::to_string(j + 1);
      const CentileRow row = j < last.rows.size() ? last.rows[j] : CentileRow();
      session.eval.rValues.set("c_" + s, rval(row.centile));
      session.eval.rValues.set("lb_" + s, rval(row.lower));
      session.eval.rValues.set("ub_" + s, rval(row.upper));
      posted.insert(posted.end(), {"c_" + s, "lb_" + s, "ub_" + s});
    }
  }
  session.eval.rValues.set("N", results.empty() ? 0.0 : static_cast<double>(results.back().n));
  session.eval.rValues.set("n_cent", static_cast<double>(k));
  {
    std::string list;
    for (double pct : req.percents) list += (list.empty() ? "" : " ") + centilePlainText(pct);
    session.eval.rStrings.set("centiles", list);
  }
  eval::declareRPosted(session.eval, "centile", std::move(posted));

  // The table.
  std::ostringstream out;
  out << "\n";
  switch (req.method) {
    case CentileMethod::Interpolated:
      out << std::string(58, ' ') << "Binom. interp.   \n";
      break;
    case CentileMethod::Exact:
      out << std::string(58, ' ') << "Binomial exact   \n";
      break;
    case CentileMethod::Normal:
      out << std::string(35, ' ') << "-- Normal, based on observed centiles --\n";
      break;
    case CentileMethod::MeanSd:
      out << std::string(35, ' ') << "-- Normal, based on mean and std. dev.--\n";
      break;
  }
  const std::string head = "    Variable |       Obs  Percentile    Centile";
  const std::string ci = "[" + centilePlainText(req.level) + "% conf. interval]";
  out << head << std::string(ci.size() < 28 ? 28 - ci.size() : 1, ' ') << ci << "\n";
  out << std::string(13, '-') << "+" << std::string(61, '-') << "\n";

  bool anyHeld = false, lastHeld = false;
  char buf[64];
  for (const Printed& pr : results) {
    const std::string stub = fitLeft(abbrevCp(pr.name, 12), 12);
    if (pr.n == 0) {
      std::snprintf(buf, sizeof(buf), "%8lld", pr.n);
      out << stub << " |" << buf << "\n";
      lastHeld = false;
      continue;
    }
    for (size_t j = 0; j < pr.rows.size(); ++j) {
      const CentileRow& row = pr.rows[j];
      if (j == 0) {
        std::snprintf(buf, sizeof(buf), "%10lld", pr.n);
        out << stub << " |" << buf;
      } else {
        out << std::string(12, ' ') << " |" << std::string(10, ' ');
      }
      out << " " << centileCell(req.percents[j], 10) << " " << centileCell(row.centile, 11) << " "
          << centileCell(row.lower, 15) << " " << centileCell(row.upper, 11);
      const bool held = row.held &&
                        (req.method == CentileMethod::Interpolated ||
                         req.method == CentileMethod::Exact);
      if (held) out << "*";
      out << "\n";
      anyHeld = anyHeld || held;
      lastHeld = held;
    }
  }
  if (anyHeld) {
    out << "\n"
        << (lastHeld ? "*" : "")
        << " Lower (upper) confidence limit held at minimum (maximum) of sample\n";
  }
  return displayBlock(out.str());
}

}  // namespace piet::handlers
