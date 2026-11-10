// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data append.
#include "data_combine_internal.h"
#include "piet/handlers/data_combine.h"
#include "piet/handlers/io.h"
#include "piet/user_paths.h"
namespace piet::handlers::data_combine_detail {


// ---------------------------------------------------------------------------
// BUG-548 / BUG-549: the keep-list of `merge ..., keepusing(varlist)` and
// `append using F, keep(varlist)` is a VARLIST parsed against the USING data's
// names, not a list of literal names. Both verbs ask this one function, so they
// cannot disagree about what `mother*` means. Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2_smalls.log section E --
//     keepusing(mother*)            rc 0, k 8 -> 10  (glob)
//     keepusing(mother_a-father_a)  rc 0, k 8 -> 11  (range, using order)
//     keepusing(zzz)                r(111) "variable zzz not found"
//     keepusing(moth)               r(111) "moth ambiguous abbreviation"
//   tests/eval/golden/aer_w2/w2_data_io.log append arms --
//     keep(z s id)  rc 0, columns land in the USING file's order (s before z)
//     keep(id zz) / keep(id m)  r(111), data untouched
//     keep(i)       rc 0 -- a UNIQUE abbreviation is accepted (-> id)
//     keep(i* z) / keep(id-s)   glob and range, rc 0
// Grammar: `*` and `?` through strutil::globMatch (no second matcher); `~` is
// the unique-match wildcard (zero or several matches = r(111), the shape the
// unab measurement in loops.cpp records for `~e` on auto); `a-b` ranges in the
// using file's column order, endpoints resolved like names; `_all`; otherwise
// an exact name, then a unique abbreviation. Result: the using file's column
// order, deduplicated. On error `out` is unspecified and the block carries the
// exact rc in `reason`.

std::optional<ojson> expandUsingKeepList(const std::string& spec,
                                         const std::vector<std::string>& usingNames,
                                         std::vector<std::string>& out) {
  out.clear();
  auto fail = [](const std::string& head, const std::string& rc) {
    // Stata's message is the parse error plus a two-line explanation naming
    // keep(varlist) -- for merge's keepusing() too (w2_smalls.log:204-206).
    return ojson(eval::errorBlock(
        "engine_error",
        head + "\n    You specified the keep(varlist) option.  The above error was the result "
               "of parsing the list of variables you supplied against the using data.",
        std::nullopt, std::nullopt, std::nullopt, "r(" + rc + ")"));
  };
  // A name or unique abbreviation -> its index in usingNames.
  auto resolveName = [&](const std::string& tok, size_t& idx) -> std::optional<ojson> {
    for (size_t i = 0; i < usingNames.size(); ++i) {
      if (usingNames[i] == tok) { idx = i; return std::nullopt; }
    }
    size_t hits = 0;
    for (size_t i = 0; i < usingNames.size(); ++i) {
      if (usingNames[i].compare(0, tok.size(), tok) == 0) { idx = i; ++hits; }
    }
    if (hits == 1) return std::nullopt;
    if (hits > 1) return fail(tok + " ambiguous abbreviation", "111");
    return fail("variable " + tok + " not found", "111");
  };

  // Commas separate like spaces; the range operator may carry whitespace on
  // either side (same collapse as loops.cpp's expandVarlistImpl -- a variable
  // name cannot contain `-`, so in a varlist it is always the range operator).
  std::string joined;
  joined.reserve(spec.size());
  for (size_t i = 0; i < spec.size(); ++i) {
    char c = spec[i] == ',' ? ' ' : spec[i];
    if (c == '-') {
      while (!joined.empty() && std::isspace(static_cast<unsigned char>(joined.back())))
        joined.pop_back();
      joined.push_back('-');
      while (i + 1 < spec.size() && std::isspace(static_cast<unsigned char>(spec[i + 1]))) ++i;
      continue;
    }
    joined.push_back(c);
  }

  std::vector<char> keep(usingNames.size(), 0);
  std::istringstream iss(joined);
  std::string tok;
  bool anyTok = false;
  while (iss >> tok) {
    anyTok = true;
    if (tok == "_all") {
      std::fill(keep.begin(), keep.end(), 1);
      continue;
    }
    if (tok.find('~') != std::string::npos) {
      std::string pat = tok;
      std::replace(pat.begin(), pat.end(), '~', '*');
      size_t hits = 0, at = 0;
      for (size_t i = 0; i < usingNames.size(); ++i) {
        if (strutil::globMatch(pat, usingNames[i])) { at = i; ++hits; }
      }
      if (hits == 1) { keep[at] = 1; continue; }
      if (hits > 1) return fail(tok + " ambiguous abbreviation", "111");
      return fail("variable " + tok + " not found", "111");
    }
    if (tok.find('*') != std::string::npos || tok.find('?') != std::string::npos) {
      bool any = false;
      for (size_t i = 0; i < usingNames.size(); ++i) {
        if (strutil::globMatch(tok, usingNames[i])) { keep[i] = 1; any = true; }
      }
      if (!any) return fail("variable " + tok + " not found", "111");
      continue;
    }
    size_t dash = tok.find('-');
    if (dash != std::string::npos) {
      std::string v1 = tok.substr(0, dash), v2 = tok.substr(dash + 1);
      if (v1.empty() || v2.empty()) return fail("- invalid name", "198");
      size_t i1 = 0, i2 = 0;
      if (auto e = resolveName(v1, i1)) return e;
      if (auto e = resolveName(v2, i2)) return e;
      // DESIGN DECISION (no oracle arm): a reversed range is refused as a
      // syntax error rather than silently read backwards.
      if (i1 > i2) return fail("variable range " + tok + " is out of order", "198");
      for (size_t k = i1; k <= i2; ++k) keep[k] = 1;
      continue;
    }
    size_t idx = 0;
    if (auto e = resolveName(tok, idx)) return e;
    keep[idx] = 1;
  }
  // DESIGN DECISION (no oracle arm): an empty keep list is Stata's generic
  // "varlist required" r(100), never "keep nothing" or "keep everything".
  if (!anyTok) {
    return ojson(eval::errorBlock("engine_error", "varlist required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  }
  for (size_t i = 0; i < usingNames.size(); ++i)
    if (keep[i]) out.push_back(usingNames[i]);
  return std::nullopt;
}


bool isNumericStataType(const std::string& t) {
  return t == "byte" || t == "int" || t == "long" || t == "float" || t == "double";
}


// BUG-666: the storage type of every variable a using .dta declares, read from
// the import-dta sidecar (the one `use` reads, fileio/dataset_use.cpp applyDtaMeta). An
// unreadable sidecar gives an empty map, so its columns keep no type claim.
std::map<std::string, std::string> sidecarStataTypes(const std::string& metaPath) {
  std::map<std::string, std::string> out;
  if (metaPath.empty()) return out;
  try {
    std::ifstream mf(cecon::to_native_path(metaPath), std::ios::binary);
    std::ostringstream ms;
    ms << mf.rdbuf();
    nlohmann::json meta = nlohmann::json::parse(ms.str());
    if (!meta.contains("variables") || !meta["variables"].is_array()) return out;
    for (const auto& v : meta["variables"]) {
      if (!v.is_object()) continue;
      const std::string name = v.value("name", std::string());
      const std::string t = v.value("stata_type", std::string());
      if (!name.empty() && !t.empty()) out[name] = t;
    }
  } catch (const std::exception&) {
    out.clear();
  }
  return out;
}


// The numeric storage type a column claims: a declaration, else gen/egen's
// implicit float (BUG-295), else none.
std::optional<std::string> numericTypeClaim(const SessionState& session, const std::string& name) {
  auto it = session.varMeta.find(name);
  if (it != session.varMeta.end() && it->second.stataType.has_value())
    return *it->second.stataType;
  if (session.implicitFloatVars.count(name) != 0) return std::string("float");
  return std::nullopt;
}


// Replace a column's storage-type claim on both planes (nullopt = no claim).
// describe, `: type`, the .dta export and tostring's float/double reading all
// read it.
void setNumericTypeClaim(SessionState& session, const std::string& name,
                         const std::optional<std::string>& t) {
  session.implicitFloatVars.erase(name);
  if (t.has_value()) {
    session.varMeta[name].stataType = *t;
  } else {
    auto it = session.varMeta.find(name);
    if (it != session.varMeta.end()) it->second.stataType.reset();
  }
}


// The current dataset's (name, dtype) pairs: the resident card when it is
// current (mutationResult has just folded the verb into it), else the
// Datastore. Reading the card avoids writing a virtual head to disk.
std::vector<std::pair<std::string, std::string>> currentNameDtypes(SessionState& session) {
  std::vector<std::pair<std::string, std::string>> out;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) out.emplace_back(vi.name, vi.dtype);
    return out;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return out;
  for (const auto& nm : ds->names()) out.emplace_back(nm, ds->dtype(nm));
  return out;
}


// BUG-666: the type append leaves on a numeric variable both datasets hold.
// StataBE 19.5 (measured 2026-09-27): float+double -> double, long+double ->
// double, long+float -> double (either way round), byte+int -> int, each with
// "(variable x was float, now double to accommodate using data's values)".
// Within byte<int<long or byte<int<float the wider one wins. A side with no
// recorded type gives no claim unless the other is double; runAppend then keeps
// that double only when the master rows are float-exact (below).
std::optional<std::string> appendPromotedType(const std::optional<std::string>& m,
                                              const std::optional<std::string>& u) {
  if (m == u) return m;
  if (m == std::string("double") || u == std::string("double")) return std::string("double");
  if (!m.has_value() || !u.has_value()) return std::nullopt;
  if ((*m == "long" && *u == "float") || (*m == "float" && *u == "long"))
    return std::string("double");
  auto rank = [](const std::string& t) {
    return t == "byte" ? 0 : t == "int" ? 1 : 2;  // long and float both rank 2
  };
  return rank(*m) >= rank(*u) ? m : u;
}


// merge.ado result(): a result word in keep()/assert() is a digit
// 1-5 or a prefix of masters (3+ letters), usings (2+), matches / matched
// (3+), match_updates (8+) or match_conflicts (8+). Anything else is
// `xyz:  invalid resulttype` r(198) (w2aw_data_combine_family.log dc_mk_bad,
// dc_ma_bad); `keep(mas mat)` is keep(1 3) (dc_mk_mas_mat).
std::optional<int> mergeResultWord(const std::string& w) {
  if (w.size() == 1 && w[0] >= '1' && w[0] <= '5') return w[0] - '0';
  auto prefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return w.size() >= minLen && w.size() <= f.size() && f.compare(0, w.size(), w) == 0;
  };
  if (prefixOf("masters", 3)) return 1;
  if (prefixOf("usings", 2)) return 2;
  if (prefixOf("matches", 3) || prefixOf("matched", 3)) return 3;
  if (prefixOf("match_updates", 8)) return 4;
  if (prefixOf("match_conflicts", 8)) return 5;
  return std::nullopt;
}


std::optional<ojson> parseMergeResults(const std::string& spec, std::vector<int>& out) {
  out.clear();
  std::istringstream iss(spec);
  for (std::string w; iss >> w;) {
    const std::optional<int> c = mergeResultWord(w);
    if (!c.has_value()) {
      return ojson(eval::errorBlock(
          "syntax_error",
          w + ":  invalid resulttype\n    results, specified in options assert() and keep(), are "
              "the integers 1 through 5, or master (equivalent to 1), using (2), match (3), "
              "match_update (4), or match_conflict (5).",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
    }
    if (std::find(out.begin(), out.end(), *c) == out.end()) out.push_back(*c);
  }
  std::sort(out.begin(), out.end());
  return std::nullopt;
}


// str# width, or -1 (strL / not a str# spelling).
int strWidthOf(const std::string& t) {
  if (t.size() <= 3 || t.compare(0, 3, "str") != 0) return -1;
  const std::string d = t.substr(3);
  if (d.empty() || !std::all_of(d.begin(), d.end(), [](unsigned char c) { return std::isdigit(c); }))
    return -1;
  return std::stoi(d);
}


// merge/append bring the using file's notes along: each variable's (and
// _dta's) using notes are numbered on after the master's, and note0 counts
// them all (w2aw_data_combine_family.log dc_ml_notes_n / dc_a_notes_n:
// `[2]|[master note]|[u note]|...|[2]|[x master note]|[x u note]|`). The using
// char table is the .dta sidecar's, or the one a native checkpoint this
// session saved recorded beside its path. A variable the result does not have
// takes none.
void appendUsingNotes(SessionState& session, const std::string& usingPath,
                      const std::string& usingMetaPath) {
  std::vector<DtaCharacteristic> uChars;
  if (!usingMetaPath.empty()) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
        for (const auto& ch : meta["characteristics"]) {
          if (!ch.is_object() || !ch.contains("value") || !ch["value"].is_string()) continue;
          uChars.push_back(DtaCharacteristic{ch.value("varname", std::string()),
                                             ch.value("charname", std::string()),
                                             ch["value"].get<std::string>()});
        }
      }
    } catch (const std::exception&) {
      return;
    }
  } else if (const SessionState::CheckpointTsDecl* ck = session.checkpointTsDecl(usingPath)) {
    uChars = ck->characteristics;
  }
  if (uChars.empty()) return;
  const auto charOf = [](const std::vector<DtaCharacteristic>& tbl, const std::string& v,
                         const std::string& c) -> const std::string* {
    for (const auto& x : tbl)
      if (x.varname == v && x.charname == c) return &x.value;
    return nullptr;
  };
  std::set<std::string> resultVars;
  for (const auto& [nm, dt] : currentNameDtypes(session)) {
    (void)dt;
    resultVars.insert(nm);
  }
  std::vector<std::string> owners;
  for (const auto& c : uChars)
    if (c.charname == "note0" && std::find(owners.begin(), owners.end(), c.varname) == owners.end())
      owners.push_back(c.varname);
  for (const auto& v : owners) {
    if (v != "_dta" && resultVars.count(v) == 0) continue;
    int nu = 0;
    try {
      nu = std::stoi(*charOf(uChars, v, "note0"));
    } catch (const std::exception&) {
      continue;
    }
    if (nu <= 0) continue;
    int nm = 0;
    if (const std::string* m0 = charOf(session.characteristics, v, "note0")) {
      try {
        nm = std::stoi(*m0);
      } catch (const std::exception&) {
        nm = 0;
      }
    }
    const auto setChar = [&session](const std::string& var, const std::string& name,
                                    const std::string& value) {
      for (auto& x : session.characteristics)
        if (x.varname == var && x.charname == name) {
          x.value = value;
          return;
        }
      session.characteristics.push_back(DtaCharacteristic{var, name, value});
    };
    int added = 0;
    for (int k = 1; k <= nu; ++k) {
      const std::string* t = charOf(uChars, v, "note" + std::to_string(k));
      if (t == nullptr) continue;
      ++added;
      setChar(v, "note" + std::to_string(nm + added), *t);
    }
    if (added > 0) setChar(v, "note0", std::to_string(nm + added));
  }
}


DupKeyResult findDupKey(Datastore& ds, const std::vector<std::string>& keylist) {
  int64_t n = ds.nRows();
  // Build per-row composite key as a joined string (safe delimiter unlikely
  // to collide with real data — good enough for O(n) duplicate detection).
  std::vector<std::vector<double>> f64Cols;
  std::vector<std::vector<int32_t>> facCols;
  std::vector<bool> isFactor;
  for (const auto& k : keylist) {
    if (ds.dtype(k) == "f64") {
      const double* d = ds.col(k).f64();
      isFactor.push_back(false);
      f64Cols.emplace_back(d, d + n);
      facCols.emplace_back();
    } else {
      const int32_t* c = ds.col(k).codes();
      isFactor.push_back(true);
      facCols.emplace_back(c, c + n);
      f64Cols.emplace_back();
    }
  }
  std::map<std::string, int64_t> seen;  // composite key -> first row index
  for (int64_t row = 0; row < n; ++row) {
    std::ostringstream keyOss;
    for (size_t ki = 0; ki < keylist.size(); ++ki) {
      if (ki) keyOss << "\x1f";
      if (isFactor[ki]) {
        keyOss << "f:" << facCols[ki][static_cast<size_t>(row)];
      } else {
        double v = f64Cols[ki][static_cast<size_t>(row)];
        if (std::isnan(v)) keyOss << "n:nan";
        // eval::pythonFloatRepr, not lossy default ostream<<double (~6 sig
        // digits) — distinct high-precision keys (e.g. 1005938 vs 1005942)
        // would otherwise collide into the same composite-key string.
        else keyOss << "n:" << eval::pythonFloatRepr(v);
      }
    }
    std::string composite = keyOss.str();
    auto it = seen.find(composite);
    if (it != seen.end()) {
      DupKeyResult r;
      r.found = true;
      int64_t dupRow = row;
      for (size_t ki = 0; ki < keylist.size(); ++ki) {
        std::ostringstream vs;
        if (isFactor[ki]) {
          const auto& levels = ds.varinfo(keylist[ki]).levels;
          int32_t code = facCols[ki][static_cast<size_t>(dupRow)];
          vs << ((code >= 0 && static_cast<size_t>(code) < levels.size()) ? levels[static_cast<size_t>(code)]
                                                                            : std::to_string(code));
        } else {
          double v = f64Cols[ki][static_cast<size_t>(dupRow)];
          // Python's DataFrame cell repr for a float key: "1.0" not "1" —
          // f64 columns are always floats in pandas, formatted via str().
          if (v == static_cast<double>(static_cast<long long>(v))) {
            vs << static_cast<long long>(v) << ".0";
          } else {
            vs << v;
          }
        }
        r.keyValueStrs.push_back(vs.str());
      }
      return r;
    }
    seen[composite] = row;
  }
  return DupKeyResult{};
}
}
namespace piet::handlers {
using namespace data_combine_detail;


std::string dv_detail::resolveUsingDataset(SessionState& session, const std::string& rawPath,
                                           std::string* shown) {
  std::string path = resolveUserPath(session, rawPath);
  session.materializeVirtualSave(path);  // BUG-125: a save_virtual'd tempfile becomes real on demand
  if (extOf(path).empty() && !fileExists(path)) {
    path += ".dta";
    if (shown != nullptr) *shown += ".dta";
    session.materializeVirtualSave(path);
  }
  return path;
}


void dv_detail::adoptUsingValueLabels(SessionState& session, const std::string& usingPath,
                                      const std::string& usingMetaPath,
                                      const std::vector<std::string>& brought,
                                      bool copyDefinitions) {
  // The using side's attachments and definitions: a .dta's import-dta sidecar,
  // or the record a native checkpoint this session saved keeps beside its path.
  std::map<std::string, std::string> uAttach;
  std::map<std::string, std::map<std::string, std::string>> uDefs;
  if (!usingMetaPath.empty()) {
    nlohmann::json meta;
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      meta = nlohmann::json::parse(ms.str());
    } catch (const std::exception&) {
      meta = nlohmann::json();
    }
    if (meta.contains("variables") && meta["variables"].is_array()) {
      for (const auto& v : meta["variables"]) {
        if (!v.is_object()) continue;
        const std::string vl = v.value("value_label_name", std::string());
        if (!vl.empty()) uAttach[v.value("name", std::string())] = vl;
      }
    }
    if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
      for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
        if (!it.value().is_object()) continue;
        std::map<std::string, std::string>& codeMap = uDefs[it.key()];
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit)
          if (cit.value().is_string()) codeMap[cit.key()] = cit.value().get<std::string>();
      }
    }
  } else if (const SessionState::CheckpointTsDecl* ck = session.checkpointTsDecl(usingPath)) {
    uAttach = ck->varLblname;
    uDefs = ck->labelDefs;
  } else {
    return;
  }
  for (const auto& nm : brought) {
    auto a = uAttach.find(nm);
    if (a != uAttach.end()) session.varLblname[nm] = a->second;
  }
  if (copyDefinitions) {
    for (const auto& [name, codeMap] : uDefs)
      if (!session.labelDefs.count(name)) session.labelDefs[name] = codeMap;
  }
  // Every attachment now reads through its (possibly just copied) set.
  for (const auto& [varname, lname] : session.varLblname) {
    auto d = session.labelDefs.find(lname);
    if (d == session.labelDefs.end()) continue;
    std::map<int, std::string> numericMap;
    for (const auto& [code, text] : d->second) {
      try {
        numericMap[std::stoi(code)] = text;
      } catch (const std::exception&) {
      }
    }
    session.valueLabels[varname] = numericMap;
  }
}


// What a using file states about one of its variables (dv_detail::UsingVarMeta):
// a .dta states all three in the import-dta sidecar; a native checkpoint
// (tempfile) states the type beside its path (BUG-922) and the label/format
// either beside its path (a virtual save) or in its header.
std::map<std::string, UsingVarMeta> dv_detail::usingVarMetaOf(SessionState& session, const std::string& usingPath,
                                                   const std::string& usingMetaPath,
                                                   Datastore& usingDs) {
  std::map<std::string, UsingVarMeta> out;
  if (!usingMetaPath.empty()) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("variables") && meta["variables"].is_array()) {
        for (const auto& v : meta["variables"]) {
          if (!v.is_object()) continue;
          const std::string name = v.value("name", std::string());
          if (name.empty()) continue;
          UsingVarMeta& u = out[name];
          u.label = v.value("label", std::string());
          u.format = v.value("format", std::string());
          const std::string t = v.value("stata_type", std::string());
          if (!t.empty()) u.stataType = t;
        }
      }
    } catch (const std::exception&) {
      out.clear();
    }
    return out;
  }
  for (const auto& nm : usingDs.names()) {
    UsingVarMeta& u = out[nm];
    const cecon::Column& c = usingDs.col(nm);
    u.label = c.label;
    u.format = c.format;
  }
  if (const auto* vm = session.virtualSaveMetaOf(usingPath)) {
    for (const cecon::ColMetaPatch& p : *vm) {
      auto it = out.find(p.name);
      if (it == out.end()) continue;
      if (p.label.has_value()) it->second.label = *p.label;
      if (p.format.has_value() && !p.format->empty()) it->second.format = *p.format;
    }
  }
  for (const auto& [nm, t] : session.checkpointStataTypes(usingPath)) {
    auto it = out.find(nm);
    if (it != out.end() && !t.empty()) it->second.stataType = t;
  }
  return out;
}



// ---------------------------------------------------------------------------
// append using FILE — C++ port of append.py's run_append. cobra 0.18.0's
// native `append` verb accepts .hbin/.dta/.csv directly on --using (no
// manual pre-conversion needed, verified empirically) and already
// implements the f64/factor dtype-promotion + missing-column padding the
// oracle documents (contract §22) — this only resolves the path against
// session.cwd and pre-checks existence for the oracle's exact error text.
ojson runAppend(const std::string& filepathArg, SessionState& session,
                 const std::optional<std::string>& genVar,
                 const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                 bool nonotes, std::vector<std::string>* typeNotes) {
  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  std::string shown = filepathArg;
  std::string filepath = resolveUsingDataset(session, filepathArg, &shown);
  if (!fileExists(filepath)) {
    return engineErrorBlock("append: file not found: " + filepath);
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  int64_t nMaster = ds->nRows();
  const std::vector<std::string> masterNames = ds->names();
  // append.ado picks generate()'s label name before the append reads anything, so
  // a using file's own `_append` definition cannot change it (BUG-551, below).
  const std::string genLbl = session.labelDefs.count("_append") ? "__append" : "_append";

  // B5-D10: generate(NAME) validated BEFORE the append runs (same ordering
  // destring/tostring use for their gen() targets), so a bad name never
  // leaves a half-appended dataset.
  if (genVar.has_value()) {
    // append.ado `confirm new var` (after every file was described): r(110)
    // (w2aw_data_combine_family.log dc_a_gen_exists).
    if (ds->has(*genVar)) {
      return ojson(eval::errorBlock("engine_error", "variable " + *genVar + " already defined",
                                    std::nullopt, std::nullopt, std::nullopt, "r(110)"));
    }
    if (!strutil::isValidStataIdentifier(*genVar)) {
      return engineErrorBlock("append: invalid variable name `" + *genVar + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }

  // BUG-549: keep(varlist) -- expand against the USING file's names, then hand the
  // engine the exact list (`--keepusing`, cmd_append projects the using frame
  // before the concat). The names come from the hbin header when the using file is
  // one; a .dta/.csv is converted ONCE (the same import-dta/convert merge uses for
  // its pre-checks) and the engine then appends from that converted file, so the
  // using data is still read once, not once for names and again for rows.
  // BUG-551: a .dta using is always converted here (the engine would convert it
  // anyway, so it is still read once), with the same --out-meta sidecar
  // `use` reads (it also names the using side's storage types for BUG-550's r(106)
  // message). A .csv is converted here too, so a file the converter refuses
  // (BUG-414) is answered with append's own code; a .hbin goes to the engine as is.
  std::string engineUsing = filepath;
  std::string keepCsv;
  std::string usingMetaPath;
  std::optional<std::set<std::string>> usingKept;  // keep()'s using names (BUG-666)
  const std::string ext = extOf(filepath);
  if (ext == ".dta" || usingFileIsDta(filepath)) {
    std::string conv = nextPath(session.workdir(), "append_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389
    CobraRunResult ccr = runCobra({"import-dta", filepath, conv, "--out-meta", usingMetaPath});
    if (!ccr.ok) {
      CobraError ce = errorFromRaw(ccr);
      return engineErrorBlock("append using: cannot convert '" + filepath + "': " + ce.what());
    }
    engineUsing = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "append_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult ccr = runCobra(
        {"convert", "--data", filepath, "--out", conv, "--delim", std::string(1, delim)});
    if (!ccr.ok) return delimitedUsingRefused(shown, 601);
    engineUsing = conv;
  }
  if (keepSpec.has_value()) {
    std::vector<std::string> usingNames;
    try {
      Datastore usingDs(engineUsing);  // header parse only -- the names, no column payload
      usingNames = usingDs.names();
    } catch (const std::exception& e) {
      return engineErrorBlock("append using: cannot read '" + filepath + "': " + e.what());
    }
    std::vector<std::string> kept;
    if (auto err = expandUsingKeepList(*keepSpec, usingNames, kept)) return *err;
    for (size_t i = 0; i < kept.size(); ++i) {
      if (i) keepCsv += ",";
      keepCsv += kept[i];
    }
    usingKept.emplace(kept.begin(), kept.end());
  }

  // BUG-666: the master rows of a shared numeric column decide the storage
  // claim it keeps after the append (below). They are read here, from the
  // pre-append Datastore `ds` (already a file on disk: its path is what the
  // engine is handed), and not from the post-append head: on the resident
  // plane session.datastore() would first materialize that head, writing a
  // snapshot the append otherwise avoids. Only the columns whose claim can
  // depend on their values are scanned: in both files, numeric in the using
  // file's sidecar, and without a master double/long claim.
  struct MasterRows {
    bool hasFraction = false, floatExact = true;
  };
  // BUG-922: a native checkpoint (tempfile) names its types beside its path.
  const std::map<std::string, std::string> uTypes =
      usingMetaPath.empty() ? session.checkpointStataTypes(filepath)
                            : sidecarStataTypes(usingMetaPath);
  std::map<std::string, MasterRows> masterRows;
  for (const auto& nm : masterNames) {
    auto uit = uTypes.find(nm);
    if (uit == uTypes.end() || !isNumericStataType(uit->second)) continue;
    if (usingKept && usingKept->count(nm) == 0) continue;
    if (ds->dtype(nm) != "f64") continue;
    const std::optional<std::string> m = numericTypeClaim(session, nm);
    if (m == std::string("double") || m == std::string("long")) continue;
    if (m.has_value() && !isNumericStataType(*m)) continue;
    MasterRows out;
    const double* v = ds->col(nm).f64();
    if (v == nullptr) {
      out.hasFraction = true;  // unreadable: no row is known to be exact
      out.floatExact = false;
    } else {
      for (int64_t r = 0; r < nMaster; ++r) {
        if (std::isnan(v[r])) continue;
        if (v[r] != std::trunc(v[r])) out.hasFraction = true;
        if (std::fabs(v[r]) > std::numeric_limits<float>::max() ||
            static_cast<double>(static_cast<float>(v[r])) != v[r])
          out.floatExact = false;
      }
    }
    masterRows.emplace(nm, out);
  }

  // BUG-666: append's float->double widening. A master column Stata holds as
  // float (an undeclared gen/egen, a declared float, or a byte/int a `replace`
  // made float) that the using file's type promotes to double keeps its float
  // values: StataBE widens float(v), it does not recompute v. PIET keeps gen
  // values unrounded, so those master rows are rounded to float here, before
  // the engine appends, and the column then takes an ordinary double claim
  // below. tests/eval/golden/aer_w2/w2f_residuals.log:1824 f_append_types_ctl:
  // master `generate x = 0.07*100` / `generate y = 123456789`, using double x
  // 0.1 / long y 123456789 -> x and y double, x[1] exactly 7, y[1] 123456792,
  // y[3] 123456789; :1856/:1898 tostring then stores "7"/".1" and
  // "123456792"/"123456789". The rounding is a new --data for the append, so a
  // refused append leaves the master as it was. A column the setcol payload
  // cannot carry (an extended missing .a-.z, which it would collapse to `.`, or
  // a value beyond float's range) is left unrounded and keeps the old rule
  // below: no claim unless its rows are float-exact.
  std::string masterData = ds->path();
  for (auto& [nm, rows] : masterRows) {
    if (rows.floatExact) continue;
    const std::optional<std::string> m = numericTypeClaim(session, nm);
    std::optional<std::string> mEff = m;
    if ((m == std::string("byte") || m == std::string("int")) && rows.hasFraction)
      mEff = std::string("float");
    if (mEff != std::string("float")) continue;
    if (appendPromotedType(mEff, uTypes.at(nm)) != std::string("double")) continue;
    const double* v = ds->col(nm).f64();
    if (v == nullptr) continue;
    bool carriable = true;
    for (int64_t r = 0; r < nMaster && carriable; ++r) {
      if (std::isnan(v[r]))
        carriable = cecon::missing_kind(v[r]) == cecon::kMissSys;
      else if (std::fabs(v[r]) > std::numeric_limits<float>::max())
        carriable = false;
    }
    if (!carriable) continue;
    std::string valuesPath = nextPath(session.workdir(), "append_float") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int64_t r = 0; r < nMaster; ++r) {
        if (!std::isnan(v[r]))
          csv << eval::pythonFloatRepr(static_cast<double>(static_cast<float>(v[r])));
        csv << "\n";  // missing -> empty line (system missing only, checked above)
      }
      // BUG-124 sibling: flush + check — silent short payload = silent row loss.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    // The format and label the column carries in the file: setcol replaces the
    // column whole, so they are kept on the session plane first.
    const cecon::Column& col = ds->col(nm);
    VarMeta& vm = session.varMeta[nm];
    if (!col.format.empty() && !vm.format.has_value()) vm.format = col.format;
    if (!col.label.empty() && !vm.label.has_value()) vm.label = col.label;
    const std::string roundedPath = nextPath(session.workdir(), "append_float");
    CobraRunResult rcr = runCobra({"setcol", "--data", masterData, "--var", nm, "--values",
                                   valuesPath, "--dtype", "f64", "--out", roundedPath});
    cecon::native_unlink(valuesPath);
    if (!rcr.ok) {
      CobraError ce = errorFromRaw(rcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    masterData = roundedPath;
    rows.floatExact = true;
  }

  // What the using file states about its variables (label, format, str#),
  // read while it is a file on disk (DC5: dc_a_base_v7 onlyu `str3|[only in u]`).
  std::map<std::string, UsingVarMeta> uMeta;
  std::vector<std::string> usingOrder;  // the using file's variable order (BUG-1092)
  if (!usingMetaPath.empty() || extOf(engineUsing) != ".csv") {
    try {
      Datastore usingDs(engineUsing);
      uMeta = usingVarMetaOf(session, filepath, usingMetaPath, usingDs);
      usingOrder = usingDs.names();
    } catch (const std::exception&) {
      uMeta.clear();  // the engine's own read below answers for the file
    }
  }

  std::string outPath = nextPath(session.workdir(), "append");
  std::vector<std::string> args = {"append", "--data", masterData, "--using", engineUsing, "--out",
                                    outPath};
  if (keepSpec.has_value()) {
    args.push_back("--keepusing");
    args.push_back(keepCsv);
  }
  // BUG-550: Stata's type rule, not the engine's promote-both-to-f64 default (which
  // turned the using side's string codes into numbers at rc 0). w2_data_io.log
  // app15: numeric s in master, str3 s in using -> r(106); app16/app17 under force:
  // s = . on the using rows.
  args.push_back("--type-clash");
  args.push_back(force ? "force" : "error");
  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane — append was the largest of the
    // eight verbs still writing a whole --out file per call. Its only stateless
    // dependency was the stderr notice PIET parsed for n_using; on the resident leg
    // the count is exact arithmetic (append's result has nm+nu rows), so it is read
    // off the resident row count instead. The stderr parse stays as the stateless
    // fallback, exactly like merge's counts (854fcbed).
    SessionState::ResidentEligible residentAppend(session);
    cr = runCobra(args);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    if (ce.detail.rfind("append_type_clash:", 0) == 0) {
      // Stata's two-line explanation (app15: "variable s is float in master but str3
      // in using data"). The engine carries no Stata storage type, so it says
      // numeric/string; the storage types are filled in here where they are known:
      // the master's from varMeta (else `float`, describe's fallback for an
      // undeclared numeric), the using side's from the .dta sidecar.
      const std::string nm = ce.detail.substr(std::string("append_type_clash:").size());
      const bool masterStr =
          std::string(ce.what()).find(" is string in master") != std::string::npos;
      std::string mType = masterStr ? "string" : "float";
      auto mIt = session.varMeta.find(nm);
      if (mIt != session.varMeta.end() && mIt->second.stataType.has_value())
        mType = *mIt->second.stataType;
      std::string uType = masterStr ? "numeric" : "string";
      if (!usingMetaPath.empty()) {
        try {
          std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
          std::ostringstream ms;
          ms << mf.rdbuf();
          nlohmann::json meta = nlohmann::json::parse(ms.str());
          if (meta.contains("variables") && meta["variables"].is_array())
            for (const auto& v : meta["variables"])
              if (v.is_object() && v.value("name", std::string()) == nm &&
                  !v.value("stata_type", std::string()).empty())
                uType = v.value("stata_type", std::string());
        } catch (const std::exception&) {
          // keep the generic word: the refusal itself does not depend on the sidecar
        }
      }
      std::string msg = "variable " + nm + " is " + mType + " in master but " + uType +
                        " in using data";
      if (!masterStr)
        msg += "\n    You could specify append's force option to ignore this numeric/string "
               "mismatch.  The using variable would then be treated as if\n    it contained "
               "numeric missing value.";
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                    "r(106)"));
    }
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  ojson result = mutationResult(session, "append");
  // BUG-666: storage types, which tostring's float/double reading, describe,
  // `: type` and the .dta export all read. A variable only the using file has
  // takes the using file's type (before, it had none, and a stale claim left
  // under its name by a renamed or dropped column could stand in). One both
  // hold takes append's promotion of the two. A using file with no .dta sidecar
  // names no types: its new columns get no claim and shared ones keep the
  // master's.
  {
    const std::set<std::string> inMaster(masterNames.begin(), masterNames.end());
    // The master rows of a shared column, scanned before the append (above).
    // A column missing from that scan was not a candidate; it is read as not
    // float-exact, the safe side (no double claim).
    auto scanMasterRows = [&](const std::string& nm) {
      auto it = masterRows.find(nm);
      if (it != masterRows.end()) return it->second;
      MasterRows out;
      out.hasFraction = true;
      out.floatExact = false;
      return out;
    };
    // BUG-1092 (FU-421): StataBE names each master variable the using data
    // widen (measured 2026-11-03: float+double, float+long and byte+double give
    // double, byte+int gives int, str2+str5 gives str5; a double master taking a
    // float using is silent), in the using file's variable order (using y s x
    // onto master x s y prints y, s, x). "was" is the type `: type` answered
    // before.
    std::vector<std::pair<std::string, std::string>> widened;
    auto noteWidening = [&](const std::string& nm, const std::string& was,
                            const std::string& now) {
      if (typeNotes != nullptr && was != now)
        widened.emplace_back(nm, "(variable " + nm + " was " + was + ", now " + now +
                                     " to accommodate using data's values)");
    };
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      auto uit = uTypes.find(nm);
      const bool usingHas = uit != uTypes.end() && (!usingKept || usingKept->count(nm) != 0);
      std::optional<std::string> u;
      if (usingHas && isNumericStataType(uit->second)) u = uit->second;
      const std::optional<std::string> m = numericTypeClaim(session, nm);
      auto umIt = uMeta.find(nm);
      const UsingVarMeta* um = umIt == uMeta.end() ? nullptr : &umIt->second;
      if (!inMaster.count(nm)) {
        // format-display-default-lifecycle: and the format the using file
        // states for it (merge's rule, af_merge).
        session.adoptVirtualSaveFormats(filepath, {nm});
        if (dtype == "f64") {
          setNumericTypeClaim(session, nm, u);
        } else {
          if (m.has_value() && isNumericStataType(*m))
            setNumericTypeClaim(session, nm, std::nullopt);  // stale numeric claim on a string
          if (um != nullptr && um->stataType.has_value() && !isNumericStataType(*um->stataType))
            session.varMeta[nm].stataType = *um->stataType;
        }
        // The using file's variable label and format come along (dc_a_base_v7).
        if (um != nullptr && !um->label.empty()) session.setVarLabel(nm, um->label);
        if (um != nullptr && !um->format.empty()) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = um->format;
        }
        continue;
      }
      // A string both files hold widens to the wider str# (dc_a_base_v3 s str5
      // from str2 + str5).
      if (dtype == "factor" && usingHas && um != nullptr && um->stataType.has_value() &&
          m.has_value()) {
        const int mw = strWidthOf(*m), uw = strWidthOf(*um->stataType);
        if (*um->stataType == "strL" && *m != "strL")
          session.varMeta[nm].stataType = std::string("strL");
        else if (mw >= 0 && uw > mw)
          session.varMeta[nm].stataType = *um->stataType;
        noteWidening(nm, *m, session.varMeta[nm].stataType.value_or(*m));
        continue;
      }
      if (!u.has_value() || dtype != "f64") continue;
      if (m.has_value() && !isNumericStataType(*m)) continue;
      // A master byte/int holding a fraction was made float by a `replace`
      // PIET does not record (runTostring reads it as float the same way).
      // StataBE then promotes that float like any other.
      const MasterRows rows = scanMasterRows(nm);
      std::optional<std::string> mEff = m;
      if ((m == std::string("byte") || m == std::string("int")) && rows.hasFraction)
        mEff = std::string("float");
      std::optional<std::string> promoted = appendPromotedType(mEff, u);
      // Promotion to double turns a float master row into double(float(v)).
      // A float master's rows were rounded to float before the append (above),
      // so they are float-exact now. A master with NO claim may be float in
      // Stata too (import delimited, collapse outputs, clonevar, a renamed
      // gen): its rows are not rounded, since its Stata type is not known. It
      // takes the double claim only when its rows are float-exact, and read
      // the same either way; otherwise it keeps no claim, and tostring reads it
      // both ways and refuses by name when they disagree. The same holds for a
      // float master the rounding could not carry (extended missing). A master
      // long (or a long holding a fraction, i.e. a double) is exact.
      if (promoted == std::string("double") && mEff != std::string("double") &&
          mEff != std::string("long")) {
        if (!rows.floatExact) promoted = std::nullopt;
      }
      if (promoted != m) {
        // The variable keeps the display format it was created with (see
        // stataDefaultFormat), which tostring's usedisplayformat reads: record
        // the master's creation-time default before the claim changes. The
        // declared type, not mEff: a byte a `replace` made float keeps %8.0g.
        VarMeta& vm = session.varMeta[nm];
        if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
        setNumericTypeClaim(session, nm, promoted);
        noteWidening(nm, mEff.value_or("float"), promoted.value_or("float"));
      }
    }
    auto usingPos = [&usingOrder](const std::string& nm) {
      return std::find(usingOrder.begin(), usingOrder.end(), nm) - usingOrder.begin();
    };
    std::stable_sort(widened.begin(), widened.end(), [&](const auto& a, const auto& b) {
      return usingPos(a.first) < usingPos(b.first);
    });
    for (auto& w : widened) typeNotes->push_back(std::move(w.second));
  }
  int64_t nUsing = 0;
  {
    // Stateless leg: the one-shot stderr notice. Resident leg: no notice — the
    // count is exact arithmetic off the row count the card build just published
    // (append's result has nm+nu rows), read from the same block the user sees.
    static const std::regex reCounts(R"(\((\d+)\s+master\s*\+\s*(\d+)\s+using\s*=)");
    std::smatch m;
    if (std::regex_search(cr.stderr_text, m, reCounts)) {
      try {
        nUsing = std::stoll(m.str(2));
      } catch (...) {
      }
    } else if (result.contains("n_rows_after") && result["n_rows_after"].is_number_integer()) {
      const int64_t after = result["n_rows_after"].get<int64_t>();
      if (after >= nMaster) nUsing = after - nMaster;
    }
  }
  result["n_master"] = nMaster;
  result["n_using"] = nUsing;

  // BUG-551: copy the using file's value-label DEFINITIONS; a definition already
  // in memory is never replaced (w2_data_io.log app1..app10/app13: alab, defined
  // only in the using file, is defined after the append -- also under keep() that
  // drops its variable; app11/app12: not under nolabel). A shared variable's
  // attachment is not copied: the master's `a` stays unlabelled (app0 lists 10,
  // not "ten").
  if (!nolabel && !usingMetaPath.empty()) {
    nlohmann::json meta;
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      meta = nlohmann::json::parse(ms.str());
    } catch (const std::exception& e) {
      return engineErrorBlock("append using: cannot read the value labels of '" + filepath +
                              "': " + e.what());
    }
    if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
      for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
        if (session.labelDefs.count(it.key()) || !it.value().is_object()) continue;
        if (genVar.has_value() && it.key() == genLbl) continue;  // defined "before" the append
        std::map<std::string, std::string> codeMap;
        std::map<int, std::string> numericMap;
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit) {
          if (!cit.value().is_string()) continue;
          codeMap[cit.key()] = cit.value().get<std::string>();
          try {
            numericMap[std::stoi(cit.key())] = cit.value().get<std::string>();
          } catch (const std::exception&) {
          }
        }
        session.labelDefs[it.key()] = codeMap;
        // A master variable already pointing at this (so far undefined) name now
        // displays through it, as `label values` before `label define` does.
        for (const auto& [varname, lname] : session.varLblname)
          if (lname == it.key()) session.valueLabels[varname] = numericMap;
      }
    }
  }
  // ...while a variable only the using file has keeps the set it is attached to
  // there (BUG-1157: `append using` of a file whose u carries ul lists u
  // through ul, StataBE, as merge's brought-in variables do). A native
  // checkpoint states its definitions beside its path, so they come here too.
  {
    const std::set<std::string> inMaster(masterNames.begin(), masterNames.end());
    std::vector<std::string> brought;
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      (void)dtype;
      if (!inMaster.count(nm)) brought.push_back(nm);
    }
    adoptUsingValueLabels(session, filepath, usingMetaPath, brought,
                          !nolabel && usingMetaPath.empty());
  }

  if (!nonotes) appendUsingNotes(session, filepath, usingMetaPath);

  if (genVar.has_value()) {
    // B5-D10 (measured B4REC|append|generate|0|, tests/eval/golden/aer_b4/
    // pins_oracle_r.log:96-97): src marks which side each row came from —
    // 0 for the nMaster original master rows, 1 for the nUsing rows appended
    // from `using`. Same setcol --new mechanism winsor uses for its genName
    // column (winsor.cpp), applied to the head the append above
    // just produced.
    std::string valuesPath = nextPath(session.workdir(), "append_gen") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int64_t r = 0; r < nMaster; ++r) csv << "0\n";
      for (int64_t r = 0; r < nUsing; ++r) csv << "1\n";
      // BUG-124 sibling: flush + check — silent short payload = silent row loss.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::string genOutPath = nextPath(session.workdir(), "append_gen");
    CobraRunResult gcr = runCobra({"setcol", "--data", *session.currentHbin(), "--var", *genVar,
                                    "--values", valuesPath, "--dtype", "f64", "--new", "--out",
                                    genOutPath});
    if (!gcr.ok) {
      CobraError ce = errorFromRaw(gcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(genOutPath);
    // BUG-551: Stata's append.ado creates the marker BEFORE the append (`gen byte
    // NAME = 0`), so it sits right after the master's variables (w2_data_io.log
    // app8: id a m src s z), is a byte labelled "Dataset source", and carries the
    // value label `_append` (or `__append` when `_append` is taken) with 0
    // "Master" and 1 "Appended dataset 1" (app8/app9/app10 list those texts).
    Datastore* after = session.datastore();
    if (after != nullptr && !masterNames.empty() &&
        after->names().size() > masterNames.size() + 1) {
      std::string ordOut = nextPath(session.workdir(), "append_gen_order");
      CobraRunResult ocr = runCobra({"order", "--data", *session.currentHbin(), "--vars", *genVar,
                                     "--after", masterNames.back(), "--out", ordOut});
      if (!ocr.ok) {
        CobraError ce = errorFromRaw(ocr);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(ordOut);
    }
    session.labelDefs[genLbl] = {{"0", "Master"}, {"1", "Appended dataset 1"}};
    session.varLblname[*genVar] = genLbl;
    session.valueLabels[*genVar] = {{0, "Master"}, {1, "Appended dataset 1"}};
    session.varMeta[*genVar].stataType = "byte";
    // Attaching the value label widens the display to its longest text,
    // "Appended dataset 1" (dc_a_gen_v7 `byte|%18.0g`).
    session.varMeta[*genVar].format = std::string("%18.0g");
    session.setVarLabel(*genVar, "Dataset source");
    result = mutationResult(session, "append");
    result["n_master"] = nMaster;
    result["n_using"] = nUsing;
  }
  return result;
}


// ---------------------------------------------------------------------------
// append using FILE [FILE ...] — append.ado's own loop over runAppend: every
// file is described first (`file X not found` r(601) before anything moves;
// the last description leaves r(N) = that file's _N, dc_a_base_r 2), then
// generate() is created, then each file is appended in turn with the marker
// set to its number k and labelled "Appended dataset k" (dc_a_two,
// dc_a_gen_two). Appending into an empty dataset (after `clear`) is the using
// data itself (dc_a_empty; with generate() the marker is the first variable).
ojson runAppendFiles(const std::vector<std::string>& files, SessionState& session,
                     const std::optional<std::string>& genVar,
                     const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                     bool nonotes, std::vector<std::string>* typeNotes) {
  auto rcError = [](const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };
  if (files.empty()) return rcError("invalid file specification", "198");
  // An empty master (after `clear`: no variables) takes the first file whole.
  // Decided before any using file is materialized, which can move the plane.
  bool emptyMaster = !session.currentHbin().has_value();
  if (!emptyMaster) {
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      emptyMaster = card->empty();
    } else if (Datastore* ds = session.datastore(); ds == nullptr || ds->names().empty()) {
      emptyMaster = true;
    }
  }
  std::optional<int64_t> lastRows;
  for (const auto& f : files) {
    // append.ado names the file as typed when it is missing (`file nof not
    // found`, not `nof.dta`), while the stem still reads NAME.dta.
    const std::string path = resolveUsingDataset(session, f);
    if (!fileExists(path)) return rcError("file " + f + " not found", "601");
    lastRows.reset();
    if (extOf(path) == ".dta" || usingFileIsDta(path)) {
      lastRows = usingDtaRows(path);
    } else if (extOf(path) != ".csv") {
      try {
        lastRows = Datastore(path).nRows();
      } catch (const std::exception&) {
        lastRows.reset();
      }
    }
  }
  if (lastRows.has_value()) session.eval.rValues.set("N", static_cast<double>(*lastRows));

  if (genVar.has_value() && !emptyMaster) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(*genVar))
      return rcError("variable " + *genVar + " already defined", "110");
  }
  if (files.size() == 1 && !emptyMaster)
    return runAppend(files[0], session, genVar, keepSpec, force, nolabel, nonotes, typeNotes);

  // The marker's code per row, filled as the files land (multi-file / empty master).
  std::vector<int> codes;
  ojson result;
  size_t first = 0;
  if (emptyMaster) {
    result = runUse(files[0], session);
    if (result.is_object() && result.value("type", std::string()) == "error") {
      // A .csv `use` refuses at r(610) "not Stata format"; append's answer for
      // it is "not found" r(601) (delimitedUsingRefused).
      if (result.value("reason", std::string()) == "r(610)") {
        result["reason"] = "r(601)";
        std::string msg = result.value("message", std::string());
        const size_t at = msg.find(" not Stata format");
        if (at != std::string::npos) msg.replace(at, 17, " not found");
        result["message"] = msg;
      }
      return result;
    }
    int64_t n1 = -1;
    if (result.is_object() && result.contains("n_rows_after") &&
        result["n_rows_after"].is_number_integer())
      n1 = result["n_rows_after"].get<int64_t>();
    if (n1 < 0) {
      Datastore* ds = session.datastore();
      if (ds == nullptr) return eval::noDataLoaded();
      n1 = ds->nRows();
    }
    codes.assign(static_cast<size_t>(n1), 1);
    first = 1;
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    const int64_t n0 = ds->nRows();
    result = runAppend(files[0], session, genVar, keepSpec, force, nolabel, nonotes, typeNotes);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    codes.assign(static_cast<size_t>(n0), 0);
    const int64_t nu = result.value("n_using", int64_t{0});
    codes.insert(codes.end(), static_cast<size_t>(nu), 1);
    first = 1;
  }
  for (size_t k = first; k < files.size(); ++k) {
    result = runAppend(files[k], session, std::nullopt, keepSpec, force, nolabel, nonotes, typeNotes);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    const int64_t nu = result.value("n_using", int64_t{0});
    codes.insert(codes.end(), static_cast<size_t>(nu), static_cast<int>(k + 1));
  }
  if (!genVar.has_value()) return result;

  // Write the marker's codes over the result (a new first column after an
  // empty master, else the column the first append created).
  {
    (void)mutationResult(session, "append");
    std::string valuesPath = nextPath(session.workdir(), "append_gen") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int c : codes) csv << c << "\n";
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::vector<std::string> sargs = {"setcol", "--data", *session.currentHbin(), "--var", *genVar,
                                      "--values", valuesPath, "--dtype", "f64"};
    if (emptyMaster) sargs.push_back("--new");
    const std::string genOut = nextPath(session.workdir(), "append_gen");
    sargs.push_back("--out");
    sargs.push_back(genOut);
    CobraRunResult gcr = runCobra(sargs);
    cecon::native_unlink(valuesPath);
    if (!gcr.ok) {
      CobraError ce = errorFromRaw(gcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(genOut);
    if (emptyMaster) {
      const std::string ordOut = nextPath(session.workdir(), "append_gen_order");
      CobraRunResult ocr = runCobra({"order", "--data", *session.currentHbin(), "--vars", *genVar,
                                     "--first", "--out", ordOut});
      if (!ocr.ok) {
        CobraError ce = errorFromRaw(ocr);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(ordOut);
    }
  }
  // append.ado's label: `_append` (or `__append` when `_append` is taken), 0
  // "Master", k "Appended dataset k" (dc_a_gen_two_gl); a byte "Dataset source"
  // displayed as wide as its longest text.
  const std::string genLbl =
      (emptyMaster || !session.labelDefs.count("_append") ||
       session.varLblname.count(*genVar))
          ? (session.varLblname.count(*genVar) ? session.varLblname[*genVar] : std::string("_append"))
          : std::string("__append");
  std::map<std::string, std::string> defs = {{"0", "Master"}};
  std::map<int, std::string> numeric = {{0, "Master"}};
  size_t widest = 6;
  for (size_t k = 1; k <= files.size(); ++k) {
    const std::string text = "Appended dataset " + std::to_string(k);
    defs[std::to_string(k)] = text;
    numeric[static_cast<int>(k)] = text;
    widest = std::max(widest, text.size());
  }
  session.labelDefs[genLbl] = defs;
  session.varLblname[*genVar] = genLbl;
  session.valueLabels[*genVar] = numeric;
  setNumericTypeClaim(session, *genVar, std::string("byte"));
  session.varMeta[*genVar].format = "%" + std::to_string(widest) + ".0g";
  session.setVarLabel(*genVar, "Dataset source");
  return mutationResult(session, "append");
}
}
