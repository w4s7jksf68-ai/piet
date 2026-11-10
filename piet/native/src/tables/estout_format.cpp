// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// estout format.
#include "tables_internal.h"
#include "estout_internal.h"
namespace piet::handlers::table_detail {


std::string eoTrim(const std::string& s) { return strutil::trim(s); }


// `di %-#s`, `di %#s`, `di %~#s`.
// Widths count CHARACTERS, not bytes (StataBE, scratch-r150/uni-v/p4.do).
std::string eoPadR(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  return (w <= 0 || static_cast<int>(n) >= w) ? s : s + std::string(w - n, ' ');
}

std::string eoPadL(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  return (w <= 0 || static_cast<int>(n) >= w) ? s : std::string(w - n, ' ') + s;
}

std::string eoCentre(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  if (w <= 0 || static_cast<int>(n) >= w) return s;
  const size_t left = (w - n) / 2;
  return std::string(left, ' ') + s + std::string(w - n - left, ' ');
}


// Stata's abbrev(s, n) for a NAME, as BUG-490 measured it (w1b_esttab_abbrev2)
// with the 32-character window estout's own Abbrev runs it on
// (estout; ek_e43 L8 `Second regressor w~l`): keep the trailing
// digit run (or the last character), as much of the head as fits, `~` between.
// Every length is in CHARACTERS (a Unicode name is cut and counted by code
// points, never mid-sequence): StataBE abbreviates by characters.
std::string eoStataAbbrev(std::string s, int w) {
  namespace id = cecon::ident;
  if (w <= 0 || static_cast<int>(id::codePointCount(s)) <= w) return s;
  s = id::truncateCodePoints(s, 32);
  if (static_cast<int>(id::codePointCount(s)) <= w) return s;
  if (w < 3) return id::truncateCodePoints(s, static_cast<size_t>(w));
  size_t k = 0;  // trailing ASCII digits: one byte, one character each
  while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[s.size() - 1 - k]))) ++k;
  size_t tailBytes = k;
  if (k == 0) {  // no digit run: the last CHARACTER
    size_t i = s.size();
    do {
      --i;
    } while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
    tailBytes = s.size() - i;
    k = 1;
  }
  if (k > static_cast<size_t>(w) - 2) {
    k = static_cast<size_t>(w) - 2;
    tailBytes = k;
  }
  return id::truncateCodePoints(s, static_cast<size_t>(w) - 1 - k) + "~" +
         s.substr(s.size() - tailBytes);
}


// estout vFormat (without par/dmarker/msign, which the caller adds).
// Returns false when the format is not one Stata accepts (r(198)).
// estout reads every cell through a macro first (`local value = St[r,m]`,
// estout), so it formats the value's 16-digit macro text read
// back: StataBE writes estadd's 0.715/5.2 (0x1.1999999999999p-3, which
// `display %9.3f` shows as 0.137) as 0.138 (uni-oracle/et2.do; AER 007's
// Table_A9_Panel_A beta/control mean).
bool eoFormatNumber(double vIn, const std::string& fmtIn, std::string* out) {
  double v = vIn;
  if (std::isfinite(v)) {
    const std::string mt = eval::stataMacroRealText(v);
    if (mt != ".") v = std::strtod(mt.c_str(), nullptr);
  }
  std::string fmt = eoTrim(fmtIn);
  if (fmt.empty()) fmt = "%9.0g";
  if (std::isnan(v)) {
    *out = ".";
    return true;
  }
  if (fmt[0] == 'a') {
    // SignificantDigits (estout).
    int d = 3;
    if (fmt.size() > 1) {
      try {
        size_t used = 0;
        d = std::stoi(fmt.substr(1), &used);
        if (used + 1 != fmt.size()) return false;
      } catch (...) {
        return false;
      }
    }
    *out = sigDigits(v, d, /*minDec=*/1, /*comma=*/false);
    return true;
  }
  bool isInt = !fmt.empty();
  for (char c : fmt) {
    if (!std::isdigit(static_cast<unsigned char>(c))) isInt = false;
  }
  if (isInt) {
    // `fmt(#)` is %(#+9).#f (estout).
    const int dec = std::stoi(fmt);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
    *out = buf;
    return true;
  }
  if (fmt == "%g" || fmt == "g") fmt = "%9.0g";
  if (fmt[0] != '%') return false;
  *out = eoTrim(eval::applyStataFmt(v, fmt));
  return true;
}


// The full vFormat: number, then (when `lz` is off) the leading zero dropped,
// then par() around it.
bool eoVFormat(double v, const std::string& fmt, bool lz, const std::vector<std::string>& par,
               std::string* out) {
  std::string val;
  if (!eoFormatNumber(v, fmt, &val)) return false;
  if (!lz) {
    if (val.rfind("0.", 0) == 0 || val.find("-0.") != std::string::npos) {
      const size_t at = val.find("0.");
      if (at != std::string::npos) val.replace(at, 2, ".");
    }
  }
  if (!par.empty()) {
    val = par[0] + val + (par.size() > 1 ? par[1] : std::string());
  }
  *out = val;
  return true;
}


// estout Stars.
std::string eoStars(const std::vector<std::pair<std::string, std::string>>& lv, const EoV& p) {
  if (p.kind != EoV::kNum || p.v < 0.0 || p.v > 1.0) return std::string();
  std::string out;
  for (const auto& [sym, lvl] : lv) {
    double th = 0.0;
    try {
      th = std::stod(lvl);
    } catch (...) {
      continue;
    }
    if (th <= p.v) break;
    out = sym;
  }
  return out;
}


// estout CheckStarvals (the legend text).
std::string eoStarLegend(const EoSpec& sp) {
  std::string leg;
  for (const auto& [sym, lvl] : sp.starlevels) {
    if (!leg.empty()) leg += sp.starlevelsDelim;
    std::string lab = sp.starlevelsLabel;
    if (lab.find('@') != std::string::npos) lab = eoReplaceAll(lab, "@", lvl);
    else lab += lvl;
    leg += sym + lab;
  }
  return leg;
}


// The whole write phase. `out` receives the file text (every line ends `\n`).
bool eoRender(const EoSpec& sp, std::string* out, EoError* err, int* ccolsOut ) {
  SessionState& session = *sp.session;
  std::vector<EoModel> models = sp.models;  // TableIsAMess may rename equations
  const size_t M = models.size();
  // rename() (estout RenameCoefs), per model before the union: every
  // row named `from` (in `eq` when it is given) is called `to`.
  if (!sp.rename.empty()) {
    for (auto& em : models) {
      auto ren = [&](const EoKey& k) {
        EoKey out = k;
        for (const auto& [from, to] : sp.rename) {
          std::string equ, x = from;
          const size_t colon = from.find(':');
          if (colon != std::string::npos) {
            equ = from.substr(0, colon);
            if (equ == "_") equ = "__";
            x = from.substr(colon + 1);
          }
          if (!equ.empty() && equ != k.first) continue;
          if (out.second == x) out.second = to;
        }
        return out;
      };
      for (auto& k : em.coefRows) k = ren(k);
      std::map<EoKey, EoCoefVals> nv;
      for (const auto& [k, v] : em.vals) nv[ren(k)] = v;
      em.vals = nv;
      for (auto& [st, ord] : em.extraOrder) {
        for (auto& k : ord) k = ren(k);
      }
      for (auto& [st, mp] : em.extra) {
        std::map<EoKey, double> nm;
        for (const auto& [k, v] : mp) nm[ren(k)] = v;
        mp = nm;
      }
      std::map<std::string, double> nb;
      for (const auto& [k, v] : em.beta) nb[ren(EoKey{"__", k}).second] = v;
      em.beta = nb;
    }
  }
  const EoCells& cells = sp.cells;

  // ---- which statistics name rows (GetCoefs) ----------------------------------
  bool anyCoefStat = false;
  std::vector<std::string> matStats;
  for (const auto& el : cells.elems) {
    if (eoIsCoefStat(el.name) || el.name == "beta") {
      anyCoefStat = true;
    } else if (std::find(matStats.begin(), matStats.end(), el.name) == matStats.end()) {
      matStats.push_back(el.name);
    }
  }
  bool tablehasstars = false;
  for (const auto& el : cells.elems) {
    if (el.star || el.name == "_star" || el.name == "_sigsign") tablehasstars = true;
  }
  // r(ccols): the coefficient statistics a model carries (estout plus
  // the p column stars add).
  int ccols = 0;
  {
    std::set<std::string> vals;
    for (const auto& el : cells.elems) vals.insert(el.name);
    if (tablehasstars) {
      for (const auto& el : cells.elems) {
        if (el.star) vals.insert(el.pvalue);
      }
    }
    ccols = static_cast<int>(vals.size());
  }
  auto rowsOf = [&](const EoModel& m) {
    std::vector<EoKey> rows;
    if (anyCoefStat) rows = m.coefRows;
    bool modelHasNoEq = rows.empty();
    for (const auto& k : rows) {
      if (k.first == "__") modelHasNoEq = true;
    }
    for (const auto& st : matStats) {
      auto it = m.extraOrder.find(st);
      if (it == m.extraOrder.end()) continue;
      bool allNoEq = true;
      for (const auto& k : it->second) {
        if (k.first != "__") allNoEq = false;
      }
      // SEQMerge (estout): an equation-less matrix fills by name into
      // a model that has equations rather than adding rows.
      if (allNoEq && !modelHasNoEq) continue;
      eoCapp(&rows, it->second);
    }
    return rows;
  };
  std::vector<std::vector<EoKey>> mrows(M);
  std::vector<EoKey> B;
  auto buildUnion = [&]() {
    B.clear();
    for (size_t m = 0; m < M; ++m) {
      mrows[m] = rowsOf(models[m]);
      eoCapp(&B, mrows[m]);
    }
  };
  buildUnion();
  bool hasB = !B.empty();

  // ---- TableIsAMess (estout) ----------------------------------
  if (!sp.equationsGiven && !sp.unstack && hasB) {
    std::vector<std::string> ueq;
    for (const auto& k : B) {
      if (std::find(ueq.begin(), ueq.end(), k.first) == ueq.end()) ueq.push_back(k.first);
    }
    if (ueq.size() > 1) {
      bool mess = false;
      for (size_t m = 1; m < M && !mess; ++m) {
        bool has = false;
        for (const auto& k : mrows[m]) {
          if (k.first == ueq[0]) has = true;
        }
        if (!has) mess = true;
      }
      if (mess) {
        for (auto& em : models) {
          const std::vector<EoKey> r0 = rowsOf(em);
          if (r0.empty()) continue;
          const std::string first = r0.front().first;
          auto ren = [&](const EoKey& k) { return k.first == first ? EoKey{"main", k.second} : k; };
          for (auto& k : em.coefRows) k = ren(k);
          std::map<EoKey, EoCoefVals> nv;
          for (const auto& [k, v] : em.vals) nv[ren(k)] = v;
          em.vals = nv;
          for (auto& [st, ord] : em.extraOrder) {
            for (auto& k : ord) k = ren(k);
          }
          for (auto& [st, mp] : em.extra) {
            std::map<EoKey, double> nm;
            for (const auto& [k, v] : mp) nm[ren(k)] = v;
            mp = nm;
          }
        }
        buildUnion();
      }
    }
  }

  // ---- order / indicate / keep / drop (estout) -------------------------
  std::vector<std::pair<std::string, std::vector<std::string>>> indicateLbls;  // name, per col
  if (sp.order.has_value() && !B.empty()) {
    std::vector<EoSpecItem> ex;
    std::vector<EoKey> app;
    if (!eoExpandEqVarlist(*sp.order, B, /*append=*/true, &ex, &app, err)) return false;
    for (const auto& a : app) {
      if (std::find(B.begin(), B.end(), a) == B.end()) B.push_back(a);
    }
    eoOrder(*sp.order, ex, &B);
  }
  for (const auto& ind : sp.indicate) {
    std::vector<EoSpecItem> ex;
    if (!B.empty()) {
      if (!eoExpandEqVarlist(ind.vars, B, false, &ex, nullptr, err)) return false;
    }
    std::vector<std::string> lbls;
    for (size_t m = 0; m < M; ++m) {
      bool yes = false;
      for (const auto& k : mrows[m]) {
        if (eoSpecHits(ex, k) && std::find(B.begin(), B.end(), k) != B.end()) yes = true;
      }
      lbls.push_back(yes ? sp.indicateYes : sp.indicateNo);
    }
    std::string name = ind.name;
    if (name.empty()) {
      const std::vector<std::string> t = eoTokens(ind.vars);
      name = t.empty() ? std::string() : t[0];
    }
    indicateLbls.emplace_back(name, lbls);
    eoDropOrKeep(false, ex, &B);
  }
  if (sp.keep.has_value() && !B.empty()) {
    std::vector<EoSpecItem> ex;
    if (!eoExpandEqVarlist(*sp.keep, B, false, &ex, nullptr, err)) return false;
    eoDropOrKeep(true, ex, &B);
  }
  if (sp.drop.has_value() && !B.empty()) {
    std::string dl = *sp.drop;
    if (sp.dropRelax) dl += ", relax";
    std::vector<EoSpecItem> ex;
    if (!eoExpandEqVarlist(dl, B, false, &ex, nullptr, err)) return false;
    eoDropOrKeep(false, ex, &B);
  }
  hasB = !B.empty();
  if (ccolsOut != nullptr) *ccolsOut = hasB ? ccols : -1;

  // ---- names and equations of the final set ------------------------------------------
  size_t R = B.size();
  std::vector<std::string> eqlist;
  for (const auto& k : B) eqlist.push_back(k.first);
  std::vector<std::string> eqs;
  std::vector<int> eqsdims;
  auto uniqEqs = [&]() {
    eqs.clear();
    eqsdims.clear();
    std::string last;
    bool any = false;
    for (const auto& e : eqlist) {
      if (!any || e != last) {
        eqs.push_back(e);
        eqsdims.push_back(0);
      }
      ++eqsdims.back();
      last = e;
      any = true;
    }
    if (eqs.empty()) {
      eqs.push_back("__");
      eqsdims.push_back(0);
    }
  };
  uniqEqs();
  std::vector<std::string> varlist;  // unstack: the coefficient names
  std::vector<std::string> eqswide{"__"};
  if (sp.unstack && R > 0) {
    // RerrangeEqs: tie each equation's rows together.
    std::vector<std::string> ue;
    for (const auto& e : eqlist) {
      if (std::find(ue.begin(), ue.end(), e) == ue.end()) ue.push_back(e);
    }
    std::vector<EoKey> nb;
    for (const auto& e : ue) {
      for (const auto& k : B) {
        if (k.first == e) nb.push_back(k);
      }
    }
    B = nb;
    eqlist.clear();
    for (const auto& k : B) eqlist.push_back(k.first);
    uniqEqs();
    for (const auto& k : B) {
      if (std::find(varlist.begin(), varlist.end(), k.second) == varlist.end()) varlist.push_back(k.second);
    }
    // order() names first, `_cons` last unless order() names it (estout)
    std::vector<std::string> ordNames;
    if (sp.order.has_value()) {
      for (const auto& t : eoTokens(*sp.order)) {
        const size_t c = t.find(':');
        ordNames.push_back(c == std::string::npos ? t : t.substr(c + 1));
      }
    }
    std::vector<std::string> nv;
    for (const auto& o : ordNames) {
      if (std::find(varlist.begin(), varlist.end(), o) != varlist.end() &&
          std::find(nv.begin(), nv.end(), o) == nv.end()) {
        nv.push_back(o);
      }
    }
    for (const auto& v : varlist) {
      if (std::find(nv.begin(), nv.end(), v) == nv.end()) nv.push_back(v);
    }
    if (std::find(ordNames.begin(), ordNames.end(), "_cons") == ordNames.end()) {
      auto it = std::find(nv.begin(), nv.end(), "_cons");
      if (it != nv.end()) {
        nv.erase(it);
        nv.push_back("_cons");
      }
    }
    varlist = nv;
    R = varlist.size();
    eqswide = eqs;
  }
  const size_t numeqs = eqs.size();
  auto rowExists = [&](const EoKey& k) { return std::find(B.begin(), B.end(), k) != B.end(); };

  // ---- model labels (estout) -----------------------------------------------
  std::vector<std::string> mlabels = sp.mlabels.list;
  {
    const bool titles = sp.mlabels.titles || (!sp.mlabels.notitles && sp.label);
    for (size_t i = mlabels.size(); i < M; ++i) {
      std::string lab;
      if (sp.mlabels.depvars) {
        const std::string var = models[i].depname;
        if (sp.label) {
          const size_t dot = var.find('.');
          const std::string tail = dot == std::string::npos ? var : var.substr(dot + 1);
          const std::string head = dot == std::string::npos ? std::string() : var.substr(0, dot + 1);
          bool ex = false;
          std::string l = eoVarLabel(session, tail, &ex);
          if (l.empty()) l = tail;
          lab = head + l;
        } else {
          lab = var;
        }
      } else if (titles) {
        lab = models[i].title;
        if (lab.empty()) lab = models[i].name;
      } else {
        lab = models[i].name;
      }
      mlabels.push_back(lab);
    }
    if (sp.mlabels.numbers) {
      for (size_t i = 0; i < M; ++i) {
        const std::string num = "(" + std::to_string(i + 1) + ")";
        mlabels[i] = mlabels[i].empty() ? num : num + " " + mlabels[i];
      }
    }
  }

  // ---- equation labels (estout) ----------------------------------------------
  std::vector<std::string> eqlabels = sp.eqlabels.list;
  for (size_t i = eqlabels.size(); i < numeqs; ++i) {
    const std::string& eq = eqs[i];
    std::string value;
    if (sp.label) {
      bool ex = false;
      value = eoVarLabel(session, eq, &ex);
    }
    if (value.empty()) value = (eq == "__") ? "_" : eq;
    eqlabels.push_back(value);
  }
  // EqReplaceCons: under eqlabels(none) a one-row equation's `_cons` takes the
  // equation's label.
  std::vector<std::string> varlabelPairs = sp.varlabels.list;
  if (sp.eqlabels.none && numeqs > 1 && !sp.unstack && !(sp.eqlabels.given && sp.eqlabels.list.size() == 1 && sp.eqlabels.list[0].empty())) {
    std::vector<std::string> vlabv;
    for (size_t i = 0; i < varlabelPairs.size(); i += 2) vlabv.push_back(varlabelPairs[i]);
    std::map<std::string, int> cnt;
    for (const auto& e : eqlist) ++cnt[e];
    std::vector<std::string> add;
    std::string last;
    size_t ei = 0;
    std::string eqlab;
    for (size_t i = 0; i < eqlist.size(); ++i) {
      const std::string& eq = eqlist[i];
      if (i == 0 || eq != last) {
        eqlab = ei < eqlabels.size() ? eqlabels[ei] : std::string();
        ++ei;
      }
      last = eq;
      if (cnt[eq] > 1 || eq == "__") continue;
      if (B[i].second == "_cons" &&
          std::find(vlabv.begin(), vlabv.end(), eq + ":_cons") == vlabv.end()) {
        add.push_back(eq + ":_cons");
        add.push_back(eqlab);
      }
    }
    add.insert(add.end(), varlabelPairs.begin(), varlabelPairs.end());
    varlabelPairs = add;
  }

  // ---- column labels (estout) ----------------------------------------------------
  std::vector<std::string> collabels;
  bool collabelsOn = false;
  if (sp.collabels.given && !sp.collabels.none && !sp.collabels.list.empty()) {
    collabels = sp.collabels.list;
    collabelsOn = true;
  } else if (!sp.collabels.none) {
    for (size_t j = 0; j < cells.ncols; ++j) {
      std::string temp;
      for (const auto& row : cells.rows) {
        if (j >= row.size()) continue;
        for (int idx : row[j]) {
          if (!temp.empty()) temp += "/";
          const EoElem& el = cells.elems[static_cast<size_t>(idx)];
          std::string lab = el.labelSet ? el.label : el.name;
          if (!el.labelSet) {
            if (el.name == "ci") lab = "ci" + std::to_string(sp.level);
            if (el.name == "ci_l") lab = "min" + std::to_string(sp.level);
            if (el.name == "ci_u") lab = "max" + std::to_string(sp.level);
          }
          temp += lab;
        }
      }
      collabels.push_back(temp);
    }
    collabelsOn = !collabels.empty();
  }

  // ---- table layout (estout) -------------------------------------------------------
  std::vector<int> modelsrow, eqsrow, colsrow, starsrow;  // -1 = an extracols column
  // vblock[i][c] = (element index, off-for-this-model); an empty list is `.`
  const size_t nrows = cells.rows.size();
  std::vector<std::vector<std::vector<std::pair<int, bool>>>> vblock(nrows);
  bool statsStarAny = false;
  for (const auto& st : sp.stats) {
    if (st.star) statsStarAny = true;
  }
  for (size_t m = 0; m < M; ++m) {
    for (size_t e = 0; e < eqswide.size(); ++e) {
      size_t stc = 0;
      if (sp.unstack && R > 0) {
        bool any = false;
        for (const auto& k : mrows[m]) {
          if (k.first == eqswide[e] && rowExists(k)) any = true;
        }
        if (!any) continue;
      }
      eqsrow.push_back(static_cast<int>(e));
      modelsrow.push_back(static_cast<int>(m));
      size_t k = 0;
      bool something = false;
      for (size_t j = 0; j < cells.ncols; ++j) {
        bool nocol = true, colhasstats = false, starcol = false;
        std::vector<std::vector<std::pair<int, bool>>> col(nrows);
        for (size_t i = 0; i < nrows; ++i) {
          if (j >= cells.rows[i].size()) continue;
          for (int idx : cells.rows[i][j]) {
            const EoElem& el = cells.elems[static_cast<size_t>(idx)];
            colhasstats = true;
            const bool off = m < el.pattern.size() && el.pattern[m] == "0";
            if (!off) {
              nocol = false;
              if (el.star) starcol = true;
            }
            col[i].emplace_back(idx, off);
          }
        }
        if (!colhasstats) nocol = false;
        if (nocol) continue;
        colsrow.push_back(static_cast<int>(j));
        if (++k > 1) {
          modelsrow.push_back(static_cast<int>(m));
          eqsrow.push_back(static_cast<int>(e));
        }
        if (stc++ == 0 && statsStarAny) starcol = true;
        starsrow.push_back(starcol ? 1 : 0);
        for (size_t i = 0; i < nrows; ++i) vblock[i].push_back(col[i]);
        something = true;
      }
      if (!something) {
        for (size_t i = 0; i < nrows; ++i) vblock[i].emplace_back();
        colsrow.push_back(0);
        bool starcol = (stc++ == 0 && statsStarAny);
        starsrow.push_back(starcol ? 1 : 0);
      }
    }
  }
  int neqs = 0;
  {
    int m0 = -1, e0 = -1;
    for (size_t i = 0; i < modelsrow.size(); ++i) {
      if (modelsrow[i] != m0 || eqsrow[i] != e0) ++neqs;
      m0 = modelsrow[i];
      e0 = eqsrow[i];
    }
  }
  if (!sp.extracols.empty()) {
    std::vector<int> ec = sp.extracols;
    std::sort(ec.begin(), ec.end());
    auto ins = [&](auto& row, auto symb) {
      using T = typename std::decay_t<decltype(row)>::value_type;
      std::decay_t<decltype(row)> nv;
      size_t ci = 0;
      for (size_t i = 0; i < row.size(); ++i) {
        while (ci < ec.size() && ec[ci] <= static_cast<int>(i + 1)) {
          nv.push_back(T(symb));
          ++ci;
        }
        nv.push_back(row[i]);
      }
      while (ci < ec.size()) {
        nv.push_back(T(symb));
        ++ci;
      }
      row = nv;
    };
    ins(modelsrow, -1);
    ins(eqsrow, -1);
    ins(colsrow, -1);
    ins(starsrow, -1);
    for (auto& r : vblock) ins(r, std::vector<std::pair<int, bool>>{});
  }
  const size_t nphys = starsrow.size();
  const int ncols = static_cast<int>(nphys) + 1 + (sp.labcol2 ? 1 : 0);

  // ---- widths (estout) --------------------------------------------------------------
  const std::vector<int>& mwv = sp.modelwidth;
  auto mw = [&](size_t c) { return mwv.empty() ? 0 : mwv[c % mwv.size()]; };
  const bool modelwidthzero = (mwv.size() == 1 && mwv[0] == 0) || mwv.empty();
  const int labcol2width = sp.labcol2Width >= 0 ? sp.labcol2Width : (mwv.empty() ? 0 : mwv[0]);
  std::vector<std::pair<std::string, std::string>> starlevels = sp.starlevels;
  if ((tablehasstars || statsStarAny) && starlevels.empty()) {
    starlevels = {{"*", "0.05"}, {"**", "0.01"}, {"***", "0.001"}};
  }
  int starwidth = 0;
  if (!modelwidthzero && (tablehasstars || statsStarAny)) {
    for (const auto& lv : starlevels) starwidth = std::max(starwidth, static_cast<int>(lv.first.size()));
  }
  const bool wrap = sp.wrap && sp.varwidth >= 2;
  const int delwidth = static_cast<int>(sp.delimiter.size());
  int totcharwidth = sp.varwidth;
  if (sp.labcol2) totcharwidth += delwidth + labcol2width;
  for (size_t c = 0; c < nphys; ++c) {
    totcharwidth += delwidth + mw(c);
    if (starsrow[c] != 0) totcharwidth += starwidth;
  }
  std::string hline;
  {
    const int hl = std::min(totcharwidth, 400);
    for (int i = 0; i < hl; ++i) hline += sp.hlinechar;
  }
  const bool anySpan = sp.mgroups.span || sp.mlabels.span || sp.eqlabels.span || sp.collabels.span;
  const int delwidthSpan = anySpan ? (modelwidthzero ? 0 : delwidth) : delwidth;
  const std::string starlegend =
      (tablehasstars || statsStarAny) ? [&]() {
        EoSpec tmp;
        tmp.starlevels = starlevels;
        tmp.starlevelsLabel = sp.starlevelsLabel;
        tmp.starlevelsDelim = sp.starlevelsDelim;
        return eoStarLegend(tmp);
      }()
                                      : std::string();
  const std::string lastend = sp.lastendSet ? sp.lastend : sp.end;

  // ---- output helpers -----------------------------------------------------------------------------
  std::string buf;
  auto w = [&](const std::string& s) { buf += s; };
  auto nl = [&]() { buf += "\n"; };
  auto insertAt = [&](const std::string& v, int type, const std::string& span) {
    std::string s = eoReplaceAll(v, "@span", span);
    if (type == 1) return s;
    s = eoReplaceAll(s, "@M", std::to_string(M));
    s = eoReplaceAll(s, "@E", std::to_string(neqs));
    s = eoReplaceAll(s, "@width", std::to_string(totcharwidth));
    s = eoReplaceAll(s, "@hline", hline);
    if (type == 2) return s;
    s = eoReplaceAll(s, "@title", sp.title);
    s = eoReplaceAll(s, "@note", sp.note);
    s = eoReplaceAll(s, "@discrete", std::string());
    s = eoReplaceAll(s, "@starlegend", starlegend);
    return s;
  };
  auto insertAtLines = [&](const std::vector<std::string>& v, int type) {
    std::vector<std::string> o;
    for (const auto& l : v) o.push_back(insertAt(l, type, std::to_string(ncols)));
    return o;
  };
  auto writeStrLines = [&](const std::vector<std::string>& lines) {
    bool first = true;
    for (const auto& l : lines) {
      if (!first) nl();
      w(l);
      first = false;
    }
  };
  auto writeBegin = [&](const std::vector<std::string>& pre, const std::string& begin,
                        const std::string& post) {
    writeStrLines(pre);
    w(begin);
    w(post);
  };
  auto writeEnd = [&](const std::string& end, const std::vector<std::string>& post,
                      const std::vector<std::string>& post2) {
    w(end);
    writeStrLines(post);
    writeStrLines(post2);
    nl();
  };
  auto fmtV = [&](const std::string& s) { return eoPadR(s, sp.varwidth); };
  auto fmtL2 = [&](const std::string& s) { return eoCentre(s, labcol2width); };
  std::vector<std::string> l2title = sp.labcol2Title;
  std::vector<std::string> l2vals = sp.labcol2Vals;
  auto popChunk = [](std::vector<std::string>* v) {
    std::string c;
    if (!v->empty()) {
      c = v->front();
      v->erase(v->begin());
    }
    return c;
  };
  auto writeL2 = [&](std::vector<std::string>* src) {
    if (!sp.labcol2) return;
    const std::string chunk = popChunk(src);
    w(sp.delimiter);
    w(fmtL2(eoAbbrev(labcol2width, chunk, sp.abbrev)));
  };
  auto starPad = [&](size_t c) {
    if (c < starsrow.size() && starsrow[c] == 1) w(std::string(static_cast<size_t>(starwidth), ' '));
  };

  // WriteCaption (estout). Group ids are 1-based; -1 is an extracols column.
  auto writeCaption = [&](const std::vector<int>& row, const std::vector<int>& rowtwo,
                          const std::vector<std::string>& labels, bool span,
                          const std::string& repeat, const std::string& prefix,
                          const std::string& suffix) {
    int c0 = 2 + (sp.labcol2 ? 1 : 0);
    int nspan = 0;
    int spanwidth = -delwidthSpan;
    std::string repeatlist;
    int lastr = -9, lastrtwo = -9;
    auto label = [&](int r) {
      return (r >= 1 && static_cast<size_t>(r) <= labels.size()) ? labels[static_cast<size_t>(r) - 1]
                                                                 : std::string();
    };
    for (size_t c = 0; c < row.size(); ++c) {
      const int r = row[c];
      const int rtwo = c < rowtwo.size() ? rowtwo[c] : -9;
      const int cw = mw(c);
      if (r == -1) {
        ++c0;
        w(sp.delimiter);
        w(eoPadL("", cw));
        continue;
      }
      if (!span) {
        std::string value;
        if (r != lastr || rtwo != lastrtwo || rowtwo.empty()) {
          value = eoAbbrev(cw, label(r), sp.abbrev);
          value = prefix + value + suffix;
          value = insertAt(value, 1, "1");
        }
        w(sp.delimiter);
        w(eoPadL(value, cw));
        starPad(c);
        lastr = r;
        lastrtwo = rtwo;
      } else {
        ++nspan;
        spanwidth += cw + delwidthSpan;
        if (starsrow[c] == 1) spanwidth += starwidth;
        const int nextr = (c + 1 < row.size()) ? row[c + 1] : -9;
        const int nextrtwo = (c + 1 < rowtwo.size()) ? rowtwo[c + 1] : -9;
        if (r != nextr || rtwo != nextrtwo || rowtwo.empty()) {
          std::string value = eoAbbrev(spanwidth, label(r), sp.abbrev);
          value = prefix + value + suffix;
          value = insertAt(value, 1, std::to_string(nspan));
          w(sp.delimiter);
          w(spanwidth > 0 ? eoPadR(value, spanwidth) : value);
          repeatlist += insertAt(repeat, 1, std::to_string(c0) + "-" + std::to_string(c0 + nspan - 1));
          c0 += nspan;
          nspan = 0;
          spanwidth = -delwidthSpan;
        }
      }
    }
    return repeatlist;
  };
  // WriteEqrow (estout)
  auto writeEqrow = [&](const std::string& value0, bool span, const std::string& prefix,
                        const std::string& suffix) {
    if (!span) {
      std::string value = eoAbbrev(sp.varwidth, value0, sp.abbrev);
      value = prefix + value + suffix;
      value = insertAt(value, 1, "1");
      w(fmtV(value));
      if (sp.labcol2) {
        w(sp.delimiter);
        w(fmtL2(""));
      }
      for (size_t c = 0; c < nphys; ++c) {
        w(sp.delimiter);
        w(eoPadL("", mw(c)));
        if (starsrow[c] == 1) w(std::string(static_cast<size_t>(starwidth), ' '));
      }
      return;
    }
    int nspan = 1;
    int spanwidth = sp.varwidth;
    if (sp.labcol2) {
      ++nspan;
      spanwidth += delwidthSpan + labcol2width;
    }
    for (size_t c = 0; c < nphys; ++c) {
      ++nspan;
      spanwidth += delwidthSpan + mw(c);
      if (starsrow[c] == 1) spanwidth += starwidth;
    }
    std::string value = eoAbbrev(spanwidth, value0, sp.abbrev);
    value = prefix + value + suffix;
    value = insertAt(value, 1, std::to_string(nspan));
    w(spanwidth > 0 ? eoPadR(value, spanwidth) : value);
  };
  // WriteStrRow (estout): one label per (model, equation).
  auto writeStrRow = [&](const std::vector<std::string>& labels) {
    int lastm = -9, laste = -9;
    for (size_t c = 0; c < nphys; ++c) {
      const int cw = mw(c);
      if (modelsrow[c] == -1) {
        w(sp.delimiter);
        w(eoPadL("", cw));
        continue;
      }
      std::string value;
      if (modelsrow[c] != lastm || eqsrow[c] != laste) {
        const size_t idx = static_cast<size_t>(modelsrow[c]) * eqswide.size() +
                           static_cast<size_t>(eqsrow[c]);
        value = idx < labels.size() ? labels[idx] : std::string();
        value = eoAbbrev(cw, value, sp.abbrev);
      }
      w(sp.delimiter);
      w(eoPadL(value, cw));
      starPad(c);
      lastm = modelsrow[c];
      laste = eqsrow[c];
    }
  };
  auto fmtFor = [](const EoElem& el, size_t r) {
    if (el.fmt.empty()) return std::string("%9.0g");
    return r < el.fmt.size() ? el.fmt[r] : el.fmt.back();
  };
  auto pValue = [&](const EoModel& m, const EoKey& key, const EoElem& el) {
    return eoElemValue(m, key, el.pvalue);
  };

  // ---- prehead -------------------------------------------------------------------------------------
  {
    std::vector<std::string> pre = sp.prehead, post = sp.postfoot;
    // estout: with no hooks at all, title/note become them.
    if (sp.prehead.empty() && sp.posthead.empty() && sp.prefoot.empty() && sp.postfoot.empty()) {
      if (!sp.title.empty()) pre = {sp.title};
    }
    for (const auto& line : pre) {
      w(insertAt(line, 0, std::to_string(ncols)));
      nl();
    }
  }

  std::vector<int> modelIds, eqIds, colIds;
  for (size_t c = 0; c < nphys; ++c) {
    modelIds.push_back(modelsrow[c] < 0 ? -1 : modelsrow[c] + 1);
    eqIds.push_back(eqsrow[c] < 0 ? -1 : eqsrow[c] + 1);
    colIds.push_back(colsrow[c] < 0 ? -1 : colsrow[c] + 1);
  }

  // ---- head: model groups ------------------------------------------------------------------------------
  if (sp.mgroups.given && !sp.mgroups.none && !sp.mgroups.list.empty()) {
    const std::vector<std::string> mgb = insertAtLines(sp.mgroups.begin, 2);
    const std::vector<std::string> mge = insertAtLines(sp.mgroups.end, 2);
    std::string tb = sp.begin, te = sp.end;
    if (sp.mgroups.replace) {
      if (!mgb.empty()) tb.clear();
      if (!mge.empty()) te.clear();
    }
    // MgroupsPattern (estout)
    std::vector<int> pat;
    {
      int i = 0, m0 = -9, j = 0;
      for (size_t c = 0; c < nphys; ++c) {
        const int m = modelIds[c];
        if (m == -1) {
          pat.push_back(-1);
          continue;
        }
        if (m != m0) {
          ++i;
          std::string p = (static_cast<size_t>(i) <= sp.mgroups.pattern.size())
                              ? sp.mgroups.pattern[static_cast<size_t>(i) - 1]
                              : std::string();
          if (i == 1) p = "1";
          if (p == "1") ++j;
        }
        pat.push_back(j);
        m0 = m;
      }
    }
    writeBegin(mgb, tb, fmtV(eoAbbrev(sp.varwidth, sp.mgroups.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(pat, pat, sp.mgroups.list, sp.mgroups.span,
                                         sp.mgroups.erepeat, sp.mgroups.prefix, sp.mgroups.suffix);
    writeEnd(te, mge, {rep});
  }
  // ---- head: model numbers ------------------------------------------------------------------------------
  if (sp.numbers) {
    w(sp.begin);
    w(fmtV(""));
    writeL2(&l2title);
    std::vector<std::string> nums;
    for (size_t i = 0; i < M; ++i) nums.push_back(std::to_string(i + 1));
    writeCaption(modelIds, modelIds, nums, sp.mlabels.span, "", sp.numL, sp.numR);
    w(sp.end);
    nl();
  }
  // ---- head: model labels -------------------------------------------------------------------------------
  {
    bool none = sp.mlabels.none;
    if (M == 1 && models[0].name == "." && mlabels.size() == 1 && mlabels[0] == ".") none = true;
    if (!none) {
      const std::vector<std::string> mb = insertAtLines(sp.mlabels.begin, 2);
      const std::vector<std::string> me = insertAtLines(sp.mlabels.end, 2);
      std::string tb = sp.begin, te = sp.end;
      if (sp.mlabels.replace) {
        if (!mb.empty()) tb.clear();
        if (!me.empty()) te.clear();
      }
      writeBegin(mb, tb, fmtV(eoAbbrev(sp.varwidth, sp.mlabels.lhs, sp.abbrev)));
      writeL2(&l2title);
      const std::string rep = writeCaption(modelIds, modelIds, mlabels, sp.mlabels.span,
                                           sp.mlabels.erepeat, sp.mlabels.prefix, sp.mlabels.suffix);
      writeEnd(te, me, {rep});
    }
  }
  // ---- head: equation labels (unstack) -----------------------------------------------------------------
  std::vector<std::string> eqlabelsbegin, eqlabelsend;
  if (!sp.eqlabels.none) {
    eqlabelsbegin = insertAtLines(sp.eqlabels.begin, 2);
    eqlabelsend = insertAtLines(sp.eqlabels.end, 2);
  }
  if (!(eqswide.size() == 1 && eqswide[0] == "__") && !sp.eqlabels.none) {
    std::string tb = sp.begin, te = sp.end;
    if (sp.eqlabels.replace) {
      if (!eqlabelsbegin.empty()) tb.clear();
      if (!eqlabelsend.empty()) te.clear();
    }
    writeBegin(eqlabelsbegin, tb, fmtV(eoAbbrev(sp.varwidth, sp.eqlabels.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(eqIds, modelIds, eqlabels, sp.eqlabels.span,
                                         sp.eqlabels.erepeat, sp.eqlabels.prefix, sp.eqlabels.suffix);
    writeEnd(te, eqlabelsend, {rep});
  }
  // ---- head: column labels ----------------------------------------------------------------------------------
  if (collabelsOn && !sp.collabels.none) {
    const std::vector<std::string> cb = insertAtLines(sp.collabels.begin, 2);
    const std::vector<std::string> ce = insertAtLines(sp.collabels.end, 2);
    std::string tb = sp.begin, te = sp.end;
    if (sp.collabels.replace) {
      if (!cb.empty()) tb.clear();
      if (!ce.empty()) te.clear();
    }
    writeBegin(cb, tb, fmtV(eoAbbrev(sp.varwidth, sp.collabels.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(colIds, {}, collabels, sp.collabels.span,
                                         sp.collabels.erepeat, sp.collabels.prefix, sp.collabels.suffix);
    writeEnd(te, ce, {rep});
  }
  // ---- posthead ------------------------------------------------------------------------------------------------
  for (const auto& line : sp.posthead) {
    w(insertAt(line, 0, std::to_string(ncols)));
    nl();
  }

  // ---- body (estout) ------------------------------------------------------------------------------
  const bool varlabelsnone = sp.varlabels.none;
  const bool varlabelsFirst = sp.varlabels.first != 0;
  const bool varlabelsLast = sp.varlabels.last != 0;
  const bool eqlabelsFirst = sp.eqlabels.first != 0;
  const bool eqlabelsLast = sp.eqlabels.last != 0;
  const std::vector<std::string> varlabelsbegin = insertAtLines(sp.varlabels.begin, 2);
  std::vector<std::string> varlabelsend = insertAtLines(sp.varlabels.end, 2);
  const size_t nind = indicateLbls.size();
  auto varInList = [&](const std::string& var, const std::string& eqvar, const std::string& eq,
                       const std::vector<std::string>& list, bool* found) {
    *found = false;
    for (size_t l = 0; l + 1 <= list.size(); l += 2) {
      std::string lvar = list[l];
      const std::string lab = l + 1 < list.size() ? list[l + 1] : std::string();
      if (sp.unstack) {
        if (var == lvar) {
          *found = true;
          return lab;
        }
      } else {
        if (lvar.rfind("_:", 0) == 0) lvar = "_" + lvar;
        if (lvar == var || lvar == eqvar || lvar == eq + ":") {
          *found = true;
          return lab;
        }
      }
    }
    return std::string();
  };
  std::vector<std::string> varlabelsbegin0 = varlabelsbegin;
  std::vector<std::string> eqlabelsbegin0 = eqlabelsFirst ? eqlabelsbegin : std::vector<std::string>{};
  std::vector<std::string> refcatcoefs, refcatnames;
  for (const auto& [c, n] : sp.refcat) {
    refcatcoefs.push_back(c);
    refcatnames.push_back(n);
  }
  if (nrows > 0 || true) {
    const size_t RI = R + nind;
    size_t e = 0;
    size_t eqdim = R + nind;
    size_t weqcnt = 0;
    std::string theeqlabel;
    std::string eqr, eqrlast;
    bool eqrInit = false;
    for (size_t r = 0; r < R; ++r) {
      std::vector<std::string> varlabelsend0 = varlabelsend;
      const std::string var = sp.unstack ? varlist[r] : B[r].second;
      std::string eqvar;
      if (!sp.unstack) {
        eqvar = B[r].first + ":" + var;
        if (!(eqs.size() == 1 && eqs[0] == "__")) {
          eqrlast = eqr;
          eqr = B[r].first;
          if ((!eqrInit || eqr != eqrlast) && !sp.eqlabels.none) {
            const std::string value = e < eqlabels.size() ? eqlabels[e] : std::string();
            ++e;
            eqdim = static_cast<size_t>(e - 1 < eqsdims.size() ? eqsdims[e - 1] : 0);
            weqcnt = 0;
            if (e == numeqs) {
              if (!eqlabelsLast) eqlabelsend.clear();
              eqdim += nind;
            }
            if (sp.eqlabels.merge) {
              theeqlabel = sp.eqlabels.prefix + value + sp.eqlabels.suffix;
            } else {
              std::string tb = sp.begin, te = sp.end;
              if (sp.eqlabels.replace) {
                if (!eqlabelsbegin0.empty()) tb.clear();
                if (!eqlabelsend.empty()) te.clear();
              }
              writeBegin(eqlabelsbegin0, tb, "");
              writeEqrow(value, sp.eqlabels.span, sp.eqlabels.prefix, sp.eqlabels.suffix);
              writeEnd(te, eqlabelsend, {});
              eqlabelsbegin0 = eqlabelsbegin;
            }
          }
          eqrInit = true;
        }
      }
      ++weqcnt;
      if (weqcnt == 1 && !varlabelsFirst) varlabelsbegin0.clear();

      // rows of the cell block this coefficient writes
      std::vector<size_t> rv;
      for (size_t i = 0; i < nrows; ++i) {
        bool skiprow = true, rowhasstats = false;
        for (size_t c = 0; c < nphys && skiprow; ++c) {
          EoKey key = B.empty() ? EoKey{} : (sp.unstack ? EoKey{} : B[r]);
          if (sp.unstack) {
            const int eqi = eqsrow[c];
            key = EoKey{eqi >= 0 ? eqswide[static_cast<size_t>(eqi)] : std::string(), var};
          }
          for (const auto& [idx, off] : vblock[i][c]) {
            (void)idx;
            if (rowExists(key)) {
              rowhasstats = true;
              if (off) continue;
              skiprow = false;
              break;
            }
          }
        }
        if (!rowhasstats) skiprow = false;
        if (i + 1 == nrows && rv.empty()) skiprow = false;
        if (!skiprow) rv.push_back(i);
      }
      const size_t nrv = rv.size();

      // refcat() above the coefficient
      size_t isref = 0;
      for (size_t i = 0; i < refcatcoefs.size(); ++i) {
        if (refcatcoefs[i] == var) {
          isref = i + 1;
          break;
        }
      }
      auto refcatRow = [&]() {
        std::vector<std::string> lbls;
        for (size_t m = 0; m < M; ++m) {
          for (const auto& eq : (sp.unstack ? eqswide : std::vector<std::string>{eqr.empty() ? B[r].first : eqr})) {
            bool has = false;
            for (const auto& k : mrows[m]) {
              if (k.first == eq && k.second == var) has = true;
            }
            lbls.push_back(has ? sp.refcatLabel : std::string());
          }
        }
        return lbls;
      };
      auto writeRefcat = [&](bool last) {
        std::string tb = sp.begin, te = sp.end;
        if (last && nind == 0 && sp.stats.empty()) te = lastend;
        std::vector<std::string> ve0 = varlabelsend0;
        if (last && weqcnt == eqdim && !varlabelsLast) ve0.clear();
        if (sp.varlabels.replace) {
          if (!varlabelsbegin0.empty()) tb.clear();
          if (!ve0.empty()) te.clear();
        }
        std::string value = varlabelsnone ? std::string()
                                          : eoAbbrev(sp.varwidth, refcatnames[isref - 1], sp.abbrev);
        value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
        writeBegin(varlabelsbegin0, tb, value);
        writeL2(&l2vals);
        writeStrRow(refcatRow());
        writeEnd(te, last ? ve0 : varlabelsend0, {});
        varlabelsbegin0 = varlabelsbegin;
      };
      if (isref != 0 && !sp.refcatBelow) writeRefcat(false);

      // the label
      std::string tmpbegin = sp.begin;
      std::string varl;
      if (!varlabelsnone) {
        bool found = false;
        const std::string bl = varInList(var, eqvar, eqr, sp.varlabels.blist, &found);
        if (found) {
          const std::vector<std::string> lines =
              insertAtLines(bl.find('"') == std::string::npos ? std::vector<std::string>{bl} : eoTokens(bl), 2);
          writeStrLines(lines);
          if (sp.varlabels.replace) {
            tmpbegin.clear();
            varlabelsbegin0.clear();
          }
        }
        varl = sp.label ? eoCompileVarl(session, var, sp.interaction) : var;
        const std::string lab = varInList(var, eqvar, eqr, varlabelPairs, &found);
        if (found) varl = lab;
      }
      if (sp.varlabels.replace && !varlabelsbegin0.empty()) tmpbegin.clear();
      int wrap_i = 1;
      std::string value;
      if (wrap && nrv > 1) {
        value = eoPiece(wrap_i, sp.varwidth, theeqlabel + varl);
        value = eoAbbrev(sp.varwidth, value, sp.abbrev);
      } else {
        value = eoAbbrev(sp.varwidth, theeqlabel + varl, sp.abbrev);
      }
      value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
      writeBegin(varlabelsbegin0, tmpbegin, value);
      writeL2(&l2vals);
      varlabelsbegin0 = varlabelsbegin;

      // the cells
      bool newrow = false;
      for (size_t i : rv) {
        for (size_t c = 0; c < nphys; ++c) {
          if (newrow && c == 0) {
            std::string v;
            if (wrap && nrv > 1) {
              std::string space;
              for (;;) {
                const std::string temp = eoPiece(++wrap_i, sp.varwidth, varl);
                if (temp.empty()) break;
                v += space + temp;
                if (static_cast<size_t>(wrap_i) < nrv) break;
                space = " ";
              }
              v = fmtV(sp.varlabels.prefix + eoAbbrev(sp.varwidth, v, sp.abbrev) + sp.varlabels.suffix);
            } else {
              v = std::string(static_cast<size_t>(std::max(sp.varwidth, 0)), ' ');
            }
            w(sp.end);
            nl();
            w(sp.begin);
            w(v);
            if (sp.labcol2) {
              w(sp.delimiter);
              w(fmtL2(""));
            }
          }
          const int mi = modelsrow[c];
          const auto& elems = vblock[i][c];
          int cwEff = mw(c);
          std::string thevalue;
          EoKey key = sp.unstack ? EoKey{} : B[r];
          bool skipcoef = false;
          if (sp.unstack && mi >= 0) {
            const int eqi = eqsrow[c];
            key = EoKey{eqi >= 0 ? eqswide[static_cast<size_t>(eqi)] : std::string(), var};
            if (!rowExists(key)) skipcoef = true;
          }
          for (size_t t = 0; t < elems.size(); ++t) {
            const EoElem& el = cells.elems[static_cast<size_t>(elems[t].first)];
            const bool off = elems[t].second || mi < 0;
            std::string v;
            if (off) {
              v.clear();
            } else if (skipcoef) {
              v = el.vacant;
            } else {
              const EoModel& em = models[static_cast<size_t>(mi)];
              const size_t fr = sp.unstack ? r : r;
              if (el.name == "ci") {
                const EoV lo = eoElemValue(em, key, "ci_l");
                const EoV hi = eoElemValue(em, key, "ci_u");
                if (lo.kind == EoV::kVacant) {
                  v = el.vacant;
                } else {
                  std::string a, b2;
                  const std::vector<std::string> pl{el.par.size() > 0 ? el.par[0] : "",
                                                    el.par.size() > 1 ? el.par[1] : ""};
                  const std::vector<std::string> pu{"", el.par.size() > 2 ? el.par[2] : ""};
                  if (!eoVFormat(lo.kind == EoV::kNum ? lo.v : kEoNaN, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? pl : std::vector<std::string>{}, &a) ||
                      !eoVFormat(hi.kind == EoV::kNum ? hi.v : kEoNaN, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? pu : std::vector<std::string>{}, &b2)) {
                    err->msg = fmtFor(el, fr) + ": invalid format";
                    err->rc = 198;
                    return false;
                  }
                  v = a + b2;
                }
              } else {
                const EoV x = eoElemValue(em, key, el.name);
                if (x.kind == EoV::kVacant) {
                  v = el.vacant;
                } else if (el.name == "_star") {
                  v = eoStars(starlevels, pValue(em, key, el));
                  if (el.hasPar && el.par.size() >= 1) {
                    v = el.par[0] + v + (el.par.size() > 1 ? el.par[1] : std::string());
                  }
                } else if (el.name == "_sign" || el.name == "_sigsign") {
                  err->msg = "estout: cells(" + el.name + ") is not implemented in this build";
                  err->rc = 198;
                  return false;
                } else {
                  double num = x.kind == EoV::kNum ? x.v : kEoNaN;
                  if (el.abs && (el.name == "t" || el.name == "z") && !std::isnan(num)) num = std::fabs(num);
                  if (!eoVFormat(num, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? el.par : std::vector<std::string>{}, &v)) {
                    err->msg = fmtFor(el, fr) + ": invalid format";
                    err->rc = 198;
                    return false;
                  }
                }
              }
            }
            thevalue += v;
            if (c < starsrow.size() && starsrow[c] == 1) {
              cwEff = mw(c) + starwidth;
              std::string sv;
              if (!off && el.star && !skipcoef) {
                sv = eoStars(starlevels, pValue(models[static_cast<size_t>(mi)], key, el));
              }
              if (t + 1 == elems.size()) sv = eoPadR(sv, starwidth);
              thevalue += sv;
            }
            if (t + 1 < elems.size()) thevalue += sp.incelldelimiter;
          }
          w(sp.delimiter);
          w(eoPadL(thevalue, cwEff));
        }
        newrow = true;
      }

      // end of the table row
      std::string tmpend = sp.end;
      if (!(isref != 0 && sp.refcatBelow)) {
        if (weqcnt == eqdim && !varlabelsLast) varlabelsend0.clear();
        if (r + 1 == RI && sp.stats.empty()) tmpend = lastend;
      }
      if (sp.varlabels.replace && !varlabelsend0.empty()) tmpend.clear();
      std::vector<std::string> elistLines;
      {
        bool found = false;
        const std::string el = varInList(var, eqvar, eqr, sp.varlabels.elist, &found);
        if (found) {
          elistLines = insertAtLines(el.find('"') == std::string::npos ? std::vector<std::string>{el}
                                                                       : eoTokens(el),
                                     2);
          if (sp.varlabels.replace) varlabelsend0.clear();
        }
      }
      writeEnd(tmpend, varlabelsend0, elistLines);
      if (isref != 0 && sp.refcatBelow) writeRefcat(r + 1 == RI);
    }
  }

  // ---- indicator sets (estout) --------------------------------------------------------------
  for (size_t i = 0; i < nind; ++i) {
    std::string tb = sp.begin, te = sp.end;
    if (i + 1 == nind) {
      if (sp.stats.empty()) te = lastend;
      if (!varlabelsLast) varlabelsend.clear();
    }
    if (sp.varlabels.replace) {
      if (!varlabelsbegin0.empty()) tb.clear();
      if (!varlabelsend.empty()) te.clear();
    }
    std::string value = varlabelsnone ? std::string()
                                      : eoAbbrev(sp.varwidth, indicateLbls[i].first, sp.abbrev);
    value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
    writeBegin(varlabelsbegin0, tb, value);
    writeL2(&l2vals);
    writeStrRow(indicateLbls[i].second);
    writeEnd(te, varlabelsend, {});
  }

  // ---- prefoot ------------------------------------------------------------------------------------------------
  for (const auto& line : sp.prefoot) {
    w(insertAt(line, 0, std::to_string(ncols)));
    nl();
  }

  // ---- statistics (estout) ----------------------------------------------------------------------
  {
    const std::vector<std::string> slb = insertAtLines(sp.statslabels.begin, 2);
    std::vector<std::string> sle = insertAtLines(sp.statslabels.end, 2);
    std::vector<std::string> slb0 = slb;
    const bool sFirst = sp.statslabels.first != 0;
    const bool sLast = sp.statslabels.last != 0;
    const size_t S = sp.stats.size();
    for (size_t r = 0; r < S; ++r) {
      const EoStatRow& st = sp.stats[r];
      std::string stat = r < sp.statslabels.list.size() ? sp.statslabels.list[r] : std::string();
      if (stat.empty()) stat = st.key;
      if (sp.statslabels.none) stat.clear();
      if (r == 0 && !sFirst) slb0.clear();
      std::string tb = sp.begin;
      if (sp.statslabels.replace && !slb0.empty()) tb.clear();
      const std::string value =
          fmtV(sp.statslabels.prefix + eoAbbrev(sp.varwidth, stat, sp.abbrev) + sp.statslabels.suffix);
      writeBegin(slb0, tb, value);
      if (r == 0 && !sFirst) slb0 = slb;
      writeL2(&l2vals);
      int lastm = -9;
      size_t stc = 0;
      for (size_t c = 0; c < nphys; ++c) {
        const int cw = mw(c);
        const int m = modelsrow[c];
        if (m == -1) {
          w(sp.delimiter);
          w(eoPadL("", cw));
          continue;
        }
        if (m != lastm) stc = 0;
        ++stc;
        std::string cell;
        bool has = false;
        if (stc == 1) {
          const EoV v = eoStatValue(models[static_cast<size_t>(m)], st.key);
          if (v.kind == EoV::kText) {
            cell = v.text;
            has = true;
          } else if (v.kind == EoV::kNum || v.kind == EoV::kMissing) {
            if (!eoVFormat(v.kind == EoV::kNum ? v.v : kEoNaN, st.fmt, sp.lz, {}, &cell)) {
              err->msg = st.fmt + ": invalid format";
              err->rc = 198;
              return false;
            }
            has = true;
          }
        }
        w(sp.delimiter);
        w(eoPadL(cell, cw));
        if (starsrow[c] == 1) {
          if (st.star && has) {
            const EoV p = eoStatValue(models[static_cast<size_t>(m)], "p");
            w(eoPadR(eoStars(starlevels, p), starwidth));
          } else {
            w(std::string(static_cast<size_t>(starwidth), ' '));
          }
        }
        lastm = m;
      }
      std::string te = sp.end;
      if (r + 1 == S) {
        if (!sLast) sle.clear();
        te = lastend;
      }
      if (sp.statslabels.replace && !sle.empty()) te.clear();
      writeEnd(te, sle, {});
    }
  }

  // ---- postfoot and legend ---------------------------------------------------------------------------------------
  {
    std::vector<std::string> post = sp.postfoot;
    if (sp.prehead.empty() && sp.posthead.empty() && sp.prefoot.empty() && sp.postfoot.empty()) {
      if (!sp.note.empty()) post = {sp.note};
    }
    for (const auto& line : post) {
      w(insertAt(line, 0, std::to_string(ncols)));
      nl();
    }
  }
  if (sp.legend && !starlegend.empty()) {
    w(starlegend);
    nl();
  }

  // ---- substitute() on every line (estout) ------------------------------------------------------------
  if (!sp.substitute.empty()) {
    std::string res;
    size_t start = 0;
    while (start < buf.size()) {
      size_t eol = buf.find('\n', start);
      if (eol == std::string::npos) eol = buf.size();
      std::string line = buf.substr(start, eol - start);
      for (const auto& [from, to] : sp.substitute) {
        if (!(from.empty() && to.empty())) line = eoReplaceAll(line, from, to);
      }
      res += line;
      if (eol < buf.size()) res += "\n";
      start = eol + 1;
    }
    buf = res;
  }
  *out = buf;
  return true;
}
}
