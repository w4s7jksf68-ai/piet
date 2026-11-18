// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset_command.cpp — the `serset` command family (`serset`, `serset [set] #`,
// `serset use`, `clear`, `drop`, `dir`). See include/piet/serset.h.
#include "piet/serset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <set>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/handlers/graph.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"
#include "serset_internal.h"

namespace piet::serset {

using ojson = nlohmann::ordered_json;
using namespace serset_detail;

namespace {

bool parseInt(const std::string& s, int* out) {
  const std::string t = trimText(s);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') i = 1;
  if (i >= t.size()) return false;
  for (size_t j = i; j < t.size(); ++j)
    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
  try {
    *out = std::stoi(t);
  } catch (...) {
    return false;
  }
  return true;
}


std::string csvNumber(double v) {
  if (std::isnan(v)) return ".";
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string csvString(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// `serset use`: replaces the data in memory with the serset's variables, their
// names and storage types (the w2al helper reads `: type` of each).
ojson useSerset(SessionState& session, const Serset& s) {
  ojson out = ojson::array();
  if (!s.retained) {
    out.push_back(refusal("serset use: this serset holds " + std::to_string(s.n) + " observations on " +
                              std::to_string(s.cols.size()) +
                              " variables, more than PIET keeps for read-back; its data were not "
                              "retained",
                          910));
    return out;
  }
  const std::string hbin = mintScratchPath(session.workdir(), "serset");
  const std::string csv = hbin + ".csv";
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      out.push_back(refusal("serset use: could not stage the serset's data", 603));
      return out;
    }
    for (size_t c = 0; c < s.cols.size(); ++c) {
      if (c) f << ",";
      f << s.cols[c].name;
    }
    f << "\n";
    for (int64_t i = 0; i < s.n; ++i) {
      for (size_t c = 0; c < s.cols.size(); ++c) {
        if (c) f << ",";
        const Column& col = s.cols[c];
        const size_t r = static_cast<size_t>(i);
        if (col.isStr) {
          f << csvString(r < col.str.size() ? col.str[r] : std::string());
        } else {
          f << csvNumber(r < col.num.size() ? col.num[r] : kNaN);
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csv);
      out.push_back(refusal("serset use: could not stage the serset's data (disk full or I/O error)",
                            693));
      return out;
    }
  }
  std::string factorList;
  for (const Column& c : s.cols) {
    if (!c.isStr) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += c.name;
  }
  std::vector<std::string> args = {"convert", "--data", csv, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  const CobraRunResult cr = runCobra(args);
  cecon::native_unlink(csv);
  if (!cr.ok || !cecon::native_exists(hbin)) {
    cecon::native_unlink(hbin);
    out.push_back(refusal("serset use: the serset's data could not be loaded", 693));
    return out;
  }
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.invalidateDatastore();
  for (const Column& c : s.cols) session.varMeta[c.name].stataType = c.type;
  out.push_back(handlers::mutationResult(session, "use"));
  session.datasetChanged = false;  // the loaded serset is the dataset as it stands: c(changed) = 0 (bank)
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// serset create / summarize / sort / reset_id

namespace {

// `typed` is `full` or an abbreviation of it of at least `minLen` letters (Stata's {cmdab:}).
bool isAbbrev(const std::string& typed, const std::string& full, size_t minLen) {
  return typed.size() >= minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
}

struct OptSpec {
  const char* full;
  size_t minLen;
  bool takesValue;
};

// Resolves every typed option against `allowed`; the canonical names come back in `out`. An option the
// subcommand does not have is r(198) "option X not allowed", as Stata answers it.
std::optional<ojson> resolveOptions(const std::vector<SersetOption>& opts, const std::vector<OptSpec>& allowed,
                                    std::vector<std::pair<std::string, std::string>>* out) {
  for (const SersetOption& o : opts) {
    const std::string k = strutil::lower(o.name);
    const OptSpec* hit = nullptr;
    for (const OptSpec& a : allowed) {
      if (isAbbrev(k, a.full, a.minLen)) {
        hit = &a;
        break;
      }
    }
    if (hit == nullptr) {
      return refusal("option " + k + (o.isBool ? "" : "()") + " not allowed", 198);
    }
    if (hit->takesValue == o.isBool) {
      return refusal("option " + std::string(hit->full) + " incorrectly specified", 198);
    }
    out->emplace_back(hit->full, o.value);
  }
  return std::nullopt;
}

// Stable ascending sort of the rows of `s` on the columns `keys` in turn, missing values (and the empty
// string) last.
void sortRows(Serset& s, const std::vector<size_t>& keys) {
  std::vector<size_t> order(static_cast<size_t>(s.n));
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    for (size_t k : keys) {
      const Column& c = s.cols[k];
      if (c.isStr) {
        if (c.str[a] != c.str[b]) return c.str[a] < c.str[b];
      } else {
        const double x = c.num[a], y = c.num[b];
        const bool mx = std::isnan(x), my = std::isnan(y);
        if (mx != my) return !mx;
        if (!mx && x != y) return x < y;
      }
    }
    return false;
  });
  for (Column& c : s.cols) {
    if (c.isStr) {
      std::vector<std::string> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(c.str[i]);
      c.str = std::move(v);
    } else {
      std::vector<double> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(c.num[i]);
      c.num = std::move(v);
    }
  }
}

// A serset variable named by name or by its number (1-based): the column index, or -1.
int findSersetVar(const Serset& s, const std::string& svn) {
  for (size_t i = 0; i < s.cols.size(); ++i)
    if (s.cols[i].name == svn) return static_cast<int>(i);
  int num = 0;
  if (parseInt(svn, &num) && num >= 1 && static_cast<size_t>(num) <= s.cols.size()) return num - 1;
  return -1;
}

// The dataset variables a `serset create` varlist names: `*` / `?` patterns, `a-b` ranges and unique
// abbreviations are expanded; an unknown name is r(111).
std::optional<ojson> expandCreateVars(SessionState& session, Datastore* ds, const std::vector<std::string>& words,
                                      std::vector<std::string>* vars) {
  for (const std::string& w : words) {
    // time-series operators (L.x, F2.x, D.x, S.x): they need a tsset time variable, and this build does
    // not evaluate them for a serset
    static const std::regex reTsOp(R"(^[LFDSlfds][0-9]*\.[A-Za-z_])");
    if (std::regex_search(w, reTsOp)) {
      if (!session.timevar.has_value() || session.timevar->empty()) {
        return refusal("time variable not set, use tsset varname ...", 111);
      }
      return refusal("serset create: time-series operators (" + w + ") are not implemented in this version of PIET",
                     198);
    }
    if (ds == nullptr) return refusal("variable " + w + " not found", 111);
    if (w.find_first_of("*?") != std::string::npos) {
      size_t before = vars->size();
      for (const auto& nm : ds->names())
        if (strutil::globMatch(w, nm)) vars->push_back(nm);
      if (vars->size() == before) return refusal("variable " + w + " not found", 111);
      continue;
    }
    const size_t dash = w.find('-');
    if (dash != std::string::npos && dash > 0 && dash + 1 < w.size() && !ds->has(w)) {
      const std::string a = handlers::unabbreviateGraphVar(session, *ds, w.substr(0, dash));
      const std::string b = handlers::unabbreviateGraphVar(session, *ds, w.substr(dash + 1));
      if (!ds->has(a)) return refusal("variable " + w.substr(0, dash) + " not found", 111);
      if (!ds->has(b)) return refusal("variable " + w.substr(dash + 1) + " not found", 111);
      const auto names = ds->names();
      size_t ia = names.size(), ib = names.size();
      for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == a) ia = i;
        if (names[i] == b) ib = i;
      }
      if (ia > ib) return refusal("variable " + w + " not found", 111);
      for (size_t i = ia; i <= ib; ++i) vars->push_back(names[i]);
      continue;
    }
    const std::string name = handlers::unabbreviateGraphVar(session, *ds, w);
    if (!ds->has(name)) return refusal("variable " + w + " not found", 111);
    vars->push_back(name);
  }
  return std::nullopt;
}

// `serset create varlist [if] [in] [, omitanymiss omitallmiss omitdupmiss omitnothing sort(varlist)]`.
// The default is omitanymiss: an observation with a missing value in any NUMERIC variable is left out;
// omitallmiss leaves out those where every numeric variable is missing; omitdupmiss keeps only the first
// of the all-missing observations that repeat; omitnothing keeps every observation of the sample. Strings
// never count as missing. The sort order of the data is not touched; sort() orders the serset.
ojson createSerset(SessionState& session, const std::vector<std::string>& args,
                   const std::vector<SersetOption>& opts, Datastore* sample) {
  ojson out = ojson::array();
  std::vector<std::pair<std::string, std::string>> got;
  if (auto bad = resolveOptions(opts,
                                {{"omitanymiss", 8, false}, {"omitallmiss", 8, false}, {"omitdupmiss", 8, false},
                                 {"omitnothing", 5, false}, {"sort", 4, true}},
                                &got)) {
    out.push_back(*bad);
    return out;
  }
  const std::vector<std::string> words(args.begin() + 1, args.end());
  if (words.empty()) {
    out.push_back(refusal("varlist required", 100));
    return out;
  }
  std::vector<std::string> vars;
  if (auto bad = expandCreateVars(session, sample, words, &vars)) {
    out.push_back(*bad);
    return out;
  }
  std::string omit = "omitanymiss";  // when several are typed the last one is used (not measured)
  std::vector<std::string> sortVars;
  for (const auto& [k, v] : got) {
    if (k == "sort") {
      std::vector<std::string> sw;
      std::string cur;
      for (char c : v + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
          if (!cur.empty()) sw.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (auto bad = expandCreateVars(session, sample, sw, &sortVars)) {
        out.push_back(*bad);
        return out;
      }
    } else {
      omit = k;
    }
  }
  Datastore& ds = *sample;
  const int64_t nAll = ds.nRows();
  std::vector<int64_t> all(static_cast<size_t>(nAll));
  for (int64_t i = 0; i < nAll; ++i) all[static_cast<size_t>(i)] = i;
  std::vector<Column> cols;
  for (const std::string& v : vars) cols.push_back(variableColumn(session, ds, v, all));
  std::vector<size_t> numericCols;
  for (size_t c = 0; c < cols.size(); ++c)
    if (!cols[c].isStr) numericCols.push_back(c);

  std::vector<int64_t> keep;
  for (int64_t r = 0; r < nAll; ++r) {
    size_t nMiss = 0;
    for (size_t c : numericCols)
      if (std::isnan(cols[c].num[static_cast<size_t>(r)])) ++nMiss;
    const bool anyMiss = nMiss > 0;
    const bool allMiss = !numericCols.empty() && nMiss == numericCols.size();
    bool drop = false;
    if (omit == "omitanymiss") drop = anyMiss;
    else if (omit == "omitallmiss") drop = allMiss;
    if (!drop) keep.push_back(r);
  }
  if (omit == "omitdupmiss") {
    // among the observations whose numeric values are all missing, a repeat of an earlier one (same
    // strings too) is left out
    std::vector<int64_t> kept;
    std::set<std::vector<std::string>> seen;
    for (int64_t r : keep) {
      bool allMiss = !numericCols.empty();
      for (size_t c : numericCols)
        if (!std::isnan(cols[c].num[static_cast<size_t>(r)])) allMiss = false;
      if (allMiss) {
        std::vector<std::string> key;
        for (const Column& c : cols)
          if (c.isStr) key.push_back(c.str[static_cast<size_t>(r)]);
        if (!seen.insert(key).second) continue;
      }
      kept.push_back(r);
    }
    keep = std::move(kept);
  }

  Serset s;
  s.n = static_cast<int64_t>(keep.size());
  for (size_t c = 0; c < cols.size(); ++c) {
    Column nc;
    nc.name = cols[c].name;
    nc.type = cols[c].type;
    nc.isStr = cols[c].isStr;
    for (int64_t r : keep) {
      if (nc.isStr) nc.str.push_back(cols[c].str[static_cast<size_t>(r)]);
      else nc.num.push_back(cols[c].num[static_cast<size_t>(r)]);
    }
    s.cols.push_back(std::move(nc));
  }
  if (!sortVars.empty()) {
    // the sort() variables come from the data, not the serset: order the kept rows on them
    std::vector<Column> keyCols;
    for (const std::string& v : sortVars) keyCols.push_back(variableColumn(session, ds, v, keep));
    Serset tmp;
    tmp.n = s.n;
    tmp.cols = keyCols;
    std::vector<size_t> ks;
    for (size_t i = 0; i < keyCols.size(); ++i) ks.push_back(i);
    // sort a copy of the key columns together with the serset by sorting an index first
    std::vector<size_t> order(static_cast<size_t>(s.n));
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    // carry the row number as the last key column so the permutation can be read back
    Column rowNo;
    rowNo.name = "__row";
    rowNo.type = "double";
    for (size_t i = 0; i < order.size(); ++i) rowNo.num.push_back(static_cast<double>(i));
    tmp.cols.push_back(std::move(rowNo));
    sortRows(tmp, ks);
    const Column& perm = tmp.cols.back();
    for (Column& c : s.cols) {
      if (c.isStr) {
        std::vector<std::string> v;
        for (double p : perm.num) v.push_back(c.str[static_cast<size_t>(p)]);
        c.str = std::move(v);
      } else {
        std::vector<double> v;
        for (double p : perm.num) v.push_back(c.num[static_cast<size_t>(p)]);
        c.num = std::move(v);
      }
    }
  }
  const size_t k = s.cols.size();
  const int64_t n = s.n;
  const int id = session.sersets.add(std::move(s));
  if (id < 0) {
    out.push_back(refusal("too many sersets", 1900));
    return out;
  }
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("id", static_cast<double>(id));
  session.eval.rValues.set("k", static_cast<double>(k));
  session.eval.rValues.set("N", static_cast<double>(n));
  eval::declareRPosted(session.eval, "serset", {"id", "k", "N"});
  return out;
}

// `serset summarize svn [, detail]`: r(N sum_w mean Var sd min max sum); detail adds skewness, kurtosis
// and the percentiles 1 5 10 25 50 75 90 95 99, defined as summarize defines them.
ojson summarizeSerset(SessionState& session, const std::vector<std::string>& args,
                      const std::vector<SersetOption>& opts) {
  ojson out = ojson::array();
  std::vector<std::pair<std::string, std::string>> got;
  if (auto bad = resolveOptions(opts, {{"detail", 1, false}}, &got)) {
    out.push_back(*bad);
    return out;
  }
  const bool detail = !got.empty();
  const Serset* s = session.sersets.currentSet();
  if (s == nullptr) {
    out.push_back(refusal("no serset in use", 198));
    return out;
  }
  const std::string svn = args.size() > 1 ? args[1] : std::string();
  const int k = svn.empty() ? -1 : findSersetVar(*s, svn);
  if (k < 0) {
    out.push_back(refusal("serset variable " + svn + " not found", 111));
    return out;
  }
  const Column& col = s->cols[static_cast<size_t>(k)];
  if (col.isStr) {
    out.push_back(refusal("type mismatch", 109));
    return out;
  }
  if (!s->retained) {
    out.push_back(refusal("serset summarize: this serset's data were not retained", 910));
    return out;
  }
  std::vector<double> x;
  for (double v : col.num)
    if (!std::isnan(v)) x.push_back(v);
  const double n = static_cast<double>(x.size());
  eval::replaceRClass(session.eval);
  auto set = [&](const char* name, double v) { session.eval.rValues.set(name, v); };
  std::vector<std::string> posted = {"N", "sum_w", "mean", "Var", "sd", "min", "max", "sum"};
  double sum = 0.0;
  for (double v : x) sum += v;
  set("N", n);
  set("sum_w", n);
  if (x.empty()) {
    // no observation: Stata leaves mean, Var, sd, min and max missing
    for (const char* nm : {"mean", "Var", "sd", "min", "max"}) set(nm, kNaN);
    set("sum", 0.0);
    if (detail) posted.insert(posted.end(), {"skewness", "kurtosis"});
    eval::declareRPosted(session.eval, "serset summarize", posted);
    return out;
  }
  const double mean = sum / n;
  double ss = 0.0, m2 = 0.0, m3 = 0.0, m4 = 0.0;
  for (double v : x) {
    const double d = v - mean;
    ss += d * d;
    m2 += d * d;
    m3 += d * d * d;
    m4 += d * d * d * d;
  }
  const double var = n > 1 ? ss / (n - 1.0) : kNaN;
  set("mean", mean);
  set("Var", var);
  set("sd", n > 1 ? std::sqrt(var) : kNaN);
  set("min", *std::min_element(x.begin(), x.end()));
  set("max", *std::max_element(x.begin(), x.end()));
  set("sum", sum);
  if (detail) {
    std::sort(x.begin(), x.end());
    m2 /= n;
    m3 /= n;
    m4 /= n;
    set("skewness", m2 > 0 ? m3 / std::pow(m2, 1.5) : kNaN);
    set("kurtosis", m2 > 0 ? m4 / (m2 * m2) : kNaN);
    posted.insert(posted.end(), {"skewness", "kurtosis"});
    for (int p : {1, 5, 10, 25, 50, 75, 90, 95, 99}) {
      // summarize's percentile: with np = N p / 100, the average of the np-th and the next order
      // statistic when np is whole, otherwise the next one up
      const double np = n * p / 100.0;
      double q;
      if (std::floor(np) == np) {
        const size_t i = static_cast<size_t>(np);
        q = i >= x.size() ? x.back() : (x[i - 1] + x[i]) / 2.0;
      } else {
        q = x[static_cast<size_t>(std::ceil(np)) - 1];
      }
      const std::string name = "p" + std::to_string(p);
      set(name.c_str(), q);
      posted.push_back(name);
    }
  }
  eval::declareRPosted(session.eval, "serset summarize", posted);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The `serset` command family

ojson runSerset(SessionState& session, const std::vector<std::string>& args,
                const std::vector<SersetOption>& opts, Datastore* sample) {
  ojson out = ojson::array();
  Store& st = session.sersets;
  const std::string sub = args.empty() ? std::string() : strutil::lower(args[0]);
  bool clearOpt = false;

  // The options each subcommand takes; create / summarize resolve their own.
  const bool ownOpts = isAbbrev(sub, "create", 2) || isAbbrev(sub, "summarize", 2);
  if (!ownOpts) {
    std::vector<std::pair<std::string, std::string>> got;
    const bool isUse = sub == "use";
    if (auto bad = resolveOptions(opts, isUse ? std::vector<OptSpec>{{"clear", 1, false}} : std::vector<OptSpec>{},
                                  &got)) {
      out.push_back(*bad);
      return out;
    }
    clearOpt = isUse && !got.empty();
  }
  (void)clearOpt;

  // `serset`: r(id) of the current serset (-1 when none), r(k), r(N).
  if (sub.empty()) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("id", static_cast<double>(st.current()));
    std::vector<std::string> posted = {"id"};
    if (const Serset* s = st.currentSet()) {
      session.eval.rValues.set("k", static_cast<double>(s->cols.size()));
      session.eval.rValues.set("N", static_cast<double>(s->n));
      posted = {"id", "k", "N"};
    }
    eval::declareRPosted(session.eval, "serset", posted);
    return out;
  }

  if (isAbbrev(sub, "create", 2)) return createSerset(session, args, opts, sample);
  if (isAbbrev(sub, "summarize", 2)) return summarizeSerset(session, args, opts);

  if (sub == "create_xmedians" || sub == "create_cspline") {
    out.push_back(refusal("serset " + sub + " is not implemented in this version of PIET", 199));
    return out;
  }

  // `serset sort [svn ...]`: the current serset's observations in the order of those variables (all of
  // them in turn when none is named).
  if (sub == "sort") {
    const Serset* cur = st.currentSet();
    if (cur == nullptr) {
      out.push_back(refusal("no serset in use", 198));
      return out;
    }
    std::vector<size_t> keys;
    for (size_t i = 1; i < args.size(); ++i) {
      const int k = findSersetVar(*cur, args[i]);
      if (k < 0) {
        out.push_back(refusal("serset variable " + args[i] + " not found", 111));
        return out;
      }
      keys.push_back(static_cast<size_t>(k));
    }
    if (keys.empty())
      for (size_t i = 0; i < cur->cols.size(); ++i) keys.push_back(i);
    if (cur->retained) sortRows(*st.getMutable(st.current()), keys);
    return out;
  }

  // `serset reset_id #`: the current serset takes the number # (0..1999, not in use).
  if (sub == "reset_id") {
    int newId = 0;
    if (st.currentSet() == nullptr) {
      out.push_back(refusal("no serset in use", 198));
      return out;
    }
    if (args.size() < 2 || !parseInt(args[1], &newId)) {
      out.push_back(refusal("serset reset_id: a serset number is required", 198));
      return out;
    }
    if (newId < 0 || newId > 1999) {
      out.push_back(refusal("serset number out of range", 198));
      return out;
    }
    if (!st.resetId(st.current(), newId)) {
      out.push_back(refusal("serset " + std::to_string(newId) + " already exists", 198));
    }
    return out;
  }

  // `serset [set] #`
  int id = 0;
  if (sub == "set" || parseInt(args[0], &id)) {
    const std::string num = sub == "set" ? (args.size() > 1 ? args[1] : std::string()) : args[0];
    if (!parseInt(num, &id)) {
      out.push_back(refusal("serset set: a serset number is required", 198));
      return out;
    }
    if (!st.select(id)) {
      out.push_back(refusal("series " + std::to_string(id) + " not found", 111));
      return out;
    }
    return out;
  }

  // `serset use [, clear]`: the current serset becomes the data (w2al log:183-188
  // `no serset in use`, r(198)).
  if (sub == "use") {
    const Serset* s = st.currentSet();
    if (s == nullptr) {
      out.push_back(refusal("no serset in use", 198));
      return out;
    }
    const Serset copy = *s;
    ojson blocks = useSerset(session, copy);
    for (auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        out.push_back(b);
        return out;
      }
    }
    eval::replaceRClass(session.eval);
    std::string names;
    for (size_t i = 0; i < copy.cols.size(); ++i) {
      if (i) names += " ";
      names += copy.cols[i].name;
    }
    session.eval.rStrings.set("varnames", names);
    eval::declareRPosted(session.eval, "serset use", {"varnames"});
    for (auto& b : blocks) out.push_back(b);
    return out;
  }

  if (sub == "clear") {
    st.clear();
    return out;
  }

  if (sub == "drop") {
    if (args.size() < 2) {
      st.clear();
      return out;
    }
    std::vector<int> ids;
    for (size_t i = 1; i < args.size(); ++i) {
      const std::string a = args[i];
      if (strutil::lower(a) == "_all") {
        st.clear();
        return out;
      }
      const size_t slash = a.find('/');
      int lo = 0, hi = 0;
      if (slash != std::string::npos && parseInt(a.substr(0, slash), &lo) &&
          parseInt(a.substr(slash + 1), &hi)) {
        for (int k = lo; k <= hi; ++k) ids.push_back(k);
      } else if (parseInt(a, &lo)) {
        ids.push_back(lo);
      } else {
        out.push_back(refusal("serset drop: `" + a + "' is not a serset number", 198));
        return out;
      }
    }
    for (int k : ids) st.drop(k);
    return out;
  }

  // `serset dir` (w2al log:5418-5429): the names as stored, a renamed duplicate
  // under its own (StataBE 19.5, BUG-1543: `y x y x` where `serset use` loads
  // `y x y1 x1`).
  if (sub == "dir") {
    std::string text;
    for (const auto& kv : st.all()) {
      const Serset& s = kv.second;
      char head[96];
      std::snprintf(head, sizeof(head), "%3d.  %lld observation%s on %zu variable%s", kv.first,
                    static_cast<long long>(s.n), s.n == 1 ? "" : "s", s.cols.size(),
                    s.cols.size() == 1 ? "" : "s");
      std::string names;
      for (size_t i = 0; i < s.cols.size(); ++i) {
        if (i) names += " ";
        names += storedName(s.cols[i]);
      }
      text += "\n" + std::string(head) + "\n      " + names + "\n";
    }
    if (!text.empty()) out.push_back(displayBlock(text));
    return out;
  }

  out.push_back(refusal("serset " + sub + " is not supported in this version of PIET", 199));
  return out;
}

}  // namespace piet::serset
