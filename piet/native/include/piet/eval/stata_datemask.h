// Stata's string-to-date translation: date(), clock(), monthly() and friends.
//
// AER scope 2, B3 stage (a). These take a STRING first argument, so like the
// rest of the string family they live only in PIET -- cobra's expression
// grammar refuses a string operand outright. The numeric half of the answer is
// still built by the shared calendar in src/data/stata_dates.h, so a date
// parsed here and a date built by mdy() in the engine are the same number by
// construction.
//
// Every rule below was captured from StataNow 19.5 BE before any of it was
// written (tests/validate/b3_funcs_oracle.{do,json}, rounds 5-6). The ones that
// are not guessable:
//
//   * A mask is CASE-SENSITIVE. "YMD" parses, "ymd" answers missing -- and the
//     time components really are lowercase (h, m, s), so it is not simply
//     "uppercase everything".
//   * Separators are irrelevant and interchangeable: "2020-03-15", "2020/03/15",
//     "2020.03.15" and "  2020-03-15  " all give the same day.
//   * With NO separators at all, Stata falls back to fixed widths -- "20200315"
//     under "YMD" works, and "2020315" is missing because 7 digits cannot be
//     split 4/2/2.
//   * An unparseable string is MISSING with rc 0. It is not an error. Writing an
//     "honest error" here would be a gate-1 violation, which is what the plan
//     originally prescribed (see the plan's section 7.7).
//   * A trailing "am"/"pm" token is honoured even though no mask letter asks for
//     it: clock("2020-03-15 1:45 pm","YMDhm") is 13:45, and 12am/12pm follow the
//     usual convention (00:30 and 12:30).
//   * A two-digit year is missing unless the mask says 19Y/20Y or a topyear was
//     given; with topyear the window is [topyear-99, topyear].
#ifndef PIET_NATIVE_EVAL_STATA_DATEMASK_H
#define PIET_NATIVE_EVAL_STATA_DATEMASK_H

#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "data/stata_dates.h"

namespace piet::eval::datemask {

enum class Comp { Year4, Year19, Year20, Month, Day, Hour, Minute, Second,
                  Quarter, Half, Week, Skip };

// What the caller wants built out of the components.
enum class Kind { Daily, Clock, ClockLeap, Monthly, Quarterly, Weekly,
                  Halfyearly, Yearly };

inline const double kNA = std::numeric_limits<double>::quiet_NaN();

inline bool parse_mask(const std::string& m, std::vector<Comp>& out) {
  for (size_t i = 0; i < m.size();) {
    if (m[i] == ' ' || m[i] == '\t') { ++i; continue; }
    if (i + 2 < m.size() && m[i] == '1' && m[i + 1] == '9' && m[i + 2] == 'Y') {
      out.push_back(Comp::Year19); i += 3; continue;
    }
    if (i + 2 < m.size() && m[i] == '2' && m[i + 1] == '0' && m[i + 2] == 'Y') {
      out.push_back(Comp::Year20); i += 3; continue;
    }
    switch (m[i]) {
      case 'Y': out.push_back(Comp::Year4); break;
      case 'M': out.push_back(Comp::Month); break;
      case 'D': out.push_back(Comp::Day); break;
      case 'h': out.push_back(Comp::Hour); break;
      case 'm': out.push_back(Comp::Minute); break;
      case 's': out.push_back(Comp::Second); break;
      case 'Q': out.push_back(Comp::Quarter); break;
      case 'H': out.push_back(Comp::Half); break;
      case 'W': out.push_back(Comp::Week); break;
      case '#': out.push_back(Comp::Skip); break;
      default: return false;   // anything else, lowercase 'y' included
    }
    ++i;
  }
  return !out.empty();
}

struct Token {
  std::string text;
  bool numeric = false;
  bool dot_prefixed = false;  // immediately preceded by '.', for "56.500"
};

inline std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  bool dot = false;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isdigit(c)) {
      size_t j = i;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      out.push_back({s.substr(i, j - i), true, dot});
      dot = false;
      i = j;
    } else if (std::isalpha(c)) {
      size_t j = i;
      while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
      out.push_back({s.substr(i, j - i), false, dot});
      dot = false;
      i = j;
    } else {
      dot = (c == '.');
      ++i;
    }
  }
  return out;
}

// Month names: Stata takes "mar", "March" and any case.
inline int month_from_name(const std::string& w) {
  static const char* kNames[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec"};
  if (w.size() < 3) return 0;
  std::string k;
  for (size_t i = 0; i < 3; ++i) k.push_back(static_cast<char>(
      std::tolower(static_cast<unsigned char>(w[i]))));
  for (int i = 0; i < 12; ++i) if (k == kNames[i]) return i + 1;
  return 0;
}

inline bool is_ampm(const std::string& w, bool& pm) {
  if (w.size() != 2) return false;
  const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(w[0])));
  const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(w[1])));
  if (b != 'm') return false;
  if (a == 'a') { pm = false; return true; }
  if (a == 'p') { pm = true; return true; }
  return false;
}

// Width of a component when the string carries no separators at all.
inline int fixed_width(Comp c) {
  switch (c) {
    case Comp::Year4: return 4;
    case Comp::Year19: case Comp::Year20: return 2;
    case Comp::Quarter: case Comp::Half: return 1;
    case Comp::Skip: return 0;
    default: return 2;   // M D h m s W
  }
}

// The whole translation. Returns the scale value, or missing.
inline double translate(const std::string& s, const std::string& mask, Kind kind,
                        bool have_top, double topyear) {
  std::vector<Comp> comps;
  if (!parse_mask(mask, comps)) return kNA;

  std::vector<Token> toks = tokenize(s);

  // "am"/"pm" is an extra token the mask never mentions; lift it out first.
  bool pm = false, have_ampm = false;
  for (size_t i = 0; i < toks.size(); ++i) {
    bool p = false;
    if (!toks[i].numeric && is_ampm(toks[i].text, p) && month_from_name(toks[i].text) == 0) {
      pm = p; have_ampm = true;
      toks.erase(toks.begin() + static_cast<long>(i));
      break;
    }
  }

  // Fractional seconds arrive as a separate dot-prefixed token, because '.' is
  // an ordinary separator ("2020.03.15" has to keep working). Fold it back only
  // when the mask actually ends in seconds and the extra token is the last one.
  double frac = 0.0;
  if (toks.size() == comps.size() + 1 && !comps.empty() &&
      comps.back() == Comp::Second && toks.back().dot_prefixed &&
      toks.back().numeric) {
    const std::string& f = toks.back().text;
    frac = std::stod("0." + f);
    toks.pop_back();
  }

  // Compact period spellings put the scale letter INSIDE the string --
  // "2020q1", "2020m3", "2020w10", "2020h1" all parse in Stata. Tokenising sees
  // that letter as a third token, so drop stray SINGLE letters when there is
  // exactly one token too many. Month names survive: "mar" is three characters.
  while (toks.size() > comps.size()) {
    size_t drop = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
      if (!toks[i].numeric && toks[i].text.size() == 1) { drop = i; break; }
    }
    if (drop == toks.size()) break;
    toks.erase(toks.begin() + static_cast<long>(drop));
  }

  // No separators at all: split the single run by the component widths.
  if (toks.size() == 1 && toks[0].numeric && comps.size() > 1) {
    int need = 0;
    for (Comp c : comps) need += fixed_width(c);
    const std::string& d = toks[0].text;
    if (static_cast<int>(d.size()) != need) return kNA;
    std::vector<Token> split;
    size_t at = 0;
    for (Comp c : comps) {
      const int w = fixed_width(c);
      split.push_back({d.substr(at, static_cast<size_t>(w)), true, false});
      at += static_cast<size_t>(w);
    }
    toks.swap(split);
  }

  if (toks.size() != comps.size()) return kNA;

  double Y = kNA, Mo = 1, D = 1, h = 0, mi = 0, sec = 0, Q = kNA, Hf = kNA, W = kNA;
  for (size_t i = 0; i < comps.size(); ++i) {
    const Comp c = comps[i];
    const Token& t = toks[i];
    if (c == Comp::Skip) continue;
    if (c == Comp::Month && !t.numeric) {
      const int m = month_from_name(t.text);
      if (m == 0) return kNA;
      Mo = m;
      continue;
    }
    if (!t.numeric || t.text.empty()) return kNA;
    double v = 0;
    for (char ch : t.text) v = v * 10 + (ch - '0');
    switch (c) {
      case Comp::Year4:
        if (t.text.size() == 2) {
          if (!have_top) return kNA;   // measured: missing without a topyear
          const double ty = std::floor(topyear);
          v = ty - std::fmod(std::fmod(ty - v, 100.0) + 100.0, 100.0);
        }
        Y = v; break;
      case Comp::Year19: Y = 1900 + v; break;
      case Comp::Year20: Y = 2000 + v; break;
      case Comp::Month:  Mo = v; break;
      case Comp::Day:    D = v; break;
      case Comp::Hour:   h = v; break;
      case Comp::Minute: mi = v; break;
      case Comp::Second: sec = v; break;
      case Comp::Quarter: Q = v; break;
      case Comp::Half:    Hf = v; break;
      case Comp::Week:    W = v; break;
      case Comp::Skip: break;
    }
  }
  // A time-only mask has no year at all, and Stata answers a time-of-day on the
  // %tc line rather than missing: clock("13:45","hm") is 49500000, i.e. 13:45 on
  // 1960-01-01. Every other kind needs a year.
  bool has_year = false;
  for (Comp c : comps)
    if (c == Comp::Year4 || c == Comp::Year19 || c == Comp::Year20) has_year = true;
  if (std::isnan(Y)) {
    if (!has_year && (kind == Kind::Clock || kind == Kind::ClockLeap)) {
      Y = 1960; Mo = 1; D = 1;
    } else {
      return kNA;
    }
  }
  if (have_ampm) {
    if (h < 1 || h > 12) return kNA;
    if (pm && h < 12) h += 12;
    if (!pm && h == 12) h = 0;
  }

  switch (kind) {
    case Kind::Yearly:     return Y;
    case Kind::Monthly:    return cecon::stata_ym(Y, Mo);
    case Kind::Quarterly:  return cecon::stata_yq(Y, Q);
    case Kind::Halfyearly: return cecon::stata_yh(Y, Hf);
    case Kind::Weekly:     return cecon::stata_yw(Y, W);
    case Kind::Daily:      return cecon::stata_mdy(Mo, D, Y);
    case Kind::Clock:
    case Kind::ClockLeap: {
      const double day = cecon::stata_mdy(Mo, D, Y);
      if (std::isnan(day)) return kNA;
      const double within = cecon::stata_hms(h, mi, sec + frac);
      if (std::isnan(within)) return kNA;
      const double base = (kind == Kind::ClockLeap) ? cecon::stata_Cofd(day)
                                                    : cecon::stata_cofd(day);
      if (std::isnan(base)) return kNA;
      return base + within;
    }
  }
  return kNA;
}

}  // namespace piet::eval::datemask

#endif  // PIET_NATIVE_EVAL_STATA_DATEMASK_H
