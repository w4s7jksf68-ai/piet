// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table outreg.
#include "tables_internal.h"
#include "outreg2_internal.h"
#include "piet/handlers/estimates_tables.h"
#include "piet/user_paths.h"
namespace piet::handlers::table_detail {


// Portions derived from outreg2 2.3.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// outreg2
//
// outreg2 reports the ACTIVE estimation (or the stored ones named in
// `outreg2 [m1 m2] using ...`), and its
// `append` crosses SESSIONS -- run the same do-file twice and the second run's
// append lands on the first run's file. So the `.txt` ledger IS the source of
// truth for append (plan decision 7); an in-memory ledger would answer
// differently on the second run, which is the rejected alternative.
// ===========================================================================



// `addtext(Controls, Yes, Year FE, Yes)` / `addstat(F, e(F), R2adj, e(r2_a))`
// -- both are FLAT comma lists of alternating name and value.
std::vector<std::pair<std::string, std::string>> parseCommaPairs(const std::string& spec) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec), ',');
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out.emplace_back(strutil::trim(dequote(strutil::trim(toks[i]))),
                     strutil::trim(dequote(strutil::trim(toks[i + 1]))));
  }
  return out;
}


bool o2IsMissing(double v) { return std::isnan(v); }


// `round(x - int(round(x, 1e-10)), 1e-10) == 0` -- the integer test autogen
// and autodigits2 share (outreg2).
bool o2Whole(double v) {
  const double r = std::floor(v / 1e-10 + 0.5) * 1e-10;
  const double d = (v - std::trunc(r)) / 1e-10;
  return d >= -0.5 && d < 0.5;
}


// string(v, "%12.<d>e")
std::string o2Exp(double v, int d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*e", std::max(0, d), v);
  return buf;
}


// string(v, "%12.<dec>fc") -- the width-12 comma format every fixed cell uses
// (fmtgen). Its three overflow regimes are outreg2AdecCell's, measured.
std::string o2Fixed(double v, int dec) {
  if (o2IsMissing(v)) return kMissingCell;
  return outreg2AdecCell(v, dec).text;
}


// The digit count autogen (:6655) and autodigits2 (:6771) derive. They differ
// in ONE branch: past `auto` integer digits autogen gives auto-left-1 (then
// floors at 0) and autodigits2 gives 0 -- the same number after the floor.
int o2AutoDec(double v, int autoD, int less) {
  int aadec = 0;
  if (!o2Whole(v)) {
    const double frac = std::fabs(v - std::trunc(v));
    const int times =
        std::abs(static_cast<int>(std::trunc(std::log(frac) / std::log(10.0) - 1)));
    const int left = static_cast<int>(std::trunc(std::log(std::fabs(v)) / std::log(10.0) + 1));
    if (left > 0) {
      aadec = (left <= autoD) ? autoD - left + 1 : autoD - left - 1;
    } else {
      aadec = times + autoD - 1;
    }
  }
  aadec -= less;
  return aadec < 0 ? 0 : aadec;
}


// autogen (:6655-6740): %12.<aadec>fc, (auto-1)e from seven decimals on, auto e
// beyond +-1e6. A MISSING value gets an empty format and `string(., "")` is
// empty -- which is why o_stats.txt's depvar row is blank and o09's (bdec(3),
// so fmtgen) reads `.`.
std::string o2AutoCell(double v, int autoD, int less) {
  if (o2IsMissing(v)) return std::string();
  if (v > 1000000 || v < -1000000) return o2Exp(v, autoD);
  const int aadec = o2AutoDec(v, autoD, less);
  if (aadec >= 7) return o2Exp(v, autoD - 1);
  return o2Fixed(v, aadec);
}


// autodigits2 as _addstat_parse uses it (:10420): string(v, "%12.<n>f") -- no
// separators, because r(valstr) is `<n>f`.
std::string o2AutoDigits2Cell(double v, int autoD, int less) {
  if (o2IsMissing(v)) return kMissingCell;
  if (v > 1000000 || v < -1000000) return o2Exp(v, autoD);
  const int aadec = o2AutoDec(v, autoD, less);
  if (aadec >= 7) return o2Exp(v, autoD - 1);
  const AdecCell c = outreg2AdecCell(v, aadec);
  if (c.seps == 0) return c.text;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", aadec, v);
  return buf;
}


// Split an option body at top-level commas, keeping EMPTY items (a leading,
// doubled or trailing comma is an empty ctitle row, :6029-6050). Each item is
// trimmed and loses one pair of outer double quotes, as gettoken strips them.
std::vector<std::string> o2SplitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQ = false;
  for (char c : s) {
    if (c == '"') inQ = !inQ;
    if (!inQ) {
      if (c == '(') ++depth;
      if (c == ')' && depth > 0) --depth;
      if (c == ',' && depth == 0) {
        out.push_back(dequote(strutil::trim(cur)));
        cur.clear();
        continue;
      }
    }
    cur += c;
  }
  out.push_back(dequote(strutil::trim(cur)));
  return out;
}


bool o2IsFactorPart(const std::string& p) {
  // `1.g`, `0b.g`, `2o.g`, `1bn.g` -- a level, then a dot.
  const size_t dot = p.find('.');
  if (dot == std::string::npos || dot == 0) return false;
  if (!std::isdigit(static_cast<unsigned char>(p[0]))) return false;
  for (size_t i = 0; i < dot; ++i) {
    const char c = p[i];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'b' && c != 'o' && c != 'n') {
      return false;
    }
  }
  return true;
}


// Stata's e(b) spelling of an interaction: factor parts first, continuous parts
// as `c.<var>` (w2at o21: `regress y c.x1#i.g` writes `1.g#c.x1`). This build's
// coeftable keeps the typed order and drops the `c.` (`x1#1.g`).
std::string o2StataName(const std::string& n) {
  if (n.find('#') == std::string::npos) return n;
  std::vector<std::string> fac, con;
  size_t pos = 0;
  while (true) {
    const size_t h = n.find('#', pos);
    std::string part = n.substr(pos, h == std::string::npos ? std::string::npos : h - pos);
    if (o2IsFactorPart(part)) {
      fac.push_back(part);
    } else {
      if (part.rfind("c.", 0) != 0) part = "c." + part;
      con.push_back(part);
    }
    if (h == std::string::npos) break;
    pos = h + 1;
  }
  std::string out;
  for (const auto& p : fac) out += (out.empty() ? "" : "#") + p;
  for (const auto& p : con) out += (out.empty() ? "" : "#") + p;
  return out;
}


// fvts_label (:10145-10241): the LABELS cell of one e(b) name.
std::string o2FvtsLabel(const std::string& name, SessionState& session) {
  if (name.find('#') != std::string::npos) return std::string();  // poundsign split: no branch
  Datastore* ds = session.datastore();
  auto exists = [&](const std::string& v) { return ds != nullptr && ds->has(v); };
  auto varLabelOf = [&](const std::string& v) {
    std::string l = strutil::trim(session.eval.varLabels.get(v));
    return l.empty() ? v : l;
  };
  const size_t dot = name.find('.');
  if (dot == std::string::npos) {
    return exists(name) ? varLabelOf(name) : std::string();
  }
  const std::string first = name.substr(0, dot);
  const std::string third = name.substr(dot + 1);
  if (first.empty() || third.empty() || third.find('.') != std::string::npos) return std::string();
  if (!exists(third)) return std::string();
  const std::string varlabel = varLabelOf(third);
  std::string end;
  std::string bare;
  bool firstIsNumber = !first.empty();
  for (char c : first) {
    if (!std::isdigit(static_cast<unsigned char>(c))) firstIsNumber = false;
  }
  if (!firstIsNumber) {
    std::string baseOrOmitted;
    if (first == "co") {
      baseOrOmitted = "continuous omitted";
    } else {
      bare = first.substr(0, first.size() - 1);
      const char suffix = first.back();
      bool bareNum = !bare.empty();
      for (char c : bare) {
        if (!std::isdigit(static_cast<unsigned char>(c))) bareNum = false;
      }
      if (bareNum) {
        if (suffix == 'o') baseOrOmitted = "omitted";
        if (suffix == 'b') baseOrOmitted = "base";
        if (suffix == 'n') {
          bare = first.substr(0, first.size() >= 2 ? first.size() - 2 : 0);
          baseOrOmitted = "base";
        }
      }
    }
    end = ", " + baseOrOmitted;
  }
  // `: value label third` then `: label lblname first` -- which answers the
  // value itself when the set has no text for it.
  std::string labelOfValue;
  auto lb = session.varLblname.find(third);
  if (lb != session.varLblname.end() && !lb->second.empty()) {
    auto set = session.labelDefs.find(lb->second);
    if (set != session.labelDefs.end()) {
      auto hit = set->second.find(first);
      labelOfValue = (hit != set->second.end()) ? hit->second : first;
    }
  }
  if (labelOfValue.empty() && !bare.empty()) return varlabel + " = " + bare + end;
  if (labelOfValue.empty()) return varlabel + " = " + first + end;
  return varlabel + " = " + first + ", " + labelOfValue + end;
}

std::vector<O2Token> o2ExpandTokens(const std::string& spec, SessionState& session) {
  std::vector<O2Token> out;
  Datastore* ds = session.datastore();
  for (const auto& tok : splitWs(dequote(spec))) {
    if (tok.find('#') != std::string::npos) {
      out.push_back({o2StataName(tok), false});
      continue;
    }
    const size_t dot = tok.find('.');
    if (dot != std::string::npos) {
      const std::string head = tok.substr(0, dot);
      if (head == "i") {
        out.push_back({tok.substr(dot + 1), true});
      } else if (head == "c") {
        out.push_back({tok.substr(dot + 1), false});
      } else {
        out.push_back({tok, false});
      }
      continue;
    }
    if (ds == nullptr) continue;
    for (const auto& v : ds->names()) {
      if (strutil::globMatch(tok, v)) out.push_back({v, false});
    }
  }
  return out;
}


bool o2TokenHits(const std::vector<O2Token>& toks, const std::string& stataName) {
  for (const auto& t : toks) {
    if (t.anyLevel) {
      const size_t dot = stataName.find('.');
      if (dot != std::string::npos && stataName.find('#') == std::string::npos &&
          o2IsFactorPart(stataName) && stataName.substr(dot + 1) == t.text) {
        return true;
      }
    } else if (t.text == stataName) {
      return true;
    }
  }
  return false;
}


// The sort key Stata's `sort` uses: missing sorts after every number.
int o2Cmp(double a, double b) {
  const bool ma = o2IsMissing(a), mb = o2IsMissing(b);
  if (ma && mb) return 0;
  if (ma) return 1;
  if (mb) return -1;
  return a < b ? -1 : (a > b ? 1 : 0);
}


int o2CmpKeys(const std::vector<double>& a, const std::vector<double>& b) {
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const int c = o2Cmp(a[i], b[i]);
    if (c != 0) return c;
  }
  return 0;
}


std::string o2TrimCell(const std::string& s) { return strutil::trim(s); }


// The Vorder header pass both halves of appendFile run (:3019-3025, :3487-3496):
// the top blank row is 0, each header row above it one less, and the topmost
// of them -99 -- so header rows merge by their DISTANCE from the body, and the
// column-number row always meets the column-number row.
void o2HeaderOrder(const std::vector<int>& rowtype, std::vector<double>* vorder) {
  const size_t n = rowtype.size();
  for (size_t i = 0; i < n; ++i) (*vorder)[i] = rowtype[i] == 0 ? 0.0 : kO2Missing;
  for (int pass = 0; pass < 100 && n > 0 && o2IsMissing((*vorder)[0]); ++pass) {
    bool moved = false;
    for (size_t i = 0; i + 1 < n; ++i) {
      if (rowtype[i] == -1 && o2IsMissing((*vorder)[i]) && !o2IsMissing((*vorder)[i + 1])) {
        (*vorder)[i] = (*vorder)[i + 1] - 1;
        moved = true;
      }
    }
    if (!moved) break;
  }
  double mn = kO2Missing;
  for (size_t i = 0; i < n; ++i) {
    if (!o2IsMissing((*vorder)[i]) && (o2IsMissing(mn) || (*vorder)[i] < mn)) mn = (*vorder)[i];
  }
  if (!o2IsMissing(mn)) {
    for (size_t i = 0; i < n; ++i) {
      if ((*vorder)[i] == mn) (*vorder)[i] = -99;
    }
  }
}


// sortvar() tokens: `tsunab` each one, keeping it literally when that fails
// (:545-568).
std::vector<std::string> o2SortvarList(const std::string& spec, SessionState& session) {
  std::vector<std::string> out;
  Datastore* ds = session.datastore();
  for (const auto& tok : splitWs(dequote(spec))) {
    bool any = false;
    if (ds != nullptr && tok.find('.') == std::string::npos) {
      for (const auto& v : ds->names()) {
        if (strutil::globMatch(tok, v)) {
          if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
          any = true;
        }
      }
    }
    if (!any && std::find(out.begin(), out.end(), tok) == out.end()) out.push_back(tok);
  }
  return out;
}


// appendFile (:2991-3843). `f1` is the ledger as it stands, LABELS column and
// title rows already stripped; `f2` the new column. Returns the merged raw
// grid: f1's columns, then the new one.
O2Grid o2AppendFile(const O2Grid& f1, const std::vector<O2Row>& f2In,
                    const std::vector<std::string>& sortvar,
                    std::vector<std::string>* bodyNamesOut) {
  const size_t k1 = f1.empty() ? 1 : f1[0].size();
  const size_t n1 = f1.size();

  // ---- the master (file 1) ------------------------------------------------
  struct M {
    std::vector<std::string> v;
    int rowtype = -1;
    std::string mergeOn;
    double varnum = kO2Missing, vorder = kO2Missing, vorder0 = kO2Missing,
           vorder1 = kO2Missing, sortvarCol = kO2Missing, vord = kO2Missing,
           eqOrder = 1;
    bool constant = false;
  };
  std::vector<M> m(n1);
  std::string variables1;
  for (size_t i = 0; i < n1; ++i) {
    m[i].v = f1[i];
    if (variables1.empty() && !f1[i][0].empty()) variables1 = f1[i][0];
  }
  {
    // rowmiss, and only the row right under VARIABLES may count as blank.
    std::vector<int> rowmiss(n1, 0);
    for (size_t i = 0; i < n1; ++i) {
      for (const auto& c : f1[i]) rowmiss[i] += c.empty() ? 0 : 1;
      const std::string prev = i == 0 ? std::string() : f1[i - 1][0];
      if (prev != variables1) rowmiss[i] = 1;
    }
    std::vector<int> rt(n1, -1);
    std::vector<bool> after(n1, false);
    for (size_t i = 0; i < n1; ++i) {
      if (rowmiss[i] == 0) rt[i] = 0;
      if (i > 0 && (rowmiss[i - 1] == 0 || after[i - 1])) after[i] = true;
    }
    // The bottom row: the LAST row whose first cell is empty (:3384-3398).
    size_t bottom = 0;  // 1-based; 0 = none
    for (size_t i = n1; i >= 1; --i) {
      if (f1[i - 1][0].empty()) {
        bottom = i;
        break;
      }
    }
    for (size_t i = 0; i < n1; ++i) {
      if (after[i]) rt[i] = 1;
      if (bottom != 0 && i + 1 == bottom) rt[i] = 2;
      if (bottom != 0 && i + 1 > bottom) rt[i] = 3;
      m[i].rowtype = rt[i];
    }
  }
  for (auto& r : m) {
    if (r.rowtype == -1) r.v[0].clear();
  }
  std::vector<std::string> v2plus(n1);
  for (size_t i = 0; i < n1; ++i) {
    v2plus[i] = (k1 >= 2 ? o2TrimCell(m[i].v[1]) : std::string()) +
                (k1 >= 3 ? o2TrimCell(m[i].v[2]) : std::string());
    m[i].mergeOn = m[i].v[0];
  }
  for (size_t i = 0; i < n1; ++i) {
    if (o2TrimCell(m[i].v[0]).empty()) {
      m[i].mergeOn = (i == 0 ? std::string() : m[i - 1].mergeOn) + "!";
    }
  }
  {
    std::vector<int> rt(n1);
    std::vector<double> vo(n1);
    for (size_t i = 0; i < n1; ++i) rt[i] = m[i].rowtype;
    o2HeaderOrder(rt, &vo);
    for (size_t i = 0; i < n1; ++i) m[i].vorder = vo[i];
  }
  for (auto& r : m) {
    if (r.rowtype == 2) r.mergeOn = "bottomRow";
    if (r.rowtype == 0) r.mergeOn = "topRow";
    if (r.rowtype == -1) r.mergeOn = "_000";
    if (!o2IsMissing(r.vorder) && r.vorder < 1) r.varnum = r.vorder;
  }
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].rowtype >= 2) m[i].vorder = 3.7;
  }
  for (size_t i = 1; i < n1; ++i) {
    if (!o2IsMissing(m[i].vorder) && m[i].vorder >= 3.7 && m[i].rowtype == 3) {
      m[i].vorder = m[i - 1].vorder + .0001;
    }
  }
  for (auto& r : m) {
    if (r.rowtype == 2) r.vorder = 2;
    if (r.v[0] == "Observations") r.vorder = 3.5;
    if (r.v[0] == "R-squared") r.vorder = 3.6;
  }
  for (size_t i = 1; i < n1; ++i) {
    const double p = m[i - 1].vorder;
    if (o2IsMissing(m[i].vorder) && !o2IsMissing(p) && p <= 1) m[i].vorder = 1;
  }
  for (size_t i = 0; i < n1; ++i) {
    const std::string v2 = k1 >= 2 ? m[i].v[1] : std::string();
    if (v2.empty() && v2plus[i].empty() && !o2IsMissing(m[i].vorder) && m[i].vorder > 3.5) {
      m[i].vorder = 4;
    }
  }
  double maxnum = 1;
  if (!sortvar.empty()) {
    for (size_t t = 0; t < sortvar.size(); ++t) {
      for (auto& r : m) {
        if (r.v[0] == sortvar[t]) r.vorder0 = static_cast<double>(t + 1);
      }
    }
    maxnum = static_cast<double>(sortvar.size() + 1);
  }
  for (auto& r : m) r.sortvarCol = r.vorder0;
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].vorder == 1 && !o2TrimCell(m[i].mergeOn).empty() &&
        !o2TrimCell(m[i].v[0]).empty() && o2IsMissing(m[i].vorder0)) {
      m[i].vorder0 = static_cast<double>(i + 1) + maxnum;
    }
  }
  for (size_t i = 1; i < n1; ++i) {
    if (o2IsMissing(m[i].vorder0) && m[i].vorder == 1) m[i].vorder0 = m[i - 1].vorder0;
  }
  for (auto& r : m) {
    if (!o2IsMissing(r.vorder0) && r.vorder0 < maxnum) r.sortvarCol = r.vorder0;
  }
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].vorder == 1 && !m[i].v[0].empty()) m[i].vorder1 = m[i].vorder0;
  }
  for (size_t i = 1; i < n1; ++i) {
    if (o2IsMissing(m[i].vorder1) && m[i].vorder == 1 && !o2IsMissing(m[i - 1].vorder1)) {
      m[i].vorder1 = m[i - 1].vorder1 + .01;
    }
  }
  for (size_t i = 0; i < n1; ++i) {
    m[i].eqOrder = m[i].rowtype == 2 ? kO2Missing : 1;
    m[i].vord = static_cast<double>(i + 1);
    if (m[i].rowtype == 1 && !m[i].v[0].empty() && bodyNamesOut != nullptr) {
      bodyNamesOut->push_back(m[i].v[0]);
    }
  }
  std::stable_sort(m.begin(), m.end(), [](const M& a, const M& b) {
    return o2CmpKeys({a.vorder, a.eqOrder, a.vorder1, a.vord},
                     {b.vorder, b.eqOrder, b.vorder1, b.vord}) < 0;
  });
  for (size_t i = 0; i < n1; ++i) {
    m[i].vord = static_cast<double>(i + 1);
    m[i].constant = m[i].v[0] == "Constant" || m[i].v[0] == "_cons";
    if (i > 0 && m[i].mergeOn == m[i - 1].mergeOn + "!" && m[i - 1].constant) {
      m[i].constant = true;
    }
  }

  // ---- the using side (file 2, the new column) ----------------------------
  struct U {
    std::string report, cell, varname, mergeOn;
    int rowtype = 1;
    double varnum = kO2Missing, vorder = kO2Missing, vorder0 = kO2Missing,
           vorder1 = kO2Missing, sortvarCol = kO2Missing, vord = kO2Missing, mrgrow = 0;
    bool constant = false;
  };
  std::vector<U> u(f2In.size());
  const size_t n2 = u.size();
  std::string variables2;
  for (size_t i = 0; i < n2; ++i) {
    u[i].report = f2In[i].report;
    u[i].cell = f2In[i].cell;
    u[i].varname = f2In[i].varname;
    u[i].rowtype = f2In[i].rowtype;
    u[i].mrgrow = static_cast<double>(i + 1);
    if (variables2.empty() && !u[i].report.empty()) variables2 = u[i].report;
  }
  for (auto& r : u) {
    if (r.report == variables2 && r.rowtype == -1) r.report.clear();
  }
  {
    std::vector<int> rt(n2);
    std::vector<double> vo(n2);
    for (size_t i = 0; i < n2; ++i) rt[i] = u[i].rowtype;
    o2HeaderOrder(rt, &vo);
    for (size_t i = 0; i < n2; ++i) u[i].vorder = vo[i];
  }
  for (auto& r : u) {
    if (r.rowtype >= 2) r.vorder = 3.8;
  }
  for (size_t i = 1; i < n2; ++i) {
    if (!o2IsMissing(u[i].vorder) && u[i].vorder >= 3.8 && u[i].rowtype == 3) {
      u[i].vorder = u[i - 1].vorder + .0001;
    }
  }
  for (auto& r : u) {
    if (r.rowtype == 2) r.vorder = 2;
    if (r.report == "Observations") r.vorder = 3.5;
    if (r.report == "R-squared") r.vorder = 3.6;
  }
  for (size_t i = 1; i < n2; ++i) {
    const double p = u[i - 1].vorder;
    if (o2IsMissing(u[i].vorder) && !o2IsMissing(p) && p <= 1) u[i].vorder = 1;
  }
  for (size_t i = 1; i < n2; ++i) {
    const double p = u[i - 1].vorder;
    if (u[i].report.empty() && (p == 2 || p == 2.5)) u[i].vorder = 2.5;
  }
  if (!sortvar.empty()) {
    for (size_t t = 0; t < sortvar.size(); ++t) {
      for (auto& r : u) {
        if (r.varname == sortvar[t]) r.vorder0 = static_cast<double>(t + 1);
      }
    }
  }
  for (auto& r : u) r.sortvarCol = r.vorder0;
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].vorder == 1 && !u[i].report.empty() && o2IsMissing(u[i].vorder0)) {
      u[i].vorder0 = static_cast<double>(i + 1) + maxnum;
    }
  }
  for (size_t i = 1; i < n2; ++i) {
    if (o2IsMissing(u[i].vorder0) && u[i].vorder == 1) u[i].vorder0 = u[i - 1].vorder0;
  }
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].vorder == 1 && !u[i].report.empty()) u[i].vorder1 = u[i].vorder0;
  }
  for (size_t i = 1; i < n2; ++i) {
    if (o2IsMissing(u[i].vorder1) && u[i].vorder == 1 && !o2IsMissing(u[i - 1].vorder1)) {
      u[i].vorder1 = u[i - 1].vorder1 + .01;
    }
  }
  std::stable_sort(u.begin(), u.end(), [](const U& a, const U& b) {
    return o2CmpKeys({a.vorder, a.vorder1, a.mrgrow}, {b.vorder, b.vorder1, b.mrgrow}) < 0;
  });
  for (size_t i = 0; i < n2; ++i) {
    u[i].mergeOn = u[i].report;
    if (o2TrimCell(u[i].mergeOn).empty()) {
      u[i].mergeOn = (i == 0 ? std::string() : u[i - 1].mergeOn) + "!";
    }
  }
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].rowtype == 2) u[i].mergeOn = "bottomRow";
    if (u[i].rowtype == 0) u[i].mergeOn = "topRow";
    if (u[i].rowtype == -1) u[i].mergeOn = "_000";
    if (!o2IsMissing(u[i].vorder) && u[i].vorder < 1) u[i].varnum = u[i].vorder;
    u[i].vord = static_cast<double>(i + 1);
    u[i].constant = u[i].report == "Constant" || u[i].report == "_cons" ||
                    u[i].varname == "Constant" || u[i].varname == "_cons";
    if (i > 0 && u[i].mergeOn == u[i - 1].mergeOn + "!" && u[i - 1].constant) {
      u[i].constant = true;
    }
  }

  // ---- `merge mergeVar varnum using` ---------------------------------------
  // Old-style match merge: rows pair up by (mergeVar, varnum); inside a key
  // the k-th master row meets the k-th using row, and the shorter side's last
  // row repeats -- Stata's m:m rule.
  struct J {
    int mi = -1, ui = -1;
  };
  std::vector<J> joined;
  {
    auto keyLess = [](const std::string& a, double av, const std::string& b, double bv) {
      if (a != b) return a < b;
      return o2Cmp(av, bv) < 0;
    };
    std::vector<int> mo(n1), uo(n2);
    for (size_t i = 0; i < n1; ++i) mo[i] = static_cast<int>(i);
    for (size_t i = 0; i < n2; ++i) uo[i] = static_cast<int>(i);
    std::stable_sort(mo.begin(), mo.end(), [&](int a, int b) {
      return keyLess(m[a].mergeOn, m[a].varnum, m[b].mergeOn, m[b].varnum);
    });
    std::stable_sort(uo.begin(), uo.end(), [&](int a, int b) {
      return keyLess(u[a].mergeOn, u[a].varnum, u[b].mergeOn, u[b].varnum);
    });
    size_t a = 0, b = 0;
    while (a < n1 || b < n2) {
      if (b >= n2 ||
          (a < n1 && keyLess(m[mo[a]].mergeOn, m[mo[a]].varnum, u[uo[b]].mergeOn, u[uo[b]].varnum))) {
        joined.push_back({mo[a], -1});
        ++a;
        continue;
      }
      if (a >= n1 ||
          keyLess(u[uo[b]].mergeOn, u[uo[b]].varnum, m[mo[a]].mergeOn, m[mo[a]].varnum)) {
        joined.push_back({-1, uo[b]});
        ++b;
        continue;
      }
      size_t ae = a, be = b;
      while (ae < n1 && m[mo[ae]].mergeOn == m[mo[a]].mergeOn &&
             o2Cmp(m[mo[ae]].varnum, m[mo[a]].varnum) == 0) {
        ++ae;
      }
      while (be < n2 && u[uo[be]].mergeOn == u[uo[b]].mergeOn &&
             o2Cmp(u[uo[be]].varnum, u[uo[b]].varnum) == 0) {
        ++be;
      }
      const size_t cm = ae - a, cu = be - b;
      for (size_t q = 0; q < std::max(cm, cu); ++q) {
        joined.push_back({mo[a + std::min(q, cm - 1)], uo[b + std::min(q, cu - 1)]});
      }
      a = ae;
      b = be;
    }
  }

  struct R {
    int mi = -1, ui = -1;
    double vorder = kO2Missing, eq0 = 1, sortvarCol = kO2Missing, vo1 = kO2Missing,
           vo2 = kO2Missing, merge2 = 0, vord1 = kO2Missing, vord2 = kO2Missing,
           vorder2_0 = kO2Missing, vorder1_0 = kO2Missing;
  };
  std::vector<R> rows;
  rows.reserve(joined.size());
  for (const auto& j : joined) {
    R r;
    r.mi = j.mi;
    r.ui = j.ui;
    const double vorder1 = j.mi >= 0 ? m[j.mi].vorder : kO2Missing;
    const double vorder2 = j.ui >= 0 ? u[j.ui].vorder : kO2Missing;
    r.vorder = vorder2;
    if (o2IsMissing(r.vorder) || (!o2IsMissing(vorder1) && vorder1 > 3.5 && vorder1 < 4)) {
      r.vorder = vorder1;
    }
    r.merge2 = (j.mi < 0) ? 1 : 0;
    r.vord1 = j.mi >= 0 ? m[j.mi].vord : kO2Missing;
    r.vord2 = j.ui >= 0 ? u[j.ui].vord : kO2Missing;
    r.vo1 = j.mi >= 0 ? m[j.mi].vorder1 : kO2Missing;
    r.vo2 = j.ui >= 0 ? u[j.ui].vorder1 : kO2Missing;
    r.vorder1_0 = j.mi >= 0 ? m[j.mi].vorder0 : kO2Missing;
    r.vorder2_0 = j.ui >= 0 ? u[j.ui].vorder0 : kO2Missing;
    const bool c1 = j.mi >= 0 && m[j.mi].constant;
    const bool c2 = j.ui >= 0 && u[j.ui].constant;
    const bool constant = j.mi >= 0 ? (c1 || c2) : c2;
    r.eq0 = constant ? 1.5 : 1;
    const double s1 = j.mi >= 0 ? m[j.mi].sortvarCol : kO2Missing;
    const double s2 = j.ui >= 0 ? u[j.ui].sortvarCol : kO2Missing;
    r.sortvarCol = o2IsMissing(s1) ? s2 : s1;
    rows.push_back(r);
  }
  // Fill the gaps a differing number of statistic rows leaves (:3640-3648).
  {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const R& a, const R& b) { return o2Cmp(a.vo2, b.vo2) < 0; });
    for (size_t i = 1; i < rows.size(); ++i) {
      R& r = rows[i];
      const R& p = rows[i - 1];
      if (!o2IsMissing(r.vorder2_0) && r.vorder2_0 == p.vorder2_0 && o2IsMissing(r.vord1) &&
          !o2IsMissing(p.vord1) && r.vorder == 1) {
        r.vord1 = p.vord1 + .01;
      }
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const R& a, const R& b) { return o2Cmp(a.vo1, b.vo1) < 0; });
    for (size_t i = 1; i < rows.size(); ++i) {
      R& r = rows[i];
      const R& p = rows[i - 1];
      if (!o2IsMissing(r.vorder1_0) && r.vorder1_0 == p.vorder1_0 && o2IsMissing(r.vord2) &&
          !o2IsMissing(p.vord2) && r.vorder == 1) {
        r.vord2 = p.vord2 + .01;
      }
    }
    for (auto& r : rows) {
      if (r.vorder == 1) {
        r.vo2 = r.vord2;
        r.vo1 = r.vord1;
      }
    }
  }
  size_t countV1 = 0, countV2 = 0;
  for (const auto& r : rows) {
    if (!o2IsMissing(r.vorder2_0) && r.vorder2_0 >= 1) ++countV2;
    if (!o2IsMissing(r.vorder1_0) && r.vorder1_0 >= 1) ++countV1;
  }
  const bool usingFirst = countV2 > countV1;
  std::stable_sort(rows.begin(), rows.end(), [&](const R& a, const R& b) {
    if (usingFirst) {
      return o2CmpKeys({a.vorder, a.eq0, a.sortvarCol, a.vo2, a.vo1, a.merge2, a.vord2, a.vord1},
                       {b.vorder, b.eq0, b.sortvarCol, b.vo2, b.vo1, b.merge2, b.vord2,
                        b.vord1}) < 0;
    }
    return o2CmpKeys({a.vorder, a.eq0, a.sortvarCol, a.vo1, a.vo2, a.merge2, a.vord1, a.vord2},
                     {b.vorder, b.eq0, b.sortvarCol, b.vo1, b.vo2, b.merge2, b.vord1,
                      b.vord2}) < 0;
  });

  O2Grid out;
  std::vector<int> rowtype1;
  out.reserve(rows.size());
  for (const auto& r : rows) {
    std::vector<std::string> line(k1 + 1);
    if (r.mi >= 0) {
      for (size_t c = 0; c < k1; ++c) line[c] = m[r.mi].v[c];
    }
    if (r.ui >= 0) {
      line[k1] = u[r.ui].cell;
      if (line[0].empty() && !u[r.ui].report.empty()) line[0] = u[r.ui].report;
    }
    out.push_back(std::move(line));
    rowtype1.push_back(r.mi >= 0 ? m[r.mi].rowtype : -2);  // -2 == missing
  }
  if (!out.empty()) {
    if (k1 == 2) out[0][1] = "(1)";
    out[0][k1] = "(" + std::to_string(k1) + ")";
  }
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    if (rowtype1[i + 1] == 0) out[i][0] = variables2;
  }
  for (size_t i = 0; i < out.size(); ++i) {
    if (rowtype1[i] == 0 && (i == 0 || out[i - 1][0] != variables2)) out[i][0] = variables2;
  }
  return out;
}


// cleanFile's two borders (:9178-9200).
void o2Borders(const O2Grid& g, size_t* headRow, size_t* bottomRow) {
  const size_t n = g.size();
  auto rowmiss = [&](size_t i) {
    int c = 0;
    for (const auto& s : g[i]) c += s.empty() ? 0 : 1;
    return c;
  };
  size_t num = 0;
  int content = 1;
  while (content == 1 && num < n) {
    content = rowmiss(n - 1 - num);
    ++num;
  }
  *bottomRow = n - num + 1;
  size_t h = 1;
  std::string first;
  while (first.empty() && h <= n) {
    first = g[h - 1][0];
    ++h;
  }
  *headRow = h - 1;
}


std::string o2RenderLedger(const O2Grid& g) {
  std::string out;
  for (const auto& row : g) {
    for (size_t c = 0; c < row.size(); ++c) {
      if (c) out += '\t';
      out += row[c];
    }
    out += '\n';
  }
  return out;
}


O2Grid o2ParseLedger(const std::string& text) {
  O2Grid g;
  size_t pos = 0;
  size_t width = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? text.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cells;
    size_t p = 0;
    while (true) {
      const size_t t = line.find('\t', p);
      cells.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
      if (t == std::string::npos) break;
      p = t + 1;
    }
    width = std::max(width, cells.size());
    g.push_back(std::move(cells));
  }
  for (auto& r : g) r.resize(width);
  return g;
}


std::string o2Subinstr(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string out;
  size_t pos = 0;
  while (true) {
    const size_t at = s.find(from, pos);
    if (at == std::string::npos) {
      out += s.substr(pos);
      break;
    }
    out += s.substr(pos, at - pos) + to;
    pos = at + from.size();
  }
  return out;
}


// has_eqn: two or more `$` (:7212-7215).
bool o2HasEqn(const std::string& s) {
  const size_t a = s.find('$');
  return a != std::string::npos && s.find('$', a + 1) != std::string::npos;
}


// _texout (:7059-7383) over the label-merged grid (1-based borders).
std::string o2RenderTex(O2Grid g, size_t headBorder, size_t bottomBorder, const O2TexOpts& o) {
  const size_t n = g.size();
  const size_t k = g.empty() ? 2 : std::max<size_t>(2, g[0].size());
  for (auto& r : g) r.resize(k);
  // `$<$` for the rows from bottomBorder on: v1 re-tokenised on blanks and `<`.
  for (size_t r = bottomBorder; r >= 1 && r <= n; ++r) {
    const std::string src = g[r - 1][0];
    std::vector<std::string> toks;
    std::string cur;
    for (char c : src) {
      if (c == ' ') {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
      } else if (c == '<') {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
        toks.push_back("<");
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) toks.push_back(cur);
    std::string out;
    bool noSpace = false;
    for (const auto& t : toks) {
      if (t == "<") {
        out += "$<$";
        noSpace = true;
      } else if (noSpace) {
        out += t;
        noSpace = false;
      } else {
        out += " " + t;
      }
    }
    g[r - 1][0] = out;
  }
  for (size_t r = 1; r <= n; ++r) {
    std::string& v = g[r - 1][0];
    if (!o2HasEqn(v)) v = o2Subinstr(v, "_", "\\_");
    v = o2Subinstr(v, "%", "\\%");
    v = o2Subinstr(v, "#", "\\#");
    for (size_t c = 1; c < k; ++c) {
      std::string& s = g[r - 1][c];
      if (r <= headBorder) {
        if (!o2HasEqn(s)) s = o2Subinstr(s, "_", "\\_");
        s = o2Subinstr(s, "%", "\\%");
        s = o2Subinstr(s, "#", "\\#");
      }
      if (r >= headBorder + 1 && r <= bottomBorder) {
        s = o2Subinstr(s, "<", "$<$");
        s = o2Subinstr(s, ">", "$>$");
        s = o2Subinstr(s, "{", "\\{");
        s = o2Subinstr(s, "}", "\\}");
        s = o2Subinstr(s, "|", "$|$");
        s = o2Subinstr(s, "_", "\\_");
        s = o2Subinstr(s, "%", "\\$");
        s = o2Subinstr(s, "#", "\\#");
      }
    }
  }
  std::string out;
  if (!o.fragment) {
    const std::string pw = o.landscape ? "11in" : "8.5in";
    const std::string ph = o.landscape ? "8.5in" : "11in";
    out += std::string("\\documentclass[") + (o.landscape ? "landscape" : "") + "]{article}\n";
    out += "\\setlength{\\pdfpagewidth}{" + pw + "} \\setlength{\\pdfpageheight}{" + ph + "}\n";
    out += "\\begin{document}\n";
  }
  out += "\\begin{tabular}{l" + std::string(k - 1, 'c') + "} \\hline\n";
  const std::string numcols = std::to_string(k);
  for (size_t r = 1; r <= n; ++r) {
    std::string pad0, pad1, padN;
    if (bottomBorder < n && r >= bottomBorder + 1) {
      pad0 = "\\multicolumn{" + numcols + "}{c}{";
      padN = "} \\\\";
    }
    if (r <= bottomBorder) {
      pad1 = " & ";
      padN += " \\\\";
      if (headBorder != 0 && r == headBorder) padN += " \\hline";
      if (r == bottomBorder) padN += " \\hline";
    }
    std::string line = pad0 + g[r - 1][0] + pad1;
    for (size_t c = 1; c < k; ++c) {
      line += g[r - 1][c];
      if (c + 1 < k) line += pad1;
    }
    line += padN;
    out += line + "\n";
  }
  out += "\\end{tabular}\n";
  if (!o.fragment) out += "\\end{document}\n";
  return out;
}


// _xmlout + xmlstack (:7652-8112), without `legible`: one line, no newline.
std::string o2RenderXml(const O2Grid& g, size_t headBorder, size_t bottomBorder) {
  const size_t n = g.size();
  const size_t k = g.empty() ? 1 : g[0].size();
  std::string o;
  o += "<?xml version=\"1.0\" encoding=\"US-ASCII\" standalone=\"yes\"?>";
  o += "<?mso-application progid=\"Excel.Sheet\"?>";
  o += "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"";
  o += " xmlns:o=\"urn:schemas-microsoft-com:office:office\"";
  o += " xmlns:x=\"urn:schemas-microsoft-com:office:excel\"";
  o += " xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"";
  o += " xmlns:html=\"http://www.w3.org/TR/REC-html40\">";
  o += "<DocumentProperties xmlns=\"urn:schemas-microsoft-com:office:office\">";
  o += "<Author></Author><LastAuthor></LastAuthor><Created></Created><LastSaved></LastSaved>";
  o += "<Company></Company><Version></Version></DocumentProperties>";
  o += "<ExcelWorkbook  xmlns=\"urn:schemas-microsoft-com:office:excel\">";
  o += "<ProtectStructure>False</ProtectStructure><ProtectWindows>False</ProtectWindows>";
  o += "</ExcelWorkbook><Styles>";
  o += "<Style ss:ID=\"Default\" ss:Name=\"Normal\"><Alignment ss:Vertical=\"Bottom\"/><Borders/>";
  o += "<Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Interior/><NumberFormat/><Protection/>";
  o += "</Style>";
  o += "<Style ss:ID=\"s1\"><Font ss:FontName=\"Calibri\" ss:Bold=\"1\" ss:Size=\"12\"/>";
  o += "<NumberFormat/></Style>";
  o += "<Style ss:ID=\"s21\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<NumberFormat/><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Top\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s22\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><NumberFormat/></Style>";
  o += "<Style ss:ID=\"s23\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<NumberFormat/><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Bottom\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s24\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><NumberFormat/>";
  o += "</Style>";
  o += "<Style ss:ID=\"s31\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Top\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s32\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders/></Style>";
  o += "<Style ss:ID=\"s33\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Bottom\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "</Styles><Names></Names><Worksheet ss:Name=\"Sheet1\">";
  o += "<Table ss:ExpandedColumnCount=\"" + std::to_string(k) + "\" ss:ExpandedRowCount=\"" +
       std::to_string(n + 1) + "\" x:FullColumns=\"1\" x:FullRows=\"1\">";
  // Column 1's width follows the longest first cell between the borders.
  size_t len = 1;
  for (size_t r = headBorder; r >= 1 && r <= bottomBorder && r <= n; ++r) {
    len = std::max(len, g[r - 1][0].size());
  }
  int w1 = len > 10 ? static_cast<int>(len * 6) : 100;
  if (len > 40) w1 -= static_cast<int>(static_cast<double>(len) * .2);
  if (len > 60) w1 = 220;
  for (size_t c = 0; c < k; ++c) {
    o += "<Column ss:AutoFitWidth=\"0\" ss:Width=\"" + std::to_string(c == 0 ? w1 : 58) + "\"/>";
  }
  o += "<Row></Row>";
  auto esc = [](const std::string& s) {
    return o2Subinstr(o2Subinstr(s, "<", "&lt;"), ">", "&gt;");
  };
  auto stack = [&](size_t r, const char* style, const char* style1, bool firstOnly) {
    if (r < 1 || r > n) return;
    o += "<Row>";
    const size_t kk = firstOnly ? 1 : k;
    for (size_t c = 0; c < kk; ++c) {
      o += std::string("<Cell ss:StyleID=\"") + (c == 0 ? style1 : style) +
           "\"><Data ss:Type=\"String\">" + esc(g[r - 1][c]) + "</Data></Cell>";
    }
    o += "</Row>";
  };
  size_t total = 1;
  stack(total, "s21", "s31", false);
  ++total;
  for (size_t r = total, hb = headBorder; r <= hb; ++r) {
    stack(r, "s22", "s32", false);
    ++total;
  }
  stack(total, "s21", "s31", false);
  ++total;
  if (bottomBorder >= 1) {
    for (size_t r = total, last = bottomBorder - 1; r <= last; ++r) {
      stack(r, "s22", "s32", false);
      ++total;
    }
  }
  stack(total, "s23", "s33", false);
  ++total;
  // The note rows ("goldfish") -- written only when MORE than one row is left:
  // `if N > total` with total already one past the bottom border, so a table
  // with a single note line drops it (w2at o14).
  if (n > total) {
    for (size_t r = total; r <= n; ++r) stack(r, "s24", "s24", true);
  }
  o += "</Table><WorksheetOptions xmlns=\"urn:schemas-microsoft-com:office:excel\">";
  o += "<ProtectedObjects>False</ProtectedObjects><ProtectedScenarios>False</ProtectedScenarios>";
  o += "</WorksheetOptions></Worksheet></Workbook>";
  return o;
}
}
namespace piet::handlers {
using namespace table_detail;


ojson runOutreg2(const eval::ParsedCommand& parsed, SessionState& session) {
  // BEFORE the option net, deliberately. `word` is a real outreg2 option, so it
  // sits in the `unimplemented` set -- and the net would therefore answer
  // "valid Stata option, not yet implemented" (rc 198) for it. The honest
  // answer is that the whole OUTPUT FORMAT has no writer here, which is a
  // command-level refusal at rc 199, so it has to be decided first.
  if (hasFlag(parsed.opts, "word")) {
    return ojson(eval::refusedCommand("outreg2", "no_rtf_writer"));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutreg2.known,
                                            kVocabOutreg2.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("outreg2: `using FILENAME' is required");
  }
  // `outreg2 [m_a m_b] using ...` -- the stored estimates, one column each
  // Whatever else precedes `using` is a varlist, which keep()
  // supersedes.
  std::vector<std::string> estNames;
  std::string varlistSpec;
  {
    std::string pre = joinWith(words, " ");
    const size_t open = pre.find('[');
    const size_t close = pre.find(']');
    if (open != std::string::npos && close != std::string::npos && close > open) {
      estNames = splitWs(pre.substr(open + 1, close - open - 1));
      pre = pre.substr(0, open) + " " + pre.substr(close + 1);
    }
    varlistSpec = strutil::trim(pre);
  }

  const auto& opts = parsed.opts;
  // :465-469.
  if (optStr(opts, "keep").has_value() && optStr(opts, "drop").has_value()) {
    return tblErrorRc("cannot specify both keep( ) and drop( )", 198);
  }

  // ---- file names (outreg2) ---------------------------
  // A name with no dot gets `.txt`; a tex / xls-family suffix turns that
  // output ON and moves the ledger to <stem>.txt. The companions are always
  // <stem>.<suffix>. (Dots in a DIRECTORY name are not the extension -- the ado
  // reads them as one, which is its bug, not a format.)
  std::string userFile = *usingPath;
  {
    const size_t slash = userFile.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? userFile : userFile.substr(slash + 1);
    if (base.find('.') == std::string::npos) userFile += ".txt";
  }
  const size_t lastDot = userFile.find_last_of('.');
  const std::string stem = userFile.substr(0, lastDot);
  const std::string suffix = userFile.substr(lastDot + 1);
  bool texOn = hasFlag(opts, "tex");
  bool excelOn = hasFlag(opts, "excel");
  std::string texFile = "tex", excelFile = "xml";
  std::string ledgerUser = userFile;
  if (suffix == "rtf" || suffix == "doc" || suffix == "docx") {
    return ojson(eval::refusedCommand("outreg2", "no_rtf_writer"));
  }
  if (suffix == "xls" || suffix == "xlsx" || suffix == "xml" || suffix == "xlm" ||
      suffix == "csv") {
    excelOn = true;
    excelFile = suffix;
    ledgerUser = stem + ".txt";
  }
  if (suffix == "tex") {
    texOn = true;
    ledgerUser = stem + ".txt";
  }
  // `replace` wins over `append`; neither means append (:519-528).
  const bool replaceMode = hasFlag(opts, "replace");

  O2TexOpts texOpts;
  if (auto tx = optStr(opts, "tex")) {
    for (const auto& w : splitWs(strutil::lower(dequote(*tx)))) {
      if (w.size() >= 1 && std::string("fragment").rfind(w, 0) == 0) {
        texOpts.fragment = true;
      } else if (w.size() >= 1 && std::string("landscape").rfind(w, 0) == 0) {
        texOpts.landscape = true;
      } else if (w == "nopretty" || w == "nopr") {
        // the default
      } else {
        return ojson(eval::unsupportedOption(
            "tex", "outreg2 tex(" + w + "): this build writes tex(), tex(fragment) and "
                   "tex(landscape); `" + w + "` is not implemented"));
      }
    }
  }

  // ---- decimals and formats (:5503-5543) -----------------------------------
  // `numlist int >=0 <=11 max=1` (:449-457): -1 = not a number, -2 = out of range.
  auto readDec = [&](const char* key) -> std::optional<int> {
    if (auto v = optStr(opts, key)) {
      const std::string t = strutil::trim(dequote(*v));
      try {
        size_t pos = 0;
        const int n = std::stoi(t, &pos);
        if (pos != t.size()) return -1;
        return (n >= 0 && n <= 11) ? n : -2;
      } catch (...) {
      }
      return -1;
    }
    return std::nullopt;
  };
  const std::optional<int> dec0 = readDec("dec"), bdec0 = readDec("bdec"),
                           sdec0 = readDec("sdec"), tdec0 = readDec("tdec"),
                           pdec0 = readDec("pdec"), rdec0 = readDec("rdec");
  for (const auto& [nm, v] : {std::pair<const char*, std::optional<int>>{"dec", dec0},
                              {"bdec", bdec0},
                              {"sdec", sdec0},
                              {"tdec", tdec0},
                              {"pdec", pdec0},
                              {"rdec", rdec0}}) {
    if (v.has_value() && *v == -1) {
      return tblErrorRc(std::string(nm) + "() invalid -- invalid numlist", 121);
    }
    if (v.has_value() && *v == -2) {
      return tblErrorRc(std::string(nm) +
                            "() invalid -- invalid number, outside of allowed range",
                        125);
    }
  }
  const int decBase = dec0.value_or(3);
  const int bdec = bdec0.value_or(decBase), sdec = sdec0.value_or(decBase),
            tdec = tdec0.value_or(decBase), pdec = pdec0.value_or(decBase),
            rdec = rdec0.value_or(decBase);
  int autoD = 3;
  if (auto av = optStr(opts, "auto")) {
    try {
      autoD = std::stoi(dequote(*av));
    } catch (...) {
      return tblErrorRc("auto() invalid -- integer required", 198);
    }
  }
  const int less = 0;
  // `dec()` or `bdec()` switches the adaptive shape off for EVERY statistic
  // (:5541-5543).
  const bool noauto = hasFlag(opts, "noauto") || dec0.has_value() || bdec0.has_value();

  // `adec(#)`: outreg2 `numlist int >=0 <=11 max=1`, each bound with its
  // own rc and message (w1b_adec_rule.log d2/g1/g2/g3).
  int adec = -1;
  if (auto av = optStr(opts, "adec")) {
    const std::vector<std::string> toks = splitWs(dequote(*av));
    if (toks.size() != 1) return tblErrorRc("adec() invalid -- single number required", 123);
    size_t pos = 0;
    double d = 0;
    bool numeric = true;
    try {
      d = std::stod(toks[0], &pos);
    } catch (const std::exception&) {
      numeric = false;
    }
    if (!numeric || pos != toks[0].size()) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    if (d != std::trunc(d)) {
      return tblErrorRc("adec() invalid -- invalid number, noninteger value", 126);
    }
    if (d < 0 || d > 11) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    adec = static_cast<int>(d);
    if (!optStr(opts, "addstat")) {
      return tblErrorRc("cannot choose adec option without addstat option", 198);
    }
  }

  // ---- stats() (:1491-1611) --------------------------------------------------
  const bool tstatOpt = hasFlag(opts, "tstat");
  // `nose` keeps the coefficient row only (StataBE: the file loses its two
  // standard-error rows, the legend stays), and is refused beside stats().
  const bool noSe = hasFlag(opts, "nose");
  std::vector<std::string> statsList;
  const bool statsGiven = optStr(opts, "stats").has_value();
  if (statsGiven && noSe)
    return tblErrorRc("cannot specify both st:ats( ) and nose options", 198);
  if (noSe) {
    statsList = {"coef"};
  } else if (statsGiven) {
    std::string spec = dequote(*optStr(opts, "stats"));
    for (char& c : spec) {
      if (c == ',') c = ' ';
    }
    statsList = splitWs(spec);
    if (statsList.empty()) return tblErrorRc("stats() invalid", 198);
    if (tstatOpt) return tblErrorRc("cannot specify both st:ats( ) and tstat options", 198);
  } else {
    statsList = {"coef", tstatOpt ? "tstat" : "se"};
  }
  for (auto& s : statsList) {
    s = strutil::lower(s);
    if (s != "coef" && s != "se" && s != "tstat" && s != "pval") {
      return ojson(eval::unsupportedOption(
          "stats", "outreg2 stats(): this build knows coef, se, tstat and pval; `" + s +
                       "` names none of them"));
    }
  }
  // parenthesis: the EVEN positions of the list (:5784-5800), `se` when none.
  std::vector<bool> paren(statsList.size(), false);
  std::string parenFirst;
  for (size_t i = 1; i < statsList.size(); i += 2) {
    paren[i] = true;
    if (parenFirst.empty()) parenFirst = statsList[i];
  }
  if (parenFirst.empty()) parenFirst = "se";
  // The legend (:6215-6243). `statsList` carries a leading blank when stats()
  // was typed, so only the option forms meet the exact "coef se" test.
  std::string statxt;
  if (!statsGiven && !tstatOpt) {
    statxt = "Standard errors";
  } else if (parenFirst == "se") {
    statxt = "Standard errors";
  } else if (parenFirst == "tstat") {
    statxt = "t-statistics";  // z when e(df_r) is missing, settled per model below
  } else {
    statxt = parenFirst;
  }
  // `stats()` other than (coef se) shows the dependent variable as its own,
  // empty, first row (:4225-4229).
  const bool depvarRow =
      statsGiven && !(statsList.size() == 2 && statsList[0] == "coef" && statsList[1] == "se");

  const bool label = hasFlag(opts, "label");
  // eval.varLabels is a PROJECTION of the variable plane, refreshed on demand
  // (session_state.h, BUG-208): the label column and the depvar ctitle read it.
  if (label) session.syncEvalVarLabels();
  const bool nocons = hasFlag(opts, "nocons");
  const bool noaster = hasFlag(opts, "noaster");
  const bool nonotes = hasFlag(opts, "nonotes");
  const bool noobs = hasFlag(opts, "noobs");
  const bool nor2 = hasFlag(opts, "nor2");
  const bool adjr2 = hasFlag(opts, "adjr2");

  std::vector<O2Token> keepToks, dropToks;
  bool keepGiven = false;
  if (auto kp = optStr(opts, "keep")) {
    keepToks = o2ExpandTokens(*kp, session);
    keepGiven = true;
  } else if (!varlistSpec.empty()) {
    keepToks = o2ExpandTokens(varlistSpec, session);
    keepGiven = true;
  }
  if (auto dp = optStr(opts, "drop")) dropToks = o2ExpandTokens(*dp, session);
  const std::vector<std::string> sortvar =
      optStr(opts, "sortvar") ? o2SortvarList(*optStr(opts, "sortvar"), session)
                              : std::vector<std::string>{};

  // ---- addstat, evaluated ONCE before the estimates loop (:610, :10245) ------
  std::vector<std::pair<std::string, std::string>> addstatRows;
  if (auto as = optStr(opts, "addstat")) {
    std::vector<std::string> items = o2SplitCommas(*as);
    std::vector<std::pair<std::string, std::string>> stats;
    for (size_t i = 0; i + 1 < items.size(); i += 2) stats.emplace_back(items[i], items[i + 1]);
    for (size_t j = 0; j < stats.size(); ++j) {
      const std::string& name = stats[j].first;
      const std::string expr = strutil::trim(stats[j].second);
      if (name.empty()) return tblErrorRc("empty strings not allowed in addstat() option", 6);
      std::optional<double> v = piet::evalScalarExpr(expr, session);
      if (!v.has_value() && session.datastore() != nullptr && session.datastore()->has(expr)) {
        // `local value = VAR` reads the first observation.
        v = piet::evalScalarExpr(expr + "[1]", session);
      }
      std::string cell;
      if (!v.has_value()) {
        // `local value = <expr>` (:10289): an e() reference that is missing
        // writes `.`; an r() one dereferences to nothing; any other name is
        // evaluated as an expression and dies there -- `addstat(Controls, Yes)`
        // is r(111) "Yes not found" (w2at o20, log 3846).
        if (expr.find("e(") != std::string::npos) {
          cell = kMissingCell;
        } else if (expr.rfind("r(", 0) == 0) {
          cell.clear();
        } else {
          return tblErrorRc(expr + " not found", 111);
        }
      } else if (adec >= 0) {
        const AdecCell ac = outreg2AdecCell(*v, adec);
        if (ac.seps == 1) {
          // BUG-493, followed bug-for-bug on the User's 2026-09-18 ruling: the
          // separator re-enters outreg2's addstat parse as a field separator. A
          // TRAILING pair is left without a value (r(198)); an earlier one pushes
          // the next pair's name into a value slot (`B not found`, r(111)).
          // w1b_adec_rule.log d4/corpus_big and d3.
          if (j + 1 < stats.size()) {
            return tblErrorRc(stats[j + 1].first + " not found", 111);
          }
          return tblErrorRc(
              "outreg2: `" + name + "' is " + ac.text + " under adec(" + std::to_string(adec) +
                  "), and outreg2 re-reads that thousands separator as an addstat() field"
                  " separator -- Stata answers `invalid syntax' here too. Lower adec() until"
                  " the separator no longer fits, or scale the statistic.",
              198);
        }
        if (ac.seps >= 2) {
          // The one place this build does NOT follow the ado: two separators make
          // outreg2 write a truncated value in a mis-parsed table (w1b_adec_rule3
          // t5, rc 0). Refused rather than published.
          return tblErrorRc(
              "outreg2: `" + name + "' is " + ac.text + " under adec(" + std::to_string(adec) +
                  "), which outreg2 2.x mis-parses into two rows with a truncated value (it"
                  " reports 9 for 9999999). This build refuses rather than write that table;"
                  " lower adec() or scale the statistic.",
              198);
        }
        cell = ac.text;
      } else {
        // BUG-494: no adec() -- `string(v)` (%9.0g) first, then autodigits2 on
        // that text (:10289-10301, :10406-10420).
        double vv = *v;
        const std::string g9 = piet::fmtStataDefaultG(*v);
        try {
          vv = std::stod(g9);
        } catch (...) {
        }
        cell = o2AutoDigits2Cell(vv, autoD, less);
      }
      addstatRows.emplace_back(name, cell);
    }
  }
  std::vector<std::pair<std::string, std::string>> addtextRows;
  if (auto at = optStr(opts, "addtext")) {
    std::vector<std::string> items = o2SplitCommas(*at);
    for (size_t i = 0; i < items.size(); i += 2) {
      addtextRows.emplace_back(items[i], i + 1 < items.size() ? items[i + 1] : std::string());
    }
  }
  std::vector<std::string> addnotes;
  if (auto an = optStr(opts, "addnote")) addnotes = o2SplitCommas(*an);

  // ---- the models -----------------------------------------------------------
  std::vector<Model> models;
  {
    ojson err;
    if (estNames.empty()) {
      Model m;
      if (!activeModel(session, "outreg2", &m, &err)) return err;
      models.push_back(std::move(m));
    } else {
      models = collectModels(session, "outreg2", estNames, &err);
      if (models.empty()) return err.is_null() ? tblErrorRc("estimation result not found", 111)
                                               : err;
    }
  }

  const std::string ledgerResolved = resolveUserPath(session, ledgerUser);
  bool fileExist = cecon::native_exists(ledgerResolved);
  bool append = !replaceMode;
  O2Grid ledger;  // the raw (pre-cleanFile) ledger
  if (append && fileExist) {
    bool ok = false;
    const std::string existing = readWholeFile(ledgerResolved, &ok);
    if (!ok) return tblError("outreg2: cannot read " + ledgerUser, "data_io");
    ledger = o2ParseLedger(existing);
    if (ledger.empty() || ledger[0].size() < 2) {
      return tblError("outreg2: " + ledgerUser + " is not an outreg2 table");
    }
  }
  std::vector<std::string> coefNamesAll;  // varname_list1 + varname_list2

  for (size_t e = 0; e < models.size(); ++e) {
    const Model& m = models[e];
    if (e == 1 && !append) append = true;  // :1708-1711
    const std::string depvar = m.snap.value("depvar", std::string());
    std::string ctDepvar = depvar;
    if (label && !depvar.empty()) {
      const std::string l = session.eval.varLabels.get(depvar);
      if (!l.empty()) ctDepvar = l;
    }
    std::string ctitleList;
    if (auto ct = optStr(opts, "ctitle")) {
      ctitleList = *ct;  // per-item quotes are o2SplitCommas' to strip
    } else if (!estNames.empty()) {
      ctitleList = m.name + ", " + ctDepvar;  // :2150-2152
    } else {
      ctitleList = ctDepvar;
    }
    std::vector<std::string> ctitles = o2SplitCommas(ctitleList);
    if (ctitles.empty()) ctitles.push_back(std::string());

    std::vector<O2Row> col;
    col.push_back({"", "(1)", "", -1});
    for (size_t i = 0; i < ctitles.size(); ++i) {
      col.push_back({i + 1 == ctitles.size() ? "VARIABLES" : "", ctitles[i], "", -1});
    }
    col.push_back({"", "", "", 0});

    // The body (makeFile :4080-4900, :5503-5937).
    const bool dfMissing = !m.escalars.contains("df_r") || !m.escalars["df_r"].is_number();
    auto num = [](const ojson& r, const char* k) -> double {
      if (!r.contains(k) || !r[k].is_number()) return kO2Missing;
      return r[k].get<double>();
    };
    auto statCell = [&](const std::string& st, double b, double se, double t, double p) {
      double v = kO2Missing;
      bool fixed = noauto;
      int d = 3;
      if (st == "coef") {
        v = b;
        fixed = fixed || bdec0.has_value();
        d = bdec;
      } else if (st == "se") {
        v = se;
        fixed = fixed || sdec0.has_value();
        d = sdec;
      } else if (st == "tstat") {
        v = t;
        fixed = fixed || tdec0.has_value();
        d = tdec;
      } else {
        v = p;
        fixed = fixed || pdec0.has_value();
        d = pdec;
      }
      return fixed ? o2Fixed(v, d) : o2AutoCell(v, autoD, less);
    };
    auto pushCoef = [&](const std::string& report, const std::string& varname, double b,
                        double se, double t, double p, bool omitted) {
      for (size_t si = 0; si < statsList.size(); ++si) {
        std::string cell;
        if (omitted) {
          cell = si == 0 ? "-" : std::string();
        } else {
          cell = statCell(statsList[si], b, se, t, p);
          if (statsList[si] == "coef" && !noaster && !o2IsMissing(p)) {
            const double ap = std::fabs(p);
            if (ap < .01) {
              cell += "***";
            } else if (ap < .05) {
              cell += "**";
            } else if (ap < .10) {
              cell += "*";
            }
          }
          if (paren[si] && !cell.empty()) cell = "(" + cell + ")";
        }
        col.push_back({si == 0 ? report : std::string(), cell, varname, 1});
      }
      coefNamesAll.push_back(varname);
    };
    if (depvarRow && !depvar.empty()) {
      pushCoef(depvar, depvar, kO2Missing, kO2Missing, kO2Missing, kO2Missing, false);
    }
    if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
      // e(b) order: this build's coeftable leads with the intercept, e(b) (and
      // so outreg2's rows) ends with it -- o_plain.txt, w2at o05a.
      std::vector<const ojson*> order;
      for (const auto& r : m.snap["coeftable"]) {
        const std::string nm = r.value("name", std::string());
        if (nm != "_cons" && nm != "(intercept)") order.push_back(&r);
      }
      for (const auto& r : m.snap["coeftable"]) {
        const std::string nm = r.value("name", std::string());
        if (nm == "_cons" || nm == "(intercept)") order.push_back(&r);
      }
      for (const ojson* rp : order) {
        const ojson& r = *rp;
        std::string raw = r.value("name", std::string());
        if (raw == "(intercept)") raw = "_cons";
        const std::string name = o2StataName(raw);
        const bool isCons = name == "_cons";
        if (keepGiven && !isCons && !o2TokenHits(keepToks, name)) continue;
        if (!dropToks.empty() && o2TokenHits(dropToks, name)) continue;
        if (isCons && nocons) continue;
        // nobase (:4891): a base level is not reported.
        const size_t dot = name.find('.');
        if (dot != std::string::npos && name.find('#') == std::string::npos &&
            o2IsFactorPart(name) && name[dot - 1] == 'b') {
          continue;
        }
        const double b = num(r, "b"), se = num(r, "se");
        // fvts_type "o" (`2o.g`, `o.x2`) with a zero estimate prints `-` (:5729).
        const bool omitted = dot != std::string::npos && dot > 0 &&
                             name.find('#') == std::string::npos && name[dot - 1] == 'o' &&
                             b == 0 && se == 0;
        pushCoef(isCons ? "Constant" : name, name, b, se, num(r, "t"), num(r, "p"), omitted);
      }
    }
    col.push_back({"", "", "", 2});
    if (!noobs) {
      const double nObs = m.escalars.contains("N") && m.escalars["N"].is_number()
                              ? m.escalars["N"].get<double>()
                              : kO2Missing;
      col.push_back({"Observations", o2Fixed(nObs, 0), "", 3});
    }
    if (!nor2) {
      const char* key = adjr2 ? "r2_a" : "r2";
      const double rsq = m.escalars.contains(key) && m.escalars[key].is_number()
                             ? m.escalars[key].get<double>()
                             : kO2Missing;
      if (adjr2 && o2IsMissing(rsq)) {
        return tblErrorRc(
            "Adjusted R-squared (e(r2_a)) not defined; cannot use adjr2 option", 198);
      }
      if (!o2IsMissing(rsq)) {
        col.push_back({adjr2 ? "Adjusted R-squared" : "R-squared", o2Fixed(rsq, rdec), "", 3});
      }
    }
    for (const auto& [n, v] : addtextRows) col.push_back({n, v, "", 3});
    for (const auto& [n, v] : addstatRows) col.push_back({n, v, "", 3});
    if (!nonotes && (!append || !fileExist)) {
      std::string st = statxt;
      if (st == "t-statistics" && dfMissing) st = "z-statistics";
      // `robust = e(vcetype)`, and "Robust" prefixes the lowered legend
      // (:1931, :6235-6237): vce(robust) and vce(cluster) both say so. A stored
      // estimate answers with ITS e(), as `estimates restore` would install it.
      std::string vcetype = session.eval.eStrings.get("vcetype");
      if (!estNames.empty()) {
        auto sm = session.estimatesModels.find(m.name);
        vcetype = (sm != session.estimatesModels.end() && sm->second != nullptr)
                      ? sm->second->eStrings.get("vcetype")
                      : std::string();
      }
      if (vcetype == "Robust") st = "Robust " + strutil::lower(st);
      col.push_back({st + " in parentheses", "", "", 3});
      if (!noaster) col.push_back({"*** p<0.01, ** p<0.05, * p<0.1", "", "", 3});
    }
    if (!append || !fileExist) {
      for (const auto& n : addnotes) col.push_back({n, "", "", 3});
    }

    if (append && fileExist) {
      // appendFile strips the LABELS column and the title rows of what is on
      // disk before merging (:3276-3311).
      O2Grid f1 = ledger;
      bool hasLabels = false;
      for (const auto& r : f1) {
        if (r.size() >= 2 && r[1] == "LABELS") hasLabels = true;
      }
      if (hasLabels) {
        for (auto& r : f1) {
          if (r.size() >= 2) r.erase(r.begin() + 1);
        }
      }
      while (!f1.empty() && !f1[0][0].empty() && (f1[0].size() < 2 || f1[0][1].empty())) {
        f1.erase(f1.begin());
      }
      if (f1.empty() || f1[0].size() < 2) {
        return tblError("outreg2: " + ledgerUser + " is not an outreg2 table");
      }
      ledger = o2AppendFile(f1, col, sortvar, &coefNamesAll);
    } else {
      ledger.clear();
      for (const auto& r : col) ledger.push_back({r.report, r.cell});
      fileExist = true;
    }
  }

  // ---- cleanFile (:9087-9384) -----------------------------------------------
  size_t headRow = 0, bottomRow = 0;
  o2Borders(ledger, &headRow, &bottomRow);
  O2Grid clean = ledger;
  if (label) {
    std::map<std::string, std::string> labels;
    for (const auto& n : coefNamesAll) {
      if (n.empty() || labels.count(n)) continue;
      labels[n] = (n == "_cons" || n == "Constant") ? std::string("Constant")
                                                     : o2FvtsLabel(n, session);
    }
    labels["_cons"] = "Constant";
    labels["Constant"] = "Constant";
    for (size_t i = 0; i < clean.size(); ++i) {
      auto it = labels.find(clean[i][0]);
      std::string l = it == labels.end() ? std::string() : it->second;
      if (i + 1 == headRow) l = "LABELS";
      clean[i].insert(clean[i].begin() + 1, l);
    }
  }

  ojson out = ojson::array();
  {
    ojson werr =
        writeTextFileAtomic("outreg2", ledgerResolved, ledgerUser, o2RenderLedger(clean));
    if (!werr.is_null()) return werr;
  }

  // The grid the companions render: the label column merged into the name
  // column, LABELS -> VARIABLES (:2754-2766).
  O2Grid shown = clean;
  if (label) {
    for (auto& r : shown) {
      if (r.size() >= 2 && r[1].empty()) r[1] = r[0];
      r.erase(r.begin());
      if (r[0] == "LABELS") r[0] = "VARIABLES";
    }
  }
  if (texOn) {
    const std::string texUser = stem + "." + texFile;
    const std::string texResolved = resolveUserPath(session, texUser);
    ojson werr = writeTextFileAtomic("outreg2", texResolved, texUser,
                                     o2RenderTex(shown, headRow, bottomRow, texOpts));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock(texUser));
  }
  if (excelOn) {
    const std::string xmlUser = stem + "." + excelFile;
    const std::string xmlResolved = resolveUserPath(session, xmlUser);
    ojson werr = writeTextFileAtomic("outreg2", xmlResolved, xmlUser,
                                     o2RenderXml(shown, headRow, bottomRow));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock(xmlUser));
  }
  out.push_back(displayBlock("dir : seeout"));
  return out;
}
}
