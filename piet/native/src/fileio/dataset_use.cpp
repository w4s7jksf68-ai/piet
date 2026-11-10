// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"
#include "piet/handlers/session_commands.h"
#include "piet/user_paths.h"
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
#include "engine/engine_call_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace engine_call_detail;

namespace {

bool fileExists(const std::string& path) { return cecon::native_exists(path); }


std::string ext(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && pos < slash) return "";  // a dotted directory (BUG-1130)
  return lower(path.substr(pos));
}

// BUG A / hbin-sniff (W3 fuzzer stabilization, fail_024): a file's CONTENT,
// not its NAME, decides how `use` should read it. Before runSaveData's
// honest-.dta-error fix (this same milestone), `save FILE.dta` byte-copied
// the session's internal .hbin straight to a `.dta`-named path, so
// mislabeled hbin-content files with a `.dta` extension already exist from
// that broken era (and could reappear by simple user renaming-mistake even
// after the fix). Sniff the on-disk magic BEFORE the extension dispatch so
// any such file loads correctly regardless of its name — this is the SAME
// 8-byte "CECONHB1" marker src/io/columnar.cpp's Dataset::open() checks
// (kMagic there); reading it here is inspecting the file's bytes, not
// touching engine source. A genuine .dta (Stata's own magic: "<stata_dta>"
// XML-ish header, or the older binary release markers) or .csv/.tsv text
// file cannot coincidentally start with this exact ASCII sequence, so this
// never misfires on a real file of either format.
bool looksLikeHbin(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return false;
  char buf[8];
  f.read(buf, sizeof(buf));
  if (f.gcount() != static_cast<std::streamsize>(sizeof(buf))) return false;
  static const char kHbinMagic[8] = {'C', 'E', 'C', 'O', 'N', 'H', 'B', '1'};
  return std::memcmp(buf, kHbinMagic, sizeof(buf)) == 0;
}


// PP-L2A / K1a escape hatch (the C1/C2/C4 convention: one env var that restores
// the pre-optimization path verbatim, so every gate gets an A/B arm and the
// merge gets its seeded-RED). Per-call getenv on purpose — the selftest legs
// setenv/unsetenv around single dispatches, which a static cache would ignore.
bool useConvCacheDisabled() {
  const char* e = std::getenv("PIET_NO_USE_CACHE");
  return e && *e;
}

// PP-L2A / K1a: the entire cache guard, in one string — inode, size, and mtime
// (nanoseconds where the platform carries them) of the SOURCE file. A memo is
// reused only when a fresh key is byte-identical to the key measured when the
// memo was written; a single moved field means "convert".
//
// Returns "" on ANY stat failure, and every caller reads "" as "not cacheable,
// take the disk path" — the guard is never silently weakened to a subset of its
// fields, and an unreadable source still reaches the ordinary code that reports
// the honest error.
//
// st_ino is 0 on toolchains whose stat shim carries no real inode (the Windows
// CRT); the key there degrades to (0, size, mtime) — one field lighter, but
// still a guard, and in the only direction that is safe: a dropped field can
// turn a would-be hit into a miss (an extra conversion), never a mismatch into
// a hit (stale bytes).
std::string useCacheStatKey(const std::string& path) {
#ifdef _WIN32
  // Narrow ::stat would decode a Korean path through CP949 and miss the file
  // entirely (BUG-050), turning every `use` into a cache miss. native_file_stamp
  // is the wide equivalent; it carries no inode and no sub-second mtime, so the
  // key degrades to (0, size, mtime, 0) — exactly the safe direction described
  // above (more misses, never a stale hit).
  std::int64_t stampSize = 0;
  std::int64_t stampMtimeNs = 0;
  if (!cecon::native_file_stamp(path, stampSize, stampMtimeNs)) return {};
  const unsigned long long ino = 0;
  const long long size = static_cast<long long>(stampSize);
  const long long mtime = static_cast<long long>(stampMtimeNs / 1000000000LL);
  const long long nsec = 0;
#else
  struct stat st;
  if (::stat(cecon::to_native_path(path).c_str(), &st) != 0) return {};
  long long nsec = 0;
#if defined(__APPLE__)
  nsec = static_cast<long long>(st.st_mtimespec.tv_nsec);
#elif defined(__linux__)
  nsec = static_cast<long long>(st.st_mtim.tv_nsec);
#endif
  const unsigned long long ino = static_cast<unsigned long long>(st.st_ino);
  const long long size = static_cast<long long>(st.st_size);
  const long long mtime = static_cast<long long>(st.st_mtime);
#endif
  char buf[192];
  std::snprintf(buf, sizeof(buf), "%llu:%lld:%lld:%lld", ino, size, mtime, nsec);
  return buf;
}


// -- .dta loading (M3b): cobra 0.18.0's native `import-dta` reader ---------
//
// C++ port of routes_data.py's _convert_to_hbin (.dta branch) +
// _apply_dta_var_labels: `cobra import-dta IN.dta OUT.hbin --out-meta
// META.json` writes the hbin (value-labeled numerics stay f64 raw codes,
// genuine strings become factor columns — the reader's own doc string,
// verified: `cobra import-dta --help`) plus a metadata sidecar with
// {format_version, data_label, n_rows, variables:[{name,label,format,
// stata_type,value_label_name}], value_labels:{lblname:{code_str:text}}}.
// This mirrors the oracle's TWO-PASS design (raw read for hbin + a
// StataReader metadata pass for labels) with ONE cobra call — the metadata
// is a byproduct of the same native parse, not a second file read.
//
// import-dta's success path writes a plain-text status line to stderr and
// EMPTY stdout (verified empirically) — unlike `eval`/`feols`/etc it does
// NOT print a JSON envelope on success, so this uses runCobra() (raw) and
// inspects .ok, not runCobraJson().
struct DtaMetaResult {
  bool ok = false;
  std::string errorMessage;
  std::string errorCode;
};

DtaMetaResult applyDtaMeta(const std::string& metaPath, SessionState& session) {
  DtaMetaResult r;
  std::ifstream f(cecon::to_native_path(metaPath), std::ios::binary);
  if (!f) {
    r.errorMessage = "use: .dta metadata sidecar missing: " + metaPath;
    r.errorCode = "engine_error";
    return r;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  nlohmann::json meta;
  try {
    meta = nlohmann::json::parse(ss.str());
  } catch (const nlohmann::json::exception&) {
    r.errorMessage = "use: .dta metadata sidecar is not valid JSON: " + metaPath;
    r.errorCode = "engine_error";
    return r;
  }

  // variable metadata: label / format / stata_type -> session.varMeta;
  // value_label_name (if non-empty) -> session.varLblname (mirrors
  // _apply_dta_var_labels' session.set_var_label / _var_meta population).
  if (meta.contains("variables") && meta["variables"].is_array()) {
    for (const auto& v : meta["variables"]) {
      std::string name = v.value("name", std::string());
      if (name.empty()) continue;
      VarMeta vm;
      // BUG-1372: text that is not UTF-8 arrives escaped (cmd_import_dta).
      std::string label = v.value("label", std::string());
      cecon::hbin_level_unescape(label);
      if (!label.empty()) vm.label = label;
      std::string fmt = v.value("format", std::string());
      if (!fmt.empty()) vm.format = fmt;
      std::string stataType = v.value("stata_type", std::string());
      if (!stataType.empty()) vm.stataType = stataType;
      session.varMeta[name] = vm;
      std::string vlName = v.value("value_label_name", std::string());
      if (!vlName.empty()) session.varLblname[name] = vlName;
    }
  }

  // value_labels: {lblname: {code_str: text}} -> session.labelDefs
  // (labelDefs is keyed by STRING code — mirrors varlistWithMeta's lookup
  // against factor `levels` entries, which are strings; datastore.py's
  // describe/browse layer resolves value-labeled f64 columns via a
  // separate numeric-keyed valueLabels map for display-time substitution —
  // see below).
  if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
    for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
      const std::string& lblname = it.key();
      std::map<std::string, std::string> codeMap;
      std::map<int, std::string> numericMap;
      if (it.value().is_object()) {
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit) {
          std::string text = cit.value().get<std::string>();
          cecon::hbin_level_unescape(text);
          codeMap[cit.key()] = text;
          try {
            numericMap[std::stoi(cit.key())] = text;
          } catch (const std::exception&) {
            // non-numeric code key (shouldn't happen for Stata value
            // labels) — skip the numeric-keyed side, keep the string side.
          }
        }
      }
      session.labelDefs[lblname] = codeMap;
      // Populate valueLabels for every variable currently pointing at this
      // label set (variables are already registered above, so varLblname
      // is complete by the time value_labels is processed — variables[]
      // precedes value_labels in the meta JSON's own field order, and both
      // blocks fully run before any command sees the session).
      for (const auto& [varname, lname] : session.varLblname) {
        if (lname == lblname) session.valueLabels[varname] = numericMap;
      }
    }
  }

  // data_label: dataset-level label (Stata `label data`) -> session.dataLabel.
  if (meta.contains("data_label") && meta["data_label"].is_string()) {
    session.dataLabel = meta["data_label"].get<std::string>();
    cecon::hbin_level_unescape(session.dataLabel);
  }

  // characteristics: [{varname, charname, value}, …] -> session.characteristics,
  // verbatim — exactly export-dta --in-meta's own parse (cmds_data.cpp) so the
  // sidecar this reads and the one `save` re-emits (writeDtaMetaSidecar,
  // dataset_save.cpp) agree on shape.
  if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
    for (const auto& ch : meta["characteristics"]) {
      if (!ch.is_object()) continue;
      DtaCharacteristic c;
      if (ch.contains("varname") && ch["varname"].is_string())
        c.varname = ch["varname"].get<std::string>();
      if (ch.contains("charname") && ch["charname"].is_string())
        c.charname = ch["charname"].get<std::string>();
      if (ch.contains("value") && ch["value"].is_string()) {
        c.value = ch["value"].get<std::string>();
        cecon::hbin_level_unescape(c.value);
      }
      if (c.varname.empty() || c.charname.empty()) continue;
      session.characteristics.push_back(std::move(c));
    }
  }

  // [D] label language, Methods and formulas (p.669): active labels are in
  // the ordinary DTA fields; every inactive language lives in _lang_* chars.
  std::string current = "default", languages = "default";
  for (const auto& c : session.characteristics) {
    if (c.varname != "_dta") continue;
    if (c.charname == "_lang_list" && !c.value.empty()) languages = c.value;
    else if (c.charname == "_lang_c" && !c.value.empty()) current = c.value;
  }
  session.labelLangs.clear();
  std::istringstream languageStream(languages);
  for (std::string name; languageStream >> name;)
    if (std::find(session.labelLangs.begin(), session.labelLangs.end(), name) == session.labelLangs.end())
      session.labelLangs.push_back(name);
  if (session.labelLangs.empty()) session.labelLangs.push_back("default");
  session.labelLang = std::find(session.labelLangs.begin(), session.labelLangs.end(), current) != session.labelLangs.end()
                          ? current : session.labelLangs.front();
  session.labelLangStash.clear();
  for (const auto& language : session.labelLangs) {
    if (language == session.labelLang) continue;
    auto& labels = session.labelLangStash[language];
    for (const auto& c : session.characteristics) {
      if (c.charname == "_lang_v_" + language) {
        if (c.varname == "_dta") labels.dataLabel = c.value;
        else labels.varLabels[c.varname] = c.value;
      } else if (c.charname == "_lang_l_" + language && c.varname != "_dta") {
        labels.varLblname[c.varname] = c.value;
      }
    }
  }

  r.ok = true;
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// use

ojson runUse(const std::string& filepathArg, SessionState& session) {
  std::string filepath = resolveUserPath(session, filepathArg);
  // xtset-file-persistence: clearMetadata() below drops the ts declaration with
  // the char table (Stata keeps it IN the table). A .dta states its own and is
  // re-adopted from it. A native hbin / `.tmp` has no char table: one this
  // session saved restates the declaration recorded at its `save` (BUG-659,
  // SessionState::noteCheckpointTsDecl); any other keeps the declaration live
  // before the load — the pre-existing behaviour for a native file nothing
  // here knows the declaration of.
  const std::optional<std::string> priorPanelvar = session.panelvar;
  const std::optional<std::string> priorTimevar = session.timevar;
  const std::optional<double> priorDelta = session.delta;
  auto nativeTsDecl = [&]() {
    if (session.adoptCheckpointTsDecl(filepath)) return;
    session.panelvar = priorPanelvar;
    session.timevar = priorTimevar;
    session.delta = priorDelta;
    session.writeTsChars();
  };

  // MAXPERF Stage 2 (item 2): a tempfile that was `save`d into the engine's
  // virtual-file table (save_virtual) has NO disk file — it lives in the
  // worker's RAM. Restore it from there (O(columns), virtual-first) instead of
  // the disk-open path below, which would fail fileExists(). loadFromVirtual()
  // returns false if the entry isn't live (never save_virtual'd here, or lost to
  // a worker restart), in which case we fall through: the stateless save wrote a
  // real file, so the disk path picks it up — and a truly missing tempfile hits
  // the honest "File not found" below. The success block MUST be byte-identical
  // to the disk path's (a `.tmp` use adds no honesty note — origExt==".tmp" is
  // carved out there too), so it renders through the SAME varlist card shape
  // either plane emits (VarInfo::toJson via varlistWithMeta*).
  if (session.isVirtualSave(filepath) && session.loadFromVirtual(filepath)) {
    counters::addUseVirtualHit();  // PP-L1: virtual-table restore, no disk open
    session.clearMetadata();  // a `.tmp` hbin carries no dta sidecar labels (matches disk path).
    nativeTsDecl();           // …nor a char table: the declaration rides beside it (see top).
    // BUG-198: …but it DOES carry the session's own format/label layer, parked
    // beside the virtual entry by `save` because a RAM-only save has no header
    // to write it into. The disk arm gets the identical layer back out of the
    // file's header two blocks down; this is the same data by construction
    // (both sides are built by hbinMetaPatch), so the two planes agree.
    session.seedVarMetaFromVirtualSave(filepath);
    // C4-L2B (D1 card plane, brief §1 use row + §3 reclassification): the
    // loadFromVirtual above ADOPTED the incoming frame as a dirty virtual head
    // and seeded the shadow card from the SAME CobraFrameMeta the worker just
    // reported (full column list + rows). Rendering varlist_after through
    // session.datastore() here forced an immediate EMH frame snapshot of the
    // frame we JUST received from worker RAM — a whole-file disk write whose
    // only consumer was this metadata render (a2's 10 use-closures were 100%
    // this site). The card answers the render without the disk round-trip:
    // varlistWithMetaFrom(shadowCard_) is the SAME render the mutation-card
    // fold has always emitted (session_state.cpp fold), and VarInfo::toJson
    // serializes exactly {name,dtype,format,label,n_levels,n_missing} — the
    // frame-var card carries every one of those fields byte-identically to
    // Datastore::buildVarInfo's (both leave format/label at the same defaults;
    // levels strings are F5-thinned out of the JSON on both planes). Honest
    // fallback: an un-vouched meta (rows unknown) or the escape arm
    // (PIET_NO_RESIDENT_READ=1, via residentReadCard's gate) drops to the
    // pre-D1 datastore() render verbatim — order and bytes unchanged.
    const std::vector<VarInfo>* card = session.residentReadCard();
    const std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
    if (card != nullptr && cardRows.has_value()) {
      session.setCurrentFile(filepath);
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "open";
      out["source_file"] = session.currentFile();
      out["filepath"] = session.currentFile();
      ojson varlist = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlist.push_back(v.toJson());
      session.updateNMissCache(vlist);  // keep the C2-F13a cache coherent (fold-render law)
      out["varlist_after"] = varlist;
      out["n_rows_after"] = *cardRows;
      return out;
    }
    Datastore* ds = session.datastore();  // materializes the virtual head on demand.
    session.setCurrentFile(filepath);
    ojson out;
    out["type"] = "data_mutation";
    out["action"] = "open";
    out["source_file"] = session.currentFile();
    out["filepath"] = session.currentFile();
    ojson varlist = ojson::array();
    auto vlist = session.varlistWithMeta(ds);
    for (const auto& v : vlist) varlist.push_back(v.toJson());
    session.updateNMissCache(vlist);
    out["varlist_after"] = varlist;
    out["n_rows_after"] = ds ? ds->nRows() : 0;
    return out;
  }

  // [D] use: "If filename is specified without an extension, .dta is assumed."
  // PIET's own extensionless saves are hbin files under the bare name, so the
  // bare name wins when it exists; otherwise NAME.dta is what Stata reads
  // (w2d_uncovered.log postfile_exists_noreplace: `use "w2d_pf"` after
  // `save "w2d_pf.dta"` loads it, N 1 k 1).
  const bool noExt = ext(filepath).empty();
  if (!fileExists(filepath) && noExt && fileExists(filepath + ".dta")) filepath += ".dta";
  if (!fileExists(filepath)) {
    // StataBE: `file nofile.dta not found` r(601) for `use nofile`, the name as
    // typed with the .dta it stood for (BUG-410).
    return eval::errorBlock("engine_error",
                            "file " + typedUserPath(filepathArg) + (noExt ? ".dta" : "") +
                                " not found",
                            std::nullopt, std::nullopt, std::nullopt, std::string("r(601)"));
  }

  // BUG A / hbin-sniff: content decides format, not the name — see
  // looksLikeHbin()'s header comment above. This runs BEFORE the ordinary
  // extension dispatch so a mislabeled hbin-content file (most likely a
  // `.dta`-named survivor of the earlier `save`-honesty bug, but any name
  // works) loads correctly regardless of what its extension claims.
  std::string origExt = ext(filepath);
  std::string e = origExt;
  bool hbinMismatch = false;
  if (e != ".hbin" && looksLikeHbin(filepath)) {
    hbinMismatch = true;
    e = ".hbin";
  } else if (e != ".dta" && e != ".csv" && e != ".tsv") {
    // Stata's own bytes under another name (`saveold` into a `.tmp` tempfile
    // writes a genuine .dta there, as Stata does): the XML header of 117/118,
    // or the 113-115 binary header's format byte, byte order and filetype.
    std::ifstream sn(cecon::to_native_path(filepath), std::ios::binary);
    char hb[11] = {0};
    sn.read(hb, sizeof hb);
    const std::streamsize got = sn.gcount();
    const bool xmlDta = got == 11 && std::string(hb, 11) == "<stata_dta>";
    const unsigned char b0 = static_cast<unsigned char>(hb[0]);
    const bool oldDta = got >= 3 && (b0 == 113 || b0 == 114 || b0 == 115) &&
                        (hb[1] == 1 || hb[1] == 2) && hb[2] == 1;
    if (xmlDta || oldDta) e = ".dta";
  }
  std::string fmt = (e == ".dta") ? "dta" : (e == ".hbin") ? "hbin"
                     : (e == ".csv" || e == ".tsv")        ? "csv"
                                                             : "hbin";

  std::string dtaMetaPath;  // populated by the dta branch below; consumed
                            // AFTER session.load()/clearMetadata() further down
  std::string loadPath = filepath;

  // The (inode,size,mtime) stat key of the source, measured ONCE here and shared
  // by BOTH `use` caches below (K1b stash first, then K1a conversion): a hit and
  // a miss's memo must describe the SAME observation of the source, and a second
  // stat would open a window for the file to change between them. "" => the
  // escape (PIET_NO_USE_CACHE=1) or a stat failure, which both caches read as
  // "not cacheable, take the disk path".
  const std::string useCacheKey = useConvCacheDisabled() ? std::string() : useCacheStatKey(filepath);

  // PP-L2B / K1b (PINGPONG-SCOPE-brief §2 K1b): the RESIDENT-STASH cache — the
  // layer above K1a. A source `use`d before, whose frame is still stashed in the
  // worker's RAM and that has not moved since, restores in O(columns) via
  // frameUnstash: NO conversion, NO disk open, NO whole-file re-hash — and the
  // worker is left HOLDING the frame, so the next compute verb pays no frame-use
  // either (that is the whole win; use_disk_loads drops toward 1). On a hit we
  // re-point loadPath/dtaMetaPath at the memo's byte-twin and set k1bRestored,
  // then fall through to the SHARED card/note tail below — every downstream step
  // (sidecar re-apply, datastore render) runs on the identical bytes, so
  // varlist_after / n_rows_after / any hbin-mismatch note are byte-identical to
  // the disk path by construction. A dead stash (worker restart) makes
  // residentRestore return false, and we fall through to the ordinary disk path
  // — the honest §27 tri-state fallback.
  bool k1bRestored = false;
  if (!useCacheKey.empty()) {
    const UseStashCacheEntry* smemo = session.useStashCacheGet(filepath, useCacheKey);
    if (smemo != nullptr && fileExists(smemo->hbinPath) &&
        (smemo->metaPath.empty() || fileExists(smemo->metaPath)) &&
        session.residentRestore(smemo->stashName, smemo->hbinPath, /*keepStash=*/true)) {
      counters::addUseStashHit();  // PP-L1/K1b: a use_disk_loads that did NOT happen
      loadPath = smemo->hbinPath;
      dtaMetaPath = smemo->metaPath;
      k1bRestored = true;
    }
  }

  // PP-L2A / K1a (PINGPONG-SCOPE-brief §2 K1a; PP-L1 measured dta_converts at
  // 1 PER DEPVAR against one unchanging corpus file — instruments/
  // pp_use_split.md §1): a `.dta`/`.csv` source that has not moved since this
  // session last converted it produces a byte-identical hbin, so the second and
  // later `use` of it can re-open the FIRST conversion's output read-only
  // instead of re-running import-dta and re-writing a fresh scratch hbin.
  // BUG-051 is not in scope: this is a read-only re-open of an existing path,
  // never a rewrite of a mapped file (the write only ever happens on a miss,
  // and a miss always mints a fresh scratchSeqSuffix name).
  //
  // The stat key was measured ONCE above (useCacheKey), shared with K1b: a hit
  // and the miss's memo must describe the same observation of the source. When
  // K1b already restored the resident stash, the whole convert+load path below
  // is skipped (k1bRestored) — the frame is already in RAM.
  std::string convCacheKey;       // "" => uncacheable (stat failed, or escape on)
  bool convertedThisRun = false;  // memo is written only for a real conversion
  const UseConvCacheEntry* convHit = nullptr;
  if (!k1bRestored && (fmt == "dta" || fmt == "csv")) {
    convCacheKey = useCacheKey;
    const UseConvCacheEntry* memo = session.useConvCacheGet(filepath, convCacheKey);
    // The memoed files must still be there. Eviction cannot have taken the
    // hbin (session_state.cpp's exclusion 3 pins live memos), so this check is
    // about everything OUTSIDE this session's bookkeeping — a user or an
    // external tool clearing the scratch dir mid-run. Missing => convert, which
    // is exactly what the pre-K1a code did every time anyway.
    if (memo != nullptr && fileExists(memo->hbinPath) &&
        (memo->metaPath.empty() || fileExists(memo->metaPath))) {
      convHit = memo;
    }
  }

  if (k1bRestored) {
    // K1b already restored the resident stash — loadPath/dtaMetaPath point at
    // the memo's byte-twin and the frame is in worker RAM. Skip the whole
    // convert+disk-load dispatch; the shared tail below renders the card.
  } else if (convHit != nullptr) {
    // K1a HIT: re-open the earlier conversion's output. Nothing else about the
    // load changes — session.load()/clearMetadata()/applyDtaMeta below run on
    // the identical bytes and the identical sidecar the miss path would have
    // produced, so varlist_after / n_rows_after / the label plane are
    // byte-identical by construction. counters::addDtaConvert() is deliberately
    // NOT reached (no conversion was paid — that drop IS the K1a win signal);
    // addUseDiskLoad() below still fires, because the hbin open still happens.
    loadPath = convHit->hbinPath;
    dtaMetaPath = convHit->metaPath;  // "" on the csv branch — same as a fresh csv conversion
  } else if (fmt == "dta") {
    // M3b: cobra 0.18.0's native `import-dta` reader (src/io/dta_reader.cpp,
    // formats 115/117/118) — replaces the M2 TODO stub. Mirrors the oracle's
    // routes_data.py _convert_to_hbin (.dta branch) + _apply_dta_var_labels:
    // value-labeled numerics stay f64 raw codes, genuine strings become
    // factor columns; labels captured to a --out-meta sidecar (applied to
    // SessionState AFTER session.load()/clearMetadata(), same ordering the
    // oracle uses — _apply_dta_var_labels runs after the hbin swap).
    // BUG-051 class (fixed-name variant): a FIXED conversion-output name means a
    // 2nd `use` rewrites the loaded.hbin the session still has memory-mapped —
    // on Windows the truncating reopen fails ("use: cannot write ...loaded.hbin")
    // or, on the csv branch, the failure was masked and the STALE previous
    // dataset silently reloaded. A per-use unique name (same monotonic suffix
    // mechanism as nextPath) never targets a mapped file; empty suffix on POSIX
    // keeps macOS/Linux names byte-identical. Meta sidecar shares the suffix.
    const std::string useSeq = scratchSeqSuffix();
    std::string out = session.workdir() + "/loaded" + useSeq + ".hbin";
    dtaMetaPath = session.workdir() + "/loaded_meta" + useSeq + ".json";
    registerScratchMint(out);          // BUG-389
    registerScratchMint(dtaMetaPath);  // BUG-389: the sidecar is owned only while a memo holds it
    std::vector<std::string> args = {"import-dta", filepath, out, "--out-meta", dtaMetaPath};
    counters::addDtaConvert();  // PP-L1: a full .dta→hbin conversion is being paid
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      // import-dta's error path prints a structured {"error":{"code",
      // "message"}} JSON to stderr (verified against cecon_cli.cpp's shared
      // error-emission helper — the SAME shape errorFromResult() in
      // cobra_runner.cpp already parses), so route through the identical
      // CobraError construction rather than hand-rolling a second parser.
      std::string stderrText = cr.stderr_text;
      while (!stderrText.empty() &&
             (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
        stderrText.pop_back();
      }
      std::string code = "engine_error";
      std::string message = stderrText;
      try {
        nlohmann::json payload = nlohmann::json::parse(stderrText);
        nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
        if (err.is_object() && err.contains("message")) {
          code = err.value("code", std::string("engine_error"));
          message = err.at("message").get<std::string>();
        }
      } catch (const nlohmann::json::exception&) {
        if (message.empty()) message = "use: .dta import failed for " + filepath;
      }
      return engineErrorBlock(std::string("use: ") + message, code);
    }
    if (!fileExists(out)) {
      return engineErrorBlock("use: .dta conversion failed for " + filepath);
    }
    loadPath = out;
    convertedThisRun = true;
  } else if (fmt == "csv") {
    // Unique per-use output — same rationale as the dta branch above.
    std::string out = session.workdir() + "/loaded" + scratchSeqSuffix() + ".hbin";
    char delim = (e == ".tsv") ? '\t' : ',';
    std::vector<std::string> args = {"convert", "--data", filepath, "--out", out, "--delim",
                                      std::string(1, delim)};
    // N3a (P8.1-W2) · BUG-082: the prior `try { runCobra(args); } catch (const
    // CobraError&)` was DEAD CODE — runCobra() returns a CobraRunResult and
    // NEVER throws (CobraError is runCobraJson()'s throw type, not
    // runCobra()'s). A failed `cobra convert` (unreadable / garbage CSV) thus
    // slipped straight past the catch to the fileExists() backstop below and
    // surfaced a generic "conversion failed for <path>", swallowing cobra's
    // own diagnostic. Mirror the dta branch above exactly: capture the result
    // and, on !ok, answer as Stata does (below). The fileExists() check stays
    // as the belt-and-suspenders backstop for the (ok && no output produced)
    // case.
    CobraRunResult cr = runCobra(args);  // `convert` writes status to stderr, nothing on stdout
    if (!cr.ok) {
      // Stata's `use` reads no text file: any .csv is "file X not Stata
      // format" r(610), and the data in memory are gone by then (StataBE
      // `use f.csv, clear` leaves k 0 N 0). Reading one is PIET's own
      // extension, so a file it cannot read faithfully -- BUG-414: a row wider
      // than the names line, whose extra field the converter used to drop at
      // rc 0 -- gets the same answer and the file as typed.
      std::string shown = strutil::trim(filepathArg);
      if (shown.size() >= 2 && shown.front() == '"' && shown.back() == '"')
        shown = shown.substr(1, shown.size() - 2);
      if (session.hasData()) (void)runClear("", session);
      // Stata's words exactly; the converter's reason is not part of them.
      return ojson(eval::errorBlock("engine_error", "file " + shown + " not Stata format",
                                    std::nullopt, std::nullopt, std::nullopt, "r(610)"));
    }
    if (!fileExists(out)) {
      return engineErrorBlock("use: conversion failed for " + filepath);
    }
    loadPath = out;
    convertedThisRun = true;
  }

  if (!k1bRestored) {
    counters::addUseDiskLoad();  // PP-L1: disk-open path (any format) actually taken
    // Self-reload witness (counters.h, report-only). Narrow on purpose: only a
    // re-read of a file THIS session wrote with `save` counts, because only
    // that one is provably work done twice. A first load of a foreign file is
    // irreducible and must not be allowed to inflate it.
    if (session.isSavedPath(filepath)) counters::addDatastoreReload();
    // STALE-FRAME GUARD (SessionState::residentDropToken carries the full
    // case): a `use` that reaches disk NEVER continues the resident chain, so
    // the worker's frame must be declared out of sync BEFORE load() gets a
    // chance to read `loadPath == residentToken_` as a chain continuation.
    //
    // That used to be two statements and a comment asking the next author to
    // remember the first one. It is now ONE statement that cannot be
    // half-written: FreshRead IS the drop (SessionState::LoadIntent). Every
    // other load() caller in the tree minted its own --out and continues the
    // chain, which is the default — so this is the single site that has to say
    // anything at all, and it says it in the call.
    session.load(loadPath, SessionState::LoadIntent::FreshRead);
  }
  session.clearMetadata();
  if (!dtaMetaPath.empty()) {
    // Apply AFTER clearMetadata() — mirrors the oracle's ordering
    // (_convert_to_hbin/session.load happens, THEN _apply_dta_var_labels
    // populates var_meta/value labels fresh for the newly-loaded dataset). On a
    // K1b hit dtaMetaPath is the memo's cached sidecar — re-applied identically,
    // so the label plane matches the disk path byte-for-byte.
    DtaMetaResult mr = applyDtaMeta(dtaMetaPath, session);
    if (!mr.ok) {
      return engineErrorBlock(mr.errorMessage, mr.errorCode);
    }
  }
  // xtset-file-persistence: a .dta's char table is the declaration — adopted
  // when present, cleared when absent (w2_eststo_prefix.log:441 B1 r(459);
  // :478-500 B2 xtreg/L./chars restored). A native file: see top. A csv states
  // none, so it stays cleared (as `import delimited` does).
  if (fmt == "dta") {
    session.adoptTsFromChars();
  } else if (fmt == "hbin") {
    nativeTsDecl();
  }
  // PP-L2A / K1a: memoize AFTER the conversion has proven itself all the way
  // through load() + the sidecar apply — an output that could not be loaded or
  // whose sidecar was rejected is not a result worth handing to the next `use`.
  // After load(), too, so the memo's own predecessor (if this source was
  // converted before and has since changed) is no longer the mapped head when
  // useConvCachePut releases it to the scratch GC. (k1bRestored ⇒
  // convertedThisRun is false, so this never fires on a stash hit.)
  if (convertedThisRun && !convCacheKey.empty()) {
    session.useConvCachePut(filepath, UseConvCacheEntry{convCacheKey, loadPath, dtaMetaPath});
  }
  // PP-L2B / K1b: stash the freshly-loaded head so the NEXT `use` of this
  // unchanged source restores from worker RAM. Only on a real disk load (a stash
  // hit already has its memo); only when the source was keyable (useCacheKey).
  // useStashSnapshot returns false when the resident plane is unavailable
  // (§26-only engine / serve down) — then we simply don't memoize and the next
  // `use` pays the disk path (honest tri-state fallback). loadPath here is the
  // on-disk twin (the conversion output for .dta/.csv, the source itself for
  // .hbin) that the restore re-adopts as a clean head; dtaMetaPath rides along
  // so the label plane is re-applied on the hit.
  if (!k1bRestored && !useCacheKey.empty()) {
    const std::string stashName = piet::SessionState::useStashName(filepath);
    if (session.useStashSnapshot(stashName)) {
      session.useStashCachePut(filepath,
                               UseStashCacheEntry{useCacheKey, stashName, loadPath, dtaMetaPath});
    }
  }
  // BUG-023 FIX (2.0-scope, deliberate divergence from the frozen 1.x/piet2
  // oracle — see the internal bug ledger BUG-023 disposition): earlier
  // M2/M3 milestones bug-for-bug replicated routes_data.py:94's `use`
  // unconditionally rewriting session.cwd to the loaded file's directory.
  // Real Stata NEVER moves cwd on `use` — relative paths keep resolving
  // against the session's own working directory. This bit the analysis.do
  // acceptance directly: its tempfile+use checkpoint idiom (`use
  // "`tempfile'", clear`) dragged cwd into a temp dir, breaking every later
  // relative `graph export "result/..."`. Per the BUGLOG disposition, the
  // 2.0 core does NOT replicate this bug — session.cwd is intentionally
  // left untouched here. This is a KNOWN, pre-authorized divergence from
  // the frozen piet2 corpus's `use`-into-tempdir-adjacent cells; Eval
  // regenerates the affected corpus cells to reflect the fix (maintainer-routed).
  session.invalidateDatastore();
  Datastore* ds = session.datastore();
  // BUG-198 (load half): a native .hbin states its own display formats and
  // variable labels in the header (P1-A0 `format`, BUG-198 `label`). Adopt them
  // as this session's overrides so the layer survives use -> mutate -> save and
  // reaches every varMeta consumer (`save X.dta`'s --in-meta sidecar,
  // `tostring, usedisplayformat`, describe) — not just the render fallback that
  // reads the card directly.
  //
  // SKIPPED on the .dta/.csv branches, deliberately: `dtaMetaPath` non-empty
  // means applyDtaMeta() just populated varMeta from the SIDECAR, which is
  // strictly richer (value-label sets, declared Stata types) and is the
  // authority for that format. The hbin under it is a conversion artifact, and
  // re-seeding from it here would let the artifact overwrite the source.
  if (dtaMetaPath.empty()) session.seedVarMetaFromLoadedFile(ds);
  // P8 "Honest Cockpit" (U1): record the RESOLVED user-facing path (not
  // loadPath, which may be a scratch hbin conversion target for .dta/.csv
  // sources) now that the load has actually succeeded (ds() above would have
  // thrown on a bad hbin, so reaching here is the true success checkpoint).
  session.setCurrentFile(filepath);
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "open";
  // Additive (contract_B v2.1.0): source_file is the canonical field (mirrors
  // GET /api/varlist's source_file); filepath is the same value, kept for the
  // frontend's existing action:"open" read site. Both cover console `use`/
  // `sysuse` (blocks[]) and POST /api/open (routes.cpp's open()) since both
  // funnel through this one runUse().
  out["source_file"] = session.currentFile();
  out["filepath"] = session.currentFile();
  ojson varlist = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlist.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: refresh the cache for the newly-loaded dataset
  out["varlist_after"] = varlist;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  // RED-fix (piet_2.1.0_gate VERDICT.md Leg 3, test_postfile_using_replace_
  // roundtrips#5): skip the honesty note when THIS session's own postfile
  // machinery is what wrote the hbin bytes under this exact resolved path
  // (the routine `tempfile pf` / `postfile ... using \`pf'` / `postclose` /
  // `use \`pf'` idiom) — there is nothing to explain to the user about a
  // file they didn't misname; the sniff/format-redirect above still applies
  // unchanged (content still decides how to load). A genuinely external or
  // stale-save-era mislabeled file (not in postfileWrittenPaths) still gets
  // the note below.
  //
  // BUG-085 UX residual (SE-044/045 follow-on): a `.tmp` name ALSO gets no
  // note. `.tmp` is PIET's OWN minted tempfile suffix (runTempfile,
  // language/temporary_names.cpp) and — exactly as the save handler treats it
  // (dataset_save.cpp accepts `.tmp` as a dta-format name) — a `.tmp` name makes
  // NO format claim, so there is nothing dishonest to flag: the classroom-core
  // `tempfile p` / `save "\`p'"` / `use "\`p'"` checkpoint idiom writes hbin
  // under `.tmp` and reads it straight back. This is a name-based carve-out
  // (parallel to save accepting `.tmp`), not a written-path lookup, so it also
  // covers the save-to-`.tmp` half that postfileWrittenPaths never tracks. A
  // foreign-ext misnaming (e.g. stale-era `.dta`) still gets the note.
  if (hbinMismatch && (session.isPostfileWrittenPath(filepath) || origExt == ".tmp")) {
    hbinMismatch = false;
  }
  if (hbinMismatch) {
    // Honesty note (I-3): the file's NAME said `origExt` but its CONTENT was
    // PIET's native .hbin — say so, rather than silently loading it as if
    // nothing were odd. dispatcher.cpp's `cmd == "use"` arm lifts this into
    // its own prepended echo block for the console/do-file path.
    out["note"] = "`" + filepath + "` is named `" +
                  (origExt.empty() ? "(no extension)" : origExt) +
                  "` but its content is PIET's native .hbin format (this happens with files "
                  "saved before the `.dta`-honesty fix, or any file simply misnamed) — loaded "
                  "it as .hbin. Rename it to end in `.hbin` to avoid this note next time.";
  }
  return out;
}

}  // namespace piet::handlers
