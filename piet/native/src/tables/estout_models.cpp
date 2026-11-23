// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// estout models.
#include "tables_internal.h"
#include "estout_internal.h"
#include "piet/user_paths.h"
namespace piet::handlers {
// postestimation/likelihood_ratio_test.cpp: chi2tail(df, x) for an integer df
// (NaN otherwise) -- the expression layer has no chi2tail().
double chi2UpperTailIntegerDf(double df, double x);
}  // namespace piet::handlers
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
    if (stat == "var") return eoNum(c.zeroVar ? 0.0 : (std::isnan(c.se) ? kEoNaN : c.se * c.se));
    if (stat == "t" || stat == "z") return eoNum(c.t);
    if (stat == "p") return eoNum(c.p);
    if (stat == "ci_l") return eoNum(c.cil);
    if (stat == "ci_u") return eoNum(c.ciu);
    return EoV{};
  }
  // esttab's beta option fills m.beta; without it `cells(beta)` is the e(beta) matrix like any
  // other (estadd beta, bank b1da7033), read below.
  if (stat == "beta" && !m.beta.empty()) {
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


// One footer statistic, as estout's GetStats reads it -- the ONE resolver of
// the printed stats() cell and of r(stats):
//   aic, bic, rank   -2 e(ll) + 2 rank, -2 e(ll) + ln(e(N)) rank, rank, where
//                    rank is the number of coefficients e(V) does not zero;
//   an e() scalar    its value (the fit's, then an estadd scalar);
//   p                when e(p) is not posted: Ftail(e(df_m), e(df_r), e(F)),
//                    else chi2tail(e(df_m), e(chi2)) (an integer df), else
//                    nothing;
//   an e() macro     its text (kText; r(stats) `.y`, r(m#_stat) the text),
//                    an estadd local included;
//   anything else    nothing (kVacant; r(stats) `.z`).
// Names are Stata's own: `stats(ar2)` asks for e(ar2), which no fit posts.
EoV eoStatValue(const EoModel& em, const std::string& key, SessionState* session) {
  if (em.model == nullptr) return EoV{};
  const Model& m = *em.model;
  auto scalar = [&](const std::string& k, double* v) {
    for (const ojson* store : {&m.escalars, m.added.contains("scalars") ? &m.added["scalars"]
                                                                       : static_cast<const ojson*>(nullptr)}) {
      if (store == nullptr || !store->is_object() || !store->contains(k)) continue;
      const ojson& x = (*store)[k];
      if (x.is_number()) {
        *v = x.get<double>();
        return true;
      }
      if (x.is_null()) {
        *v = kEoNaN;
        return true;
      }
    }
    return false;
  };
  double v = 0.0;
  if (key == "aic" || key == "bic" || key == "rank") {
    double rank = 0.0;
    for (const auto& [k, c] : em.vals) {
      (void)k;
      if (!c.zeroVar && !std::isnan(c.se)) rank += 1.0;
    }
    if (key == "rank") return eoNum(rank);
    double ll = 0.0, n = 0.0;
    if (!scalar("ll", &ll)) return eoNum(kEoNaN);
    if (key == "aic") return eoNum(-2.0 * ll + 2.0 * rank);
    if (!scalar("N", &n)) return eoNum(kEoNaN);
    return eoNum(-2.0 * ll + std::log(n) * rank);
  }
  if (scalar(key, &v)) return eoNum(v);
  if (key == "p") {
    auto num = [](double x) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", x);
      return std::string(buf);
    };
    double dfm = 0.0, dfr = 0.0, f = 0.0;
    std::optional<double> r;
    if (scalar("F", &f)) {
      if (session == nullptr || !scalar("df_m", &dfm) || !scalar("df_r", &dfr) || std::isnan(f) ||
          std::isnan(dfm) || std::isnan(dfr)) {
        return eoNum(kEoNaN);
      }
      r = evalScalarExpr("Ftail(" + num(dfm) + "," + num(dfr) + "," + num(f) + ")", *session);
    } else if (scalar("chi2", &f)) {
      if (!scalar("df_m", &dfm) || std::isnan(f) || std::isnan(dfm) || dfm != std::floor(dfm)) {
        return eoNum(kEoNaN);
      }
      r = chi2UpperTailIntegerDf(dfm, f);
      if (std::isnan(*r)) r.reset();
    } else {
      return EoV{};
    }
    if (!r.has_value() || *r >= eval::kStataMissingSentinel) return eoNum(kEoNaN);
    return eoNum(*r);
  }
  for (const ojson* store : {&m.emacros, m.added.contains("locals") ? &m.added["locals"]
                                                                   : static_cast<const ojson*>(nullptr)}) {
    if (store == nullptr || !store->is_object() || !store->contains(key)) continue;
    const ojson& x = (*store)[key];
    EoV r;
    r.kind = EoV::kText;
    r.text = x.is_string() ? x.get<std::string>() : std::string();
    if (!r.text.empty()) return r;  // an empty e() macro is not posted
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
  // The single-equation likelihood fits whose e(b) names its (first) equation after the dependent
  // variable -- the ancillary parameters (/var(e.y), /cut#, /lnalpha) sit in equations of their own
  // -- so their rows never meet those of an estimator with bare names (regress).
  if (estimator == "logit" || estimator == "probit" || estimator == "logistic" || estimator == "tobit" ||
      estimator == "ologit" || estimator == "oprobit" || estimator == "nbreg") {
    // a stored ordered-outcome fit records no dependent variable (its block carries none), so its
    // equation cannot be named here and stays `__`
    return depname.empty() ? std::string("__") : depname;
  }
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


// estout_omitted_and_base (estout's Mata helper) over the e(b) stripe the
// fit recorded: a column whose name holds `o.` is dropped under noomitted, else
// its `o.` marks are removed (`o.x` -> `x`, `0b.g#co.x` -> `0b.g#c.x`); then a
// column whose name holds `b.` is dropped under nobaselevels, else `b.` reads
// `.` (`0b.g` -> `0.g`). Base and omitted columns hold b 0 and se 0. nullopt
// when the snapshot carries no stripe that matches its coefficient table.
std::optional<ojson> eoStripeTable(const ojson& snap, bool dropOmitted, bool withBase) {
  if (!snap.contains("eb_stripe") || !snap["eb_stripe"].is_array() || snap["eb_stripe"].empty()) {
    return std::nullopt;
  }
  const std::vector<eval::EbColumnRow> cols = eval::stataEbColumns(snap);
  const ojson& st = snap["eb_stripe"];
  if (cols.size() != st.size()) return std::nullopt;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (!st[i].is_object() || st[i].value("name", std::string()) != cols[i].name) return std::nullopt;
  }
  ojson out = ojson::array();
  for (const auto& c : cols) {
    std::string name = c.name;
    if (name.find("o.") != std::string::npos) {
      if (dropOmitted) continue;
      if (name.rfind("o.", 0) == 0) name = name.substr(2);
      name = eoReplaceAll(name, "o.", ".");
    }
    if (name.find("b.") != std::string::npos) {
      if (!withBase) continue;
      name = eoReplaceAll(name, "b.", ".");
    }
    ojson row = c.row != nullptr ? *c.row : ojson{{"b", 0.0}, {"se", 0.0}};
    row["name"] = name;
    if (name != c.name) row["__piet_eb_name"] = c.name;  // e(b)'s marked name (EoModel::ebMarked)
    out.push_back(std::move(row));
  }
  return out;
}


// One stored model as the write phase reads it: e(b)'s rows with b/se/t/p/ci,
// the estadd/estpost matrices, and the transform() applied the way
// _estout_getres does (b, se and the interval; t and p stay untransformed).
// topfile(file) / bottomfile(file): the file's lines come before / after the table,
// both on screen and in the output file; a file that is not there is r(601).
bool eoWrapWithFiles(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session, std::string* text, EoError* err) {
  std::string top, bottom;
  for (const auto& [k, v] : opts) {
    if ((k != "topfile" && k != "bottomfile") || v.isBool) continue;
    const std::string path = eoTrim(dequote(v.strValue));
    bool ok = false;
    std::string body = readWholeFile(resolveUserPath(session, path), &ok);
    if (!ok) {
      err->msg = "file " + path + " not found";
      err->rc = 601;
      return false;
    }
    if (!body.empty() && body.back() != '\n') body += "\n";
    (k == "topfile" ? top : bottom) = body;
  }
  *text = top + *text + bottom;
  return true;
}


bool eoParseMargin(const std::vector<std::pair<std::string, eval::OptValue>>& opts, EoSpec* sp,
                   EoError* err) {
  bool margin = false, nomargin = false, nodiscrete = false;
  std::optional<std::string> marginArg, discreteArg;
  for (const auto& [k, v] : opts) {
    if (k == "margin") {
      margin = true;
      if (!v.isBool) marginArg = eoTrim(eoStringArg(v.strValue));
    } else if (k == "nomargin") {
      nomargin = true;
    } else if (k == "nodiscrete") {
      nodiscrete = true;
    } else if (k == "discrete" && !v.isBool) {
      discreteArg = v.strValue;
    }
  }
  if (margin && !marginArg.value_or("margin").empty()) {
    const std::string m = marginArg.value_or("margin");
    if (m != "margin" && m != "u" && m != "c" && m != "p") {
      err->msg = "margin(" + m + ") invalid";
      err->rc = 198;
      return false;
    }
  }
  sp->margin = margin && !nomargin;
  if (!sp->margin) return true;
  // The legend text: the user's, else the default marker and its explanation. The
  // first word is the marker (discrete), the rest the explanation (discrete2).
  std::string text = discreteArg.has_value() ? *discreteArg
                                             : (nodiscrete ? std::string()
                                                           : std::string("\" (d)\" for discrete change of dummy variable from 0 to 1"));
  const std::vector<std::string> toks = eoTokens(text);
  if (!toks.empty()) {
    sp->discrete = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) sp->discrete2 += (i > 1 ? " " : " ") + toks[i];
  }
  return true;
}


// level(#) other than the one the fit was stored at: estout recomputes the
// interval itself as b -/+ invttail(e(df_r), (100-level)/200) * se, or with the
// normal quantile when the fit has no residual degrees of freedom. A base level,
// an omitted column and a zero variance keep the interval collapsed to b.
void eoApplyLevel(const Model& m, EoModel& em, int level, SessionState& session) {
  const double tail = (100.0 - level) / 200.0;
  double crit = kEoNaN;
  {
    char buf[128];
    if (m.escalars.contains("df_r") && m.escalars["df_r"].is_number()) {
      std::snprintf(buf, sizeof(buf), "invttail(%.17g, %.17g)", m.escalars["df_r"].get<double>(), tail);
    } else {
      std::snprintf(buf, sizeof(buf), "invnormal(%.17g)", 1.0 - tail);
    }
    if (auto v = piet::evalScalarExpr(buf, session)) crit = *v;
  }
  for (auto& [key, v] : em.vals) {
    (void)key;
    if (v.zeroVar) continue;
    if (std::isnan(v.b) || std::isnan(v.se) || std::isnan(crit)) {
      v.cil = kEoNaN;
      v.ciu = kEoNaN;
      continue;
    }
    v.cil = v.b - crit * v.se;
    v.ciu = v.b + crit * v.se;
  }
}


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

  // `selected`: the rows were already chosen by e(b)'s own names (the stripe
  // branch below), so a zero-variance row is not dropped here.
  auto addTable = [&](const ojson& ct, const std::string& eq, bool baseEq, bool selected = false) {
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
      if (omitted && dropOmitted && !selected) continue;
      // estout: var==0 -> se, t and p are missing, and the
      // interval collapses to b.
      if (v.se == 0.0 || omitted || baseEq) {
        v.zeroVar = true;
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
      if (row.contains("__piet_eb_name") && row["__piet_eb_name"].is_string()) {
        em.ebMarked[key] = row["__piet_eb_name"].get<std::string>();
      } else if (baseEq || (omitted && !selected)) {
        em.ebMarked[key] = "o." + name;  // a base outcome's equation / an omitted column
      }
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

  // Results posted without e(b) (estpost: `eret post` with no b, StataBE
  // `matrix list e(b)` r(111)) have no coefficient rows: their table carries the
  // screen's summary, and estout reads b/se/t/p from e(b)/e(V) only, so those
  // statistics are `.y` and the rows come from the posted matrices
  // (_estout_getres GetCoefs).
  const bool postsEb = !(m.snap.contains("posts_e_b") && m.snap["posts_e_b"].is_boolean() &&
                         !m.snap["posts_e_b"].get<bool>());
  if (!postsEb) {
    // no e(b): nothing here
  } else if (m.snap.contains("outcomes") && m.snap["outcomes"].is_array() &&
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
  } else if (const std::optional<ojson> stripeRows =
                 m.snap.contains("coeftable") ? eoStripeTable(m.snap, dropOmitted, withBase)
                                              : std::nullopt) {
    // The fit recorded Stata's e(b) stripe (eval::stataEbColumns): the rows are
    // e(b)'s columns under e(b)'s names -- `1.foreign#c.mpg` where the engine
    // says `mpg#1.foreign`, the base levels `0b.foreign#co.mpg` and the omitted
    // `o.x` included --, chosen and renamed as estout_omitted_and_base does.
    addTable(*stripeRows, eoEquationOf(est, em.depname), false, /*selected=*/true);
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
        v.zeroVar = true;
        em.vals[key] = v;
        if (e.contains("level") && e.contains("var") && e["var"].is_string()) {
          em.ebMarked[key] = std::to_string(e.value("level", 0LL)) + "b." + e["var"].get<std::string>();
        }
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
  m.mode0 = (mode == "booktabs") ? "tex" : (mode == "scsv" ? "csv" : (mode == "html5" ? "html" : mode));
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
  } else if (m.mode0 == "html") {
    // esttab's html and html5 presets: a table of rows, `@span` columns wide rules
    const bool h5 = mode == "html5";
    m.open = {h5 ? "<table>" : "<table border=\"0\" width=\"*\">"};
    m.close = {"</table>"};
    m.caption = {"<caption>@title</caption>"};
    if (!h5) {
      m.toprule = m.midrule = m.bottomrule = {"<tr><td colspan=@span><hr></td></tr>"};
      m.midgap = {"<tr><td colspan=@span>&nbsp;</td></tr>"};
      m.eqruleBegin = {"<tr><td colspan=@span><hr></td></tr>", ""};
    } else {
      m.eqruleBegin = {"</tbody>", "<tbody>", ""};
    }
    m.hasEqrule = true;
    m.ssl = {"<i>N</i>", "<i>R</i><sup>2</sup>", "adj. <i>R</i><sup>2</sup>",
             "pseudo <i>R</i><sup>2</sup>", "<i>AIC</i>", "<i>BIC</i>"};
    m.lsl = {"Observations", "<i>R</i><sup>2</sup>", "Adjusted <i>R</i><sup>2</sup>",
             "Pseudo <i>R</i><sup>2</sup>", "<i>AIC</i>", "<i>BIC</i>"};
    m.starlevels = {{"<sup>*</sup>", "0.05"}, {"<sup>**</sup>", "0.01"}, {"<sup>***</sup>", "0.001"}};
    m.starlevlab = " <i>p</i> < ";
    m.starlevlabSet = true;
    m.begin = "<tr><td>";
    m.beginSet = true;
    m.delimiter = "</td><td>";
    m.delimiterSet = true;
    m.end = "</td></tr>";
    m.endSet = true;
    m.incelldel = "<br />";
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.tstatlab = "<i>t</i> statistics";
    m.zstatlab = "<i>z</i> statistics";
    m.pvallab = "<i>p</i>-values";
    m.cilab = std::to_string(level) + "% confidence intervals";
    m.open0 = {"<!doctype html>", "<html>", "<head>", "<title>@title</title>", "</head>", "<body>", ""};
    m.close0 = {"", "</body>", "</html>", ""};
    if (!h5) m.open0.erase(m.open0.begin());
  } else if (m.mode0 == "mmd") {
    m.open = {""};
    m.close = {""};
    m.caption = {"@title", ""};
    m.hasEqrule = true;  // an empty equation rule: `nofirst` alone
    m.eqruleBegin = {};
    m.ssl = {"*N*", "*R*<sup>2</sup>", "adj. *R*<sup>2</sup>", "pseudo *R*<sup>2</sup>", "*AIC*", "*BIC*"};
    m.lsl = {"Observations", "*R*<sup>2</sup>", "Adjusted *R*<sup>2</sup>", "Pseudo *R*<sup>2</sup>", "*AIC*",
             "*BIC*"};
    m.starlevels = {{"<sup>\\*</sup>", "0.05"}, {"<sup>\\*\\*</sup>", "0.01"}, {"<sup>\\*\\*\\*</sup>", "0.001"}};
    m.starlevlab = " *p* < ";
    m.starlevlabSet = true;
    m.begin = "| ";
    m.beginSet = true;
    m.delimiter = " | ";
    m.delimiterSet = true;
    m.end = " |";
    m.endSet = true;
    m.incelldel = " ";
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.substitute = {{"_", "\\_"}, {"\\_cons ", "\\_cons"}};
    m.interaction = " # ";
    m.tstatlab = "*t* statistics";
    m.zstatlab = "*z* statistics";
    m.pvallab = "*p*-values";
    m.cilab = std::to_string(level) + "\\% confidence intervals";
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
    // estadd beta: b * sd(x) / sd(y) over e(sample) for every column of e(b) that `summarize` can
    // read -- a variable, a factor level `k.v` (its indicator), a continuous `c.v`, an interaction
    // of those (their product); the base level is 0.
    struct Atom {
      bool factor = false;
      double level = 0;
      std::string var;
    };
    auto atomsOf = [](const std::string& name, std::vector<Atom>* at) {
      size_t a0 = 0;
      while (a0 <= name.size()) {
        size_t h = name.find('#', a0);
        if (h == std::string::npos) h = name.size();
        std::string tok = name.substr(a0, h - a0);
        Atom x;
        const size_t dot = tok.find('.');
        if (dot == std::string::npos) {
          x.var = tok;
        } else {
          const std::string head = tok.substr(0, dot);
          x.var = tok.substr(dot + 1);
          size_t i = 0;
          while (i < head.size() && (std::isdigit(static_cast<unsigned char>(head[i])) || head[i] == '-')) ++i;
          if (i > 0 && head.size() - i <= 2) {
            x.factor = true;
            x.level = std::atof(head.substr(0, i).c_str());
          } else if (!(head == "c" || head == "co" || head == "o")) {
            return false;
          }
        }
        if (x.var.empty()) return false;
        at->push_back(x);
        a0 = h + 1;
      }
      return true;
    };
    std::vector<std::pair<std::string, std::vector<Atom>>> terms;
    std::string opErr;
    std::vector<std::string> baseNames;
    if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
      for (const auto& r : m.snap["coeftable"]) {
        const std::string n = r.value("name", std::string());
        if (n == "_cons" || n == "(intercept)") continue;
        std::vector<Atom> at;
        // `summarize` reads a variable, a factor level or `c.v`; an interaction is not one (`.z`)
        if (n.find('#') == std::string::npos) {
          // A coefficient whose name carries a time-series operator (L.x, D.x ...) has a standardized
          // value in Stata (sd of the operated series over e(sample)); this build cannot read it, so it
          // refuses by name rather than leaving the column blank.
          if (atomsOf(n, &at)) {
            terms.emplace_back(n, std::move(at));
          } else if (opErr.empty()) {
            opErr = "sd(" + n + ") is not available: a time-series operator on a coefficient is not "
                    "implemented for beta";
          }
        }
      }
    }
    if (m.snap.contains("base_levels") && m.snap["base_levels"].is_array()) {
      for (const auto& e : m.snap["base_levels"]) {
        const std::string nm = e.value("name", std::string());
        if (!nm.empty()) baseNames.push_back(nm);
      }
    }
    std::string ferr = opErr;
    try {
      Datastore& ds = *session.datastore();
      std::map<std::string, const double*> cols;
      int64_t nrows = -1;
      auto addCol = [&](const std::string& v) {
        if (cols.count(v) != 0) return true;
        if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) return false;
        const cecon::Column& c = ds.col(v);
        const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
        if (nrows < 0) nrows = n;
        if (n != nrows) return false;
        cols[v] = c.f64();
        return true;
      };
      if (!addCol(depvar)) {
        ferr = "sd(" + depvar + ") is not available: " + depvar +
               " is not a numeric variable in the dataset now in memory";
      }
      // estadd beta summarizes under the model's own weights (a pweight is read as an aweight), so
      // the sd's are weighted ones. The weight is read from e(wtype) / e(wexp); a weight that is not
      // a plain numeric variable cannot be evaluated here and refuses by name.
      std::string wtype = m.emacros.value("wtype", std::string());
      if (wtype == "pweight") wtype = "aweight";
      std::string wvar;
      if (ferr.empty() && !wtype.empty()) {
        wvar = strutil::trim(m.emacros.value("wexp", std::string()));
        if (!wvar.empty() && wvar.front() == '=') wvar = strutil::trim(wvar.substr(1));
        if (wvar.empty() || !addCol(wvar)) {
          ferr = "sd(x) is not available: the weight expression `" + m.emacros.value("wexp", std::string()) +
                 "` of " + m.name + " is not a numeric variable in the dataset; weighted beta needs one";
        }
      }
      std::vector<size_t> usable;  // the terms whose every variable is numeric and present
      for (size_t t = 0; ferr.empty() && t < terms.size(); ++t) {
        bool ok = true;
        for (const auto& a : terms[t].second) ok = addCol(a.var) && ok;
        if (ok) usable.push_back(t);
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
          std::vector<double> wts;
          if (!wvar.empty()) {
            for (size_t i : rows) wts.push_back(cols[wvar][i]);
          }
          // summarize's weighted sd: aweights are rescaled to sum to N, so the mean is
          // sum(wx)/sum(w) and var = sum(w(x-mean)^2)/sum(w) * N/(N-1); frequency and importance
          // weights count observations, var = sum(w(x-mean)^2)/(sum(w)-1).
          const bool countW = wtype == "fweight" || wtype == "iweight";
          auto sdOf = [&](const std::vector<double>& x) {
            const double n = static_cast<double>(x.size());
            double sw = 0.0, s2 = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
              const double w = wts.empty() ? 1.0 : wts[i];
              sw += w;
              s2 += w * x[i];
            }
            const double mean = s2 / sw;
            double m2 = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
              const double w = wts.empty() ? 1.0 : wts[i];
              m2 += w * (x[i] - mean) * (x[i] - mean);
            }
            if (wts.empty()) return std::sqrt(m2 / (n - 1.0));
            if (countW) return std::sqrt(m2 / (sw - 1.0));
            return std::sqrt(m2 / sw * n / (n - 1.0));
          };
          std::vector<double> y;
          for (size_t i : rows) y.push_back(cols[depvar][i]);
          const double sdy = sdOf(y);
          for (size_t t : usable) {
            const std::string& cn = terms[t].first;
            double b = std::nan("");
            for (const auto& r : m.snap["coeftable"]) {
              if (r.value("name", std::string()) == cn && r.contains("b") && r["b"].is_number()) {
                b = r["b"].get<double>();
              }
            }
            if (std::isnan(b)) continue;
            std::vector<double> x;
            for (size_t i : rows) {
              double v = 1.0;
              for (const auto& a : terms[t].second) {
                const double c = cols[a.var][i];
                v *= a.factor ? (c == a.level ? 1.0 : 0.0) : c;
              }
              x.push_back(v);
            }
            (*out)[mi][cn] = b * sdOf(x) / sdy;
          }
          for (const auto& bn : baseNames) (*out)[mi][bn] = 0.0;
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


// estout's coefs() list (estout: the cells() elements, each occurrence
// its own column; the pvalue() of a starred element appended when no element
// is that statistic; `ci` split into ci_l ci_u).
std::vector<std::string> eoCoefColumns(const EoCells& cells) {
  std::vector<std::string> out;
  std::vector<std::string> pvalues;
  for (const auto& el : cells.elems) {
    if (el.star || el.name == "_star" || el.name == "_sigsign") {
      const std::string pv = el.pvalue.empty() ? std::string("p") : el.pvalue;
      if (std::find(pvalues.begin(), pvalues.end(), pv) == pvalues.end()) pvalues.push_back(pv);
    }
  }
  for (const auto& el : cells.elems) {
    if (el.name == "ci") {
      out.push_back("ci_l");
      out.push_back("ci_u");
    } else {
      out.push_back(el.name);
    }
  }
  for (const auto& pv : pvalues) {
    bool isElem = false;
    for (const auto& el : cells.elems) {
      if (el.name == pv) isElem = true;
    }
    if (!isElem) out.push_back(pv);
  }
  return out;
}


namespace {
// One `#`-atom of a stripe name: a factor level (`0.foreign`, `2bn.rep78`),
// or anything else kept verbatim (`c.mpg`, `mpg`, `L.x`).
struct StripeAtom {
  bool factor = false;
  long long level = 0;
  std::string mark;  // "", "b", "bn", "o", "bo", "bno"
  std::string var;
  std::string text;
};

StripeAtom stripeAtom(const std::string& a) {
  StripeAtom out;
  out.text = a;
  const size_t dot = a.find('.');
  if (dot == std::string::npos || dot == 0) return out;
  const std::string head = a.substr(0, dot);
  size_t d = 0;
  while (d < head.size() && std::isdigit(static_cast<unsigned char>(head[d]))) ++d;
  if (d == 0) return out;
  const std::string mark = head.substr(d);
  if (!(mark.empty() || mark == "b" || mark == "bn" || mark == "o" || mark == "bo" || mark == "bno")) {
    return out;
  }
  try {
    out.level = std::stoll(head.substr(0, d));
  } catch (...) {
    return out;
  }
  out.factor = true;
  out.mark = mark;
  out.var = a.substr(dot + 1);
  return out;
}
}  // namespace


// Stata re-reads a stripe's factor levels when it stores one: a term whose
// factor position carries two or more levels and none of them a base is
// stored with its lowest level marked `bn` (StataBE: estout's r(coefs) rows
// `0.foreign 1.foreign` read back `0bn.foreign 1.foreign`, `2.rep78`..`5.rep78`
// read back `2bn.rep78 3.rep78 ...`, a lone `1.foreign` stays `1.foreign`).
std::vector<std::string> eoStripeNames(const std::vector<EoKey>& rows) {
  std::vector<std::vector<StripeAtom>> atoms(rows.size());
  std::vector<std::string> sig(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::string& name = rows[i].second;
    size_t a = 0;
    while (a <= name.size()) {
      size_t h = name.find('#', a);
      if (h == std::string::npos) h = name.size();
      atoms[i].push_back(stripeAtom(name.substr(a, h - a)));
      a = h + 1;
    }
    sig[i] = rows[i].first + ":";
    for (const auto& at : atoms[i]) sig[i] += (at.factor ? "F." + at.var : "V." + at.text) + "#";
  }
  std::vector<bool> done(rows.size(), false);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (done[i]) continue;
    std::vector<size_t> group;
    for (size_t j = i; j < rows.size(); ++j) {
      if (sig[j] == sig[i]) {
        group.push_back(j);
        done[j] = true;
      }
    }
    for (size_t pos = 0; pos < atoms[i].size(); ++pos) {
      if (!atoms[i][pos].factor) continue;
      std::set<long long> levels;
      bool hasBase = false;
      for (size_t j : group) {
        const StripeAtom& at = atoms[j][pos];
        levels.insert(at.level);
        if (at.mark.find('b') != std::string::npos) hasBase = true;
      }
      if (hasBase || levels.size() < 2) continue;
      const long long lowest = *levels.begin();
      for (size_t j : group) {
        StripeAtom& at = atoms[j][pos];
        if (at.level == lowest && at.mark.empty()) at.mark = "bn";
      }
    }
  }
  bool anyEq = false;
  for (const auto& r : rows) {
    if (r.first != "__" && r.first != "_" && !r.first.empty()) anyEq = true;
  }
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string name;
    for (size_t pos = 0; pos < atoms[i].size(); ++pos) {
      const StripeAtom& at = atoms[i][pos];
      if (pos > 0) name += "#";
      name += at.factor ? std::to_string(at.level) + at.mark + "." + at.var : at.text;
    }
    const std::string& eq = rows[i].first;
    const bool noEq = eq == "__" || eq == "_" || eq.empty();
    out.push_back(noEq ? (anyEq ? "_:" + name : name) : eq + ":" + name);
  }
  return out;
}


namespace {
const double kMissY = eval::stataMissingWithKind(25);  // `.y`: the model has the row, not the statistic
const double kMissZ = eval::stataMissingWithKind(26);  // `.z`: the model lacks the row / statistic

double asStata(double v) { return std::isnan(v) ? eval::kStataMissingSentinel : v; }

// One r(coefs) cell of a model that has the row (_estout_getres GetCoefs and
// ComputeCoefs): b-like statistics from e(b), se/var/t/z/p/ci from e(V) (the
// engine's table carries them), anything else from the e() matrix of that
// name; `.y` when the model has no such statistic for the row.
double eoCoefCell(const EoModel& em, const EoKey& key, const std::string& stat) {
  const EoV v = eoElemValue(em, key, stat);
  // e(beta) holds `.z` for a column estadd beta cannot standardise (an interaction, a constant)
  if (v.kind == EoV::kVacant && stat == "beta" && !em.beta.empty()) return kMissZ;
  if (v.kind == EoV::kVacant) return kMissY;
  return v.kind == EoV::kNum ? v.v : eval::kStataMissingSentinel;
}

// One r(stats) cell: eoStatValue's reading (the cell the table prints), as
// GetStats stores it -- a number, `.` for a missing one, `.y` with the text in
// r(m#_stat) for an e() macro, `.z` for a statistic the model does not have.
double eoStatsCell(SessionState* session, const EoModel& em, const std::string& key,
                   std::string* macroOut) {
  const EoV v = eoStatValue(em, key, session);
  switch (v.kind) {
    case EoV::kNum:
      return v.v;
    case EoV::kMissing:
      return eval::kStataMissingSentinel;
    case EoV::kText:
      *macroOut = v.text;
      return kMissY;
    case EoV::kVacant:
      break;
  }
  return kMissZ;
}
}  // namespace


void eoCollectRPost(const EoSpec& sp, const std::vector<EoModel>& models,
                    const std::vector<std::vector<EoKey>>& mrows, const std::vector<EoKey>& B,
                    const std::vector<std::string>& coefCols, EoRPost* post) {
  *post = EoRPost{};
  const size_t M = models.size();
  // r(coefs) (_estout_getres's bbc after estout's order/indicate/keep/drop;
  // MatrixMode's matrix in matrix mode).
  if (!B.empty()) {
    eval::EvalMatrix& c = post->coefs;
    c.rownames = eoStripeNames(B);
    if (sp.matrixMode) {
      const EoModel& em = models.front();
      c.colnames = sp.matrixColNames;
      for (const auto& key : B) {
        std::vector<double> row;
        for (size_t j = 0; j < sp.matrixColNames.size(); ++j) {
          double v = eval::kStataMissingSentinel;
          auto ex = em.extra.find("c" + std::to_string(j + 1));
          if (ex != em.extra.end()) {
            auto it = ex->second.find(key);
            if (it != ex->second.end()) v = asStata(it->second);
          }
          row.push_back(v);
        }
        c.data.push_back(std::move(row));
      }
    } else {
      for (size_t m = 0; m < M; ++m) {
        const std::string eq = models[m].name == "." ? std::string("active") : models[m].name;
        for (const auto& s : coefCols) c.colnames.push_back(eq + ":" + s);
        if (sp.dropped) c.colnames.push_back(eq + ":_dropped");
        if (sp.margin) c.colnames.push_back(eq + ":_dummy");
      }
      for (const auto& key : B) {
        std::vector<double> row;
        for (size_t m = 0; m < M; ++m) {
          const bool has = std::find(mrows[m].begin(), mrows[m].end(), key) != mrows[m].end();
          for (const auto& s : coefCols) row.push_back(has ? eoCoefCell(models[m], key, s) : kMissZ);
          if (sp.dropped) {
            // DroppedCoefs: 1 where e(b) and e(V) are both 0
            auto dv = models[m].vals.find(key);
            const bool dr = has && dv != models[m].vals.end() && dv->second.zeroVar && dv->second.b == 0.0;
            row.push_back(has ? (dr ? 1.0 : 0.0) : kMissZ);
          }
          // The marker column is 0 where the model has the coefficient (no marginal-effect
          // source marks a dummy), missing where it does not.
          if (sp.margin) row.push_back(has ? 0.0 : kMissZ);
        }
        c.data.push_back(std::move(row));
      }
    }
    post->hasCoefs = !c.colnames.empty();
  }
  if (sp.matrixMode) return;
  // r(stats) (_estout_getres's bbs): the stats() keys, `p` added when a
  // statistic is starred, one column per model.
  std::vector<std::string> statKeys;
  bool statStar = false;
  for (const auto& st : sp.stats) {
    if (std::find(statKeys.begin(), statKeys.end(), st.key) == statKeys.end()) statKeys.push_back(st.key);
    if (st.star) statStar = true;
  }
  if (statStar && std::find(statKeys.begin(), statKeys.end(), "p") == statKeys.end()) statKeys.push_back("p");
  if (!statKeys.empty()) {
    eval::EvalMatrix& s = post->stats;
    s.rownames = statKeys;
    for (size_t m = 0; m < M; ++m) s.colnames.push_back(models[m].name == "." ? "active" : models[m].name);
    for (const auto& k : statKeys) {
      std::vector<double> row;
      for (size_t m = 0; m < M; ++m) {
        std::string macro;
        row.push_back(eoStatsCell(sp.session, models[m], k, &macro));
        if (!macro.empty()) post->macros.emplace_back("m" + std::to_string(m + 1) + "_" + k, macro);
      }
      s.data.push_back(std::move(row));
    }
    post->hasStats = true;
  }
  // r(m#_depname), r(m#_estimates_title) (_estout_getres): an empty one is
  // not stored.
  for (size_t m = 0; m < M; ++m) {
    const std::string idx = "m" + std::to_string(m + 1);
    if (!models[m].depname.empty()) post->macros.emplace_back(idx + "_depname", models[m].depname);
    if (!models[m].title.empty()) post->macros.emplace_back(idx + "_estimates_title", models[m].title);
  }
}


// `estimates table` posts r() (est_table.ado EstTable, Mkemat): r(names),
// r(coef) -- e(b)'s columns of every model, mat_capp'ed, one `name:b` and
// `name:var` column pair per model (`Active` for `.`), `.z` where a model
// lacks the row and `.b`/`.o` for a base/omitted column (b 0 and var 0); for
// more than one model the rows lose their base/omitted marks
// (_ms_omit_unset), for one they keep e(b)'s names -- and r(stats) for stats().
void estTablePostR(SessionState& session, const std::vector<std::string>& names,
                   const std::vector<EoModel>& ems, const std::vector<EoKey>& rows,
                   const std::vector<std::string>& stats) {
  eval::replaceRClass(session.eval);
  session.eval.rClassCmd = "estimates";
  const size_t M = ems.size();
  std::string joined;
  for (const auto& n : names) joined += (joined.empty() ? "" : " ") + n;
  if (!joined.empty()) session.eval.rStrings.set("names", joined);
  auto label = [&](size_t i) { return names[i] == "." ? std::string("Active") : names[i]; };
  if (!rows.empty()) {
    std::vector<EoKey> named = rows;
    if (M == 1) {
      for (auto& k : named) {
        auto it = ems[0].ebMarked.find(k);
        if (it != ems[0].ebMarked.end()) k.second = it->second;
      }
    }
    eval::EvalMatrix c;
    c.rownames = eoStripeNames(named);
    // regress's e(b) has no equation name: its rows stay `mpg` beside another model's `foreign:mpg`
    // (vs-stata bank d1bc0be1, 6a96dc10)
    for (auto& rn : c.rownames) {
      if (rn.rfind("_:", 0) == 0) rn = rn.substr(2);
    }
    for (size_t i = 0; i < M; ++i) {
      c.colnames.push_back(label(i) + ":b");
      c.colnames.push_back(label(i) + ":var");
    }
    for (const auto& k : rows) {
      std::vector<double> row;
      for (size_t i = 0; i < M; ++i) {
        const EoModel& em = ems[i];
        auto v = em.vals.find(k);
        const bool has = v != em.vals.end() &&
                         std::find(em.coefRows.begin(), em.coefRows.end(), k) != em.coefRows.end();
        if (!has) {
          row.push_back(kMissZ);
          row.push_back(kMissZ);
          continue;
        }
        const EoCoefVals& cv = v->second;
        auto mk = em.ebMarked.find(k);
        if (cv.zeroVar && cv.b == 0.0 && mk != em.ebMarked.end()) {
          const double code = eval::stataMissingWithKind(
              mk->second.find("b.") != std::string::npos ? 2 : 15);  // `.b` base, `.o` omitted
          row.push_back(code);
          row.push_back(code);
          continue;
        }
        row.push_back(asStata(cv.b));
        row.push_back(cv.zeroVar ? 0.0 : asStata(std::isnan(cv.se) ? cv.se : cv.se * cv.se));
      }
      c.data.push_back(std::move(row));
    }
    session.eval.rMatrices.emplace_back("coef", std::move(c));
  }
  std::vector<std::string> keys;
  for (const auto& k : stats) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  if (!keys.empty()) {
    // est_table GetStats: an e() scalar by its name, aic / bic / rank computed, `.z` for the rest.
    eval::EvalMatrix st;
    st.rownames = keys;
    for (size_t i = 0; i < M; ++i) st.colnames.push_back(label(i));
    for (const auto& k : keys) {
      std::vector<double> row;
      for (size_t i = 0; i < M; ++i) {
        const EoV v = eoStatValue(ems[i], k, &session);
        row.push_back(v.kind == EoV::kNum ? v.v : (v.kind == EoV::kMissing ? kEoNaN : kMissZ));
      }
      st.data.push_back(std::move(row));
    }
    session.eval.rMatrices.emplace_back("stats", std::move(st));
  }
}


// estout posts r() (estout): r(cmdline), r(names),
// r(nmodels), r(ccols) -- absent when no coefficient row survived (estout
// `return local ccols ""`) --, then r(coefs), r(stats) and the per-model
// macros _estout_getres returned (`return add`).
void eoPostR(SessionState& session, const std::string& cmdline, const std::vector<EoModel>& models,
             int ccols, const EoRPost* post) {
  eval::replaceRClass(session.eval);
  std::string names;
  for (const auto& m : models) {
    if (!names.empty()) names += " ";
    names += m.name;
  }
  session.eval.rStrings.set("cmdline", cmdline);
  session.eval.rStrings.set("names", names);
  session.eval.rValues.set("nmodels", static_cast<double>(models.size()));
  if (ccols >= 0) session.eval.rValues.set("ccols", static_cast<double>(ccols));
  if (post == nullptr) return;
  for (const auto& [k, v] : post->macros) {
    if (!v.empty()) session.eval.rStrings.set(k, v);
  }
  auto putMatrix = [&](const std::string& name, const eval::EvalMatrix& m) {
    eval::EvalMatrix copy = m;
    copy.gen = eval::nextStoreGeneration();
    for (auto it = session.eval.rMatrices.begin(); it != session.eval.rMatrices.end(); ++it) {
      if (it->first == name) {
        session.eval.rMatrices.erase(it);
        break;
      }
    }
    session.eval.rMatrices.emplace_back(name, std::move(copy));
  };
  if (post->hasCoefs) putMatrix("coefs", post->coefs);
  if (post->hasStats) putMatrix("stats", post->stats);
}
}
