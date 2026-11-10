// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// estout models.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// Stata macro-list tokenization (`foreach x of local`, `word # of`, gettoken):
// blank-separated words; "..." and the compound `"..."' are ONE word with one
// level of quotes removed.
std::vector<std::string> eoTokens(const std::string& s) {
  std::vector<std::string> out;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) break;
    if (s[i] == '`' && i + 1 < n && s[i + 1] == '"') {
      size_t j = i + 2;
      int depth = 1;
      std::string cur;
      while (j < n) {
        if (s[j] == '`' && j + 1 < n && s[j + 1] == '"') {
          ++depth;
          cur += "`\"";
          j += 2;
          continue;
        }
        if (s[j] == '"' && j + 1 < n && s[j + 1] == '\'') {
          if (--depth == 0) {
            j += 2;
            break;
          }
          cur += "\"'";
          j += 2;
          continue;
        }
        cur += s[j++];
      }
      out.push_back(cur);
      i = j;
      continue;
    }
    if (s[i] == '"') {
      const size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        out.push_back(s.substr(i + 1));
        break;
      }
      out.push_back(s.substr(i + 1, j - i - 1));
      i = j + 1;
      continue;
    }
    size_t j = i;
    while (j < n && s[j] != ' ' && s[j] != '\t') ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}


// estout: a hook with no `"` is ONE
// line, verbatim; otherwise every word is a line.
std::vector<std::string> eoHookLines(const std::string& raw) {
  if (raw.find('"') == std::string::npos) return {raw};
  return eoTokens(raw);
}


// `name(arg) flag name2(arg2)` -> (lower(name), arg). Parentheses nest and
// quotes protect, so `prefix(\multicolumn{@span}{c}{)` keeps its braces.
std::vector<std::pair<std::string, std::optional<std::string>>> eoSubopts(const std::string& s) {
  std::vector<std::pair<std::string, std::optional<std::string>>> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
    if (i >= n) break;
    size_t j = i;
    while (j < n && s[j] != '(' && s[j] != ' ' && s[j] != '\t') ++j;
    std::string name = strutil::lower(s.substr(i, j - i));
    if (j < n && s[j] == '(') {
      int depth = 0;
      bool inQ = false;
      size_t k = j;
      for (; k < n; ++k) {
        const char c = s[k];
        if (c == '"') inQ = !inQ;
        if (inQ) continue;
        if (c == '(') ++depth;
        if (c == ')' && --depth == 0) break;
      }
      out.emplace_back(name, s.substr(j + 1, (k < n ? k : n) - j - 1));
      i = (k < n) ? k + 1 : n;
    } else {
      out.emplace_back(name, std::nullopt);
      i = j;
    }
  }
  return out;
}

EoV eoNum(double v) {
  EoV r;
  if (std::isnan(v)) {
    r.kind = EoV::kMissing;
  } else {
    r.kind = EoV::kNum;
    r.v = v;
  }
  return r;
}


EoV eoElemValue(const EoModel& m, const EoKey& key, const std::string& stat) {
  if (eoIsCoefStat(stat)) {
    auto it = m.vals.find(key);
    if (it == m.vals.end()) return EoV{};
    const EoCoefVals& c = it->second;
    if (stat == "b" || stat == "_star" || stat == "_sign" || stat == "_sigsign") return eoNum(c.b);
    if (stat == "se") return eoNum(c.se);
    if (stat == "var") return eoNum(std::isnan(c.se) ? kEoNaN : c.se * c.se);
    if (stat == "t" || stat == "z") return eoNum(c.t);
    if (stat == "p") return eoNum(c.p);
    if (stat == "ci_l") return eoNum(c.cil);
    if (stat == "ci_u") return eoNum(c.ciu);
    return EoV{};
  }
  if (stat == "beta") {
    auto it = m.beta.find(key.second);
    if (it == m.beta.end()) {
      if (m.vals.count(key) != 0 && key.second == "_cons") return EoV{};
      return EoV{};
    }
    return eoNum(it->second);
  }
  auto ex = m.extra.find(stat);
  if (ex == m.extra.end()) return EoV{};
  auto it = ex->second.find(key);
  if (it == ex->second.end()) it = ex->second.find(EoKey{"__", key.second});
  if (it == ex->second.end()) return EoV{};
  return eoNum(it->second);
}


// One footer statistic (estout GetStats): e() scalars, then estadd scalars,
// then estadd LOCALS as text (`return(m#_stat)`, estout).
EoV eoStatValue(const EoModel& em, const std::string& key) {
  if (em.model == nullptr) return EoV{};
  const Model& m = *em.model;
  std::vector<std::string> tryKeys{key};
  if (key == "ar2") tryKeys.push_back("r2_a");
  if (key == "r2_a") tryKeys.push_back("ar2");
  if (key == "pr2" || key == "r2_p" || key == "r2_pseudo") {
    for (const char* alt : {"r2_p", "r2_pseudo"}) {
      if (std::find(tryKeys.begin(), tryKeys.end(), alt) == tryKeys.end()) tryKeys.push_back(alt);
    }
  }
  for (const auto& k : tryKeys) {
    if (m.escalars.contains(k) && m.escalars[k].is_number()) return eoNum(m.escalars[k].get<double>());
  }
  if (m.added.contains("scalars") && m.added["scalars"].contains(key)) {
    const ojson& v = m.added["scalars"][key];
    if (v.is_number()) return eoNum(v.get<double>());
    if (v.is_null()) return eoNum(kEoNaN);
  }
  if (m.added.contains("locals") && m.added["locals"].contains(key)) {
    const ojson& v = m.added["locals"][key];
    EoV r;
    r.kind = EoV::kText;
    r.text = v.is_string() ? v.get<std::string>() : std::string();
    if (r.text.empty()) return EoV{};
    return r;
  }
  return EoV{};
}


// ---- CompileVarl (estout): a coefficient's label under `label` -------
std::string eoVarLabel(SessionState& session, const std::string& var, bool* exists) {
  *exists = false;
  auto vm = session.varMeta.find(var);
  if (vm != session.varMeta.end()) {
    *exists = true;
    if (vm->second.label.has_value()) return *vm->second.label;
  }
  const std::string l = session.eval.varLabels.get(var);
  if (!l.empty()) {
    *exists = true;
    return l;
  }
  if (!*exists && session.hasData()) {
    try {
      Datastore* ds = session.datastore();
      *exists = ds != nullptr && ds->has(var);
    } catch (const std::exception&) {
      *exists = false;
    }
  }
  return std::string();
}


std::string eoCompileVarl(SessionState& session, const std::string& vnameIn,
                          const std::string& interaction) {
  // Stata names an interaction cell with its factor atoms first, then its
  // continuous ones, each in the order written (StataBE uni-oracle/io.do:
  // c.x##i.g -> 1.g#c.x, c.x#i.a#i.b -> 1.a#1.b#c.x, c.z#c.x#i.g ->
  // 1.g#c.z#c.x); the engine keeps the written order (`age_BL#1.treatment`),
  // so 007's joint-test row read `Age x Treatment=1` for Stata's
  // `Treatment=1 x Age`.
  std::string vname = vnameIn;
  if (vname.find('#') != std::string::npos) {
    std::vector<std::string> fac, con;
    size_t a = 0;
    while (a <= vname.size()) {
      size_t h = vname.find('#', a);
      if (h == std::string::npos) h = vname.size();
      const std::string atom = vname.substr(a, h - a);
      (!atom.empty() && std::isdigit(static_cast<unsigned char>(atom[0])) ? fac : con).push_back(atom);
      a = h + 1;
    }
    vname.clear();
    for (const auto* part : {&fac, &con})
      for (const auto& atom : *part) vname += (vname.empty() ? "" : "#") + atom;
  }
  std::string out;
  size_t i = 0;
  while (i <= vname.size()) {
    const size_t hash = vname.find('#', i);
    const std::string vi = vname.substr(i, hash == std::string::npos ? std::string::npos : hash - i);
    std::string xlabi;
    if (!vi.empty()) {
      const size_t dot = vi.find('.');
      if (dot == std::string::npos) {
        bool ex = false;
        xlabi = eoVarLabel(session, vi, &ex);
      } else {
        const std::string li0 = vi.substr(0, dot);
        const std::string vii = vi.substr(dot + 1);
        bool ex = false;
        const std::string vl = eoVarLabel(session, vii, &ex);
        if (ex) {
          bool isNum = !li0.empty();
          for (char c : li0) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') isNum = false;
          }
          if (!isNum) {
            xlabi = vl.empty() ? vii : vl;
            std::string li = li0;
            if (!li.empty() && li[0] == 'c') li = li.substr(1);
            if (!li.empty()) xlabi = li + "." + xlabi;
          } else {
            auto vlab = session.valueLabels.find(vii);
            int code = 0;
            try {
              code = std::stoi(li0);
            } catch (...) {
            }
            if (vlab != session.valueLabels.end()) {
              auto c = vlab->second.find(code);
              // `: label lbl #` answers the number itself for an unlabelled code
              xlabi = (c != vlab->second.end()) ? c->second : li0;
            } else {
              xlabi = (vl.empty() ? vii : vl) + "=" + li0;
            }
          }
        }
      }
    }
    if (xlabi.empty()) xlabi = vi;
    out += xlabi;
    if (hash == std::string::npos) break;
    out += interaction;
    i = hash + 1;
  }
  return out;
}


bool eoExpandEqVarlist(const std::string& list, const std::vector<EoKey>& rows, bool append,
                       std::vector<EoSpecItem>* out, std::vector<EoKey>* appendRows,
                       EoError* err) {
  std::vector<std::string> ueqs, ucoefs;
  for (const auto& r : rows) {
    if (std::find(ueqs.begin(), ueqs.end(), r.first) == ueqs.end()) ueqs.push_back(r.first);
    if (std::find(ucoefs.begin(), ucoefs.end(), r.second) == ucoefs.end()) ucoefs.push_back(r.second);
  }
  if (rows.empty()) ueqs.push_back("__");
  std::string body = list;
  bool relax = false;
  {
    std::string l, r;
    eoSplitComma(list, &l, &r);
    if (!eoTrim(r).empty()) {
      for (const auto& [nm, arg] : eoSubopts(r)) {
        if (eoAbbr(nm, "Relax")) relax = true;
      }
      body = l;
    }
  }
  for (const std::string& eqx : eoTokens(body)) {
    std::string eq, x;
    const size_t colon = eqx.find(':');
    bool eqGiven = false;
    std::string eq0;
    if (colon == std::string::npos) {
      x = eqx;
    } else if (colon == 0) {
      x = eqx.substr(1);
    } else {
      eq = eqx.substr(0, colon);
      eq0 = eq;
      if (eq == "_") eq = "__";
      x = eqx.substr(colon + 1);
      eqGiven = true;
    }
    std::vector<std::string> eqmatch;
    if (!eqGiven || std::find(ueqs.begin(), ueqs.end(), eq) != ueqs.end()) {
      if (eqGiven) eqmatch.push_back(eq);
    } else {
      for (const auto& e : ueqs) {
        if (strutil::globMatch(eq, e)) eqmatch.push_back(e);
      }
      if (eqmatch.empty() && !relax && !(append && !x.empty())) {
        err->msg = "equation " + eq0 + " not found";
        err->rc = 111;
        return false;
      }
    }
    if (x.empty()) {
      for (const auto& e : eqmatch) out->emplace_back(e, "");
      continue;
    }
    if (!eqGiven) {
      if (std::find(ucoefs.begin(), ucoefs.end(), x) != ucoefs.end()) {
        out->emplace_back("", x);
        continue;
      }
      bool any = false;
      for (const auto& c : ucoefs) {
        if (strutil::globMatch(x, c)) {
          out->emplace_back("", c);
          any = true;
        }
      }
      if (!any && x.find_first_of("*?") == std::string::npos) {
        // The same interaction in another atom order / `c.` spelling.
        const std::string kx = cecon::postest::interaction_key(x);
        for (const auto& c : ucoefs) {
          if (!kx.empty() && kx == cecon::postest::interaction_key(c)) {
            out->emplace_back("", c);
            any = true;
            break;
          }
        }
      }
      if (!any) {
        if (append) {
          if (appendRows != nullptr) appendRows->emplace_back("__", x);
          out->emplace_back("", x);
        } else if (!relax) {
          err->msg = "coefficient " + x + " not found";
          err->rc = 111;
          return false;
        }
      }
      continue;
    }
    bool any = false;
    for (const auto& r : rows) {
      if (std::find(eqmatch.begin(), eqmatch.end(), r.first) == eqmatch.end()) continue;
      if (strutil::globMatch(x, r.second)) {
        out->emplace_back(r.first, r.second);
        any = true;
      }
    }
    if (!any) {
      if (append) {
        if (appendRows != nullptr) appendRows->emplace_back(eq, x);
        out->emplace_back(eq, x);
      } else if (!relax) {
        err->msg = "coefficient " + eq0 + ":" + x + " not found";
        err->rc = 111;
        return false;
      }
    }
  }
  return true;
}


bool eoSpecHits(const std::vector<EoSpecItem>& spec, const EoKey& row) {
  for (const auto& [eq, name] : spec) {
    if (name.empty()) {
      if (eq == row.first) return true;
      continue;
    }
    if (!eq.empty() && eq != row.first) continue;
    if (name == row.second) return true;
  }
  return false;
}


void eoDropOrKeep(bool keep, const std::vector<EoSpecItem>& spec, std::vector<EoKey>* rows) {
  std::vector<EoKey> res;
  for (const auto& r : *rows) {
    if (eoSpecHits(spec, r) == keep) res.push_back(r);
  }
  *rows = res;
}


// estout Order.
void eoOrder(const std::string& specList, const std::vector<EoSpecItem>& expanded,
             std::vector<EoKey>* rows) {
  (void)specList;
  std::vector<std::string> eqlist;
  for (const auto& r : *rows) {
    if (std::find(eqlist.begin(), eqlist.end(), r.first) == eqlist.end()) eqlist.push_back(r.first);
  }
  std::vector<EoKey> bt = *rows, res;
  auto takeAll = [&](const EoSpecItem& sp) {
    for (;;) {
      auto it = std::find_if(bt.begin(), bt.end(), [&](const EoKey& r) {
        if (sp.second.empty()) return r.first == sp.first;
        return r.first == sp.first && r.second == sp.second;
      });
      if (it == bt.end()) return;
      res.push_back(*it);
      bt.erase(it);
    }
  };
  size_t i = 0;
  while (i < expanded.size()) {
    if (expanded[i].first.empty()) {
      // a run of bare names: per equation, the names in order, then the rest of
      // that equation (estout)
      std::vector<std::string> vars;
      while (i < expanded.size() && expanded[i].first.empty()) vars.push_back(expanded[i++].second);
      for (const auto& eq : eqlist) {
        for (const auto& v : vars) takeAll({eq, v});
        takeAll({eq, ""});
      }
    } else {
      takeAll(expanded[i]);
      ++i;
    }
  }
  for (const auto& r : bt) res.push_back(r);
  *rows = res;
}


// mat_capp ..., cons (estout): rows of `src` not yet in `dst` join at
// the end of their equation -- before that equation's `_cons` -- and a new
// equation joins at the end.
void eoCapp(std::vector<EoKey>* dst, const std::vector<EoKey>& src) {
  for (const auto& k : src) {
    if (std::find(dst->begin(), dst->end(), k) != dst->end()) continue;
    // the end of k's equation block
    size_t last = dst->size();
    bool seen = false;
    for (size_t i = 0; i < dst->size(); ++i) {
      if ((*dst)[i].first == k.first) {
        seen = true;
        last = i + 1;
      }
    }
    if (!seen) {
      dst->push_back(k);
      continue;
    }
    size_t at = last;
    if (k.second != "_cons" && at > 0 && (*dst)[at - 1].second == "_cons" &&
        (*dst)[at - 1].first == k.first) {
      --at;
    }
    dst->insert(dst->begin() + static_cast<long>(at), k);
  }
}


// Stata's `piece(n, w, s), nobreak`: the n-th word-wrapped chunk of s.
std::string eoPiece(int n, int width, const std::string& s) {
  const std::vector<std::string> words = splitWs(s);
  std::vector<std::string> pieces;
  std::string cur;
  for (const auto& wd : words) {
    if (cur.empty()) {
      cur = wd;
    } else if (static_cast<int>(cecon::ident::codePointCount(cur) + 1 +
                                cecon::ident::codePointCount(wd)) <= width) {  // characters
      cur += " " + wd;
    } else {
      pieces.push_back(cur);
      cur = wd;
    }
  }
  if (!cur.empty()) pieces.push_back(cur);
  return (n >= 1 && static_cast<size_t>(n) <= pieces.size()) ? pieces[n - 1] : std::string();
}


// The estimator's equation naming in e(b) -- the rules of eval/macros.cpp's
// kEStripeTable that a table needs: logit/probit name the one equation after
// the depvar, poisson after the depvar with `.` as `_`, mlogit one equation per
// outcome, everything else none (`__`, estout's spelling of `_`).
std::string eoEquationOf(const std::string& estimator, const std::string& depname) {
  if (estimator == "logit" || estimator == "probit" || estimator == "logistic") return depname;
  if (estimator == "poisson") {
    std::string d = depname;
    std::replace(d.begin(), d.end(), '.', '_');
    return d;
  }
  return "__";
}


std::string eoOutcomeName(const ojson& o) {
  if (o.contains("outcome") && o["outcome"].is_number()) {
    const double v = o["outcome"].get<double>();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
  }
  return o.value("depvar", std::string());
}


double eoJ(const ojson& r, const char* k) {
  return (r.contains(k) && r[k].is_number()) ? r[k].get<double>() : kEoNaN;
}


// One stored model as the write phase reads it: e(b)'s rows with b/se/t/p/ci,
// the estadd/estpost matrices, and the transform() applied the way
// _estout_getres does (b, se and the interval; t and p stay untransformed).
EoModel eoModelFrom(const Model& m, const std::string& listedName, SessionState& session,
                    const TransformSpec* tr, size_t mi, bool dropOmitted, bool withBase ) {
  EoModel em;
  em.name = listedName;
  em.model = &m;
  {
    const std::vector<std::string> dv = splitWs(m.snap.value("depvar", std::string()));
    em.depname = dv.empty() ? std::string() : dv[0];
  }
  if (m.snap.contains("title") && m.snap["title"].is_string()) em.title = m.snap["title"].get<std::string>();
  const std::string est = m.snap.value("estimator", std::string());

  auto addTable = [&](const ojson& ct, const std::string& eq, bool baseEq) {
    std::vector<EoKey> rows;
    bool hasCons = false;
    for (const auto& row : ct) {
      std::string name = row.value("name", std::string());
      // An equation-striped row (`treat:lower`, a posted e(b) column with its
      // equation, e.g. leebounds) carries its own equation, as estout reads it
      // from e(b)'s coleq; the bare name is the coefficient.
      std::string rowEq = eq;
      bool ownEq = false;
      if (const size_t colon = name.find(':'); colon != std::string::npos && colon > 0 &&
                                               colon + 1 < name.size()) {
        rowEq = name.substr(0, colon);
        name = name.substr(colon + 1);
        ownEq = true;
      }
      if (name == "(intercept)") name = "_cons";
      EoCoefVals v;
      v.b = eoJ(row, "b");
      v.se = eoJ(row, "se");
      v.t = eoJ(row, "t");
      v.p = eoJ(row, "p");
      v.cil = eoJ(row, "ci_lo");
      v.ciu = eoJ(row, "ci_hi");
      const bool omitted =
          !baseEq && (row.value("omitted", false) || (v.b == 0.0 && v.se == 0.0));
      if (omitted && dropOmitted) continue;
      // estout: var==0 -> se, t and p are missing, and the
      // interval collapses to b.
      if (v.se == 0.0 || omitted || baseEq) {
        v.se = kEoNaN;
        v.t = kEoNaN;
        v.p = kEoNaN;
        v.cil = v.b;
        v.ciu = v.b;
      } else if (std::isnan(v.t) && !std::isnan(v.b) && !std::isnan(v.se)) {
        v.t = v.b / v.se;
      }
      if (tr != nullptr && !tr->empty() && !std::isnan(v.b)) {
        const double b0 = v.b;
        const TransformCell tc = transformOneCell(*tr, mi, name, b0, m.snap, session);
        if (tc.matched) {
          if (tc.bMissing) v.b = kEoNaN;
          else if (tc.b.has_value()) v.b = *tc.b;
          if (tc.seFactor.has_value() && !std::isnan(v.se)) {
            v.se = std::isnan(*tc.seFactor) ? kEoNaN : v.se * std::fabs(*tc.seFactor);
          }
          for (double* ci : {&v.cil, &v.ciu}) {
            if (std::isnan(*ci)) continue;
            const TransformCell tci = transformOneCell(*tr, mi, name, *ci, m.snap, session);
            if (tci.bMissing) *ci = kEoNaN;
            else if (tci.b.has_value()) *ci = *tci.b;
          }
        }
      }
      const EoKey key{rowEq, name};
      if (em.vals.count(key) != 0) continue;
      em.vals[key] = v;
      if (ownEq) {
        rows.push_back(key);  // e(b) order within its own equation, `_cons` included
      } else if (name == "_cons") {
        hasCons = true;
      } else {
        rows.push_back(key);
      }
    }
    if (hasCons) rows.emplace_back(eq, "_cons");  // e(b) puts `_cons` last
    em.coefRows.insert(em.coefRows.end(), rows.begin(), rows.end());
  };

  if (m.snap.contains("outcomes") && m.snap["outcomes"].is_array() &&
      !(m.snap.contains("coeftable") && m.snap["coeftable"].is_array() && !m.snap["coeftable"].empty())) {
    // mlogit: one equation per outcome; the base outcome's equation is all
    // zeros with missing standard errors (e(b) carries it, e(V) is 0 there).
    ojson names = ojson::array();
    for (const auto& o : m.snap["outcomes"]) {
      if (o.contains("coeftable") && o["coeftable"].is_array() && !o["coeftable"].empty()) {
        names = o["coeftable"];
        break;
      }
    }
    for (const auto& o : m.snap["outcomes"]) {
      const std::string eq = eoOutcomeName(o);
      if (o.value("base", false) || !o.contains("coeftable") || o["coeftable"].empty()) {
        ojson zero = ojson::array();
        for (const auto& r : names) {
          ojson z;
          z["name"] = r.value("name", std::string());
          z["b"] = 0.0;
          z["se"] = 0.0;
          zero.push_back(z);
        }
        addTable(zero, eq, true);
      } else {
        addTable(o["coeftable"], eq, false);
      }
    }
  } else if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
    const std::string eq = eoEquationOf(est, em.depname);
    const size_t first = em.coefRows.size();
    addTable(m.snap["coeftable"], eq, false);
    // BASELEV: the snapshot's (or, for the live results, the data's) factor
    // base levels become `L.var` rows with b 0 and a missing se.
    ojson bl = m.snap.contains("base_levels") ? m.snap["base_levels"] : ojson();
    if (bl.is_array() && withBase) {
      for (const auto& e : bl) {
        const std::string nm = e.value("name", std::string());
        const std::string before = e.value("before", std::string());
        const std::string after = e.value("after", std::string());
        if (nm.empty()) continue;
        const EoKey key{eq, nm};
        if (em.vals.count(key) != 0) continue;
        EoCoefVals v;
        v.b = 0.0;
        v.cil = v.ciu = 0.0;
        em.vals[key] = v;
        auto pos = em.coefRows.end();
        if (!before.empty()) {
          pos = std::find(em.coefRows.begin() + static_cast<long>(first), em.coefRows.end(),
                          EoKey{eq, before});
        } else if (!after.empty()) {
          pos = std::find(em.coefRows.begin() + static_cast<long>(first), em.coefRows.end(),
                          EoKey{eq, after});
          if (pos != em.coefRows.end()) ++pos;
        }
        em.coefRows.insert(pos, key);
      }
    }
  }

  // estadd matrix / estpost: 1 x k matrices keyed by column name.
  if (m.added.contains("matrices") && m.added["matrices"].is_object()) {
    for (const auto& [mname, mat] : m.added["matrices"].items()) {
      if (!mat.is_object() || !mat.contains("colnames") || !mat.contains("data")) continue;
      const ojson& cn = mat["colnames"];
      const ojson& data = mat["data"];
      if (!cn.is_array() || !data.is_array() || data.empty() || !data[0].is_array()) continue;
      std::vector<EoKey> order;
      std::map<EoKey, double> vals;
      for (size_t j = 0; j < cn.size(); ++j) {
        if (!cn[j].is_string()) continue;
        std::string col = cn[j].get<std::string>();
        std::string eq = "__";
        const size_t colon = col.find(':');
        if (colon != std::string::npos) {
          eq = col.substr(0, colon);
          col = col.substr(colon + 1);
        }
        if (col == "(intercept)") col = "_cons";
        const EoKey key{eq, col};
        order.push_back(key);
        vals[key] = (j < data[0].size() && data[0][j].is_number()) ? data[0][j].get<double>() : kEoNaN;
      }
      // `_cons` last, as mat_capp's `cons` would place it
      auto cit = std::find_if(order.begin(), order.end(), [](const EoKey& k) { return k.second == "_cons"; });
      if (cit != order.end()) {
        const EoKey ck = *cit;
        order.erase(cit);
        order.push_back(ck);
      }
      em.extraOrder[mname] = order;
      em.extra[mname] = vals;
    }
  }
  return em;
}


EtMode esttabMode(const std::string& mode, bool label, bool plain, int level) {
  EtMode m;
  m.mode = mode;
  m.mode0 = (mode == "booktabs") ? "tex" : (mode == "scsv" ? "csv" : mode);
  const std::vector<std::string> fssl{"N", "R-sq", "adj. R-sq", "pseudo R-sq", "AIC", "BIC"};
  const std::vector<std::string> flsl{"Observations",       "R-squared", "Adjusted R-squared",
                                      "Pseudo R-squared", "AIC",       "BIC"};
  const std::vector<std::pair<std::string, std::string>> fstars{
      {"*", "0.05"}, {"**", "0.01"}, {"***", "0.001"}};
  m.cilab = std::to_string(level) + "% confidence intervals";
  if (m.mode0 == "fixed") {
    m.caption = {"@title"};
    m.toprule = m.midrule = m.bottomrule = {"@hline"};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.eqruleBegin = {"@hline", ""};
    m.hasEqrule = true;
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.delimiter = " ";
    m.delimiterSet = true;
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.abbrev = true;
  } else if (m.mode0 == "tab") {
    m.caption = {"@title"};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.delimiter = "\t";
    m.delimiterSet = true;
  } else if (m.mode0 == "csv") {
    const std::string csvlhs = plain ? "\"" : "=\"";
    m.caption = {csvlhs + "@title\""};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.begin = csvlhs;
    m.beginSet = true;
    m.delimiter = std::string("\"") + (mode == "scsv" ? ";" : ",") + csvlhs;
    m.delimiterSet = true;
    m.end = "\"";
    m.endSet = true;
  } else {  // tex, booktabs
    const bool bt = mode == "booktabs";
    m.open = {"\\begin{table}[htbp]\\centering"};
    m.close = {"\\end{table}"};
    m.caption = {"\\caption{@title}"};
    m.toprule = {bt ? "\\toprule" : "\\hline\\hline"};
    m.midrule = {bt ? "\\midrule" : "\\hline"};
    m.bottomrule = {bt ? "\\bottomrule" : "\\hline\\hline"};
    m.midgap = {bt ? "\\addlinespace" : "[1em]"};
    m.eqruleBegin = {bt ? "\\midrule" : "\\hline", ""};
    m.hasEqrule = true;
    m.ssl = {"\\(N\\)", "\\(R^{2}\\)", "adj. \\(R^{2}\\)", "pseudo \\(R^{2}\\)", "\\textit{AIC}",
             "\\textit{BIC}"};
    m.lsl = {"Observations",        "\\(R^{2}\\)",   "Adjusted \\(R^{2}\\)",
             "Pseudo \\(R^{2}\\)", "\\textit{AIC}", "\\textit{BIC}"};
    m.starlevels = {{"\\sym{*}", "0.05"}, {"\\sym{**}", "0.01"}, {"\\sym{***}", "0.001"}};
    m.starlevlab = " \\(p<@\\)";
    m.starlevlabSet = true;
    m.delimiter = "&";
    m.delimiterSet = true;
    m.end = "\\\\";
    m.endSet = true;
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.tstatlab = "\\textit{t} statistics";
    m.zstatlab = "\\textit{z} statistics";
    m.pvallab = "\\textit{p}-values";
    m.cilab = std::to_string(level) + "\\% confidence intervals";
    m.substitute = {{"_", "\\_"}, {"\\_cons ", "\\_cons"}};
    m.interaction = " $\\times$ ";
  }
  return m;
}


std::vector<int> eoIntList(const std::string& raw) {
  std::vector<int> v;
  for (const auto& t : eoTokens(raw)) {
    try {
      v.push_back(std::stoi(t));
    } catch (...) {
    }
  }
  return v;
}


// esttab, beta: b * sd(x)/sd(y) over e(sample) (estadd, called by
// esttab). PIET's store carries no e(sample) mask, so the sample is
// RECONSTRUCTED as the complete cases of {depvar, regressors} and CHECKED
// against e(N): a mismatch refuses by name rather than printing a number from
// the wrong rows (internal notes esttab-beta).
bool esttabBetaVals(SessionState& session, const std::vector<Model>& models,
                    std::vector<std::map<std::string, double>>* out, std::string* errOut) {
  out->assign(models.size(), {});
  if (!session.currentHbin().has_value()) {
    *errOut = "esttab, beta needs sd(x)/sd(y) over e(sample) and no dataset is in memory";
    return false;
  }
  for (size_t mi = 0; mi < models.size(); ++mi) {
    const Model& m = models[mi];
    const std::string depvar = m.snap.value("depvar", std::string());
    double nObs = 0.0;
    if (m.snap.contains("n_obs") && m.snap["n_obs"].is_number()) nObs = m.snap["n_obs"].get<double>();
    if (depvar.empty() || nObs <= 1.0) {
      *errOut = "esttab, beta needs e(sample) and model " + m.name +
                " carries no dependent variable or no observation count";
      return false;
    }
    std::vector<std::string> vars{depvar};
    std::vector<std::string> coefs;
    if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
      for (const auto& r : m.snap["coeftable"]) {
        const std::string n = r.value("name", std::string());
        if (n == "_cons" || n == "(intercept)") continue;
        vars.push_back(n);
        coefs.push_back(n);
      }
    }
    std::string ferr;
    try {
      Datastore& ds = *session.datastore();
      std::map<std::string, const double*> cols;
      int64_t nrows = -1;
      for (const auto& v : vars) {
        if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) {
          ferr = "sd(" + v + ") is not available: " + v +
                 " is not a numeric variable in the dataset now in memory";
          break;
        }
        const cecon::Column& c = ds.col(v);
        const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
        if (nrows < 0) nrows = n;
        if (n != nrows) {
          ferr = "the dataset columns disagree on length";
          break;
        }
        cols[v] = c.f64();
      }
      if (ferr.empty()) {
        std::vector<size_t> rows;
        for (int64_t i = 0; i < nrows; ++i) {
          bool ok = true;
          for (const auto& kv : cols) {
            if (std::isnan(kv.second[i])) ok = false;
          }
          if (ok) rows.push_back(static_cast<size_t>(i));
        }
        if (static_cast<double>(rows.size()) != nObs) {
          ferr = "this build reconstructs e(sample) as the complete cases of the model's own "
                 "variables, and for " + m.name + " that is " + std::to_string(rows.size()) +
                 " rows against e(N)=" + std::to_string(static_cast<long long>(nObs)) +
                 " -- the sample cannot be reproduced, so no standardized coefficient is printed";
        } else {
          auto sdOf = [&](const std::string& v) {
            const double* p = cols[v];
            double s = 0.0;
            for (size_t i : rows) s += p[i];
            const double mean = s / static_cast<double>(rows.size());
            double m2 = 0.0;
            for (size_t i : rows) {
              const double d = p[i] - mean;
              m2 += d * d;
            }
            return std::sqrt(m2 / (static_cast<double>(rows.size()) - 1.0));
          };
          const double sdy = sdOf(depvar);
          for (const auto& r : m.snap["coeftable"]) {
            const std::string cn = r.value("name", std::string());
            if (cols.count(cn) == 0 || !r.contains("b") || !r["b"].is_number()) continue;
            (*out)[mi][cn] = r["b"].get<double>() * sdOf(cn) / sdy;
          }
        }
      }
    } catch (const std::exception& e) {
      ferr = std::string("the dataset could not be read (") + e.what() + ")";
    }
    if (!ferr.empty()) {
      *errOut = "esttab, beta: " + ferr;
      return false;
    }
  }
  return true;
}


// estout posts r() (estout): r(cmdline), r(names),
// r(nmodels), r(ccols) -- missing when no coefficient statistic resolved.
void eoPostR(SessionState& session, const std::string& cmdline, const std::vector<EoModel>& models,
             int ccols) {
  eval::replaceRClass(session.eval);
  std::string names;
  for (const auto& m : models) {
    if (!names.empty()) names += " ";
    names += m.name;
  }
  session.eval.rStrings.set("cmdline", cmdline);
  session.eval.rStrings.set("names", names);
  session.eval.rValues.set("nmodels", static_cast<double>(models.size()));
  session.eval.rValues.set("ccols", ccols < 0 ? kEoNaN : static_cast<double>(ccols));
}
}
