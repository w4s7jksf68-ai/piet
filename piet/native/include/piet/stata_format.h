// stata_format.h — the ONE implementation of Stata's numeric display formats.
//
// WHY THIS FILE EXISTS (BUG-195).
// -------------------------------
// PIET had TWO hand-written renderers for `%w.Pg`:
//   * display_hardening.cpp's applyStataFmtLocal(), serving the `display`
//     command, and
//   * eval/exprfns.cpp's applyStataFmt(), serving `string(x, "fmt")` and
//     `tostring, usedisplayformat`.
// BUG-192 found that the first one handed the spec straight to printf, whose
// `%.0g` means "zero significant digits" — nothing like Stata's `g`, where P
// bounds the significant digits and a value that fits shows as itself. That
// was fixed in the display copy alone. The twin kept the defect, and the twin
// is the one that writes STORED DATA: `tostring VAR, usedisplayformat` renders
// through it into a string variable that survives save and export. Two
// hand-written twins of Stata's `g` rule is how they drift, so there is now
// one renderer and both layers call it.
//
// The two layers still differ in exactly two documented ways, and only these:
//   1. SPEC GRAMMAR. `display` full-matches `^%(-)?(\d+)?\.(\d+)([fgeEFG])$` so
//      that `%td`, `%9s` and friends fall through untouched; `string()`
//      prefix-matches `^%-?0?(\d+)?\.(\d+)([fge])` (its Python ancestor did).
//      Each caller keeps its own regex and hands the result here as a
//      StataFmtSpec.
//   2. PADDING. `display` right-justifies into the field width; `string()` and
//      `tostring` do NOT. This is measured, not assumed — see below.
//
// THE ORACLE.
// -----------
// tests/kitchensink/fmt_oracle/fmt_g_oracle.{do,log}: 513 StataBE 19.5 cases,
// 9 specs x 19 values x 3 surfaces (display / string() / tostring). Two facts
// the capture establishes mechanically over all 171 value-spec pairs:
//   * every `display` rendering is EXACTLY w characters wide (right-justified);
//   * every `string()` and `tostring, usedisplayformat` rendering is EXACTLY
//     the display rendering with the padding stripped — 171/171, both sections,
//     no exceptions.
// So there is one value-rendering rule and one padding step, which is precisely
// the split below: renderValue() produces the bytes, padToField() places them.
//
// THE RULE (BUG-192; each clause is a fact a captured case forced, and the
// capture is primary — where a derived rule and the capture disagree, the rule
// is wrong):
//   1. SIGNIFICANT DIGITS. sig = P when P > 0, else w-2. (%9.0g gives 7 —
//      3.14159265358979 -> 3.141593; %20.0g gives 18 -> 3.14159265358979001.)
//   2. FIXED vs SCIENTIFIC. Fixed is refused when the INTEGER part needs more
//      than w-2 digits, whatever P says: 99999999 is 8 digits, so %9.0g and
//      %9.3g both give 1.00e+08 while %10.0g gives 99999999. 1000000 stays
//      fixed at %9.0g (7 digits), 1234567890 does not (10).
//   3. DECIMALS. For |v| >= 1, decimals = max(0, sig - integer digits) — which
//      is why %9.3g 123456.789 is 123457 and not 1.23e+05: P never truncates
//      the integer part. For |v| < 1, decimals = sig + the leading zeros after
//      the point, so %12.4g 0.000012345 is .00001234.
//   4. FIELD BUDGET. The rendered value never exceeds w-1 characters NOT
//      COUNTING A MINUS SIGN — the leading column Stata reserves is the sign's
//      column, so a negative may fill all w. When significance would overflow
//      the budget, decimals are shed until it fits: %9.0g 0.000012345 wants 11
//      decimals, lands on 7, prints .0000123.
//      The "not counting a minus sign" half is a CORRECTION (2026-08-06). The
//      clause used to read "never exceeds w-1 characters" flat, because the
//      original capture's 19 values had no negative long enough to tell the two
//      readings apart. fmt_g_oracle_neg.{do,log} exists to pin it: under the
//      flat reading %9.0g -123456.789 sheds to -123457, %10.0g to -123456.8 and
//      %20.0g to -123456.789, where Stata gives -123456.8, -123456.79 and
//      -123456.789000000004. The rule was derived; the capture is primary.
//   5. COSMETICS. Trailing zeros and a bare trailing '.' are stripped, and the
//      leading '0' of "0." is dropped (.1, -.0004).
//   6. ROUNDING IS HALF-AWAY-FROM-ZERO, not printf's half-to-even: %9.2g 42.5
//      is 43, where snprintf("%.0f", 42.5) gives 42. Done on the exact decimal
//      expansion so there is no double-rounding.
//   7. SCIENTIFIC DECIMALS = min(w-7, sig-1). %9.2g 1234567890 is 1.2e+09 (one
//      decimal), %9.3g is 1.23e+09, %12.0g 12345678901234 is 1.23457e+13.
// Rider on the neighbouring branch, same capture: `%w.0e` shows w-7 decimals,
// not printf's none — %9.0e 42 is 4.20e+01.
//
// Header-only on purpose. A .cpp would have to be added to piet/native/
// CMakeLists.txt, which is outside this change's file manifest and is a
// merge surface three concurrent stints are already touching; these are small
// pure functions with no state, so there is nothing a translation unit buys.

#ifndef PIET_STATA_FORMAT_H
#define PIET_STATA_FORMAT_H

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace piet::fmt {

// A parsed numeric display format. Both layers' regexes produce one of these;
// see the grammar note in the file header for why they stay two regexes.
struct StataFmtSpec {
  bool leftJustify = false;  // the `-` flag (both layers since BUG-248)
  bool zeroFill = false;     // the `0` flag (BUG-248) — value-level, `f` only:
                             // the oracle's %09.2f rows are zero-filled on BOTH
                             // surfaces (display `000042.50` == string() len 9),
                             // while %08.0g SPACE-pads on display and strips on
                             // string() — so `0` is part of the VALUE for `f`
                             // and an ordinary field width for `g`.
  bool hasWidth = false;     // a widthless `%.3g` keeps the historical printf path
  int width = 0;             // w
  int prec = 0;              // P
  char conv = 'g';           // 'f' | 'g' | 'e', already case-folded
  // BUG-296: the two taxonomy axes the format grammar never had a slot for.
  // `commaDecimal` is Stata's `%w,pf` — the SAME format with ',' as the decimal
  // separator (and, when grouped, '.' as the thousands separator: the European
  // convention). `groupThousands` is the `c` suffix (`%12.0gc`, `%12,0fc`).
  // Both were fall-throughs to a bare number before — silent, plausible, wrong.
  bool commaDecimal = false;
  bool groupThousands = false;
};

// True for a Stata missing TOKEN: system missing `.` or an extended missing
// `.a`-`.z`. String-level, for the layer that formats an already-stringified
// value; the double-taking layer never sees one (its caller returns "." first).
inline bool isMissingToken(const std::string& t) {
  return t == "." || (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z');
}

// Renders |v| at `decimals` places with half-away-from-zero rounding (clause 6),
// working on the exact decimal expansion so the intermediate cannot round twice.
inline std::string fixedHalfAway(double v, int decimals) {
  if (decimals < 0) decimals = 0;
  const int guard = decimals + 30;
  std::string buf(static_cast<size_t>(guard) + 400, '\0');
  int n = std::snprintf(buf.data(), buf.size(), "%.*f", guard, v);
  if (n < 0 || n >= static_cast<int>(buf.size())) return std::string();
  buf.resize(static_cast<size_t>(n));
  bool neg = !buf.empty() && buf[0] == '-';
  std::string digits = neg ? buf.substr(1) : buf;
  size_t dot = digits.find('.');
  if (dot == std::string::npos) return buf;
  std::string ip = digits.substr(0, dot);
  std::string fp = digits.substr(dot + 1);
  // Decide the carry from the first dropped digit (>= '5' rounds away).
  bool roundUp = static_cast<size_t>(decimals) < fp.size() &&
                 fp[static_cast<size_t>(decimals)] >= '5';
  fp.resize(static_cast<size_t>(decimals), '0');
  if (roundUp) {
    std::string all = ip + fp;
    int i = static_cast<int>(all.size()) - 1;
    for (; i >= 0; --i) {
      if (all[static_cast<size_t>(i)] != '9') {
        ++all[static_cast<size_t>(i)];
        break;
      }
      all[static_cast<size_t>(i)] = '0';
    }
    if (i < 0) all.insert(all.begin(), '1');
    fp = all.substr(all.size() - static_cast<size_t>(decimals));
    ip = all.substr(0, all.size() - static_cast<size_t>(decimals));
  }
  std::string out = (neg ? "-" : "") + (ip.empty() ? std::string("0") : ip);
  if (decimals > 0) out += "." + fp;
  return out;
}

// Clause 5: strip trailing fractional zeros and a bare trailing '.', then drop
// the leading '0' of "0." / "-0." (Stata writes .1 and -.0004).
inline std::string trimNumber(std::string s) {
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s.rfind("0.", 0) == 0) s.erase(0, 1);
  else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
  if (s == "-0" || s.empty()) s = "0";
  return s;
}

// Clause 7.
inline std::string scientific(double v, int width, int sig) {
  int edec = width - 7;
  if (edec > sig - 1) edec = sig - 1;
  if (edec < 0) edec = 0;
  char buf[512];
  int n = std::snprintf(buf, sizeof(buf), "%.*e", edec, v);
  if (n < 0 || n >= static_cast<int>(sizeof(buf))) return std::string();
  return std::string(buf, static_cast<size_t>(n));
}

// Stata's `%w.Pg` — clauses 1-7 above.
inline std::string general(double v, int width, int prec) {
  if (std::isnan(v)) return ".";
  if (width < 2) width = 2;
  int sig = prec > 0 ? prec : width - 2;
  if (sig < 1) sig = 1;
  // Clause 4. The sign does not spend the budget — see the clause's correction
  // note in this file's header, and fmt_g_oracle_neg.log for the cases that
  // forced it.
  const size_t budget = static_cast<size_t>(width - 1) + (v < 0.0 ? 1u : 0u);
  const double a = std::fabs(v);

  if (v == 0.0) return "0";
  if (std::isinf(v)) return scientific(v, width, sig);

  const int e10 = static_cast<int>(std::floor(std::log10(a)));
  const int intDigits = e10 >= 0 ? e10 + 1 : 0;
  const int intDigitCap = width - 2 < 1 ? 1 : width - 2;
  if (intDigits <= intDigitCap) {
    int decimals = e10 >= 0 ? (sig - intDigits) : (sig + (-e10 - 1));
    if (decimals < 0) decimals = 0;
    for (; decimals >= 0; --decimals) {
      std::string s = trimNumber(fixedHalfAway(v, decimals));
      if (s.empty()) break;
      // A fixed form that has shed every significant digit is not a rendering
      // of this number, it is a rendering of zero — 1e-20 in %9.0g must be
      // 1.00e-20, never 0. Shedding decimals is how the field budget is met
      // (clause 4), so the guard has to be here rather than up front.
      if (s == "0") break;
      if (s.size() <= budget) return s;
      if (decimals == 0) break;
    }
  }
  return scientific(v, width, sig);
}

// ---------------------------------------------------------------------------
// BUG-248: the %tc / %td date families.
//
// Both rendered as their raw underlying number before this — a legitimate value
// with no diagnostic, which is the silent-wrong-answer shape. Every clause here
// is a captured fact (tests/kitchensink/fmt_oracle/exprfns_contract_oracle.log,
// sections FTC/FTD, all three surfaces):
//   * %td counts DAYS since 01jan1960 and FLOORS a fractional or negative value
//     (42.5 and 42.9 -> 12feb1960; -0.5 and -1 -> 31dec1959; 20000 -> 04oct2014).
//   * %tc counts MILLISECONDS since 01jan1960 00:00:00 and floors to the second
//     (999 -> 00:00:00, 1000 -> 00:00:01, 59999 -> 00:00:59, never rounds up);
//     1.5e12 -> 14jul2007 02:40:00, 5.5e12 -> 15apr2134 09:46:40,
//     -86400000 -> 31dec1959 00:00:00.
//   * The default renderings are `ddmonccyy` (9 bytes, lowercase 3-letter month)
//     and `ddmonccyy hh:mm:ss` (18 bytes), identical on display and string()
//     (no field padding at these widths).
// ONLY the bare `%tc`/`%td` specs are claimed: detail codes (%tdCCYY...) and the
// other calendars (%tq renders 1970q3 in the same capture, %tm/%tw/%th/%ty) keep
// the raw-number fall-through, documented in BUG-248's ledger entry.

// Days-to-civil-date, exact for the whole double-representable day range
// (Howard Hinnant's civil_from_days, days relative to 1970-01-01).
inline void civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);            // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const long long yy = static_cast<long long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                  // [0, 11]
  d = doy - (153 * mp + 2) / 5 + 1;                                         // [1, 31]
  m = mp < 10 ? mp + 3 : mp - 9;                                            // [1, 12]
  y = static_cast<int>(yy + (m <= 2));
}

inline const char* stataMonthName(unsigned m) {
  static const char* kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
  return (m >= 1 && m <= 12) ? kMon[m - 1] : "???";
}

// `%td` of `days`-since-01jan1960. nullopt = out of the range this claims
// (caller falls back to its pre-existing rendering, same contract as
// renderValue's nullopt). |days| capped at ~4e6 (~year 12925 either side) —
// beyond the capture's warrant, and far beyond any .dta in the wild.
inline std::optional<std::string> renderTd(double v) {
  if (std::isnan(v) || std::fabs(v) > 4.0e6) return std::nullopt;
  const long long days = static_cast<long long>(std::floor(v));
  int y;
  unsigned m, d;
  civilFromDays(days - 3653, y, m, d);  // 01jan1960 is day -3653 from epoch 1970
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02u%s%04d", d, stataMonthName(m), y);
  return std::string(buf);
}

// `%tc` of `ms`-since-01jan1960 00:00:00 (same range cap, in days).
inline std::optional<std::string> renderTc(double v) {
  if (std::isnan(v) || std::fabs(v) > 4.0e6 * 86400000.0) return std::nullopt;
  const long long totalSec = static_cast<long long>(std::floor(v / 1000.0));
  long long days = totalSec / 86400;
  long long rem = totalSec - days * 86400;
  if (rem < 0) {  // floor division for the pre-1960 half-line
    rem += 86400;
    days -= 1;
  }
  int y;
  unsigned m, d;
  civilFromDays(days - 3653, y, m, d);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%02u%s%04d %02lld:%02lld:%02lld", d, stataMonthName(m), y,
                rem / 3600, (rem % 3600) / 60, rem % 60);
  return std::string(buf);
}

// --- BUG-296: the %t calendars other than tc/td ------------------------------
// Measured (tests/kitchensink/wsn_oracle/wsr_oracle.log, section WSR|T, StataBE
// 19.5 IC, four values each including negatives): every one of these counts
// PERIODS since 1960 and FLOORS, exactly as %td/%tc do.
//   %tq 42.5 -> 1970q3 · 0 -> 1960q1 · -3 -> 1959q2 · 245 -> 2021q2
//   %th 42.5 -> 1981h1 · 0 -> 1960h1 · -3 -> 1958h2 · 245 -> 2082h2
//   %tm 42.5 -> 1963m7 · 0 -> 1960m1 · -3 -> 1959m10 · 245 -> 1980m6
//   %tw 42.5 -> 1960w43 · 0 -> 1960w1 · -3 -> 1959w50 · 245 -> 1964w38
// (%tw is Stata's own 52-week year, not an ISO week — 1960w43 for period 42.)
//
// %ty took a SECOND capture to pin (wsr_oracle's four values looked
// self-inconsistent: 42.5 -> `43`, 245 -> `0245`). Fourteen values in
// tests/eval/golden/reghdfe_absorb_vce_oracle.log section TY resolve it — the
// year is ROUNDED half-away-from-zero, then:
//   |year| <= 99  -> plain digits, space-padded to the 4-wide field
//                    (0 -> `0`, 43 -> `43`, 99 -> `99`, -3 -> `-3`, 42.5 -> `43`)
//   100..9999     -> ZERO-padded to 4 (100 -> `0100`, 245 -> `0245`, 1959 -> `1959`)
//   >= 10000      -> NOT CLAIMED. Exactly one sample (12345 -> `1.2e+04`), which
//                    is one point, not a rule — the raw fall-through stays there
//                    rather than a guessed overflow arm.
inline std::optional<std::string> renderPeriodic(double v, int perYear, char letter) {
  if (std::isnan(v) || std::fabs(v) > 1.0e7) return std::nullopt;
  const long long p = static_cast<long long>(std::floor(v));
  long long yoff = p / perYear;
  long long rem = p - yoff * perYear;
  if (rem < 0) {  // floor division for the pre-1960 half-line
    rem += perYear;
    yoff -= 1;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld%c%lld", 1960 + yoff, letter, rem + 1);
  return std::string(buf);
}

// %ty — the year IS the value. Two measured regimes; the >=10000 overflow is a
// single sample and is deliberately left to the caller's fall-through
// (nullopt), which is what keeps this from being a guess.
inline std::optional<std::string> renderTy(double v) {
  if (std::isnan(v) || std::fabs(v) > 1.0e15) return std::nullopt;
  const double r = v < 0.0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5);  // half away from zero
  const long long y = static_cast<long long>(r);
  if (y <= -10000 || y >= 10000) return std::nullopt;  // unclaimed overflow arm
  char buf[16];
  if (y >= 100 || y <= -100) std::snprintf(buf, sizeof(buf), "%04lld", y);
  else std::snprintf(buf, sizeof(buf), "%lld", y);
  return std::string(buf);
}

inline std::optional<std::string> renderTq(double v) { return renderPeriodic(v, 4, 'q'); }
inline std::optional<std::string> renderTh(double v) { return renderPeriodic(v, 2, 'h'); }
inline std::optional<std::string> renderTm(double v) { return renderPeriodic(v, 12, 'm'); }
inline std::optional<std::string> renderTw(double v) { return renderPeriodic(v, 52, 'w'); }

// --- BUG-296: detail-coded %td ----------------------------------------------
// A `%td` carrying a detail string (`%tdCCYY-NN-DD`, `%tdMonth_dd,_CCYY`, …)
// rendered the raw day number. Measured on day 20000 = 04oct2014
// (wsr_oracle.log, section WSR|D):
//   %tdCCYY-NN-DD      -> 2014-10-04
//   %tdDD/NN/CCYY      -> 04/10/2014
//   %tdMonth_dd,_CCYY  -> October 4, 2014
//   %tdnn/dd/YY        -> 10/4/14
//   %tdDay_DD_Mon_CCYY -> Sat 04 Oct 2014
// Token map and longest-match-first scan are the SAME ones the browse grid
// already speaks (piet/frontend/js/grid.js TD_DETAIL_TOKENS, FIX:GRID-TFMT-
// DETAIL) — two renderers of one Stata grammar is how they drift, so this one
// is written to that map plus the two forms the capture adds (`Day`, `nn`).
// `%tc` detail strings stay unclaimed, exactly as the grid leaves them.
inline const char* stataMonthFull(unsigned m) {
  static const char* kMon[12] = {"January", "February", "March",     "April",
                                 "May",     "June",     "July",      "August",
                                 "September", "October", "November", "December"};
  return (m >= 1 && m <= 12) ? kMon[m - 1] : "???";
}

inline const char* stataDayAbbrev(long long days) {
  // 01jan1960 (day 0) was a Friday.
  static const char* kDay[7] = {"Fri", "Sat", "Sun", "Mon", "Tue", "Wed", "Thu"};
  long long i = days % 7;
  if (i < 0) i += 7;
  return kDay[i];
}

inline std::optional<std::string> renderTdDetail(double v, const std::string& detail) {
  if (std::isnan(v) || std::fabs(v) > 4.0e6) return std::nullopt;
  const long long days = static_cast<long long>(std::floor(v));
  int y;
  unsigned mo, d;
  civilFromDays(days - 3653, y, mo, d);
  char num[16];
  std::string out;
  size_t i = 0;
  while (i < detail.size()) {
    if (detail.compare(i, 4, "CCYY") == 0) {
      std::snprintf(num, sizeof(num), "%04d", y);
      out += num; i += 4;
    } else if (detail.compare(i, 5, "Month") == 0) {
      out += stataMonthFull(mo); i += 5;
    } else if (detail.compare(i, 3, "Mon") == 0) {
      out += std::string(stataMonthFull(mo)).substr(0, 3); i += 3;
    } else if (detail.compare(i, 3, "Day") == 0) {
      out += stataDayAbbrev(days); i += 3;
    } else if (detail.compare(i, 2, "DD") == 0) {
      std::snprintf(num, sizeof(num), "%02u", d);
      out += num; i += 2;
    } else if (detail.compare(i, 2, "dd") == 0) {
      std::snprintf(num, sizeof(num), "%u", d);
      out += num; i += 2;
    } else if (detail.compare(i, 2, "NN") == 0) {
      std::snprintf(num, sizeof(num), "%02u", mo);
      out += num; i += 2;
    } else if (detail.compare(i, 2, "nn") == 0) {
      std::snprintf(num, sizeof(num), "%u", mo);
      out += num; i += 2;
    } else if (detail.compare(i, 2, "YY") == 0) {
      int yy = y % 100;
      if (yy < 0) yy += 100;
      std::snprintf(num, sizeof(num), "%02d", yy);
      out += num; i += 2;
    } else if (detail[i] == '_') {
      out += ' '; ++i;
    } else {
      out += detail[i]; ++i;
    }
  }
  return out;
}

// --- BUG-296: the %t families' DISPLAY field width ---------------------------
// Stata right-justifies a date rendering into a field as wide as the WIDEST
// value that spec could ever produce — not as wide as this value. Measured
// (wsr_oracle.log WSR|TA/DA vs WSR|TB/DB): `%tm 42.5` displays ` 1963m7` (7,
// because `1959m10` needs 7) while string() returns `1963m7` (6); and
// `%tdMonth_dd,_CCYY` displays into 18 (`September` is 9 + `dd` 2 + the
// literals) while `%tdDay_DD_Mon_CCYY`, whose tokens are all fixed-width, sits
// at exactly 15. So the width is a property of the FORMAT STRING, computed by
// summing each token's maximum length — which is what this does.
// string()/tostring never pad (the 171/171 rule this file's header states), so
// only the display surface calls it.
inline int tokenMaxWidth(const std::string& detail) {
  int w = 0;
  size_t i = 0;
  while (i < detail.size()) {
    if (detail.compare(i, 4, "CCYY") == 0)       { w += 4; i += 4; }
    else if (detail.compare(i, 5, "Month") == 0) { w += 9; i += 5; }  // September
    else if (detail.compare(i, 3, "Mon") == 0)   { w += 3; i += 3; }
    else if (detail.compare(i, 3, "Day") == 0)   { w += 3; i += 3; }
    else if (detail.compare(i, 2, "DD") == 0)    { w += 2; i += 2; }
    else if (detail.compare(i, 2, "dd") == 0)    { w += 2; i += 2; }
    else if (detail.compare(i, 2, "NN") == 0)    { w += 2; i += 2; }
    else if (detail.compare(i, 2, "nn") == 0)    { w += 2; i += 2; }
    else if (detail.compare(i, 2, "YY") == 0)    { w += 2; i += 2; }
    else                                          { w += 1; i += 1; }
  }
  return w;
}

// fam is the letter after %t. 0 = no default width claimed for this family.
inline int tFamilyWidth(char fam, const std::string& detail) {
  if (fam == 'd' && !detail.empty()) return tokenMaxWidth(detail);
  switch (fam) {
    case 'd': return 9;   // 04oct2014
    case 'c': return 18;  // 13feb1999 23:31:30
    case 'q': return 6;   // 1970q3
    case 'h': return 6;   // 1981h1
    case 'm': return 7;   // 1959m10
    case 'w': return 7;   // 1960w43
    case 'y': return 4;   // the year field is 4 wide (TY capture)
    default: return 0;
  }
}

// THE renderer. Produces the UNPADDED bytes for `v` under `sp` — exactly what
// Stata's string()/tostring return, and exactly what display prints once
// padToField() has placed it.
//
// nullopt means "this spec could not be rendered at all" (only reachable when
// a caller-supplied precision overflows the printf buffer, e.g. `%9.400f`).
// --- BUG-296: separators ----------------------------------------------------
// Insert `sep` every three digits of the INTEGER part only. Never applied to a
// scientific rendering: `1.234568e+12`'s mantissa digits are not a magnitude
// (the same guard grid.js's _groupThousands carries, for the same reason).
inline std::string insertGroups(const std::string& s, char sep, char decSep = '.') {
  if (s.find('e') != std::string::npos || s.find('E') != std::string::npos) return s;
  size_t start = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  size_t end = s.find(decSep, start);
  if (end == std::string::npos) end = s.size();
  const size_t nd = end - start;
  if (nd <= 3) return s;
  std::string out = s.substr(0, start);
  for (size_t i = 0; i < nd; ++i) {
    if (i != 0 && (nd - i) % 3 == 0) out += sep;
    out += s[start + i];
  }
  out += s.substr(end);
  return out;
}

// Swap printf's decimal point for the spec's own separator. MUST run before
// grouping, never after: under `%w,pfc` the group separator IS '.', so a
// swap-the-first-dot pass applied afterwards rewrites the first GROUP mark
// instead of the decimal (`1.234.568` -> `1,234.568`, measured wrong against
// WSR|GA|%12,0fc|5).
inline std::string applyDecimalSep(std::string s, const StataFmtSpec& sp) {
  if (!sp.commaDecimal) return s;
  const size_t dot = s.find('.');
  if (dot != std::string::npos) s[dot] = ',';
  return s;
}

inline char groupSepOf(const StataFmtSpec& sp) { return sp.commaDecimal ? '.' : ','; }

// The one exit point for a rendered magnitude: decimal separator first, then
// (optionally) grouping with the other separator. `insertGroups` looks for the
// decimal by the spec's own character, so both orders of separator agree.
inline std::string finishNumeric(std::string s, const StataFmtSpec& sp, bool group) {
  s = applyDecimalSep(std::move(s), sp);
  if (!group) return s;
  return insertGroups(s, groupSepOf(sp), sp.commaDecimal ? ',' : '.');
}

// Clause 4's budget: w-1 characters, NOT counting a minus sign.
inline size_t fieldBudget(double v, int width) {
  return static_cast<size_t>(width < 1 ? 0 : width - 1) + (v < 0.0 ? 1u : 0u);
}

// --- BUG-296 / WS-R: the `f` family's OVERFLOW arm ---------------------------
// Measured (tests/kitchensink/wsn_oracle/wsr_oracle.log, section WSR|F, StataBE
// 19.5 IC): when the fixed rendering is LONGER than the field, Stata does not
// widen the field — it switches to scientific. `%9.2f 1234567.89` is ` 1.23e+06`,
// not `1234567.89`. PIET printed the fixed form at any length, which is how a
// table column silently lost its alignment.
//
// The exponent's decimals follow the SAME two rules the rest of this header
// already encodes: `%w.0e` shows w-7 decimals (renderValue's `e` arm), and the
// w-1 field budget sheds decimals until the value fits (clause 4). Floor of 1
// decimal — `%6.2f 1234567.89` is `1.2e+06` (7 chars, over its own field) where
// shedding to zero would have fitted, so Stata stops at one.
inline std::string fixedOverflowScientific(double v, const StataFmtSpec& sp) {
  int edec = sp.prec;
  const int wdec = sp.width - 7;
  if (sp.prec == 0) edec = wdec;
  else if (wdec < edec) edec = wdec;
  if (edec < 1) edec = 1;
  const size_t budget = fieldBudget(v, sp.width);
  std::string out;
  for (; edec >= 1; --edec) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf), "%.*e", edec, v);
    if (n < 0 || n >= static_cast<int>(sizeof(buf))) return std::string();
    out.assign(buf, static_cast<size_t>(n));
    if (out.size() <= budget) break;
  }
  return out;
}

// Callers fall back to passing the value through untouched, which is what both
// layers already did for that case.
inline std::optional<std::string> renderValue(double v, const StataFmtSpec& sp) {
  // BUG-192: `g` gets Stata's own general-format rule, not printf's `%g`.
  // Only when the spec carries an explicit width, which every Stata display
  // format does — a widthless `%.3g` keeps the historical printf path so
  // nothing that already worked moves.
  if (sp.conv == 'g' && sp.hasWidth && sp.width > 0) {
    std::string g = general(v, sp.width, sp.prec);
    if (!sp.groupThousands || g.empty()) return finishNumeric(g, sp, false);
    // BUG-296, the `c` suffix on `g`. Measured rule (exprfns_contract_oracle.log
    // rows GA/GB|%12.0gc|%9.0gc, plus wsr_oracle.log WSR|GA|%14.0gc|%10.0gc|%8.0gc):
    // the separators SPEND FIELD. Group; if the grouped form overruns the w-1
    // budget, shed one decimal at a time and re-group; and if it still does not
    // fit at ONE decimal, drop the grouping entirely rather than the last digit.
    //   %12.0gc 1234567.89 -> 1,234,567.9  (2 decimals shed to 1: 12 chars -> 11)
    //   %10.0gc 1234567.89 -> 1234567.9    (1 decimal, 1,234,567.9 is 11 > 9 —
    //                                       grouping dropped, NOT the decimal)
    //   %9.0gc  1234567.89 -> 1234568      (no decimals to shed at all)
    const size_t budget = fieldBudget(v, sp.width);
    const std::string grouped = finishNumeric(g, sp, true);
    if (grouped.size() <= budget || grouped == g) return grouped;
    const size_t dot = g.find('.');
    int decimals = dot == std::string::npos ? 0 : static_cast<int>(g.size() - dot - 1);
    for (int d = decimals - 1; d >= 1; --d) {
      const std::string cand = finishNumeric(trimNumber(fixedHalfAway(v, d)), sp, true);
      if (cand.size() <= budget) return cand;
    }
    return finishNumeric(g, sp, false);  // grouping does not fit at all
  }
  char spec[16];
  if (sp.conv == 'f') {
    std::snprintf(spec, sizeof(spec), "%%.%df", sp.prec);
  } else if (sp.conv == 'e') {
    // BUG-192 rider, same oracle capture: Stata's `%w.0e` shows w-7 decimals
    // (`%9.0e 42` is 4.20e+01), where printf's `%.0e` shows none.
    int edec = sp.prec;
    if (sp.prec == 0 && sp.hasWidth && sp.width >= 7) edec = sp.width - 7;
    std::snprintf(spec, sizeof(spec), "%%.%de", edec);
  } else if (sp.conv == 'g') {
    std::snprintf(spec, sizeof(spec), "%%.%dg", sp.prec);
  } else {
    return std::nullopt;  // unreachable given both callers' character classes
  }
  char buf[512];
  int n = std::snprintf(buf, sizeof(buf), spec, v);
  if (n < 0 || n >= static_cast<int>(sizeof(buf))) return std::nullopt;
  std::string out(buf);
  // BUG-248: the `0` flag zero-fills the VALUE, `f` only — see the flag's
  // declaration note for the %09.2f-vs-%08.0g capture that draws the boundary.
  // `e` is deliberately left alone: no %0w.pe case is in the capture, so the
  // pre-existing rendering keeps that shape unclaimed rather than guessed.
  if (sp.zeroFill && sp.conv == 'f' && sp.hasWidth &&
      static_cast<int>(out.size()) < sp.width) {
    const bool neg = !out.empty() && out[0] == '-';
    const std::string zeros(static_cast<size_t>(sp.width) - out.size(), '0');
    out = neg ? "-" + zeros + out.substr(1) : zeros + out;  // -00042.50
  }
  // BUG-296 / WS-R, the `f` family. Order is forced by the capture: the
  // OVERFLOW test is against the UNGROUPED fixed form, and grouping is applied
  // afterwards and only when it still fits the FIELD (w, not the g rule's w-1 —
  // `%9,0fc 1234567.89` is `1.234.568`, exactly 9 in a 9-wide field).
  //   %9,0fc  123456789012.5 -> 1,23e+11   (fixed is 12 > 9: scientific, no group)
  //   %14,0fc 123456789012.5 -> 123456789012 (fits 14; grouped 15 does not)
  //   %9,0fc  -1234567.89    -> -1234568   (grouped -1.234.568 is 10 > 9)
  if (sp.conv == 'f' && sp.hasWidth && sp.width > 0 &&
      static_cast<int>(out.size()) > sp.width) {
    std::string sci = fixedOverflowScientific(v, sp);
    if (!sci.empty()) return finishNumeric(sci, sp, false);
  }
  if (sp.groupThousands && sp.conv == 'f') {
    const std::string grouped = finishNumeric(out, sp, true);
    if (!sp.hasWidth || static_cast<int>(grouped.size()) <= sp.width) return grouped;
  }
  return finishNumeric(out, sp, false);
}

// BUG-248, the string()/tostring half of the `-` flag: a LEFT-justified field
// keeps its trailing blanks in the returned value (`string(42.5, "%-9.2f")` is
// `42.50    `, len 9 — oracle rows FB/FC), while the right-justified field's
// leading blanks are stripped exactly as before (the 171/171 rule in this
// file's header). So the value layer pads trailing-only, and only under `-`.
inline std::string padValueLeftJustify(std::string s, const StataFmtSpec& sp) {
  if (sp.leftJustify && sp.hasWidth && static_cast<int>(s.size()) < sp.width) {
    s.append(static_cast<size_t>(sp.width) - s.size(), ' ');
  }
  return s;
}

// The display-only half: right-justify into the field (left-justify on `-`).
// printf semantics — a value already wider than the field is never truncated.
// string()/tostring must NOT call this: the oracle's sections B and C are the
// unpadded rendering in all 171 cases.
inline std::string padToField(std::string s, const StataFmtSpec& sp) {
  if (sp.hasWidth && static_cast<int>(s.size()) < sp.width) {
    std::string pad(static_cast<size_t>(sp.width) - s.size(), ' ');
    s = sp.leftJustify ? (s + pad) : (pad + s);
  }
  return s;
}

}  // namespace piet::fmt

#endif  // PIET_STATA_FORMAT_H
