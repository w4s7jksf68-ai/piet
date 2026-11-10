// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table number format.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// `fmt(3)` means `%9.3f`; `fmt(%9.2f)` is passed through. `fmt(a3)` is Stata's
// "adaptive" width and is REFUSED by the callers rather than silently rendered
// as a fixed 3 decimals -- refusing is the plan's decision 4.
std::string normalizeFmt(const std::string& raw, bool* okOut) {
  *okOut = true;
  const std::string f = strutil::trim(dequote(raw));
  if (f.empty()) {
    *okOut = false;
    return "%9.3f";
  }
  if (f[0] == '%') return f;
  if (f.find_first_not_of("0123456789") == std::string::npos) {
    return "%9." + f + "f";
  }
  *okOut = false;
  return "%9.3f";
}


std::string fmtNum(double v, const std::string& fmt) {
  if (std::isnan(v)) return kMissingCell;
  // W1a-5b: `a#` is esttab's own ADAPTIVE format -- at least # significant
  // digits, which is what a footer row carries when nothing overrides it.
  // Measured: `esttab, scalars(mymean)` renders 6165.257 as `6165.3`
  // (w1b_sfmt_rule.log q5), which %9.3f cannot produce. The spelling is
  // accepted here so the footer can ask for it by name instead of the caller
  // reaching for a second formatter.
  if (!fmt.empty() && fmt[0] == 'a') {
    int d = 3;
    if (fmt.size() > 1) {
      try {
        size_t used = 0;
        const int n = std::stoi(fmt.substr(1), &used);
        if (used + 1 == fmt.size() && n >= 0 && n <= 12) d = n;
      } catch (const std::exception&) {
        d = 3;
      }
    }
    return sigDigits(v, d, /*minDec=*/1, /*comma=*/false);
  }
  int prec = 3;
  char kind = 'f';
  size_t i = 0;
  if (i < fmt.size() && fmt[i] == '%') ++i;
  while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '0' || fmt[i] == ' ')) ++i;
  while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) ++i;
  if (i < fmt.size() && fmt[i] == '.') {
    ++i;
    std::string digits;
    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) digits += fmt[i++];
    if (!digits.empty()) prec = std::stoi(digits);
  }
  if (i < fmt.size() && (fmt[i] == 'f' || fmt[i] == 'g' || fmt[i] == 'e')) kind = fmt[i];
  // `%9.0g` / `%10.0g` is Stata's DEFAULT general format, not "one significant
  // digit": C's `%.0g` is treated as `%.1g` and would render 1.234 as "1".
  // `fmtStataDefaultG` is the measured nine-character rule display already uses,
  // so mat2txt's default format and `display` agree on the same number.
  if (kind == 'g' && prec == 0) return piet::fmtStataDefaultG(v);
  char buf[128];
  if (kind == 'f') {
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  } else if (kind == 'e') {
    std::snprintf(buf, sizeof(buf), "%.*e", prec, v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
  }
  return std::string(buf);
}


std::string sigDigits(double v, int d, int minDec, bool comma) {
  if (std::isnan(v)) return kMissingCell;
  int dec = 0;
  if (v == std::trunc(v)) {
    dec = 0;  // "integer: print no decimal places"
  } else if (std::fabs(v) < 1.0) {
    // `right` = zeros after the decimal point. Stata's int() truncates toward
    // zero, which is what a C cast does, so -1.0128 -> -1 on both sides.
    const double frac = std::fabs(v - std::trunc(v));
    const int right = -static_cast<int>(std::log10(frac));
    dec = std::max(1, d + right);
    if (dec > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.*e", std::min(9, d - 1), v);
      return std::string(e);
    }
  } else {
    const int left = static_cast<int>(std::log10(std::fabs(v)) + 1);
    if (left > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.0e", v);
      return std::string(e);
    }
    dec = std::max(minDec, d - left + 1);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string s(buf);
  if (!comma) return s;
  // %fc: thousands separators on the integer part only.
  const size_t dot = s.find('.');
  size_t start = (s[0] == '-') ? 1 : 0;
  size_t end = (dot == std::string::npos) ? s.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    s.insert(at, ",");
  }
  return s;
}


std::string fmtJson(const ojson& v, const std::string& fmt) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  return fmtNum(v.get<double>(), fmt);
}


// An observation count renders as an integer (Stata's `%9.0g` on an integral
// value), never as "30.000".
std::string fmtCount(const ojson& v) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  const double d = v.get<double>();
  if (std::isnan(d)) return kMissingCell;
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    return std::to_string(static_cast<long long>(d));
  }
  return fmtNum(d, "%9.3f");
}


std::string starsFor(const ojson& p, const std::vector<StarLevel>& ladder) {
  if (!p.is_number()) return "";
  const double pv = p.get<double>();
  if (std::isnan(pv)) return "";
  for (const auto& lv : ladder) {
    if (pv < lv.cutoff) return lv.mark;
  }
  return "";
}


std::string starNote(const std::vector<StarLevel>& ladder) {
  std::string s;
  for (size_t i = 0; i < ladder.size(); ++i) {
    if (i) s += ", ";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s p<%g", ladder[i].mark, ladder[i].cutoff);
    s += buf;
  }
  return s;
}


// `starlevels(* 0.10 ** 0.05 *** 0.01)` -- pairs of mark/threshold. A malformed
// list keeps the default rather than half-applying, which is the same rule
// esttab_block.cpp's parseStarOpt already follows for `star()`.
std::vector<StarLevel> parseStarLevels(const std::string& spec,
                                       const std::vector<StarLevel>& fallback,
                                       std::vector<std::string>* markStore) {
  const std::vector<std::string> toks = splitWs(dequote(spec));
  if (toks.size() < 2 || toks.size() % 2 != 0) return fallback;
  std::vector<StarLevel> out;
  markStore->clear();
  markStore->reserve(toks.size() / 2);
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    try {
      size_t pos = 0;
      const double th = std::stod(toks[i + 1], &pos);
      if (pos != toks[i + 1].size()) return fallback;
      markStore->push_back(toks[i]);
      out.push_back(StarLevel{nullptr, th});
    } catch (...) {
      return fallback;
    }
  }
  for (size_t i = 0; i < out.size(); ++i) out[i].mark = (*markStore)[i].c_str();
  std::sort(out.begin(), out.end(),
            [](const StarLevel& a, const StarLevel& b) { return a.cutoff < b.cutoff; });
  return out;
}
}
