// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data reshape.
#include "data_combine_internal.h"
namespace piet::handlers::data_combine_detail {


// _parse_ivar_jvar: opts["i"]/["j"] are space-separated token strings
// (tokenizer stores paren-option values as raw text: `i(id year)` ->
// {"i": "id year"}).
std::pair<std::vector<std::string>, std::optional<std::string>> parseIvarJvar(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::optional<std::string> iRaw, jRaw;
  bool iIsBool = false, jIsBool = false;
  for (const auto& [k, v] : opts) {
    if (k == "i") {
      if (v.isBool) iIsBool = true;
      else iRaw = v.strValue;
    }
    if (k == "j") {
      if (v.isBool) jIsBool = true;
      else jRaw = v.strValue;
    }
  }
  if (iIsBool || !iRaw.has_value() || iRaw->empty()) return {{}, std::nullopt};
  std::vector<std::string> iVars;
  {
    std::istringstream iss(*iRaw);
    std::string tok;
    while (iss >> tok) iVars.push_back(tok);
  }
  if (jIsBool || !jRaw.has_value() || jRaw->empty()) return {iVars, std::nullopt};
  std::istringstream jss(*jRaw);
  std::string jVar;
  jss >> jVar;
  return {iVars, jVar};
}
}
namespace piet::handlers {
using namespace data_combine_detail;


ojson runReshape(const std::vector<std::string>& stubsIn,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session) {
  if (stubsIn.empty()) {
    return engineErrorBlock("reshape: specify direction: `reshape long ...' or `reshape wide ...'");
  }
  std::string direction = lower(stubsIn[0]);
  std::vector<std::string> stubs(stubsIn.begin() + 1, stubsIn.end());

  // BUG-105 (옵션망): reshape accepts only i()/j() — any other residual
  // option token is a named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownReshapeOpts = kVocabReshape.known;
  // `String` abbreviates to s (reshape.ado's syntax line), below the net's
  // 2-character prefix floor: s / st / str ... string all name it.
  std::vector<std::pair<std::string, eval::OptValue>> optsC(opts);
  bool jString = false;
  for (auto& kv : optsC) {
    if (!kv.first.empty() && kv.first.size() <= 6 && std::string("string").compare(0, kv.first.size(), kv.first) == 0) {
      kv.first = "string";
      jString = true;
    }
  }
  if (auto err = eval::rejectUnknownOptions(optsC, kKnownReshapeOpts, kVocabReshape.unimplemented)) {
    return *err;
  }
  // j(t, string): the J() option's own `string` (reshape).
  for (auto& kv : optsC) {
    if (kv.first != "j" || kv.second.isBool) continue;
    const size_t comma = kv.second.strValue.find(',');
    if (comma == std::string::npos) continue;
    const std::string tail = lower(trim(kv.second.strValue.substr(comma + 1)));
    if (!tail.empty() && tail.size() <= 6 && std::string("string").compare(0, tail.size(), tail) == 0)
      jString = true;
    kv.second.strValue = trim(kv.second.strValue.substr(0, comma));
  }

  auto [iVars, jVar] = parseIvarJvar(optsC);
  if (iVars.empty()) {
    return engineErrorBlock(
        "reshape: i() option required — e.g. `reshape long inc, i(id) j(year)'");
  }
  if (!jVar.has_value()) {
    return engineErrorBlock(
        "reshape: j() option required — e.g. `reshape long inc, i(id) j(year)'");
  }
  if (direction != "long" && direction != "wide") {
    return engineErrorBlock("reshape: unknown direction `" + direction +
                            "'. Use `reshape long ...' or `reshape wide ...'.");
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return engineErrorBlock("No dataset loaded.");
  const std::vector<std::string> allNames = ds->names();  // a copy: the head is replaced below
  auto hasName = [&](const std::string& n) {
    return std::find(allNames.begin(), allNames.end(), n) != allNames.end();
  };

  if (direction == "long") {
    for (const auto& v : iVars) {
      if (!hasName(v)) return engineErrorBlock("reshape long: id variable `" + v + "' not found");
    }
    if (stubs.empty()) return engineErrorBlock("reshape long: no stub variables specified");
  } else {
    if (stubs.empty()) return engineErrorBlock("reshape wide: no stub variables specified");
    for (const auto& v : iVars) {
      if (!hasName(v)) return engineErrorBlock("reshape wide: id variable `" + v + "' not found");
    }
    if (!hasName(*jVar)) {
      return engineErrorBlock("reshape wide: j variable `" + *jVar + "' not found");
    }
    for (const auto& s : stubs) {
      if (!hasName(s)) return engineErrorBlock("reshape wide: stub variable `" + s + "' not found");
    }
  }

  std::string joinedStubs, joinedI;
  for (size_t i = 0; i < stubs.size(); ++i) {
    if (i) joinedStubs += ",";
    joinedStubs += stubs[i];
  }
  for (size_t i = 0; i < iVars.size(); ++i) {
    if (i) joinedI += ",";
    joinedI += iVars[i];
  }

  int64_t nBeforeRows = ds->nRows();
  // What the result variables take from the source ones (reshape.ado
  // mkrtmpST/Recast :1425-1464 and the wide labels): long — a stub is typed
  // by the widest of its components, with no label and its default format
  // (dc_r_long_v3 p `float|%9.0g|[]`, v4 q int); wide — each new variable
  // keeps the long stub's type and format and is labelled "<j> <stub>"
  // (dc_r_wide_v2 p1 `float|%6.2f|[1 p]`).
  const std::vector<VarInfo> cardBefore = session.varlistWithMeta(ds);
  auto typeOf = [&](const std::string& nm) -> std::optional<std::string> {
    auto it = session.varMeta.find(nm);
    if (it != session.varMeta.end() && it->second.stataType.has_value() && !it->second.stataType->empty())
      return it->second.stataType;
    if (session.implicitFloatVars.count(nm)) return std::string("float");
    return std::nullopt;
  };
  std::map<std::string, std::vector<std::string>> stubComponents;  // long: stub -> its columns
  if (direction == "long") {
    for (const auto& nm : allNames) {
      size_t best = stubs.size();
      for (size_t si = 0; si < stubs.size(); ++si)
        if (nm.size() > stubs[si].size() && nm.compare(0, stubs[si].size(), stubs[si]) == 0 &&
            (best == stubs.size() || stubs[si].size() > stubs[best].size()))
          best = si;
      if (best != stubs.size()) stubComponents[stubs[best]].push_back(nm);
    }
  }
  struct WideStubMeta {
    std::optional<std::string> type;
    std::string format;
  };
  std::map<std::string, WideStubMeta> wideStubMeta;
  if (direction == "wide") {
    for (const auto& st : stubs) {
      WideStubMeta wm;
      wm.type = typeOf(st);
      for (const VarInfo& v : cardBefore)
        if (v.name == st) wm.format = session.displayFormat(v);
      wideStubMeta[st] = wm;
    }
  }
  // (cobra reshape --stata drops PIET's hidden `__piet_*` columns itself.)
  const std::string reshapeData = ds->path();
  std::string outPath = nextPath(session.workdir(), "reshape_" + direction);
  std::vector<std::string> args = {"reshape", direction,  "--data", reshapeData, "--stubs",
                                    joinedStubs, "--i",    joinedI,  "--j",      *jVar,
                                    "--stata", "--out",   outPath};
  if (jString) args.push_back("--string");
  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane. The one stateless dependency
    // below — reading the post-reshape j column for `long` — now goes through the
    // §38 column fetch (one column, not the whole frame) with the materialized
    // head as the fallback, instead of opening the --out file that no longer exists.
    SessionState::ResidentEligible residentReshape(session);
    cr = runCobra(args);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    // reshape.ado's refusals leave the data as they were (dc_r_long_nostr
    // r(498), dc_r_long_idup r(9), dc_r_wide_jmiss r(498), dc_r_wide_jdup
    // r(9), dc_r_wide_notconst r(9), dc_r_wide_nostr r(109)).
    for (const char* rc : {"498", "9", "109"}) {
      if (ce.code != std::string("reshape_") + rc) continue;
      // A wide r(9) leaves the data sorted the way reshape.ado's check sorted
      // them (the engine names that sort in the error's detail).
      if (direction == "wide" && ce.code == "reshape_9" && !ce.detail.empty()) {
        std::vector<std::string> sortKeys;
        std::istringstream in(ce.detail);
        for (std::string w; in >> w;) sortKeys.push_back(w);
        ojson sorted = runSort(sortKeys, session);
        if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
      }
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(") + rc + ")"));
    }
    if (ce.code == "missing_column")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(111)")));
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // j_values: reshape.py's all_suffixes / sorted unique j-values, formatted
  // as strings. The output hbin's j_var column HOLDS exactly these values
  // (long: the melted j column; wide: unaffected, so read from the ORIGINAL
  // dataset instead) — read back rather than re-deriving stub-suffix
  // discovery logic, avoiding a second bug-for-bug reimplementation.
  std::vector<std::string> jValues;
  {
    Datastore* srcForJ = (direction == "long") ? nullptr : ds;  // wide: j_var lives pre-reshape
    std::unique_ptr<Datastore> outDsHolder;
    Datastore* jDs = srcForJ;
    if (direction == "long") {
      session.load(outPath);  // adopt the result first (resident: a virtual head; stateless: the file)
      std::string jFetchPath;
      outDsHolder = fetchColumns(session, {*jVar}, session.workdir(), &jFetchPath);
      jDs = outDsHolder ? outDsHolder.get() : session.datastore();
    }
    if (jDs != nullptr && jDs->has(*jVar) && jDs->dtype(*jVar) == "factor") {
      // A string j: its distinct texts in byte order are the suffixes.
      std::set<std::string> texts;
      for (const auto& lv : jDs->varinfo(*jVar).levels)
        if (!lv.empty()) texts.insert(lv);
      jValues.assign(texts.begin(), texts.end());
    } else if (jDs != nullptr && jDs->has(*jVar) && jDs->dtype(*jVar) == "f64") {
      const double* d = jDs->col(*jVar).f64();
      int64_t count = jDs->col(*jVar).nbytes / static_cast<int64_t>(sizeof(double));
      std::vector<double> uniq;
      for (int64_t i = 0; i < count; ++i) {
        if (!std::isnan(d[i])) uniq.push_back(d[i]);
      }
      std::sort(uniq.begin(), uniq.end());
      uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
      for (double v : uniq) {
        // reshape.py's j-value formatting: integer-valued -> plain int
        // string (matches _j_suffix's convention, str(int(v)) when
        // float(v)==int(float(v))).
        if (v == static_cast<double>(static_cast<long long>(v))) {
          jValues.push_back(std::to_string(static_cast<long long>(v)));
        } else {
          std::ostringstream oss;
          oss << v;
          jValues.push_back(oss.str());
        }
      }
    }
  }

  if (direction != "long") session.load(outPath);  // BUG-394: `long` adopted above
  Datastore* outDs = session.datastore();
  int64_t nAfterRows = outDs ? outDs->nRows() : 0;
  int64_t nWide = (direction == "long") ? nBeforeRows : nAfterRows;
  int64_t nLong = (direction == "long") ? nAfterRows : nBeforeRows;
  size_t k = jValues.size();

  // The result's storage types, formats and labels (see wideStubMeta above).
  {
    auto widen = [](const std::optional<std::string>& a, const std::optional<std::string>& b)
        -> std::optional<std::string> {
      if (!a.has_value()) return b;
      if (!b.has_value()) return a;
      const int wa = strWidthOf(*a), wb = strWidthOf(*b);
      if (wa >= 0 || wb >= 0) return wa >= wb ? a : b;
      if (*a == "strL" || *b == "strL") return std::string("strL");
      return appendPromotedType(a, b);
    };
    if (direction == "long") {
      for (const auto& st : stubs) {
        std::optional<std::string> t;
        for (const auto& comp : stubComponents[st]) {
          t = widen(t, typeOf(comp));
          session.forgetVarPlanes(comp);
        }
        session.forgetVarPlanes(st);
        if (t.has_value()) session.varMeta[st].stataType = *t;
      }
      // j: the smallest type that holds it (byte 1/2, int for years), or the
      // widest suffix's str# (dc_r_long_v2 t byte, dc_r_long_year_v2 year int,
      // dc_r_long_string t str2).
      session.forgetVarPlanes(*jVar);
      if (jString) {
        // (stubComponents holds copies: the pre-reshape Datastore is gone.)
        size_t w = 1;
        for (const auto& [st, comps] : stubComponents)
          for (const auto& nm : comps) w = std::max(w, nm.size() - st.size());
        session.varMeta[*jVar].stataType = "str" + std::to_string(w);
      } else if (!jValues.empty()) {
        double lo = 0, hi = 0;
        bool integral = true, first = true;
        for (const auto& jv : jValues) {
          double v = 0;
          try { v = std::stod(jv); } catch (...) { integral = false; break; }
          if (v != std::floor(v)) integral = false;
          lo = first ? v : std::min(lo, v);
          hi = first ? v : std::max(hi, v);
          first = false;
        }
        std::string t = "float";
        if (integral) {
          if (lo >= -127 && hi <= 100) t = "byte";
          else if (lo >= -32767 && hi <= 32740) t = "int";
          else t = "long";
        }
        session.varMeta[*jVar].stataType = t;
      }
    } else {
      for (const auto& st : stubs) {
        const WideStubMeta& wm = wideStubMeta[st];
        for (const auto& jv : jValues) {
          const std::string nm = st + jv;
          session.forgetVarPlanes(nm);
          VarMeta& vm = session.varMeta[nm];
          if (wm.type.has_value()) vm.stataType = *wm.type;
          if (!wm.format.empty()) vm.format = wm.format;
          vm.label = jv + " " + st;
        }
        session.forgetVarPlanes(st);
      }
      session.forgetVarPlanes(*jVar);
    }
  }
  // reshape leaves r(N) = the result's _N (dc_r_long_r 6, dc_r_wide_r 3).
  session.eval.rValues.set("N", static_cast<double>(nAfterRows));
  ojson result = mutationResult(session, "reshape");
  result["direction"] = direction;
  result["n_wide"] = nWide;
  result["n_long"] = nLong;
  ojson jArr = ojson::array();
  for (const auto& j : jValues) jArr.push_back(j);
  result["j_values"] = jArr;
  std::ostringstream note;
  if (direction == "long") {
    note << "(" << nWide << " obs × " << k << " waves = " << nLong << " obs in long format)";
  } else {
    note << "(" << nLong << " obs → " << nWide << " obs × " << k
        << " waves in wide format)";
  }
  result["note"] = note.str();
  return result;
}
}
