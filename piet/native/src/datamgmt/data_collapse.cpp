// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data collapse.
#include "data_mutation_internal.h"
#include "piet/handlers/observations.h"
namespace piet::handlers::data_mutation_detail {


// A statistic as each flavor's parser takes it, to the engine's name.
// collapse: the listed words, SEMean /
// SEBinomial / SEPoisson by abbreviation, p1..p99; else `(avg) invalid
// statistic` r(198) (cz_c_err_stat, cz_c_err_p100, cz_c_err_p0). gcollapse
// (ParseList and the quantile check): its own words, the
// short spellings sem/seb/sep/var/skew/kurt, p# below 100 but not p0; else
// `Invalid stat` r(110) (cz_g_err_stat, cz_g_err_p100). fcollapse (ftools)
// serves no SE statistic: r(110) (cz_f_semean_g, cz_f_err_stat).
// BUG-1098: every flavor reads the statistic as typed, so (MEAN), (Mean),
// (P50) and (SEM) are r(198) in collapse and r(110) in gcollapse and
// fcollapse (StataBE 19.5; this build lower-cased them and served them). And
// fcollapse checks a quantile only against `^p[0-9]+$` (fcollapse):
// (p0) is the minimum, (p100) and past it the maximum, (p050) the median, all
// at rc 0, where collapse refuses p0/p100 r(198) and gcollapse r(110).
std::optional<std::string> canonicalCollapseStat(const std::string& raw, const std::string& flavor,
                                                 std::string* rcOut) {
  auto prefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return raw.size() >= minLen && raw.size() <= f.size() && f.compare(0, raw.size(), raw) == 0;
  };
  static const std::set<std::string> kCollapseWords = {
      "mean", "median", "sd", "sum", "rawsum", "count", "max", "min",
      "iqr", "first", "firstnm", "last", "lastnm", "percent"};
  if (flavor == "gcollapse") {
    *rcOut = "110";
    static const std::set<std::string> kGtools = {
        "sum", "nansum", "mean", "geomean", "sd", "variance", "cv", "max", "min", "range",
        "count", "median", "iqr", "percent", "first", "last", "firstnm", "lastnm", "semean",
        "sebinomial", "sepoisson", "nunique", "nmissing", "skewness", "kurtosis", "gini",
        "rawsum", "rawnansum"};
    std::string st = raw;
    if (st == "var") st = "variance";
    if (st == "sem") st = "semean";
    if (st == "seb") st = "sebinomial";
    if (st == "sep") st = "sepoisson";
    if (st == "skew") st = "skewness";
    if (st == "kurt") st = "kurtosis";
    if (kGtools.count(st)) return st;
    static const std::regex reQ(R"(^p([0-9][0-9]?(\.[0-9]+)?)$)");
    if (std::regex_match(st, reQ) && st != "p0") return st;
    return std::nullopt;
  }
  *rcOut = flavor == "fcollapse" ? "110" : "198";
  if (kCollapseWords.count(raw)) return raw;
  if (flavor == "fcollapse") {
    static const std::regex reQ(R"(^p[0-9]+$)");
    return std::regex_match(raw, reQ) ? std::optional<std::string>(raw) : std::nullopt;
  }
  if (prefixOf("semean", 3)) return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("semean");
  if (prefixOf("sebinomial", 3))
    return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("sebinomial");
  if (prefixOf("sepoisson", 3))
    return flavor == "fcollapse" ? std::nullopt : std::optional<std::string>("sepoisson");
  if (raw.size() >= 2 && raw[0] == 'p' &&
      std::all_of(raw.begin() + 1, raw.end(), [](unsigned char c) { return std::isdigit(c); })) {
    const int v = std::atoi(raw.c_str() + 1);
    if (v >= 1 && v <= 99) return raw;
  }
  return std::nullopt;
}


CollapseClist parseCollapseClist(const std::string& statRaw, const std::vector<std::string>& byVars,
                                 SessionState& session, const std::string& flavor) {
  CollapseClist out;
  auto refuse = [&](const std::string& msg) {
    out.refusal = engineErrorBlock("collapse: " + msg, "syntax_error");
    return out;
  };
  auto refuseRc = [&](const std::string& msg, const std::string& rc) {
    out.refusal = ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                         std::nullopt, "r(" + rc + ")"));
    return out;
  };
  // Tokens: a parenthesised statistic, a lone `=`, or a word.
  std::vector<std::string> toks;
  for (size_t i = 0; i < statRaw.size();) {
    const char c = statRaw[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (c == '(') {
      const size_t close = statRaw.find(')', i);
      if (close == std::string::npos)
        return refuse("`" + statRaw.substr(i) + "' -- unmatched parenthesis in the statistic");
      toks.push_back(statRaw.substr(i, close - i + 1));
      i = close + 1;
    } else if (c == '=') {
      toks.emplace_back("=");
      ++i;
    } else {
      size_t j = i;
      while (j < statRaw.size() && !std::isspace(static_cast<unsigned char>(statRaw[j])) &&
             statRaw[j] != '=' && statRaw[j] != '(')
        ++j;
      toks.push_back(statRaw.substr(i, j - i));
      i = j;
    }
  }
  auto isStat = [](const std::string& t) { return !t.empty() && t.front() == '('; };
  std::string stat = "mean";
  std::vector<std::string> parts;
  // A statistic with no variable after it reaches `syntax varlist` with an
  // empty list: "varlist required" r(100) (cz_c_err_novar `(mean), by(g)`).
  bool statOpen = false;
  size_t itemsAtStat = 0;
  for (size_t k = 0; k < toks.size(); ++k) {
    const std::string& t = toks[k];
    if (isStat(t)) {
      if (statOpen && out.items.size() == itemsAtStat) return refuseRc("varlist required", "100");
      const std::string rawStat = trim(t.substr(1, t.size() - 2));
      std::string rc;
      const std::optional<std::string> canon = canonicalCollapseStat(rawStat, flavor, &rc);
      if (!canon.has_value()) {
        // collapse names a one-word `p...` statistic by what
        // follows the p: `(100) invalid statistic`, `(xx) invalid statistic`
        // for (pxx). gtools adds a hint for (p0).
        std::string shown = rawStat;
        if (flavor == "collapse" && !shown.empty() && shown[0] == 'p' &&
            shown.find_first_of(" \t") == std::string::npos)
          shown.erase(0, 1);
        if (flavor == "gcollapse" && rawStat == "p0") shown += "; maybe you meant 'min'?";
        return refuseRc(flavor == "collapse" ? "(" + shown + ") invalid statistic"
                                             : "Invalid stat: (" + shown + ")",
                        rc);
      }
      stat = *canon;
      // gtools' sepoisson does not saturate k at 2^31 the way collapse_17's
      // builtin does (cz_g_sepoisson_g_v6c): the engine's uncapped spelling.
      parts.push_back("(" + std::string(flavor == "gcollapse" && stat == "sepoisson"
                                            ? "sepoisson_nocap" : stat) + ")");
      statOpen = true;
      itemsAtStat = out.items.size();
      continue;
    }
    if (t == "=") {
      const std::string next = k + 1 < toks.size() ? " " + toks[k + 1] : std::string();
      return refuse("`=" + next + "' has no target name; the target form is "
                    "`(stat) newvar = oldvar'");
    }
    if (k + 1 < toks.size() && toks[k + 1] == "=") {
      if (k + 2 >= toks.size() || toks[k + 2] == "=" || isStat(toks[k + 2]))
        return refuse("`" + t + " =' has no source variable; the target form is "
                      "`(stat) newvar = oldvar'");
      if (!strutil::isValidStataIdentifier(t))
        return refuse("`" + t + "' invalid name -- " + strutil::stataIdentifierRuleText());
      out.items.push_back({stat, t, toks[k + 2]});
      parts.push_back(t + "=" + toks[k + 2]);
      k += 2;
      continue;
    }
    if (t.find_first_of("*?~-") != std::string::npos) {
      auto vr = loops::expandVarlist(t, session);
      if (vr.isError) {
        out.refusal = vr.error;
        return out;
      }
      for (const std::string& nm : vr.tokens) {
        out.items.push_back({stat, nm, nm});
        parts.push_back(nm);
      }
      continue;
    }
    out.items.push_back({stat, t, t});
    parts.push_back(t);
  }
  if (statOpen && out.items.size() == itemsAtStat) return refuseRc("varlist required", "100");
  if (out.items.empty()) return refuse("no variables specified");
  for (size_t i = 0; i < out.items.size(); ++i) {
    const CollapseItem& a = out.items[i];
    for (size_t j = i + 1; j < out.items.size(); ++j) {
      const CollapseItem& b = out.items[j];
      if (a.target == b.target)
        return refuse("error: " + a.target + " = (" + a.stat + ") " + a.source + " and " +
                      b.target + " = (" + b.stat + ") " + b.source + " -- name conflict");
    }
    if (std::find(byVars.begin(), byVars.end(), a.target) != byVars.end())
      return refuse(a.target + " may not be both target and by()");
  }
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.statCobra += " ";
    out.statCobra += parts[i];
  }
  return out;
}


// The statistic as each flavor spells it in the result's variable label.
// collapse hands back `p 50` for (median) and
// `p #` for (p#), the full word for an SE abbreviation; the label is
// `(fcn) lab`: w2au_collapse_family.log cz_c_median_g_v2
// `[(p 50) xb]`, cz_c_p10_g_v2 `[(p 10) xb]`. gtools labels with the stat as
// its parser normalised it (gcollapse.ado ParseList, labelformat
// `(#stat#) #sourcelabel#` :1024): cz_g_median `[(median) byte source]`,
// cz_g_p50 `[(p50) byte source]`. ftools: sprintf("(%s) %s", stat, var)
// (fcollapse_main), the stat as typed once ParseList has taken a
// leading `raw` off it (fcollapse): (rawsum) labels `(sum) x`.
std::string collapseLabelStat(const std::string& stat, const std::string& flavor) {
  if (flavor == "collapse") {
    if (stat == "median") return "p 50";
    if (stat.size() >= 2 && stat[0] == 'p' &&
        std::all_of(stat.begin() + 1, stat.end(), [](unsigned char c) { return std::isdigit(c); }))
      return "p " + stat.substr(1);
  }
  if (flavor == "fcollapse" && stat == "rawsum") return "sum";
  return stat;
}


std::vector<CollapseResultMeta> collapseResultMeta(const std::vector<CollapseItem>& items,
                                                   const std::vector<std::string>& byVars,
                                                   const std::string& flavor, const std::string& wkind,
                                                   bool clabel, SessionState& session) {
  const bool weighted = !wkind.empty();
  std::vector<CollapseResultMeta> out;
  std::vector<VarInfo> fallbackCard;
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card == nullptr) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return out;
    fallbackCard = ds->varlist();
    card = &fallbackCard;
  }
  auto findVar = [&](const std::string& nm) -> const VarInfo* {
    for (const VarInfo& v : *card)
      if (v.name == nm) return &v;
    return nullptr;
  };
  // The source's variable label as every reader resolves it: a session
  // label over the card's own (SessionState::varlistWithMetaFrom).
  auto sourceLabel = [&](const VarInfo& v) -> std::string {
    auto it = session.varMeta.find(v.name);
    if (it != session.varMeta.end() && it->second.label.has_value()) return *it->second.label;
    return v.label;
  };
  const bool gtools = flavor == "gcollapse";
  // gcollapse's smart-target pass (gcollapse.ado parse_keep_drop):
  // a target that exists but is no source is renamed away first; then, in
  // target order, the FIRST target of each source that is not itself a target,
  // not in memory and not a by() key takes over the source's column by
  // `rename`. Such a target, like a target named as its own source, "exists",
  // and parse_ok_astarget (:1524-1613) keeps its storage when it can hold the
  // statistic. That is how (count) of a double source stays double
  // (cz_g_count_g_v6 `double|%8.0g`, cz_g_noby_v3 `n|double`) while a new
  // count target is long (or double when the source also feeds a non-count
  // statistic, the `upgrade` rule :1352-1362).
  std::vector<bool> gReuses(items.size(), false);
  std::set<std::string> gUpgrade;
  if (gtools) {
    std::set<std::string> sources, targets, mem;
    for (const CollapseItem& it : items) {
      sources.insert(it.source);
      targets.insert(it.target);
    }
    for (const VarInfo& v : *card)
      if (!(targets.count(v.name) && !sources.count(v.name))) mem.insert(v.name);
    std::set<std::string> uniqLeft = sources;
    for (size_t k = 0; k < items.size(); ++k) {
      const CollapseItem& it = items[k];
      if (it.target == it.source && mem.count(it.target)) gReuses[k] = true;
      if (!uniqLeft.count(it.source)) continue;
      uniqLeft.erase(it.source);
      if (!targets.count(it.source) && !mem.count(it.target) &&
          std::find(byVars.begin(), byVars.end(), it.source) == byVars.end()) {
        mem.erase(it.source);
        mem.insert(it.target);
        gReuses[k] = true;
      }
    }
    for (const CollapseItem& it : items)
      if (it.stat != "count" && it.stat != "freq" && it.stat != "nunique" && it.stat != "nmissing")
        gUpgrade.insert(it.source);
  }
  auto sourceType = [&](const std::string& nm) -> std::optional<std::string> {
    auto it = session.varMeta.find(nm);
    if (it != session.varMeta.end() && it->second.stataType.has_value() &&
        !it->second.stataType->empty())
      return *it->second.stataType;
    if (session.implicitFloatVars.count(nm) != 0) return std::string("float");
    return std::nullopt;
  };
  const std::string cType = storage::resolveCreateType(session, "");
  for (size_t k = 0; k < items.size(); ++k) {
    const CollapseItem& item = items[k];
    const std::string& stat = item.stat;
    std::string src = item.source;
    const size_t dot = src.rfind('.');
    if (dot != std::string::npos) src = src.substr(dot + 1);  // L.x: x's format (Setnf)
    const VarInfo* v = findVar(src);
    if (v == nullptr) continue;
    CollapseResultMeta m;
    m.target = item.target;
    m.format = session.displayFormat(*v);
    // The result's variable label. collapse: `(fcn) lab`, lab the source as
    // written (the graphbar `_percent`/`_freq`
    // sources read "percent"/"frequency"). gcollapse: `(stat) <source label,
    // else source name>` (gcollapse; cz_g_tgt_064_v4 `nobs`
    // `[(count) xi]`). By-keys keep their own label.
    {
      const std::string ls = collapseLabelStat(stat, flavor);
      if (gtools) {
        const std::string sl = sourceLabel(*v);
        m.label = "(" + ls + ") " + (sl.empty() ? item.source : sl);
      } else if (flavor == "collapse" && stat == "percent" && item.source == "_percent") {
        m.label = "percent";
      } else if (flavor == "collapse" && stat == "count" && item.source == "_freq") {
        m.label = "frequency";
      } else if (clabel) {
        // collapse's `clabel` (:407-411): `fcn of lab` (BUG-1100: StataBE 19.5
        // `mean of xf`, `p 99 of xf`, `sebinomial of xf`).
        m.label = ls + " of " + item.source;
      } else {
        m.label = "(" + ls + ") " + item.source;
      }
    }
    // gtools and ftools display a count as %8.0g (gcollapse,
    // fcollapse_main); collapse keeps the source's format.
    if (flavor != "collapse" && (stat == "count" || stat == "nunique" || stat == "nmissing"))
      m.format = "%8.0g";
    const std::optional<std::string> st = sourceType(src);
    if (v->dtype == "factor") {
      m.type = st;  // first/last of a string keep its str#
    } else if (flavor == "fcollapse") {
      // ftools' infer_type (fcollapse_main): max, min, first, last,
      // firstnm and lastnm keep the source's type, count is long, every other
      // statistic double. runCollapse then compresses each one (compress_type)
      // unless `nocompress` (BUG-1100: StataBE 19.5's fcollapse (mean) of a
      // float is a double, where this build stored the collapse rule's float).
      // A pw/iw count is long too, so a non-integer one is truncated when it is
      // stored (StataBE 19.5, ftools 2.49.0: counts 3.5 and 1.5 are byte 3 and
      // 1, long with nocompress).
      if (stat == "max" || stat == "min" || stat == "first" || stat == "last" ||
          stat == "firstnm" || stat == "lastnm")
        m.type = st;
      else
        m.type = std::string(stat == "count" ? "long" : "double");
    } else if (stat == "sum" || stat == "rawsum" || (stat == "percent" && !gtools)) {
      // gcollapse types percent by its generic rule (cz_g_percent_g float).
      m.type = std::string("double");
    } else if (stat == "count" && gtools && !weighted) {
      const bool upgrade = gUpgrade.count(item.source) != 0;
      const bool keeps = gReuses[k] && (st == std::string("long") || st == std::string("double"));
      m.type = keeps ? *st : std::string(upgrade ? "double" : "long");
    } else if (stat == "count") {
      // collapse: fw/iw/pw count is double, an aweighted one long.
      m.type = std::string(weighted && !(wkind == "aw" && !gtools) ? "double" : "long");
    } else if (stat == "max" || stat == "min" || stat == "first" || stat == "last" ||
               stat == "firstnm" || stat == "lastnm") {
      m.type = st;
    } else if (st == std::string("double") || st == std::string("long")) {
      m.type = std::string("double");
    } else {
      m.type = cType;
    }
    // FU-1033 / BUG-961: a float result is claimed float, and runCollapse
    // re-stores its values as float (one `eval --store-type float`) before the
    // claim is written, so the frame holds what the label says.
    if (m.type == std::string()) m.type.reset();
    out.push_back(std::move(m));
  }
  return out;
}


// fcollapse_main.mata compress_type, applied unless `nocompress`: the
// smallest storage type that holds a result. A byte stays; a float or double
// holding a non-integer stays; otherwise -127..100 is byte, -32767..32740 int,
// -2147483647..2147483620 long over the nonmissing values, and anything else
// (all missing included) keeps `type`. BUG-1100, StataBE 19.5: (sum) of
// integers summing to 3/7/5 is byte, to 40300 long, (count) byte, (max) of a
// float holding integers long; with nocompress double, double, long, float.
std::string fcollapseCompressedType(const double* x, int64_t n, const std::string& type) {
  if (type == "byte") return type;
  double lo = 0.0, hi = 0.0;
  bool any = false, fractional = false;
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(x[i])) continue;
    if (std::trunc(x[i]) != x[i]) fractional = true;
    lo = any ? std::min(lo, x[i]) : x[i];
    hi = any ? std::max(hi, x[i]) : x[i];
    any = true;
  }
  if (!any || ((type == "float" || type == "double") && fractional)) return type;
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return type;
}


// A result's claim after compress_type. A max/min/first/last of a source whose
// type nothing recorded has no claim; its values are compressed as a double's
// would be, and an integer type is claimed only when they all fit one.
void compressFcollapseResult(CollapseResultMeta& m, const double* x, int64_t n) {
  const std::string base = m.type.value_or("double");
  const std::string t = fcollapseCompressedType(x, n, base);
  if (m.type.has_value() || t != base) m.type = t;
}


// Whether every value of x is kept by a store as `type`: what `recast` checks
// before it changes anything (it changes nothing when one value would change).
bool valuesFitType(const double* x, int64_t n, const std::string& type) {
  for (int64_t i = 0; i < n; ++i) {
    const double v = x[i];
    if (std::isnan(v) || type == "double") continue;
    if (type == "float") {
      if (static_cast<double>(static_cast<float>(v)) != v) return false;
      continue;
    }
    const double lo = type == "byte" ? -127 : type == "int" ? -32767 : -2147483647.0;
    const double hi = type == "byte" ? 100 : type == "int" ? 32740 : 2147483620.0;
    if (std::trunc(v) != v || v < lo || v > hi) return false;
  }
  return true;
}


// fcollapse's `smart` test (fcollapse): the data's sort claim starts
// with the first by() variable's name, as text (BUG-1100, StataBE 19.5: data
// sorted by gg pass for by(g), and one observation sorted by g passes), or
// there are two observations or more and none has a first by() value below the
// one before it (a string by its bytes, a numeric missing above every number).
// Without by() fcollapse tests a constant of its own, which no sort claim
// names and which is sorted once there are two observations.
bool fcollapseSortedOnFirstBy(SessionState& session, const std::vector<std::string>& byVars) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return false;
  if (!byVars.empty()) {
    if (!ds->has(byVars[0])) return false;
    if (ds->sortedBy().rfind(byVars[0], 0) == 0) return true;
  }
  if (ds->nRows() < 2) return false;
  if (byVars.empty()) return true;
  const cecon::Column& c = ds->col(byVars[0]);
  const int64_t n = ds->nRows();
  if (ds->dtype(byVars[0]) == "factor") {
    auto text = [&](int64_t r) -> std::string {
      const int32_t code = c.codes()[r];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    };
    for (int64_t i = 1; i < n; ++i)
      if (text(i) < text(i - 1)) return false;
    return true;
  }
  if (ds->dtype(byVars[0]) != "f64") return false;
  const double* x = c.f64();
  for (int64_t i = 1; i < n; ++i)
    if (!std::isnan(x[i]) && (std::isnan(x[i - 1]) || x[i] < x[i - 1])) return false;
  return true;
}


// The comma that starts collapse's options: the first one outside parentheses and
// quotes (eval::findUnquotedComma, the tokenizer's own). `raw.find(',')` cut
// `collapse ... if strpos(s, "o") > 0, by(g)` inside strpos(), and the stat list
// became `... nx=x, "o") > 0` (rc 1 `no column: x,`; w2aj_qualifiers
// qf_if_collapse_strpos).
size_t collapseOptionComma(const std::string& raw) {
  const long long c = eval::findUnquotedComma(raw);
  return c < 0 ? std::string::npos : static_cast<size_t>(c);
}
}
namespace piet::handlers {
using namespace data_mutation_detail;


ojson runCollapse(const std::string& rawLine, const std::optional<std::string>& ifExpr,
                  SessionState& session,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  const std::optional<std::string>& weightVar,
                  const std::optional<std::string>& inRange, const std::string& flavor,
                  const std::optional<std::string>& weightTypeIn) {
  // ONESHOT-LASTFLUSH: this handler used to open session.datastore() on entry and
  // then use nothing from it but `ds->path()`. On a virtual head that open is a
  // FULL materialization of the pre-collapse frame — 14 of the flushes left after
  // the §38 wave, and the most expensive ones in the run (the collapsed result is
  // ~5% of its input). *currentHbin() answers the same question without opening
  // anything; the same substitution runRename and runGsort already made.
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string raw = trim(rawLine);

  // BUG-042 (collapse if-misparse): `rawLine` is `parsed.raw` — the FULL,
  // untouched original command text, which still contains any `if COND`
  // clause verbatim (the tokenizer only strips `if` out of the internal
  // varlist/opts split it builds for `parsed.varlist`/`parsed.opts`; the
  // `raw` field it also carries is deliberately left whole). This handler
  // re-derives its own stat-varlist directly from `raw` via `reCollapse`
  // below, so an unstripped `if id<=4` used to leak through as bogus
  // "variable name" tokens ("if", the condition's own identifiers) reaching
  // cobra's `--stat` argument ("engine error: no column: if"). Strip the
  // SAME `if` clause back out here (mirroring tokenizer.cpp's own if-split:
  // only the portion before the first top-level comma is examined, so a
  // `by(...)` option tail is never touched) before parsing anything else.
  // `ifExpr` is already correctly threaded through to cobra's own
  // `--keepif` below — this fixes the PARSE, not the filtering itself.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitIfCollapse(R"(\s+if\s+)", std::regex::icase);
    std::smatch ifm;
    if (std::regex_search(head, ifm, reSplitIfCollapse)) {
      head = head.substr(0, static_cast<size_t>(ifm.position(0)));
    }
    raw = trim(head) + tail;
  }

  // BS-090 (BETA Phase G): `in a/b` sits in the same pre-comma head as `if`
  // (and, same as the if-strip above, would otherwise leak "in"/the range
  // token through as bogus stat-varlist entries). The `if`-strip above
  // already removes any `in` clause that happens to come AFTER `if` in the
  // raw text (it truncates the whole head at the `if` keyword), so this is
  // a no-op in that ordering — it only actually fires when `in` precedes
  // `if` (or `if` is absent). `inRange` is already threaded to
  // applyIfAndInFilter below — this fixes the PARSE, not the filtering.
  if (inRange.has_value() && !inRange->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    static const std::regex reSplitInCollapse(R"(\s+in\s+\S+)", std::regex::icase);
    head = std::regex_replace(head, reSplitInCollapse, "");
    raw = trim(head) + tail;
  }

  // Step-1 D3: the `[fw=w]` weight clause also sits in the pre-comma head
  // and (like the `if` above) would otherwise leak into cobra's --stat argument
  // ("engine error: no column: w]"). The weight is already threaded to cobra via
  // --fweight below, so strip the clause from the stat head here. Gated on
  // weightVar so the no-weight collapse path stays byte-identical.
  if (weightVar.has_value() && !weightVar->empty()) {
    size_t commaPos = collapseOptionComma(raw);
    std::string head = (commaPos == std::string::npos) ? raw : raw.substr(0, commaPos);
    std::string tail = (commaPos == std::string::npos) ? std::string() : raw.substr(commaPos);
    // The tokenizer's own weight grammar (eval::removeWeightClause): `[w = x]`,
    // `[cell=x]` and every other spelling tokenize() read as this weight.
    head = eval::removeWeightClause(head);
    raw = trim(head) + tail;
  }

  std::string byVal;
  {
    static const std::regex reBy(R"(\bby\s*\(\s*([^)]+)\))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(raw, m, reBy)) byVal = trim(m.str(1));
  }
  std::vector<std::string> byVars;
  {
    std::istringstream iss(byVal);
    std::string tok;
    while (iss >> tok) byVars.push_back(tok);
  }

  // stat-varlist: everything between `collapse` and the option comma (or end).
  // BUG-962: this used to stop only at `, by(`, so `, cw`, `, fast by(g)` and a
  // by()-less `, cw` left `x,` and `cw` in the stat-varlist ("no column: x,",
  // rc 1). No statistic, target or variable name holds a comma, and the `if`,
  // `in` and weight clauses were stripped above.
  static const std::regex reCollapse(R"(\bcollapse\b(.+?)(?:,|$))", std::regex::icase);
  std::smatch cm;
  if (!std::regex_search(raw, cm, reCollapse)) {
    return engineErrorBlock(
        "collapse: cannot parse stat varlist from command — expected "
        "`collapse (stat) varlist [(stat) varlist ...] [, by(varlist)]`");
  }
  std::string statRaw = trim(cm.str(1));
  if (statRaw.empty()) return engineErrorBlock("collapse: empty stat-varlist");

  // L1/M3 (CSV-shim retirement): cobra's native `collapse --by` now accepts
  // factor by-vars directly — the output by-col stays factor with a fresh,
  // self-consistent dictionary in first-seen output order (the same
  // convention `convert --factor` produces). The former workaround — dump the
  // whole frame to CSV -> convert -> materialize an egen-group() f64 proxy per
  // factor by-var -> collapse on the proxy -> restore the level text via a
  // second CSV round-trip — is gone; by-vars pass straight through to `--by`.
  //
  // The option words, read off the keys the dispatcher resolved: an accepted
  // key is the option's full name (collapse and fcollapse take no
  // abbreviation, dispatcher.cpp kOptMinCompleteLater), so an option the net
  // let through is never left unread here. The net already confined each word
  // to its flavor: clabel to collapse; nocompress, append and smart to
  // fcollapse.
  std::set<std::string> optWords;
  for (const auto& kv : opts) optWords.insert(lower(kv.first));
  const bool cw = optWords.count("cw") != 0;
  const bool fcAppend = flavor == "fcollapse" && optWords.count("append") != 0;
  // fcollapse's `smart` (fcollapse, ftools 2.49.0): on data already
  // sorted on the first by() variable it runs `collapse ..., by() fast cw`
  // instead, with collapse's checks and rules, and then ends while its own
  // `preserve` is still live -- so without `fast` the data come back as they
  // were (BUG-1100: StataBE 19.5 leaves N and k unchanged at rc 0), and with it
  // they are collapse's result. Unsorted data take the fcollapse path.
  std::string flavorRun = flavor;
  bool smartNoop = false;
  if (flavor == "fcollapse" && optWords.count("smart") != 0 && !fcAppend &&
      fcollapseSortedOnFirstBy(session, byVars)) {
    // fcollapse.ado hands collapse the weight as `[aweightw]`: its `loc eqsign
    // =` leaves eqsign empty. So any weight on this path is collapse's syntax
    // error (StataBE 19.5, ftools 2.49.0: aw, fw, iw and pw, with and without
    // fast, are `invalid syntax` r(198), and the data stay).
    if (weightVar.has_value() && !weightVar->empty())
      return ojson(eval::errorBlock("engine_error", "invalid syntax", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
    flavorRun = "collapse";
    smartNoop = optWords.count("fast") == 0;
  }
  // append reads the head as a file twice (the collapse, then the append).
  if (fcAppend) session.ensureMaterializedHead("fcollapse-append");
  std::string dataPath = *session.currentHbin();

  std::string byCsv;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) byCsv += ",";
    byCsv += byVars[i];
  }

  // The clist, parsed once (parseCollapseClist above): the engine's --stat
  // `(mean) y1 y2 (sum) z=w` -- vars stay SPACE-separated within a (stat) block
  // (cobra's own format), a target is one `new=old` token -- and the result list
  // the types, the formats, `cw` and the float re-store read.
  // The weight kind: collapse_17's `[aw fw iw pw]`, where [w=] and [weight=]
  // are aweights (the tokenizer already maps those to "aweight").
  std::string wkind;
  if (weightVar.has_value() && !weightVar->empty()) {
    const std::string wt = lower(weightTypeIn.value_or("fweight"));
    if (wt == "aw" || wt == "aweight" || wt == "weight" || wt == "w") wkind = "aw";
    else if (wt == "iw" || wt == "iweight") wkind = "iw";
    else if (wt == "pw" || wt == "pweight") wkind = "pw";
    else wkind = "fw";
  }
  auto rcError = [](const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };
  // The card: by() and every source must name a variable (`syntax ...
  // BY(varlist)` first, then each clist varlist): r(111) (cz_c_err_noby,
  // cz_c_err_nosrc, cz_c_err_nosrc_tgt).
  std::vector<VarInfo> cardCopy;
  {
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      cardCopy = *card;
    } else if (Datastore* ds = session.datastore()) {
      cardCopy = ds->varlist();
    }
  }
  auto cardVar = [&](const std::string& nm) -> const VarInfo* {
    for (const VarInfo& v : cardCopy)
      if (v.name == nm) return &v;
    return nullptr;
  };
  if (!cardCopy.empty()) {
    for (const auto& b : byVars)
      if (cardVar(b) == nullptr) return rcError("variable " + b + " not found", "111");
  }

  CollapseClist clist = parseCollapseClist(statRaw, byVars, session, flavorRun);
  if (clist.refusal.has_value()) return *clist.refusal;
  const std::string& statCobra = clist.statCobra;
  if (statCobra.empty()) return engineErrorBlock("collapse: empty stat-varlist");
  if (!cardCopy.empty()) {
    for (const CollapseItem& item : clist.items) {
      std::string src = item.source;
      const size_t dot = src.rfind('.');
      if (dot != std::string::npos) src = src.substr(dot + 1);
      const VarInfo* v = cardVar(src);
      if (v == nullptr) return rcError("variable " + src + " not found", "111");
      if (v->dtype != "factor") continue;
      // A string source: gtools refuses it outright (cz_g_str_g r(7)); collapse
      // serves first/last/firstnm/lastnm only (collapse: r(109)).
      if (flavor == "gcollapse")
        return rcError("'" + src + "' found where numeric variable expected", "7");
      if (item.stat != "first" && item.stat != "last" && item.stat != "firstnm" &&
          item.stat != "lastnm")
        return rcError("string variable " + src + " not allowed with statistic " + item.stat,
                       "109");
    }
  }
  // The weight table (collapse; gcollapse accepts the same combinations).
  if (!wkind.empty()) {
    for (const CollapseItem& item : clist.items) {
      const std::string& st = item.stat;
      const bool bad =
          (wkind == "pw" && (st == "sd" || st == "semean" || st == "sebinomial" || st == "sepoisson")) ||
          (wkind == "iw" && (st == "semean" || st == "sebinomial" || st == "sepoisson")) ||
          (wkind == "aw" && (st == "sebinomial" || st == "sepoisson"));
      if (bad) {
        const std::string wname = wkind == "pw" ? "pweights" : wkind == "iw" ? "iweights" : "aweights";
        return rcError(st + " not allowed with " + wname, "135");
      }
    }
  }

  // e(sample) T2: each result row carries its by-group's first observation's
  // marker (StataBE capture w2_esample_copies K1/K2/K4 first_diff=0, K8 = the
  // w2_esample.log:1193 E10 `0|0|3|11|`); without by() the result reads 0 (K5).
  // Read from the frame BEFORE the qualifiers pre-filter it; `restricted` = some
  // of a group's rows may not enter (if/in, a weight, `cw`).
  const bool collapseRestricted =
      (ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty()) ||
      (weightVar.has_value() && !weightVar->empty()) || cw;
  const EsampleGroupMarks esMarks = esampleGroupMarksBefore(session, byVars, collapseRestricted);

  // BUG-962: `cw` (casewise) keeps only the rows where every source variable is
  // non-missing (collapse); it was never read
  // and each statistic ran over its own variable's non-missing rows. It joins the
  // `if` qualifier of the pre-filter below.
  std::optional<std::string> filterIf = ifExpr;
  if (cw) {
    std::set<std::string> seen;
    std::string cwPred;
    for (const CollapseItem& item : clist.items) {
      if (!seen.insert(item.source).second) continue;
      if (!cwPred.empty()) cwPred += " & ";
      cwPred += "!missing(" + item.source + ")";
    }
    filterIf = (ifExpr.has_value() && !ifExpr->empty()) ? "(" + *ifExpr + ") & " + cwPred : cwPred;
  }

  // BS-090 (BETA Phase G): collapse's own cobra CLI has a --keepif of its
  // own (unlike tabstat/corr/ttest), but it runs through the SAME
  // non-bysort Frame::keep_if() as every other --keepif call, so an
  // `in`-derived `_n` predicate can't go through it either. Pre-filter
  // `dataPath` (the raw session hbin) via the shared helper and point the
  // collapse call at the result instead of forwarding a raw `--keepif`.
  if ((filterIf.has_value() && !filterIf->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, filterIf, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "collapse");
    dataPath = fr.dataPath;
    // collapse: no observation left after if/in/cw is "no
    // observations" r(2000), and the data stay as they were. The filtered copy
    // is a real file, so its row count costs no head materialization.
    // fcollapse's append keeps every observation and leaves the empty sample
    // to Mata's factor(), whose assertion is r(3498) (BUG-1100, StataBE 19.5,
    // ftools 2.49.0: `_assert_abort():  3498  no observations` for if, in and
    // cw alike, the data kept).
    int64_t rowsLeft = -1;
    try {
      rowsLeft = Datastore(dataPath).nRows();
    } catch (const std::exception&) {
      rowsLeft = -1;  // unreadable: the engine call below answers
    }
    if (rowsLeft == 0)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string(fcAppend ? "r(3498)" : "r(2000)")));
  }

  std::vector<CollapseResultMeta> resultMeta = collapseResultMeta(
      clist.items, byVars, flavorRun, wkind, optWords.count("clabel") != 0, session);
  // smart without fast: collapse's checks have run, and the data stay.
  if (smartNoop) return mutationResult(session, "collapse");

  std::string outPath = nextPath(session.workdir(), "collapse");
  std::vector<std::string> args = {"collapse", "--data", dataPath, "--stat", statCobra, "--out",
                                    outPath};
  if (!byCsv.empty()) {
    args.push_back("--by");
    args.push_back(byCsv);
  }
  // Step-1 D3: forward the frequency weight to cobra's collapse. cobra
  // owns the Σw math (and rejects what it doesn't support, e.g. weighted
  // median/p## — PIET does not pre-empt that here, it just passes through).
  if (wkind == "fw") {
    args.push_back("--fweight");
    args.push_back(*weightVar);
  } else if (!wkind.empty()) {
    args.push_back("--weight");
    args.push_back(*weightVar);
    args.push_back("--weight-type");
    args.push_back(wkind);
  }
  // Stata's `sort by`: a string by() variable's groups in text order
  // (cz_c_bystr_v1c `[]|[a]|[c]|[d]|[eee]|...`).
  args.push_back("--stata-by-order");
  {
    // ONESHOT-LASTFLUSH: collapse's `--out` becomes the head (session.load
    // immediately below) and is never read back as a file — which is exactly and
    // only what ResidentEligible asserts. The guard is scoped to THIS call, not
    // the whole handler, because applyIfAndInFilter above writes a temp that IS
    // read back as a file (collapse's own --data); eligibility is a session
    // counter, so an opt-in spanning both would silently drag the filter onto the
    // resident plane and hand cobra a --data that was never written.
    //
    // Unlike gen/replace, collapse reports card_stable=false (the row count and
    // column set both move), so mutation_result.cpp still rebuilds a Datastore
    // afterwards. That rebuild materializes the COLLAPSED head — measured at ~5%
    // of the frame this used to flush on the way in, and the engine no longer
    // reads the big one back at all.
    // fcollapse's append reads --out back as a file (the append's --using).
    std::optional<SessionState::ResidentEligible> residentCollapse;
    if (!fcAppend) residentCollapse.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      // `mark touse [wgt]`: a negative aw/fw/pw is r(402), a fractional fw r(401)
      // (cz_c_wr_fw_neg, cz_c_wr_aw_neg, cz_c_wr_fw_nonint).
      const std::string what = ce.what();
      if (what.find("has a negative value") != std::string::npos)
        return rcError("negative weights encountered", "402");
      if (what.find("has a non-integer value") != std::string::npos)
        return rcError("may not use noninteger frequency weights", "401");
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }

  // fcollapse's `append` (fcollapse_main): the results go below the
  // data instead of replacing them. The by() keys and the results fill the new
  // observations, every other variable is missing there, and nothing is sorted.
  // A result that names a variable already there is stored into it, and the
  // variable is recast to the result's (compressed) type when every value then
  // fits it (recast changes nothing otherwise); a new one is added with that
  // type, in its type's default format. Neither is relabelled or reformatted
  // (BUG-1100, StataBE 19.5: xi float %6.2f "the xi" became byte %6.2f "the
  // xi"; m double %10.0g, n byte %8.0g, mx float %9.0g, all unlabelled; an xi
  // holding .5 stays float).
  if (fcAppend) {
    if (optWords.count("nocompress") == 0) {
      Datastore res(outPath);
      for (CollapseResultMeta& m : resultMeta)
        if (res.has(m.target) && res.dtype(m.target) == "f64")
          compressFcollapseResult(m, res.col(m.target).f64(), res.nRows());
    }
    const std::string appended = nextPath(session.workdir(), "collapse");
    {
      SessionState::ResidentEligible residentAppend(session);  // the append's --out is the head
      CobraRunResult ar = runCobra({"append", "--data", *session.currentHbin(), "--using", outPath,
                                    "--out", appended, "--type-clash", "error"});
      if (!ar.ok) {
        CobraError ce = errorFromRaw(ar);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
    }
    session.load(appended);
    Datastore* ds = session.datastore();
    std::map<std::string, std::vector<std::string>> restore;  // type -> targets stored as it
    for (const CollapseResultMeta& m : resultMeta) {
      if (ds == nullptr || !ds->has(m.target) || ds->dtype(m.target) != "f64") continue;
      const double* x = ds->col(m.target).f64();
      std::optional<std::string> type = m.type;
      if (const VarInfo* before = cardVar(m.target)) {
        std::optional<std::string> was;
        auto vm = session.varMeta.find(m.target);
        if (vm != session.varMeta.end() && vm->second.stataType.has_value())
          was = *vm->second.stataType;
        else if (session.implicitFloatVars.count(m.target) != 0)
          was = std::string("float");
        if (!type.has_value()) {
          // A result whose type nothing recorded (a max/min/first/last of an
          // untyped source) has no type to recast to. The target keeps its claim
          // while its values fit it, and is widened as replace widens it when
          // they do not, never left claiming a type its values overflow.
          type = was;
          if (was.has_value() && !valuesFitType(x, ds->nRows(), *was)) {
            bool fraction = false, any = false;
            double lo = 0.0, hi = 0.0;
            for (int64_t i = 0; i < ds->nRows(); ++i) {
              if (std::isnan(x[i])) continue;
              fraction = fraction || std::trunc(x[i]) != x[i];
              lo = any ? std::min(lo, x[i]) : x[i];
              hi = any ? std::max(hi, x[i]) : x[i];
              any = true;
            }
            type = storage::promotedType(*was, fraction, lo, hi);
          }
        } else if (!valuesFitType(x, ds->nRows(), *type)) {
          type = was;
        }
        if (type != was) session.prepareStorageChange(*before);
      } else {
        VarMeta& fresh = session.varMeta[m.target];
        fresh.format.reset();
        fresh.label.reset();
      }
      if (type.has_value() && !valuesFitType(x, ds->nRows(), *type))
        restore[*type].push_back(m.target);
      session.varMeta[m.target].stataType = type;
      session.implicitFloatVars.erase(m.target);
    }
    // A value its variable cannot hold is stored as that type stores it. The
    // append's card is taken first, as before the float re-store below: left
    // pending, it was overwritten by the re-store's, and a new result that was
    // not re-stored dropped out of the varlist (`ds` and _all lost mx beside
    // re-stored counts c and c2, while c(k) still counted it).
    if (!restore.empty()) (void)mutationResult(session, "collapse");
    for (const auto& [type, targets] : restore) {
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : targets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, type);
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("fcollapse: the results were appended, but could not be stored "
                                "as " + type + ": " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
    return mutationResult(session, "collapse");
  }

  session.load(outPath);

  // FU-1033 / BUG-961: a float result (c(type) float for the mean of a non-double
  // source; a max/min/first/last of a float) holds float
  // values in Stata: w2e_float_default.log collapse_mean_float_src
  // +1.24924a0000000X+000, where the engine's double mean is 1.249249aaaaaab.
  // ONE `eval --store-type float --gen a=a ...` re-stores them through the store
  // boundary every creator uses (storage::appendStoreTypeArg -> the engine's
  // round_to_store), so there is no second rounding rule. Double and integer
  // results and the by-keys are not touched.
  {
    std::vector<std::string> floatTargets;
    for (const CollapseResultMeta& m : resultMeta)
      if (m.type == std::string("float")) floatTargets.push_back(m.target);
    if (!floatTargets.empty()) {
      // The session keeps ONE pending card delta, for the last resident commit.
      // Left pending, collapse's (a new column set) is overwritten by the
      // re-store's in-place one, which then folds onto the PRE-collapse card:
      // under `preserve` the card answered the old varlist and `: type sm` failed
      // (af_collapse, w2af_format_lifecycle.log). Take collapse's card first.
      (void)mutationResult(session, "collapse");
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : floatTargets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, "float");
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      // The same plane rule as the collapse call above: --out becomes the head.
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("collapse: the data were collapsed, but the float results could "
                                "not be stored as float: " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
  }

  // fcollapse stores each numeric result in the smallest type that holds it
  // unless `nocompress` (fcollapseCompressedType). The type is chosen from the
  // values as computed, and a value its type cannot hold (a pw/iw count of 3.5
  // in a long) is then stored as that type stores it, truncated (StataBE 19.5,
  // ftools 2.49.0: iw counts 100.75 and 1.5 are int 100 and 1).
  if (flavorRun == "fcollapse") {
    Datastore* ds = session.datastore();
    std::map<std::string, std::vector<std::string>> restore;  // type -> targets stored as it
    for (CollapseResultMeta& m : resultMeta) {
      if (ds == nullptr || !ds->has(m.target) || ds->dtype(m.target) != "f64") continue;
      const double* x = ds->col(m.target).f64();
      if (optWords.count("nocompress") == 0) compressFcollapseResult(m, x, ds->nRows());
      if (m.type.has_value() && !valuesFitType(x, ds->nRows(), *m.type))
        restore[*m.type].push_back(m.target);
    }
    if (!restore.empty()) (void)mutationResult(session, "collapse");  // as the float re-store
    for (const auto& [type, targets] : restore) {
      const std::string restorePath = nextPath(session.workdir(), "collapse");
      std::vector<std::string> rargs = {"eval", "--data", *session.currentHbin()};
      for (const std::string& t : targets) {
        rargs.push_back("--gen");
        rargs.push_back(t + "=" + t);
      }
      storage::appendStoreTypeArg(rargs, type);
      rargs.push_back("--out");
      rargs.push_back(restorePath);
      SessionState::ResidentEligible residentRestore(session);
      CobraRunResult rr = runCobra(rargs);
      if (!rr.ok) {
        CobraError ce = errorFromRaw(rr);
        return engineErrorBlock("fcollapse: the data were collapsed, but the results could not "
                                "be stored as " + type + ": " + cobraErrorStr(ce), ce.code);
      }
      session.load(restorePath);
    }
  }

  for (const CollapseResultMeta& m : resultMeta) {
    VarMeta& vm = session.varMeta[m.target];
    vm.format = m.format;
    vm.stataType = m.type;
    vm.label = m.label;
    session.implicitFloatVars.erase(m.target);
  }
  if (!esampleGroupMarksAfter(session, esMarks, byVars, EsampleLostBy::kCollapse)) {
    return engineErrorBlock(
        "collapse: the data were collapsed, but the estimation-sample marker could not be "
        "written onto the result; e(sample) is not reliable until the next estimation.");
  }
  return mutationResult(session, "collapse");
}
}
