// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/descriptive.h"
#include "piet/eval/result_order.h"  // orderRResults: describe lists r() as Stata does
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation/estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"
#include "piet/user_paths.h"  // describe using: the file as the user typed it
#include "datamgmt/variable_metadata_support.h"  // dropScratch (describe using)

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// describe

namespace {

// C4-L2B (D1 card plane): the per-variable describe entry, shared by the card
// plane and the datastore plane so the two renders cannot drift — every field
// except n_unique's f64 branch is metadata the resident card already carries
// (dtype vocabulary, n_missing) or session-side meta (stataType/format/label/
// val_label), and the field ORDER here is the frozen wire order.
// format-display-default-lifecycle: `fmt` is the caller's
// SessionState::displayFormat(card entry) answer, the one resolver every reader
// shares (session > the file's own > the declared type's default). describe
// used to keep its own fallback here (`isFactor ? "%9s" : "%9.0g"`), which
// answered %9.0g for a byte and %9s for a str12 (w2p_estim_ev.log:1725/:1827).
ojson describeEntry(const SessionState& session, const std::string& name, bool isFactor,
                    int64_t nMissing, int64_t nUnique, const std::string& fmt) {
  auto metaIt = session.varMeta.find(name);
  std::string stataType;
  if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
    stataType = *metaIt->second.stataType;
  } else {
    stataType = isFactor ? "str#" : "float";
  }
  std::string label;
  if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
    label = *metaIt->second.label;
  }

  ojson entry;
  entry["name"] = name;
  entry["dtype"] = stataType;
  entry["format"] = fmt;
  entry["label"] = label;
  entry["n_missing"] = nMissing;

  auto lblIt = session.varLblname.find(name);
  if (lblIt != session.varLblname.end()) entry["val_label"] = lblIt->second;

  entry["n_unique"] = nUnique;
  return entry;
}

}  // namespace

ojson runDescribe(const std::optional<std::vector<std::string>>& varlist, SessionState& session) {
  // C4-L2B (D1 card plane, brief §1 describe row): describe's CARD SHARE.
  // Every field the entry needs is card/session metadata EXCEPT n_unique for
  // an f64 column, which is a capped distinct-count over the column VALUES —
  // no card can answer that, so a described set containing any f64 (or a
  // factor whose level count the card's writer did not capture) falls back
  // WHOLESALE to the datastore plane below, exactly the pre-D1 path (one
  // construct, same bytes). A factor-only set answers entirely off the CURRENT
  // card: presence (a current card is authoritative for absence too — the
  // honest-currency rule), dtype, n_missing, and n_unique == n_levels (the
  // datastore plane's factor n_unique IS levels.size(), Datastore::
  // buildVarInfo, which is the same number the card carries as nLevels).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::vector<std::string> names;
    if (varlist.has_value()) {
      names = *varlist;
    } else {
      names.reserve(card->size());
      for (const auto& v : *card) names.push_back(v.name);
    }
    bool cardCanAnswer = true;
    ojson variables = ojson::array();
    for (const auto& name : names) {
      const VarInfo* vi = nullptr;
      for (const auto& v : *card) {
        if (v.name == name) {
          vi = &v;
          break;
        }
      }
      if (vi == nullptr) continue;  // authoritative absence — matches !ds->has(name)
      if (vi->dtype != "factor" || !vi->nLevels.has_value()) {
        cardCanAnswer = false;  // f64 n_unique needs column values — fall back
        break;
      }
      variables.push_back(describeEntry(session, name, /*isFactor=*/true, vi->nMissing,
                                        *vi->nLevels, session.displayFormat(*vi)));
    }
    if (cardCanAnswer) {
      ojson out;
      out["type"] = "describe";
      out["variables"] = variables;
      return out;
    }
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  std::vector<std::string> names = varlist.has_value() ? *varlist : ds->names();

  ojson variables = ojson::array();
  for (const auto& name : names) {
    if (!ds->has(name)) continue;
    VarInfo vi = ds->varinfo(name);

    int64_t nUnique;
    if (vi.dtype == "factor") {
      nUnique = static_cast<int64_t>(vi.levels.size());
    } else {
      // Unique-value count over non-missing f64 values, capped at 65536
      // (describe.py parity). O(n log n) via sort — acceptable for the
      // vertical-slice corpus's dataset sizes.
      const cecon::Column& c = ds->col(name);
      const double* d = c.f64();
      int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
      std::vector<double> valid;
      valid.reserve(count);
      for (int64_t i = 0; i < count; ++i) {
        if (!std::isnan(d[i])) valid.push_back(d[i]);
      }
      std::sort(valid.begin(), valid.end());
      valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
      nUnique = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));
    }
    variables.push_back(describeEntry(session, name, vi.dtype == "factor", ds->nMissing(name),
                                       nUnique, session.displayFormat(vi)));
  }

  ojson out;
  out["type"] = "describe";
  out["variables"] = variables;
  return out;
}

namespace {

bool usingFileExists(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  return static_cast<bool>(f);
}

// A Stata .dta by its first bytes: `<stata_dta>` (117+) or a 113-115 header.
bool usingLooksLikeDta(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  char header[11] = {};
  input.read(header, sizeof header);
  const auto size = input.gcount();
  const bool xml = size == 11 && std::string(header, 11) == "<stata_dta>";
  const auto version = static_cast<unsigned char>(header[0]);
  const bool legacy = size >= 3 && (version == 113 || version == 114 || version == 115) &&
                      (header[1] == 1 || header[1] == 2) && header[2] == 1;
  return xml || legacy;
}

// One storage type's bytes in an observation (r(width)); -1 = unknown.
double storageWidth(const std::string& t) {
  if (t == "byte") return 1;
  if (t == "int") return 2;
  if (t == "long" || t == "float") return 4;
  if (t == "double" || t == "strL") return 8;
  if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
      t.find_first_not_of("0123456789", 3) == std::string::npos)
    return std::stod(t.substr(3));
  return -1;
}

// A label the import sidecar carried escaped (BUG-1372), back as its text.
std::string unescapedLevel(std::string text) {
  cecon::hbin_level_unescape(text);
  return text;
}

ojson describeUsingError(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

}  // namespace

ojson runDescribeUsing(const std::vector<std::string>& names, const std::string& fileTyped,
                       bool shortForm, bool simple, bool varlistOpt, SessionState& session) {
  // The file as typed (quotes off) is what the messages show; `.dta` is added
  // when the name has no extension and the bare name is not there.
  std::string typed = strutil::trim(fileTyped);
  if (typed.size() >= 2 && typed.front() == '"' && typed.back() == '"')
    typed = typed.substr(1, typed.size() - 2);
  if (typed.empty()) return describeUsingError("invalid file specification", 198);
  std::string path = resolveUserPath(session, typed);
  std::string shown = typed;
  session.materializeVirtualSave(path);
  {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    const bool noExt = dot == std::string::npos || (slash != std::string::npos && dot < slash);
    if (noExt && !usingFileExists(path)) {
      path += ".dta";
      shown += ".dta";
      session.materializeVirtualSave(path);
    }
  }
  if (!usingFileExists(path)) return describeUsingError("file " + shown + " not found", 601);
  if (!usingLooksLikeDta(path)) return describeUsingError("file " + shown + " not Stata format", 610);

  const std::string conv = mintScratchPath(session.workdir(), "describe_using");
  const std::string metaPath = conv + ".meta.json";
  registerScratchMint(metaPath);
  const CobraRunResult cr = runCobra({"import-dta", path, conv, "--out-meta", metaPath});
  nlohmann::json meta;
  std::string sortedBy;
  bool readOk = cr.ok;
  if (readOk) {
    try {
      std::ifstream mf(cecon::to_native_path(metaPath), std::ios::binary);
      meta = nlohmann::json::parse(mf);
      Datastore usingDs(conv);
      sortedBy = usingDs.sortedBy();
    } catch (const std::exception&) {
      readOk = false;
    }
  }
  variable_metadata_detail::dropScratch(conv);
  variable_metadata_detail::dropScratch(metaPath);
  if (!readOk)
    return describeUsingError("file " + shown + " could not be read: " +
                                  strutil::trim(cr.stderr_text.empty() ? cr.error : cr.stderr_text),
                              610);

  struct UsingVar {
    std::string name, type, format, label, valueLabel;
  };
  std::vector<UsingVar> vars;
  if (meta.contains("variables") && meta["variables"].is_array())
    for (const auto& v : meta["variables"])
      vars.push_back({v.value("name", std::string()), v.value("stata_type", std::string()),
                      v.value("format", std::string()),
                      unescapedLevel(v.value("label", std::string())),
                      v.value("value_label_name", std::string())});
  const double rows = meta.value("n_rows", 0.0);
  const std::string dataLabel = unescapedLevel(meta.value("data_label", std::string()));

  // The variables asked for, against the FILE's names, under [D] describe's
  // rule for `describe varlist using`: names are NOT abbreviated (`displ` is
  // r(111) `variable displ not found`), `~` marks an abbreviation (`displ~`,
  // the one name it matches; several are r(111) "ambiguous abbreviation"), `*`
  // and `?` are wildcards, and a range `a-b` is an error.
  std::vector<const UsingVar*> shownVars;
  if (names.empty()) {
    for (const auto& v : vars) shownVars.push_back(&v);
  } else {
    for (const auto& want : names) {
      std::vector<const UsingVar*> hit;
      for (const auto& v : vars)
        if (v.name == want) hit = {&v};
      if (hit.empty() && want.find('~') != std::string::npos) {
        std::string pat = want;
        std::replace(pat.begin(), pat.end(), '~', '*');
        for (const auto& v : vars)
          if (strutil::globMatch(pat, v.name)) hit.push_back(&v);
        if (hit.size() > 1) return describeUsingError(want + " ambiguous abbreviation", 111);
      } else if (hit.empty() && want.find_first_of("*?") != std::string::npos) {
        for (const auto& v : vars)
          if (strutil::globMatch(want, v.name)) hit.push_back(&v);
      }
      if (hit.empty()) return describeUsingError("variable " + want + " not found", 111);
      for (const auto* h : hit) shownVars.push_back(h);
    }
  }

  // r(): the FILE's shape and the edition limits (StataBE 19.5 truth bank,
  // `describe using zusing.dta`: r(N) 74, r(k) 12, r(width) 43, r(N_max),
  // r(k_max), r(widthmax), r(datalabel)); no r(changed), which is the data in
  // memory's. r(k) and r(width) are the whole file's whatever was listed, and
  // r(datalabel) is posted only when no varlist was given. `varlist` adds
  // r(varlist) and r(sortlist). r() is replaced: the data in memory are untouched.
  eval::replaceRClass(session.eval);
  auto& r = session.eval.rValues;
  double width = 0.0;
  bool widthKnown = true;
  for (const auto& v : vars) {
    const double w = storageWidth(v.type);
    if (w < 0) widthKnown = false;
    else width += w;
  }
  r.set("N", rows);
  r.set("k", static_cast<double>(vars.size()));
  if (widthKnown) r.set("width", width);
  r.set("N_max", 2147483619.0);
  r.set("k_max", 2048.0);
  r.set("widthmax", 1048576.0);
  for (const char* h : {"N_max", "k_max", "widthmax"}) session.eval.markRHidden(h);
  std::vector<std::string> posted{"N", "k", "width", "N_max", "k_max", "widthmax"};
  if (names.empty() && !dataLabel.empty()) {
    session.eval.rStrings.set("datalabel", dataLabel);
    posted.push_back("datalabel");
  }
  if (varlistOpt) {
    std::string joined;
    for (const auto* v : shownVars) joined += (joined.empty() ? "" : " ") + v->name;
    session.eval.rStrings.set("varlist", joined);
    session.eval.rStrings.set("sortlist", sortedBy);
    posted.push_back("varlist");
    posted.push_back("sortlist");
  }
  eval::orderRResults(session.eval, {"widthmax", "k_max", "N_max", "width", "k", "N", "sortlist",
                                     "varlist", "datalabel"});
  eval::declareRPosted(session.eval, "describe", std::move(posted));

  if (shortForm || simple) {
    ojson out;
    out["type"] = "display";
    if (simple) {
      std::string joined;
      for (const auto* v : shownVars) joined += (joined.empty() ? "" : "  ") + v->name;
      out["text"] = joined;
    } else {
      std::ostringstream t;
      t << "Contains data\n Observations: " << static_cast<int64_t>(rows)
        << "\n    Variables: " << vars.size();
      if (!sortedBy.empty()) t << "\nSorted by: " << sortedBy;
      out["text"] = t.str();
    }
    return out;
  }
  ojson variables = ojson::array();
  for (const auto* v : shownVars) {
    ojson entry;
    entry["name"] = v->name;
    entry["dtype"] = v->type;
    entry["format"] = v->format;
    entry["label"] = v->label;
    if (!v->valueLabel.empty()) entry["val_label"] = v->valueLabel;
    variables.push_back(entry);
  }
  ojson out;
  out["type"] = "describe";
  out["variables"] = variables;
  out["using"] = shown;
  out["n_rows"] = static_cast<int64_t>(rows);
  return out;
}

// The edition limits describe reports beside the dataset's own shape. They are
// StataBE 19.5's c(max_N_theory), c(max_k_theory) and c(max_width_theory) (the
// vs-stata truth bank: `display c(max_N_theory)` 2.147e+09, r(N_max)
// +1.ffffff8c00000X+01e, r(k_max) 2048, r(widthmax) 1048576 after every describe).
void postDescribeResults(SessionState& session, const std::string& cmd) {
  // Rows and variables: the resident card when it is current (no Datastore is
  // built for a card-plane describe, C4-L2B), else the Datastore. An empty
  // session (no data, no variables) is described too: 0 / 0 / 0.
  int64_t rows = 0;
  std::vector<std::pair<std::string, bool>> vars;  // name, string-typed
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card != nullptr) {
    for (const auto& v : *card) vars.emplace_back(v.name, v.dtype == "factor");
  }
  if (session.hasData()) {
    if (std::optional<int64_t> r = session.residentRowsIfCurrent()) {
      rows = *r;
    } else if (Datastore* ds = session.datastore()) {
      rows = ds->nRows();
      if (card == nullptr)
        for (const auto& n : ds->names()) vars.emplace_back(n, ds->dtype(n) == "factor");
    }
  }
  // r(width): one observation's storage in bytes -- byte 1, int 2, long and
  // float 4, double and strL 8, str# its # -- over the types describe prints.
  // A string whose width was never recorded leaves it unknown.
  std::optional<double> width = 0.0;
  for (const auto& [name, isString] : vars) {
    const auto meta = session.varMeta.find(name);
    const std::string t = meta != session.varMeta.end() && meta->second.stataType.has_value()
                              ? *meta->second.stataType
                              : (isString ? "str#" : "float");
    if (t == "byte") *width += 1;
    else if (t == "int") *width += 2;
    else if (t == "long" || t == "float") *width += 4;
    else if (t == "double" || t == "strL") *width += 8;
    else if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
             t.find_first_not_of("0123456789", 3) == std::string::npos)
      *width += std::stod(t.substr(3));
    else {
      width.reset();
      break;
    }
  }
  auto& r = session.eval.rValues;
  r.set("N", static_cast<double>(rows));
  r.set("k", static_cast<double>(vars.size()));
  if (width.has_value()) r.set("width", *width);
  else r.erase("width");
  r.set("N_max", 2147483619.0);
  r.set("k_max", 2048.0);
  r.set("widthmax", 1048576.0);
  for (const char* h : {"N_max", "k_max", "widthmax"}) session.eval.markRHidden(h);
  r.set("changed", session.datasetChanged ? 1.0 : 0.0);
  std::vector<std::string> posted{"N", "k", "width", "N_max", "k_max", "widthmax", "changed"};
  // LBL-DATALABEL: r(datalabel) only when the dataset has a label (lb_var_label
  // reads "" without one).
  if (!session.dataLabel.empty()) {
    session.eval.rStrings.set("datalabel", session.dataLabel);
    posted.push_back("datalabel");
  } else {
    session.eval.rStrings.erase("datalabel");
  }
  // Stata lists them `changed widthmax k_max N_max width k N` (vs-stata truth
  // bank); an r(N) an earlier command left keeps its place otherwise.
  eval::orderRResults(session.eval,
                      {"changed", "widthmax", "k_max", "N_max", "width", "k", "N", "datalabel"});
  eval::declareRPosted(session.eval, cmd, std::move(posted));
}

}  // namespace piet::handlers
