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


// ---------------------------------------------------------------------------
// ci means | proportions | variances ([R] ci). Methods and formulas of [R] ci:
//   means:       mean +- invttail(N-1, alpha/2) * sqrt(Var/N) (aw/fw as summarize
//                weights them); with poisson (or exposure()) the exact Poisson
//                interval for the count k = sum(x) over the exposure n:
//                lb = invpoisson(k-1, 1-alpha/2)/n (0 when k = 0),
//                ub = invpoisson(k, alpha/2)/n, se = sqrt(k)/n.
//   proportions: k successes of n (0/1 variables only): exact (Clopper-Pearson,
//                the default), wald, wilson, agresti (Agresti-Coull), jeffreys.
//   variances:   (N-1)Var/invchi2tail(N-1, alpha/2) .. (N-1)Var/invchi2(N-1,
//                alpha/2); bonett: Bonett (2006) with the trimmed-mean kurtosis;
//                sd reports the square roots.
// The inverse distribution functions are solved here by bracketing and
// bisection on the regularized incomplete gamma and beta functions.
namespace {

// Regularized incomplete gamma: series for x < a+1, Lentz continued fraction
// otherwise (each returns the half it converges for; the other is 1 - it).
double ciGammaSeriesP(double a, double x) {
  double ap = a, sum = 1.0 / a, del = sum;
  for (int n = 0; n < 100000; ++n) {
    ap += 1.0;
    del *= x / ap;
    sum += del;
    if (std::fabs(del) < std::fabs(sum) * 1e-17) break;
  }
  return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}
double ciGammaCfQ(double a, double x) {
  const double tiny = 1e-300;
  double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
  for (int i = 1; i < 100000; ++i) {
    const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < tiny) d = tiny;
    c = b + an / c;
    if (std::fabs(c) < tiny) c = tiny;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-17) break;
  }
  return std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
}
double ciGammaP(double a, double x) {
  if (x <= 0) return 0.0;
  return x < a + 1.0 ? ciGammaSeriesP(a, x) : 1.0 - ciGammaCfQ(a, x);
}
double ciGammaQ(double a, double x) {
  if (x <= 0) return 1.0;
  return x < a + 1.0 ? 1.0 - ciGammaSeriesP(a, x) : ciGammaCfQ(a, x);
}

// Solve f(x) = target for a monotone f on (lo, hi): increasing when `up`.
template <class F>
double ciSolve(F f, double target, bool up, double lo, double hi, bool geometric) {
  for (int it = 0; it < 400; ++it) {
    const double mid = geometric ? std::sqrt(lo * hi) : lo + (hi - lo) / 2;
    if (!(mid > lo && mid < hi)) break;
    const double v = f(mid);
    if ((v < target) == up) lo = mid;
    else hi = mid;
    if (geometric && hi / lo - 1.0 < 1e-3) geometric = false;
  }
  return lo + (hi - lo) / 2;
}
// x with P(a, x) = p (lower) or Q(a, x) = p (upper).
double ciInvGamma(double a, double p, bool upper) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  if (!(a > 0) || !(p > 0 && p < 1)) return nan;
  double hi = std::max(1.0, a);
  while ((upper ? ciGammaQ(a, hi) > p : ciGammaP(a, hi) < p) && hi < 1e300) hi *= 2;
  if (upper) return ciSolve([&](double x) { return ciGammaQ(a, x); }, p, false, 1e-300, hi, true);
  return ciSolve([&](double x) { return ciGammaP(a, x); }, p, true, 1e-300, hi, true);
}
// x with I_x(a, b) = p.
double ciInvIbeta(double a, double b, double p) {
  if (!(p > 0 && p < 1)) return std::numeric_limits<double>::quiet_NaN();
  if (p <= 0.5)
    return ciSolve([&](double x) { return cecon::betai(a, b, x); }, p, true, 0.0, 1.0, false);
  return ciSolve([&](double x) { return cecon::betaitail(a, b, x); }, 1.0 - p, false, 0.0, 1.0,
                 false);
}

// Stata's display of a number in format `fmt`, missing as `.` right-aligned.
std::string ciFmt(double v, const std::string& fmt) {
  if (std::isnan(v)) {
    const size_t w = static_cast<size_t>(std::atoi(fmt.c_str() + 1));
    return std::string(w > 1 ? w - 1 : 0, ' ') + ".";
  }
  std::string t = eval::applyStataFmt(v, fmt);
  const bool left = fmt.size() > 1 && fmt[1] == '-';
  const size_t w = static_cast<size_t>(std::atoi(fmt.c_str() + (left ? 2 : 1)));
  if (t.size() < w) t = left ? t + std::string(w - t.size(), ' ') : std::string(w - t.size(), ' ') + t;
  return t;
}
void ciCol(std::string* line, size_t col) {  // _col(col): pad to 1-based column
  if (line->size() < col - 1) line->append(col - 1 - line->size(), ' ');
}
std::string ciOutFmt(const std::string& fmt) {
  if (fmt.size() >= 2 && fmt.back() == 'f') return "%9." + fmt.substr(fmt.size() - 2);
  if (fmt.size() >= 3 && fmt.compare(fmt.size() - 2, 2, "fc") == 0)
    return "%9." + fmt.substr(fmt.size() - 3);
  return "%9.0g";
}

}  // namespace

ojson runCi(const eval::ParsedCommand& parsed, SessionState& session) {
  auto syn = [](const std::string& m, int rc = 198) {
    return ojson(eval::errorBlock("syntax_error", m, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  std::vector<std::string> words = parsed.varlist;
  const std::string param = words.empty() ? std::string() : strutil::lower(words.front());
  auto isParam = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return param.size() <= f.size() && param == f.substr(0, std::max(minLen, param.size()));
  };
  std::string kind;
  if (!param.empty() && isParam("variances", 3)) kind = "variances";
  else if (!param.empty() && isParam("proportions", 4)) kind = "proportions";
  else if (!param.empty() && isParam("means", 4)) kind = "means";
  if (kind.empty()) {
    if (param.empty()) return syn("you must specify one of means, proportions, or variances");
    return syn("you must specify one of means, proportions, or variances following ci");
  }
  words.erase(words.begin());
  if (!words.empty()) {
    char* end = nullptr;
    std::strtod(words.front().c_str(), &end);
    if (end != nullptr && *end == '\0' && !words.front().empty())
      return syn("number found where a variable expected; perhaps you meant to use cii");
  }
  // Options.
  static const std::unordered_set<std::string> kMeans = {"level", "poisson", "exposure",
                                                         "separator", "normal", "total"};
  static const std::unordered_set<std::string> kProps = {
      "level", "exact", "wald", "wilson", "agresti", "jeffreys", "separator", "total",
      "poisson", "exposure", "binomial", "by"};
  static const std::unordered_set<std::string> kVars = {"level", "sd", "bonett", "separator",
                                                        "normal", "total"};
  const auto& known = kind == "means" ? kMeans : kind == "proportions" ? kProps : kVars;
  if (auto e = eval::rejectUnknownOptions(parsed.opts, known, {})) return ojson(*e);
  double level = session.level;
  int separator = 5;
  bool poisson = false, normalOpt = false, sdOpt = false, bonett = false, total = false;
  std::string exposure, ptype;
  for (const auto& [k, v] : parsed.opts) {
    const std::string val = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "level") {
      double lv = std::numeric_limits<double>::quiet_NaN();
      size_t pos = 0;
      try {
        lv = std::stod(val, &pos);
      } catch (...) {
        pos = 0;
      }
      if (val.empty() || pos != val.size()) return syn("option level() incorrectly specified");
      if (!(lv >= 10.0 && lv <= 99.99))
        return syn("level() must be between 10 and 99.99 inclusive");
      if (std::fabs(lv * 100.0 - std::round(lv * 100.0)) > 1e-8)
        return syn("level() can have at most two digits after the decimal point");
      level = lv;
    } else if (k == "separator") {
      char* end = nullptr;
      const long s = std::strtol(val.c_str(), &end, 10);
      if (val.empty() || end == nullptr || *end != '\0') return syn("option separator() incorrectly specified");
      if (s < 0) return syn("option separator() must be >= 0");
      separator = static_cast<int>(s);
    } else if (k == "poisson") {
      poisson = true;
    } else if (k == "normal") {
      normalOpt = true;
    } else if (k == "exposure") {
      exposure = val;
    } else if (k == "sd") {
      sdOpt = true;
    } else if (k == "bonett") {
      bonett = true;
    } else if (k == "total") {
      total = true;
    } else if (k == "exact" || k == "wald" || k == "wilson" || k == "agresti" || k == "jeffreys") {
      if (!ptype.empty() && ptype != k)
        return syn("only one of " + ptype + " or " + k + " is allowed");
      ptype = k;
    } else if (kind == "proportions" && (k == "poisson" || k == "binomial" || k == "exposure")) {
      return syn("option " + k + " is not allowed with ci proportions");
    } else if (k == "by") {
      return syn("option by() is not allowed");
    }
  }
  if (kind == "proportions") {
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      if (k == "poisson" || k == "binomial" || k == "exposure")
        return syn("option " + k + (k == "exposure" ? "()" : "") +
                   " is not allowed with ci proportions");
    }
  }
  if (total) return syn("option total may only be specified with prefix by");
  if (kind == "means" && poisson && normalOpt) return syn("only one of normal or poisson is allowed");
  if (kind == "variances" && bonett && normalOpt) return syn("only one of normal or bonett is allowed");
  if (!exposure.empty()) poisson = true;
  if (ptype.empty()) ptype = "exact";
  // Weights: means aw/fw, proportions and variances fw.
  std::string wvar;
  bool fweight = false;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && parsed.weightType.has_value()) {
    const std::string t = strutil::lower(*parsed.weightType);
    if (t == "fw" || t == "fweight") fweight = true;
    else if (!((t == "aw" || t == "aweight") && kind == "means"))
      return syn("weights not allowed", 101);
    wvar = *parsed.weightVar;
    if (!fweight && poisson) return syn("aweights not allowed", 101);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  std::vector<std::string> vars;
  {
    std::string spec;
    for (const auto& v : words) spec += (spec.empty() ? "" : " ") + v;
    if (spec.empty()) {
      for (const auto& nm : session.datastore()->names()) vars.push_back(nm);
    } else {
      loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
      if (ex.isError) return ex.error;
      vars = ex.tokens;
      if (auto err = loops::checkKnownVarlist(vars, session)) return *err;
    }
  }
  std::vector<std::string> cols = vars;
  if (!wvar.empty()) cols.push_back(wvar);
  if (!exposure.empty()) cols.push_back(exposure);
  ColumnScope scope(session, parsed, cols, "ci");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();
  const size_t nrow = static_cast<size_t>(ds.nRows());
  // The touse sample: rows with a usable weight and exposure (not the variables).
  std::vector<double> w(nrow, 1.0), ex;
  std::vector<char> use(nrow, 1);
  std::string err;
  if (!wvar.empty()) {
    if (!numericColumn(ds, wvar, &w, &err)) return aerError("ci: " + err);
    for (size_t i = 0; i < nrow; ++i) {
      if (std::isnan(w[i])) continue;
      if (w[i] < 0) return syn("negative weights encountered", 402);
      if (fweight && w[i] != std::floor(w[i]))
        return syn("may not use noninteger frequency weights", 401);
    }
    for (size_t i = 0; i < nrow; ++i)
      if (std::isnan(w[i]) || w[i] == 0) use[i] = 0;
  }
  if (!exposure.empty()) {
    if (!numericColumn(ds, exposure, &ex, &err)) return aerError("ci: " + err);
    for (size_t i = 0; i < nrow; ++i)
      if (std::isnan(ex[i])) use[i] = 0;
    for (size_t i = 0; i < nrow; ++i)
      if (use[i] && !(ex[i] > 0)) return syn("variable " + exposure + " <= 0", 402);
  }
  std::map<std::string, std::string> fmts;
  for (const auto& [n, f] : session.displayFormatMap(ds.formatCard(vars))) fmts[n] = f;

  struct Res {
    double N = 0, mean = 0, se = 0, lb = 0, ub = 0, var = 0, sd = 0, kurt = 0, varSe = 0;
    std::string mark;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double alphHalf = (100.0 - level) / 200.0;
  std::ostringstream out;
  std::string levelStr;
  {
    levelStr = eval::applyStataFmt(level, "%9.0g");
    levelStr = strutil::trim(levelStr);
  }
  const std::string spaces = levelStr.size() == 2 ? "   " : levelStr.size() == 4 ? " " : "";
  std::string warnl, warnh;
  int marked = 0;
  std::vector<std::string> rows;
  bool any = false;
  Res last;
  bool haveLast = false;

  if (kind == "proportions") {
    // Variables with at most two levels, all of them 0/1 (ci proportions).
    std::vector<std::string> bin;
    size_t no01 = 0;
    for (const auto& v : vars) {
      if (ds.dtype(v) == "factor") continue;
      std::vector<double> x;
      if (!numericColumn(ds, v, &x, &err)) return aerError("ci: " + err);
      std::set<double> lv;
      bool ok01 = true;
      for (size_t i = 0; i < nrow; ++i) {
        if (std::isnan(x[i])) continue;
        lv.insert(x[i]);
        if (x[i] != 0 && x[i] != 1) ok01 = false;
      }
      if (lv.size() > 2) continue;
      bin.push_back(v);
      if (!ok01) ++no01;
    }
    if (bin.empty() || no01 == bin.size()) {
      eval::replaceRClass(session.eval);
      return displayBlock("no binary (0/1) variables found; nothing to compute");
    }
  }

  out << "\n";
  if (kind == "variances") {
    if (bonett) out << std::string(46, ' ') << "       Bonett       \n";
    std::string h = "    Variable |";
    ciCol(&h, 23);
    h += "Obs";
    ciCol(&h, 32);
    h += sdOpt ? "Std. dev." : "Variance";
    ciCol(&h, 44);
    h += spaces + "[" + levelStr + "% conf. interval]";
    out << h << "\n" << std::string(13, '-') << "+" << std::string(52, '-') << "\n";
  } else {
    if (kind == "proportions") {
      static const std::map<std::string, std::string> kTitle = {
          {"exact", "   Binomial exact   "}, {"wald", "   Binomial Wald    "},
          {"wilson", "       Wilson       "}, {"agresti", "   Agresti\xe2\x80\x93" "Coull    "},
          {"jeffreys", "      Jeffreys      "}};
      out << std::string(57, ' ') << kTitle.at(ptype) << "\n";
    } else if (poisson) {
      out << std::string(57, ' ') << "   Poisson exact    \n";
    }
    const std::string tl1 = (kind == "means" && poisson) ? "Exposure" : "     Obs";
    const std::string spc = kind == "proportions" ? "  " : "    ";
    const std::string ttl = kind == "proportions" ? "Proportion" : "    Mean";
    out << "    Variable |   " << tl1 << spc << ttl << "    Std. err.    " << spaces << "["
        << levelStr << "% conf. interval]\n"
        << std::string(13, '-') << "+" << std::string(63, '-') << "\n";
  }
  size_t nlines = 0;
  const double kap = cecon::stata_invnormal(1.0 - alphHalf);
  for (const auto& v : vars) {
    if (ds.dtype(v) == "factor") continue;
    std::vector<double> x;
    if (!numericColumn(ds, v, &x, &err)) return aerError("ci: " + err);
    // The rows of this variable: touse and nonmissing.
    double sw = 0, swx = 0;
    size_t nobs = 0;
    bool ok = true;
    for (size_t i = 0; i < nrow; ++i) {
      if (!use[i] || std::isnan(x[i])) continue;
      if (kind == "proportions" && x[i] != 0 && x[i] != 1) ok = false;
      if (kind == "means" && poisson && x[i] < 0) ok = false;
      ++nobs;
      sw += w[i];
      swx += w[i] * x[i];
    }
    if (!ok) continue;
    Res r;
    const double meanW = nobs ? swx / sw : nan;
    // summarize's N: the sum of the frequency weights, else the row count. The
    // variance exists once N > 1, so one row of fweight 5 has Var 0 (summarize
    // mpg in 1 [fw=w], w = 5: r(N) 5, r(Var) 0) and ci's se, lb and ub follow.
    const double Nsum = fweight ? sw : static_cast<double>(nobs);
    double varW = nan;
    if (Nsum > 1) {
      double ss = 0;
      for (size_t i = 0; i < nrow; ++i)
        if (use[i] && !std::isnan(x[i])) ss += w[i] * (x[i] - meanW) * (x[i] - meanW);
      // fw: divide by sum(w)-1; aw: weights normalised to the row count.
      varW = fweight ? ss / (sw - 1) : ss * static_cast<double>(nobs) / sw /
                                           (static_cast<double>(nobs) - 1);
    }
    if (kind == "means" && !poisson) {
      r.N = Nsum;
      r.mean = meanW;
      r.se = std::sqrt(varW / Nsum);
      const double t = Nsum > 1 ? cecon::stata_invttail(Nsum - 1, alphHalf) : nan;
      r.lb = r.mean - t * r.se;
      r.ub = r.mean + t * r.se;
      if (nobs == 0) r.mean = r.se = r.lb = r.ub = nan;
    } else if (kind == "means") {
      const double n0 = Nsum;
      const double k = std::floor(meanW * n0 + 0.5);
      double n = n0;
      if (!exposure.empty()) {
        double se = 0;
        for (size_t i = 0; i < nrow; ++i)
          if (use[i] && !std::isnan(x[i])) se += w[i] * ex[i];
        n = se;
      }
      if (nobs == 0) {
        r.N = 0;
        r.mean = r.se = r.lb = r.ub = nan;
      } else {
        r.lb = k == 0 ? 0.0 : ciInvGamma(k, 1.0 - alphHalf, true) / n;
        r.ub = ciInvGamma(k + 1, alphHalf, true) / n;
        r.N = n;
        r.mean = k / n;
        r.se = std::sqrt(k) / n;
        if (k == 0) {
          r.mark = "*";
          marked = 1;
        }
      }
    } else if (kind == "proportions") {
      const double n = Nsum;
      const double k = std::floor(meanW * n + 0.5);
      r.N = n;
      r.mean = k / n;
      r.se = std::sqrt(r.mean * (1 - r.mean) / n);
      if (ptype == "exact") {
        r.lb = k == 0 ? 0.0 : ciInvIbeta(k, n - k + 1, alphHalf);
        r.ub = k == n ? 1.0 : ciInvIbeta(k + 1, n - k, 1.0 - alphHalf);
        if (k == 0 || k == n) {
          r.mark = "*";
          marked = 1;
        }
      } else {
        auto clip = [&](const std::string& name) {
          if (r.lb <= 0 && ptype != "jeffreys") {
            r.lb = 0;
            warnl = "(*) The " + name + " interval was clipped at the lower endpoint";
            r.mark = "*";
            marked = 2;
          }
          if (r.ub >= 1 && !std::isnan(r.ub) && ptype != "jeffreys") {
            r.ub = 1;
            warnh = "(**) The " + name + " interval was clipped at the upper endpoint";
            r.mark = r.mark.empty() ? "**" : "* **";
            marked = 2;
          }
        };
        if (ptype == "wald") {
          r.lb = std::max(0.0, r.mean - kap * r.se);
          r.ub = std::min(1.0, r.mean + kap * r.se);
          if (r.lb == 0) {
            warnl = "(*) The Wald interval was clipped at the lower endpoint";
            r.mark = "*";
            marked = 2;
          }
          if (r.ub == 1) {
            warnh = "(**) The Wald interval was clipped at the upper endpoint";
            r.mark = r.mark.empty() ? "**" : "* **";
            marked = 2;
          }
        } else if (ptype == "wilson") {
          const double c = (k + kap * kap / 2) / (n + kap * kap);
          const double h = (kap * std::sqrt(n) / (n + kap * kap)) *
                           std::sqrt(r.mean * (1 - r.mean) + kap * kap / (4 * n));
          r.lb = c - h;
          r.ub = c + h;
          if (r.lb < 0) {
            r.lb = 0;
            warnl = "(*) The Wilson interval was clipped at the lower endpoint";
            r.mark = "*";
            marked = 2;
          }
          if (r.ub > 1) {
            r.ub = 1;
            warnh = "(**) The Wilson interval was clipped at the upper endpoint";
            r.mark = r.mark.empty() ? "**" : "* **";
            marked = 2;
          }
        } else if (ptype == "agresti") {
          const double xt = k + kap * kap / 2, nt = n + kap * kap, pt = xt / nt, qt = 1 - pt;
          r.lb = pt - kap * std::sqrt(pt * qt) / std::sqrt(nt);
          r.ub = pt + kap * std::sqrt(pt * qt) / std::sqrt(nt);
          if (r.lb < 0) {
            r.lb = 0;
            warnl = "(*) The Agresti\xe2\x80\x93" "Coull interval was clipped at the lower endpoint";
            r.mark = "*";
            marked = 2;
          }
          if (r.ub > 1) {
            r.ub = 1;
            warnh = "(**) The Agresti\xe2\x80\x93" "Coull interval was clipped at the upper endpoint";
            r.mark = r.mark.empty() ? "**" : "* **";
            marked = 2;
          }
        } else {  // jeffreys
          r.lb = k > 0 ? ciInvIbeta(k + 0.5, n - k + 0.5, alphHalf) : 0.0;
          r.ub = k < n ? ciInvIbeta(k + 0.5, n - k + 0.5, 1.0 - alphHalf) : 1.0;
        }
        (void)clip;
      }
    } else {  // variances
      const double N = Nsum;
      r.N = N;
      r.var = varW;
      r.varSe = nan;
      const double alpha = (100.0 - level) / 100.0;
      if (bonett) {
        // Bonett (2006): the kurtosis about the trimmed mean (trim 1/(2 sqrt(n-4))
        // from each end), fw expanded to one row per unit.
        std::vector<double> xs;
        for (size_t i = 0; i < nrow; ++i)
          if (use[i] && !std::isnan(x[i]))
            for (double c = 0; c < (fweight ? w[i] : 1.0); c += 1.0) xs.push_back(x[i]);
        const double n = static_cast<double>(xs.size());
        // The trim 1/(2 sqrt(n-4)) is missing for n <= 4, and so is the range
        // lo..hi of the trimmed mean's subscript: Stata's kurtosis step stops
        // there with Mata's 3301 "subscript invalid".
        if (!(n > 4))
          return ojson(eval::errorBlock("engine_error", "kurt():  3301  subscript invalid",
                                        std::nullopt, std::nullopt, std::nullopt, "r(3301)"));
        double xbar = 0;
        for (double d : xs) xbar += d;
        xbar /= n;
        const double trim = 1.0 / (2.0 * std::sqrt(n - 4));
        const size_t lo = static_cast<size_t>(std::floor(n * trim)) + 1;
        const size_t hi = static_cast<size_t>(n) + 1 - lo;
        std::sort(xs.begin(), xs.end());
        double m = 0;
        for (size_t i = lo; i <= hi; ++i) m += xs[i - 1];
        m /= static_cast<double>(hi - lo + 1);
        double num = 0, den = 0;
        for (double d : xs) {
          num += std::pow(d - m, 4);
          den += (d - xbar) * (d - xbar);
        }
        const double g4 = n * num / (den * den);
        const double z = cecon::stata_invnormal(1 - alpha / 2);
        const double c = n / (n - z);
        const double se = c * std::sqrt((g4 - (n - 3) / n) / (n - 1));
        r.N = n;
        r.lb = std::exp(std::log(c * varW) - z * se);
        r.ub = std::exp(std::log(c * varW) + z * se);
        r.kurt = g4;
        r.varSe = se;
      } else {
        r.lb = (N - 1) * varW / (2 * ciInvGamma((N - 1) / 2, alpha / 2, true));
        r.ub = (N - 1) * varW / (2 * ciInvGamma((N - 1) / 2, alpha / 2, false));
      }
      if (sdOpt) {
        r.sd = std::sqrt(varW);
        r.lb = std::sqrt(r.lb);
        r.ub = std::sqrt(r.ub);
      }
    }
    // The row.
    if (separator > 0 && nlines % static_cast<size_t>(separator) == 0 && nlines != 0) {
      out << std::string(13, '-') << "+" << std::string(kind == "variances" ? 52 : 63, '-')
          << "\n";
    }
    ++nlines;
    const std::string ofmt = ciOutFmt(fmts.count(v) ? fmts[v] : std::string("%9.0g"));
    std::string line = fitLeft(abbrevCp(v, 12), 12) + " |";
    if (kind == "variances") {
      ciCol(&line, 16);
      line += ciFmt(r.N, "%10.0fc");
      ciCol(&line, 31);
      line += ciFmt(sdOpt ? r.sd : r.var, ofmt);
      ciCol(&line, 46);
      line += ciFmt(r.lb, ofmt);
      ciCol(&line, 58);
      line += ciFmt(r.ub, ofmt);
    } else {
      const bool pois = kind == "means" && poisson;
      ciCol(&line, pois ? 17 : 16);
      line += ciFmt(r.N, pois ? "%9.0g" : "%10.0fc");
      ciCol(&line, 29);
      line += ciFmt(r.mean, ofmt);
      ciCol(&line, 41);
      line += ciFmt(r.se, ofmt);
      ciCol(&line, 57);
      line += ciFmt(r.lb, ofmt);
      ciCol(&line, 69);
      line += ciFmt(r.ub, ofmt) + r.mark;
    }
    out << line << "\n";
    last = r;
    haveLast = true;
    any = true;
  }
  if (marked == 1) {
    out << "\n(*) one-sided, " << strutil::trim(eval::applyStataFmt(100 - (100 - level) / 2, "%9.0g"))
        << "% confidence interval\n";
  }
  if (marked == 2 && (!warnl.empty() || !warnh.empty())) {
    out << "\n";
    if (!warnl.empty()) out << warnl << "\n";
    if (!warnh.empty()) out << warnh << "\n";
  }
  (void)any;
  // r(): the last variable's results, in ci's posting order.
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  auto setS = [&](const std::string& k, double v) {
    session.eval.rValues.set(k, v);
    posted.push_back(k);
  };
  if (haveLast) {
    setS("level", level);
    setS("ub", last.ub);
    setS("lb", last.lb);
    if (kind == "variances") {
      if (bonett) setS("kurtosis", last.kurt);
      if (sdOpt) setS("sd", last.sd);
      setS("Var", last.var);
      setS("N", last.N);
      setS("Var_se", last.varSe);
      session.eval.markRHidden("Var_se");
      session.eval.rStrings.set("citype", bonett ? "bonett" : "normal");
    } else {
      setS("se", last.se);
      if (kind == "proportions") {
        setS("proportion", last.mean);
        setS("N", last.N);
        setS("mean", last.mean);
        session.eval.markRHidden("mean");
        session.eval.rStrings.set("citype", ptype);
      } else {
        setS("mean", last.mean);
        setS("N", last.N);
        if (!exposure.empty()) {
          session.eval.rStrings.set("exposure", exposure);
          posted.push_back("exposure");
        }
        session.eval.rStrings.set("citype", poisson ? "poisson" : "normal");
      }
    }
    posted.push_back("citype");
  }
  eval::declareRPosted(session.eval, "ci", std::move(posted));
  std::string text = out.str();
  while (!text.empty() && text.back() == '\n') text.pop_back();
  return displayBlock(text);
}

}  // namespace piet::handlers

namespace piet::handlers {

namespace {

// _ttest center2: three texts centred about columns 11, 40 and 70 (the third
// about 79 - len/2 when it is longer than 20), each at least one blank after
// the one before.
std::string prCenter2(const std::string& a, const std::string& b, const std::string& c) {
  std::string out;
  size_t last = 0;
  auto put = [&](const std::string& s, int center) {
    const int len = static_cast<int>(s.size());
    const int skip = std::max(last != 0 ? 1 : 0, center - len / 2 - 1 - static_cast<int>(last));
    out += std::string(static_cast<size_t>(std::max(0, skip)), ' ') + s;
    last += static_cast<size_t>(std::max(0, skip)) + s.size();
  };
  put(a, 11);
  put(b, 40);
  put(c, c.size() > 20 ? 79 - static_cast<int>(c.size()) / 2 : 70);
  return out;
}

// %8.6f with the trailing zeros dropped (prtest's H0 value), %8.0g below 1e-6.
std::string prH0Text(double p0) {
  if (p0 < 1e-6) return strutil::trim(eval::applyStataFmt(p0, "%8.0g"));
  if (p0 > 1 - 1e-6) return "0.999999";
  std::string m = strutil::trim(eval::applyStataFmt(p0, "%8.6f"));
  while (m.size() > 1 && m.back() == '0' && m.find('.') != std::string::npos) m.pop_back();
  return m;
}

struct PrSample {
  double n = 0, p = 0;
};

// The printed tables and r() of prtest / prtesti (prtesti.ado's two forms).
ojson prReport(SessionState& session, bool two, const PrSample& a, const PrSample& b, double p0,
               double level, const std::string& xname, const std::string& yname,
               const std::string& col1, bool oneSampleVarForm) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double q = cecon::stata_invnormal((100 + level) / 200);
  std::ostringstream out;
  auto line78 = [](char mid) {
    return std::string(13, '-') + mid + std::string(64, '-');
  };
  auto row = [&](const std::string& name, double mean, double se) {
    std::string l = fitLeft(abbrevCp(name, 12), 12) + " |";
    ciCol(&l, 17);
    l += ciFmt(mean, "%9.0g");
    ciCol(&l, 28);
    l += ciFmt(se, "%9.0g");
    ciCol(&l, 58);
    l += ciFmt(mean - q * se, "%9.0g");
    ciCol(&l, 70);
    l += ciFmt(mean + q * se, "%9.0g");
    return l;
  };
  const std::string levelStr = strutil::trim(eval::applyStataFmt(level, "%9.0g"));
  auto header = [&](bool showZ) {
    std::string h = "    " + col1;
    ciCol(&h, 14);
    h += "|";
    ciCol(&h, 22);
    h += "Mean";
    ciCol(&h, 29);
    h += "Std. err.";
    if (showZ) {
      ciCol(&h, 44);
      h += "z";
      ciCol(&h, 49);
      h += "P>|z|";
    }
    ciCol(&h, static_cast<size_t>(61 - static_cast<int>(levelStr.size())));
    h += "[" + levelStr + "% conf. interval]";
    return line78('-') + "\n" + h + "\n" + line78('+') + "\n";
  };
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  auto setS = [&](const std::string& k, double v) {
    session.eval.rValues.set(k, v);
    posted.push_back(k);
  };
  auto pvals = [](double z, double* pl, double* p, double* pr) {
    *p = 2 * cecon::stata_normal(-std::fabs(z));
    if (z < 0) {
      *pl = *p / 2;
      *pr = 1 - *pl;
    } else {
      *pr = *p / 2;
      *pl = 1 - *pr;
    }
  };
  auto f64 = [](double v) { return strutil::trim(eval::applyStataFmt(v, "%6.4f")); };
  if (!two) {
    const double s = std::sqrt(a.p * (1 - a.p) / a.n);
    const double se = (a.n == 1) ? nan : s;
    const double z = (a.p - p0) / std::sqrt(p0 * (1 - p0) / a.n);
    double pl, p, pr;
    pvals(z, &pl, &p, &pr);
    out << "\n";
    if (oneSampleVarForm) {
      std::string t = "One-sample test of proportion";
      ciCol(&t, 49);
      t += "Number of obs      = " + ciFmt(a.n, "%9.0g");
      out << t << "\n\n";
    } else {
      std::string t = "One-sample test of proportion";
      ciCol(&t, static_cast<size_t>(53 - static_cast<int>(xname.size())));
      t += xname;
      ciCol(&t, 53);
      t += ": Number of obs = " + ciFmt(a.n, "%8.0g");
      out << t << "\n";
    }
    out << header(false) << row(xname, a.p, se) << "\n" << line78('-') << "\n";
    std::string zl = "    p = proportion(" + abbrevCp(xname, 12) + ")";
    ciCol(&zl, 67);
    zl += "z = " + ciFmt(z, "%8.4f");
    const std::string m0 = prH0Text(p0);
    out << zl << "\nH0: p = " << m0 << "\n\n";
    out << prCenter2("Ha: p < " + m0, "Ha: p != " + m0, "Ha: p > " + m0) << "\n";
    out << prCenter2("Pr(Z < z) = " + f64(pl), "Pr(|Z| > |z|) = " + f64(p),
                     "Pr(Z > z) = " + f64(pr));
    setS("level", level);
    setS("p_u", pr);
    setS("p", p);
    setS("p_l", pl);
    setS("z", z);
    setS("lb", a.p - q * s);
    setS("ub", a.p + q * s);
    setS("se", s);
    setS("P", a.p);
    setS("N", a.n);
    setS("N_1", a.n);
    setS("P_1", a.p);
    session.eval.markRHidden("N_1");
    session.eval.markRHidden("P_1");
  } else {
    const double n = a.n + b.n;
    const double pp = (a.n * a.p + b.n * b.p) / n;
    const double s1 = std::sqrt(a.p * (1 - a.p) / a.n), s2 = std::sqrt(b.p * (1 - b.p) / b.n);
    const double sp = std::sqrt(a.p * (1 - a.p) / a.n + b.p * (1 - b.p) / b.n);
    const double spp = std::sqrt(pp * (1 - pp) / a.n + pp * (1 - pp) / b.n);
    const double dp = a.p - b.p;
    const double z = dp / spp;
    double pl, p, pr;
    pvals(z, &pl, &p, &pr);
    out << "\n";
    {
      std::string t = "Two-sample test of proportions";
      ciCol(&t, static_cast<size_t>(53 - static_cast<int>(xname.size())));
      t += xname;
      ciCol(&t, 53);
      t += ": Number of obs = " + ciFmt(a.n, "%8.0g");
      std::string t2;
      ciCol(&t2, static_cast<size_t>(53 - static_cast<int>(yname.size())));
      t2 += yname;
      ciCol(&t2, 53);
      t2 += ": Number of obs = " + ciFmt(b.n, "%8.0g");
      out << t << "\n" << t2 << "\n";
    }
    out << header(true) << row(xname, a.p, a.n == 1 ? nan : s1) << "\n"
        << row(yname, b.p, b.n == 1 ? nan : s2) << "\n" << line78('+') << "\n";
    {
      const double tval = dp / spp;
      const double pval = 2 * (1 - cecon::stata_normal(std::fabs(tval)));
      std::string l = fitLeft("diff", 12) + " |";
      ciCol(&l, 17);
      l += ciFmt(dp, "%9.0g");
      ciCol(&l, 28);
      l += ciFmt(sp, "%9.0g");
      ciCol(&l, 58);
      l += ciFmt(dp - q * sp, "%9.0g");
      ciCol(&l, 70);
      l += ciFmt(dp + q * sp, "%9.0g");
      std::string l2;
      ciCol(&l2, 14);
      l2 += "|";
      ciCol(&l2, 17);
      l2 += "under H0:";
      ciCol(&l2, 28);
      l2 += ciFmt(spp, "%9.0g");
      ciCol(&l2, 38);
      l2 += ciFmt(tval, "%8.2f");
      ciCol(&l2, 49);
      l2 += ciFmt(pval, "%5.3f");
      out << l << "\n" << l2 << "\n" << line78('-') << "\n";
    }
    std::string zl = "        diff = prop(" + xname + ") - prop(" + yname + ")";
    ciCol(&zl, 67);
    zl += "z = " + ciFmt(z, "%8.4f");
    out << zl << "\n    H0: diff = 0\n\n";
    out << prCenter2("Ha: diff < 0", "Ha: diff != 0", "Ha: diff > 0") << "\n";
    out << prCenter2("Pr(Z < z) = " + f64(pl), "Pr(|Z| > |z|) = " + f64(p),
                     "Pr(Z > z) = " + f64(pr));
    setS("level", level);
    setS("p_u", pr);
    setS("p", p);
    setS("p_l", pl);
    setS("z", z);
    setS("ub_diff", dp + q * sp);
    setS("lb_diff", dp - q * sp);
    setS("ub2", b.p + q * s2);
    setS("lb2", b.p - q * s2);
    setS("ub1", a.p + q * s1);
    setS("lb1", a.p - q * s1);
    setS("se_diff", sp);
    setS("se_diff0", spp);
    setS("se2", s2);
    setS("se1", s1);
    setS("P_diff", dp);
    setS("P2", b.p);
    setS("P1", a.p);
    setS("N2", b.n);
    setS("N1", a.n);
    setS("P_2", b.p);
    setS("P_1", a.p);
    setS("N_2", b.n);
    setS("N_1", a.n);
    for (const char* h : {"P_2", "P_1", "N_2", "N_1"}) session.eval.markRHidden(h);
  }
  eval::declareRPosted(session.eval, "prtest", std::move(posted));
  return displayBlock(out.str());
}

bool prLevel(const eval::ParsedCommand& parsed, SessionState& session, double* level, ojson* err) {
  *level = session.level;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "level") continue;
    const std::string val = v.isBool ? std::string() : strutil::trim(v.strValue);
    char* end = nullptr;
    const double lv = std::strtod(val.c_str(), &end);
    if (val.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99)) {
      *err = ojson(eval::errorBlock("syntax_error", "level() must be between 10 and 99.99 inclusive",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      return false;
    }
    *level = lv;
  }
  return true;
}

}  // namespace

// `prtesti #obs #p #p0` / `prtesti #obs1 #p1 #obs2 #p2 [, level() count]`
// ([R] prtest): the z tests of a proportion and of two proportions from
// summary numbers (count: the second number of each pair is a count of
// successes). Printed and posted as prtesti does (vs-stata bank
// 887b544d197bb526).
ojson runPrtesti(const eval::ParsedCommand& parsed, SessionState& session) {
  auto syn = [](const std::string& m, int rc) {
    return ojson(eval::errorBlock("syntax_error", m, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  if (auto e = eval::rejectUnknownOptions(parsed.opts, {"level", "count", "xname", "yname"}, {}))
    return ojson(*e);
  double level = 95;
  ojson err;
  if (!prLevel(parsed, session, &level, &err)) return err;
  bool count = false;
  // xname()/yname(): the two samples' names in the two-sample report, trimmed
  // and cut to their first 12 characters ("x" and "y" when not given). The
  // one-sample report always names its sample x, so there they change nothing.
  auto sampleName = [](const eval::OptValue& v, const std::string& dflt) {
    std::string t = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (t.empty()) return dflt;
    size_t cut = 0;
    for (int chars = 0; cut < t.size() && chars < 12; ++chars) {
      ++cut;
      while (cut < t.size() && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) ++cut;
    }
    return strutil::trim(t.substr(0, cut));
  };
  std::string xname = "x", yname = "y";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "count") count = true;
    else if (k == "xname") xname = sampleName(v, "x");
    else if (k == "yname") yname = sampleName(v, "y");
  }
  std::vector<double> x;
  for (const auto& w : parsed.varlist) {
    char* end = nullptr;
    const double d = std::strtod(w.c_str(), &end);
    if (w.empty() || end == nullptr || *end != '\0') return syn("'" + w + "' found where number expected", 7);
    x.push_back(d);
  }
  if (x.size() != 3 && x.size() != 4)
    return syn("either three arguments for a one-sample test or four arguments for a two-sample "
               "test must be specified",
               198);
  auto isInt = [](double d) { return d == std::floor(d); };
  if (!isInt(x[0])) return syn("'" + parsed.varlist[0] + "' found where integer expected", 7);
  if (x.size() == 4 && !isInt(x[2]))
    return syn("'" + parsed.varlist[2] + "' found where integer expected", 7);
  auto prop = [&](double n, double v, const std::string& typed, double* out) -> std::optional<ojson> {
    if (count) {
      if (!isInt(v)) return syn("'" + typed + "' found where integer expected", 7);
      if (v <= n) v = v / n;
      if (v > 1 || v < 0) return syn(typed + " not less than " + strutil::trim(eval::applyStataFmt(n, "%12.0g")), 198);
    } else if (v > 1 || v < 0) {
      return syn(typed + " not in [0,1]", 198);
    }
    *out = v;
    return std::nullopt;
  };
  PrSample a, b;
  a.n = x[0];
  if (x.size() == 4) {
    b.n = x[2];
    if (auto e = prop(a.n, x[1], parsed.varlist[1], &a.p)) return *e;
    if (auto e = prop(b.n, x[3], parsed.varlist[3], &b.p)) return *e;
    if (a.n <= 1)
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, "r(2001)"));
    return prReport(session, true, a, b, 0, level, xname, yname, "", false);
  }
  if (x[2] > 1 || x[2] < 0) return syn(parsed.varlist[2] + " not in [0,1]", 198);
  if (auto e = prop(a.n, x[1], parsed.varlist[1], &a.p)) return *e;
  if (a.n <= 1)
    return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                  std::nullopt, std::nullopt, "r(2001)"));
  return prReport(session, false, a, b, x[2], level, "x", "", "", false);
}

// `prtest varname == #p [if] [in]`, `prtest varname == varname2`, `prtest
// varname [if] [in], by(groupvar) [level()]` ([R] prtest) over 0/1 variables
// (vs-stata bank 23685d043856ff5e, ccc958fd8867f31d). cluster() and the rho
// options are refused by name.
ojson runPrtest(const eval::ParsedCommand& parsed, SessionState& session) {
  auto syn = [](const std::string& m, int rc) {
    return ojson(eval::errorBlock("syntax_error", m, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  if (auto e = eval::rejectUnknownOptions(parsed.opts, {"by", "level"},
                                          {"cluster", "rho", "rho1", "rho2"}))
    return ojson(*e);
  double level = 95;
  ojson err;
  if (!prLevel(parsed, session, &level, &err)) return err;
  std::string byv;
  for (const auto& [k, v] : parsed.opts)
    if (k == "by" && !v.isBool) byv = strutil::trim(v.strValue);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  // varname [== exp]
  std::string lhs, rhs;
  {
    std::string all;
    for (const auto& t : parsed.varlist) all += (all.empty() ? "" : " ") + t;
    size_t eq = all.find("==");
    size_t len = 2;
    if (eq == std::string::npos) {
      eq = all.find('=');
      len = 1;
    }
    if (eq != std::string::npos) {
      lhs = strutil::trim(all.substr(0, eq));
      rhs = strutil::trim(all.substr(eq + len));
    } else {
      lhs = strutil::trim(all);
    }
  }
  if (lhs.empty()) return syn("varlist required", 100);
  std::vector<std::string> names = {lhs};
  if (auto e = loops::checkKnownVarlist(names, session)) return *e;
  lhs = names[0];
  std::vector<std::string> cols = {lhs};
  double p0 = std::numeric_limits<double>::quiet_NaN();
  bool rhsVar = false;
  if (!rhs.empty()) {
    if (!byv.empty()) return syn("may not combine == and option by()", 198);
    char* end = nullptr;
    p0 = std::strtod(rhs.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      std::vector<std::string> r2 = {rhs};
      if (auto e = loops::checkKnownVarlist(r2, session)) return *e;
      rhs = r2[0];
      rhsVar = true;
      cols.push_back(rhs);
    }
  } else {
    if (byv.empty()) return syn("option by() required", 100);
    std::vector<std::string> b2 = {byv};
    if (auto e = loops::checkKnownVarlist(b2, session)) return *e;
    byv = b2[0];
    cols.push_back(byv);
  }
  ColumnScope scope(session, parsed, cols, "prtest");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();
  std::vector<double> x, y;
  std::string e2;
  if (ds.dtype(lhs) == "factor") return syn("type mismatch", 109);
  if (!numericColumn(ds, lhs, &x, &e2)) return aerError("prtest: " + e2);
  auto zeroOne = [](const std::vector<double>& v, const std::vector<char>* use) {
    for (size_t i = 0; i < v.size(); ++i) {
      if (use && !(*use)[i]) continue;
      if (std::isnan(v[i])) continue;
      if (v[i] != 0 && v[i] != 1) return false;
    }
    return true;
  };
  auto mean = [](const std::vector<double>& v, const std::vector<char>* use, PrSample* s) {
    double sum = 0, n = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      if (use && !(*use)[i]) continue;
      if (std::isnan(v[i])) continue;
      sum += v[i];
      n += 1;
    }
    s->n = n;
    s->p = n > 0 ? sum / n : std::numeric_limits<double>::quiet_NaN();
  };
  if (!rhs.empty() && !rhsVar) {
    if (!zeroOne(x, nullptr)) return syn(lhs + " is not a 0/1 variable", 450);
    if (!(p0 < 1 && p0 > 0)) return syn(rhs + " is not in (0,1)", 450);
    PrSample a;
    mean(x, nullptr, &a);
    if (a.n == 0)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, "r(2000)"));
    return prReport(session, false, a, a, p0, level, lhs, "", "Variable", true);
  }
  if (rhsVar) {
    if (ds.dtype(rhs) == "factor") return syn("type mismatch", 109);
    if (!numericColumn(ds, rhs, &y, &e2)) return aerError("prtest: " + e2);
    if (!zeroOne(x, nullptr)) return syn(lhs + " is not a 0/1 variable", 450);
    if (!zeroOne(y, nullptr)) return syn(rhs + " is not a 0/1 variable", 450);
    PrSample a, b;
    mean(x, nullptr, &a);
    mean(y, nullptr, &b);
    if (a.n <= 1)
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, "r(2001)"));
    return prReport(session, true, a, b, 0, level, abbrevCp(lhs, 12), abbrevCp(rhs, 12),
                    "Variable", false);
  }
  // by(): the sample is varname and groupvar present.
  const bool strBy = ds.dtype(byv) == "factor";
  if (strBy)
    return ojson(eval::unsupportedCommand("prtest", std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  if (!numericColumn(ds, byv, &y, &e2)) return aerError("prtest: " + e2);
  std::vector<char> use(x.size(), 0);
  std::set<double> xv, gv;
  for (size_t i = 0; i < x.size(); ++i) {
    if (std::isnan(x[i]) || std::isnan(y[i])) continue;
    use[i] = 1;
    xv.insert(x[i]);
    gv.insert(y[i]);
  }
  if (xv.size() != 2) return syn(lhs + " takes on " + std::to_string(xv.size()) + " values, not 2", 450);
  if (gv.size() != 2) return syn(byv + " takes on " + std::to_string(gv.size()) + " values, not 2", 450);
  if (!zeroOne(x, &use)) return syn(lhs + " is not a 0/1 variable", 450);
  const double g1 = *gv.begin(), g2 = *gv.rbegin();
  std::vector<char> u1(x.size(), 0), u2(x.size(), 0);
  for (size_t i = 0; i < x.size(); ++i) {
    if (!use[i]) continue;
    (y[i] == g1 ? u1 : u2)[i] = 1;
  }
  PrSample a, b;
  mean(x, &u1, &a);
  mean(x, &u2, &b);
  auto lab = [&](double code) {
    auto it = session.varLblname.find(byv);
    if (it != session.varLblname.end()) {
      auto d = session.labelDefs.find(it->second);
      if (d != session.labelDefs.end()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f", code);
        auto t = d->second.find(buf);
        if (t != d->second.end() && !t->second.empty()) return t->second;
      }
    }
    return strutil::trim(eval::applyStataFmt(code, "%9.0g"));
  };
  if (a.n <= 1)
    return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                  std::nullopt, std::nullopt, "r(2001)"));
  const std::string xn = lab(g1).substr(0, 12), yn = lab(g2).substr(0, 12);
  return prReport(session, true, a, b, 0, level, xn, yn, "   Group", false);
}

}  // namespace piet::handlers
