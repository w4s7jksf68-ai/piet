#include "piet/handlers.h"
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
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "piet/strutil.h"
#include "piet/translate.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabMeanProportion = piet::eval::registerOptVocab(
    {"mean", "proportion"}, {{"level"},
     {}});
// W2 estimopts (2026-09-19, BUG-539): reghdfe's `residuals[(newvar)]` and
// `noabsorb`, matched as RAW keys inside runRegress and deliberately NOT
// registered with registerOptVocab. The registry keeps one slot per command
// token and hands back the LAST token's slot, and kVocabIvRegress
// (estimation_m4.cpp) is registered on {ivregress, ivreg, ivreghdfe, reghdfe}
// -- so it IS the `reghdfe` slot, and it is the known-set runIvRegress's
// reject net reads (estimation_m4b.cpp) for ivregress/ivreg2/xtivreg2/IV
// reghdfe too. A registration here unioned `resid`/`noabsorb` into that net:
// `ivregress 2sls y (x=z), resid` passed at rc 0 with the option unread
// (review finding, attempt 2). Unregistered, the dispatcher canonicaliser
// leaves these keys exactly as typed (no member of the reghdfe set starts
// with `re` or `noa`), so every accepted spelling is listed here in full.
// reghdfe.ado:132 declares `RESiduals(name) RESiduals2`, floor `res`, so
// `re` is NOT one of them -- w2_smalls.log reghdfe_bare_re is r(198), which
// runRegress's own reject net answers. reghdfe.ado:181 declares `noAbsorb2`,
// floor `noa`, full spelling `noabsorb2`.
const std::unordered_set<std::string> kReghdfeResidSpellings = {
    "res", "resi", "resid", "residu", "residua", "residual", "residuals"};
const std::unordered_set<std::string> kReghdfeNoAbsorbSpellings = {
    "noa", "noab", "noabs", "noabso", "noabsor", "noabsorb", "noabsorb2"};
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

bool fileExists(const std::string& path) { return cecon::native_exists(path); }

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string ext(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
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

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
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

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// cobra_runner.py's `CobraError.__str__`: f"[{code}] {message}" — the
// dispatcher's universal `except CobraError as ce: ...(str(ce), code=ce.code)`
// pattern relies on this bracket-prefixed message (errors.py's error_block
// then coerces `code` to "engine_error" when it's not in the frozen
// ERROR_CODES allow-list, but the [code] prefix survives in the message
// either way — see e.g. the `[ts_no_tsset]` carve-out cells). Every
// CobraError catch site in this file must format its message through this
// helper, not ce.what() alone, to stay byte-identical with the oracle.
//
// BUG-038(a): the "[code] message" formatting itself now lives at ONE
// shared mapping point (piet::cobraErrorMessage, cobra_runner.h) that every
// such formatter across the codebase forwards to — this function stays a
// thin wrapper so its call sites in this file didn't need touching. The
// only BEHAVIOR change is cobra's `internal` code (its generic C++-
// exception catch-all, e.g. an unimplemented expression builtin): that one
// code no longer surfaces its raw "[internal] ..." shape verbatim (see
// cobra_runner.h's header comment for the two honest rewrites); every other
// code's bracket-prefixed format, and thus oracle byte-parity, is
// unchanged.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> --vcov twoway --cluster A,B (cobra's feols
// --vcov surface accepts "twoway" as a 4th mode alongside iid/hc1/cluster —
// live-verified against the D1i reghdfe 2-way oracle: `reghdfe income
// beertax, absorb(state) vce(cluster state year)` -> beertax se=1867.524
// matches byte-for-byte through this exact shape). One var -> the
// pre-existing single-cluster path, unchanged. Three+ vars -> --vcov
// multiway --cluster A,B,C for reghdfe only (allowMultiway,
// engine-multiway-cluster3, BUG-604); every other command gets a named
// refusal (unsupported_option, rc 198). Mirrors estimation_m4.cpp's
// own splitClusterVarsM4 — kept as a separate TU-local copy (same
// established pattern as this file's lower()/cobraErrorStr() thin wrappers)
// rather than introducing a new shared header for one small helper.
struct ClusterSplitResult {
  std::vector<std::string> vcovArgv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitResult splitClusterVars(const std::string& raw, bool allowMultiway = false) {
  ClusterSplitResult r;
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) {
    r.errored = true;
    r.errorMsg = "cluster variable required";
    return r;
  }
  if (toks.size() == 1) {
    r.vcovArgv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.vcovArgv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else if (allowMultiway) {
    std::string joined = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) joined += "," + toks[i];
    r.vcovArgv = {"--vcov", "multiway", "--cluster", joined};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe only";
  }
  return r;
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
      std::string label = v.value("label", std::string());
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
          codeMap[cit.key()] = cit.value().get<std::string>();
          try {
            numericMap[std::stoi(cit.key())] = cit.value().get<std::string>();
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
  }

  // characteristics: [{varname, charname, value}, …] -> session.characteristics,
  // verbatim — exactly export-dta --in-meta's own parse (cmds_data.cpp) so the
  // sidecar this reads and the one `save` re-emits (writeDtaMetaSidecar,
  // handlers_data_verbs.cpp) agree on shape.
  if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
    for (const auto& ch : meta["characteristics"]) {
      if (!ch.is_object()) continue;
      DtaCharacteristic c;
      if (ch.contains("varname") && ch["varname"].is_string())
        c.varname = ch["varname"].get<std::string>();
      if (ch.contains("charname") && ch["charname"].is_string())
        c.charname = ch["charname"].get<std::string>();
      if (ch.contains("value") && ch["value"].is_string())
        c.value = ch["value"].get<std::string>();
      if (c.varname.empty() || c.charname.empty()) continue;
      session.characteristics.push_back(std::move(c));
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

  if (!fileExists(filepath)) {
    return engineErrorBlock("File not found: " + filepath);
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
    // and, on !ok, parse cobra's structured {"error":{"code","message"}}
    // stderr (the SAME shape errorFromResult() parses) so the honest engine
    // message — naming what actually went wrong — reaches the user. The
    // fileExists() check stays as the belt-and-suspenders backstop for the
    // (ok && no output produced) case.
    CobraRunResult cr = runCobra(args);  // `convert` writes status to stderr, nothing on stdout
    if (!cr.ok) {
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
        if (message.empty()) message = "use: .csv conversion failed for " + filepath;
      }
      return engineErrorBlock(std::string("use: ") + message, code);
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
  // oracle — see tests/kitchensink/BUGLOG.md BUG-023 disposition): earlier
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
  // regenerates the affected corpus cells to reflect the fix (MILF-routed).
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
  // handlers_m3.cpp) and — exactly as the save handler reasons
  // (handlers_data_verbs.cpp: "`.tmp` implies nothing") — a `.tmp` name makes
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

// ---------------------------------------------------------------------------
// gen / replace

namespace {

// Splits "NEWVAR = RHS" into {newvar, rhs} (mutate.py's manual split on the
// FIRST '='). Returns {expr,""} unsplit if there is no '='.
std::pair<std::string, std::string> splitAssign(const std::string& expr) {
  auto pos = expr.find('=');
  if (pos == std::string::npos) return {expr, ""};
  std::string lhs = expr.substr(0, pos);
  std::string rhs = expr.substr(pos + 1);
  auto trim = [](std::string s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  return {trim(lhs), trim(rhs)};
}

}  // namespace

ojson runGen(const std::string& expr, const std::optional<std::string>& ifExpr,
             SessionState& session, const std::string& storeType) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string outPath = nextPath(session.workdir(), "gen");
  std::string newvar, genExpr;
  std::string genMask;   // B3(c): see exprHasRunningSum -- sum() cannot be cond-folded
  if (ifExpr.has_value() && !ifExpr->empty()) {
    auto [nv, rhs] = splitAssign(expr);
    newvar = nv;
    if (rhs.empty()) rhs = ".";
    if (piet::eval::exprHasRunningSum(rhs)) {
      genExpr = newvar + "=" + rhs;
      genMask = *ifExpr;
    } else {
      genExpr = newvar + "=cond((" + *ifExpr + "),(" + rhs + "),.)";
    }
  } else {
    genExpr = expr;
    auto [nv, rhs] = splitAssign(expr);
    newvar = rhs.empty() && expr.find('=') == std::string::npos ? "" : nv;
  }
  // BUG C (W3 fuzzer stabilization, shared with strmutate.cpp::runStringGen
  // and handlers_data_verbs.cpp::runRename — piet::strutil::isValidStataIdentifier
  // is the ONE validator all three call): this numeric path used to ship
  // `newvar` straight to `cobra eval --gen` with no check at all, so
  // `gen 9zzz = 1` silently created a column named "9zzz" (cobra's hbin
  // header is a bare JSON string field — it has no opinion on Stata's
  // identifier rule).
  if (!newvar.empty() && !strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "generate: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  if (!genMask.empty()) { args.push_back("--gen-mask"); args.push_back(genMask); }
  // BUG-471 T2: the resolved storage type (the dispatcher's resolver) — the
  // engine stores float(v) / trunc(v), not the full double under a label.
  storage::appendStoreTypeArg(args, storeType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  args = translate::injectTsContextForExpr(args, genExpr, session);
  args = translate::injectRngContextForExpr(args, genExpr, session);
  // Stage 1b: opt this pure-add mutation into the resident frame plane — its
  // --out becomes the head and is never read back as a file (session.load() +
  // the mutationResult shadow card render it), so the engine advances the
  // resident frame in RAM with no per-verb scratch write.
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    // dispatcher.py's generic native_handler wrapper: `except CobraError as
    // ce: make_engine_error(str(ce), code=ce.code)` — str(ce) is the
    // bracket-prefixed "[code] message" (cobra_runner.py's CobraError.__str__).
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  std::vector<std::string> newVars;
  if (!newvar.empty()) newVars.push_back(newvar);
  return mutationResult(session, "gen", newVars);
}

ojson runReplace(const std::string& expr, const std::optional<std::string>& ifExpr,
                 SessionState& session, const std::string& storeType, bool noPromote) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  auto [varname, rhs] = splitAssign(expr);
  if (rhs.empty() && expr.find('=') == std::string::npos) {
    return engineErrorBlock("replace requires `varname = expr`");
  }
  // BUG C: same shared validator as runGen (see its comment above) — an
  // invalid target name here can only mean a malformed command (a genuinely
  // EXISTING variable was always a valid identifier when it was created),
  // so this is a defensive, low-frequency check kept for symmetry with
  // gen/rename rather than a commonly-hit path.
  if (!strutil::isValidStataIdentifier(varname)) {
    return engineErrorBlock(
        "replace: invalid variable name `" + varname + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  std::string outPath = nextPath(session.workdir(), "replace");
  std::string genExpr;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    genExpr = varname + "=cond((" + *ifExpr + "),(" + rhs + ")," + varname + ")";
  } else {
    genExpr = varname + "=" + rhs;
  }
  // BUG-471 T2: the type the store takes. An integer-typed variable receiving
  // a value it cannot hold widens by Stata's lattice before the ONE store
  // (replace_byte_promote :810 ... replace_int_overflow :893); the decision reads
  // the touched values through a read-only engine eval of the same expression
  // into a scratch column of a projection — no dispatch(), so r()/e() are what
  // they were. Engine calls per replace: float/double/unrecorded target 1; an
  // integer target under nopromote, or with a literal the type holds, or with an
  // unqualified literal, 1; any other integer target 2 (probe + store), 3 when
  // the head is resident (+ the column fetch).
  std::string finalStoreType = storeType;
  {
    const bool qualified = ifExpr.has_value() && !ifExpr->empty();
    const std::string probeRhs =
        qualified ? "cond((" + *ifExpr + "),(" + rhs + "),.)" : rhs;
    storage::ReplaceTypeDecision dec = storage::decideReplaceStoreType(
        session, storeType, noPromote, varname, rhs, qualified, "--gen",
        std::string(storage::kReplaceProbeVar) + "=" + probeRhs,
        {rhs, ifExpr.value_or(std::string())}, /*drawContext=*/false);
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  storage::appendStoreTypeArg(args, finalStoreType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  args = translate::injectTsContextForExpr(args, genExpr, session);
  // Stage 1b: opt this in-place rewrite into the resident frame plane (see
  // runGen) — its --out becomes the head, never read back as a file.
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    // Nothing was stored, so the label stays the type the column still holds.
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  storage::recordReplaceType(session, varname, storeType, finalStoreType);
  // F9: replace rewrites ONLY `varname` in place — name it so the nMiss seed
  // carries every other column instead of a full rescan. rewrittenVars is
  // the shared mutationResult's 5th (LAST) parameter, not its 3rd — pass
  // newVars={} and droppedVars={} explicitly so `varname` lands in the
  // rewrittenVars slot, not droppedVars (the consolidation hazard this
  // call site is the one real instance of: this file's old local
  // mutationResult had rewrittenVars in position 3; the shared signature's
  // position 3 is droppedVars).
  return mutationResult(session, "gen", {}, {}, {varname});
}

// ---------------------------------------------------------------------------
// describe

namespace {

// C4-L2B (D1 card plane): the per-variable describe entry, shared by the card
// plane and the datastore plane so the two renders cannot drift — every field
// except n_unique's f64 branch is metadata the resident card already carries
// (dtype vocabulary, n_missing) or session-side meta (stataType/format/label/
// val_label), and the field ORDER here is the frozen wire order.
// BUG-145/FIX:DESCRIBE-FORMAT-CARRIAGE: fileFormat carries the file-derived
// display format (Datastore::col(name).format, "" when absent — the same
// carrier P1-A0B wired into browse()'s columnsMeta). Fallback order matches
// browse() exactly: session `format VAR %fmt` override > file-derived format
// > hardcoded per-dtype default. Passing "" here (the card-plane fast path
// below has no reliable file-derived source — shadowCard_ entries seeded via
// seedShadowFromFrameVars() never carry it) reproduces the pre-fix default
// exactly, so that call site is unaffected.
ojson describeEntry(const SessionState& session, const std::string& name, bool isFactor,
                    int64_t nMissing, int64_t nUnique, const std::string& fileFormat) {
  auto metaIt = session.varMeta.find(name);
  std::string stataType;
  std::string fmt;
  if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
    stataType = *metaIt->second.stataType;
  } else {
    stataType = isFactor ? "str#" : "float";
  }
  if (metaIt != session.varMeta.end() && metaIt->second.format.has_value()) {
    fmt = *metaIt->second.format;
  } else if (!fileFormat.empty()) {
    fmt = fileFormat;
  } else {
    fmt = isFactor ? "%9s" : "%9.0g";
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
                                        *vi->nLevels, /*fileFormat=*/""));
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
                                       nUnique, ds->col(name).format));
  }

  ojson out;
  out["type"] = "describe";
  out["variables"] = variables;
  return out;
}

// ---------------------------------------------------------------------------
// summarize

namespace {

ojson numpyStats(const std::vector<double>& valid, bool detail) {
  int64_t n = static_cast<int64_t>(valid.size());
  ojson s;
  if (n == 0) {
    s["N"] = 0;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    // BUG-344: count and sum stay well-defined on a degenerate (all-missing) column —
    // Stata posts r(N)=0 and r(sum)=0 there, NOT missing (the same rule cmds_summary.cpp
    // already follows for by-groups). Only mean/sd/min/max read as missing.
    s["sum"] = 0.0;
    s["sum_w"] = 0.0;
    return s;
  }
  double sum = 0.0;
  for (double v : valid) sum += v;
  double mean = sum / static_cast<double>(n);
  double sd = 0.0;
  if (n > 1) {
    double ss = 0.0;
    for (double v : valid) ss += (v - mean) * (v - mean);
    sd = std::sqrt(ss / static_cast<double>(n - 1));
  }
  double mn = *std::min_element(valid.begin(), valid.end());
  double mx = *std::max_element(valid.begin(), valid.end());
  s["N"] = n;
  s["mean"] = mean;
  s["sd"] = sd;
  s["min"] = mn;
  s["max"] = mx;
  // BUG-344: this path already summed the column to get the mean — it just never
  // published the total, so captureRFromBlock's r(sum)/r(sum_w) mapping (which has
  // been waiting for these keys all along) found nothing to map. This fallback is
  // the UNWEIGHTED path by construction (the weighted arm errors rather than falling
  // back here), hence sum_w = N.
  s["sum"] = sum;
  s["sum_w"] = static_cast<double>(n);
  if (detail) {
    std::vector<double> sorted = valid;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> double {
      // numpy.percentile linear-interpolation method (matches _numpy_stats' np.percentile default).
      double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
      int64_t lo = static_cast<int64_t>(std::floor(idx));
      int64_t hi = static_cast<int64_t>(std::ceil(idx));
      if (lo == hi) return sorted[static_cast<size_t>(lo)];
      double frac = idx - static_cast<double>(lo);
      return sorted[static_cast<size_t>(lo)] * (1.0 - frac) + sorted[static_cast<size_t>(hi)] * frac;
    };
    s["p1"] = pct(1);
    s["p25"] = pct(25);
    s["p50"] = pct(50);
    s["p75"] = pct(75);
    s["p99"] = pct(99);
  }
  return s;
}

}  // namespace

// BS-092 (BETA campaign, Phase G): the pure per-Datastore stats computation
// runSummarize below already did, hoisted into its own function so
// dispatcher.cpp's new `by:`/`bysort: summarize` grouped-dispatch loop can
// invoke the EXACT SAME computation once per by-group's own scratch slice,
// rather than re-deriving/duplicating this logic. Depends on nothing but
// `ds` itself (no SessionState, no ifExpr/inRange — those are already
// resolved into whichever Datastore the caller hands in), so it composes
// cleanly with BS-090's if/in prefilter (runSummarize resolves `ds` first,
// same as before) and BS-092's by-group slicing (dispatcher.cpp resolves
// `ds` to one group's slice) alike.
// P3 weights wave: the actual per-Datastore stats computation, parameterized
// on which cobra weight FLAG to push ("--fweight" vs "--aweight") — shared by
// summarizeCoreStats (below, [fw=] path, unchanged signature/behavior for
// freeze-preservation) and summarizeCoreStatsAweight (the new [aw=] path;
// registry.cpp's summarize row is now AweightFweight). piet/handlers.h is
// out of this batch's edit manifest (owned by a different stint) and its
// summarizeCoreStats/runSummarize declarations are frozen by existing
// callers (dispatcher.cpp's by-group summarize loop, describe's internal
// stats pull) — this helper stays file-local (not header-declared) so those
// two public signatures never change; only their SHARED body moved here.
ojson summarizeCoreStatsWeighted(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                  bool detail, const std::optional<std::string>& weightVar,
                                  const char* weightFlag) {
  if (varlist.has_value()) {
    for (const auto& n : *varlist) {
      if (!ds.has(n)) {
        return engineErrorBlock("variable " + n + " not found");
      }
    }
  }
  std::vector<std::string> names = varlist.has_value() ? *varlist : ds.names();
  std::vector<std::string> f64Vars, facVars;
  for (const auto& n : names) {
    if (!ds.has(n)) continue;
    (ds.dtype(n) == "f64" ? f64Vars : facVars).push_back(n);
  }

  ojson stats = ojson::object();

  if (!f64Vars.empty()) {
    std::string joined;
    for (size_t i = 0; i < f64Vars.size(); ++i) {
      if (i) joined += ",";
      joined += f64Vars[i];
    }
    std::vector<std::string> args = {"eval", "--data", ds.path(), "--summarize", joined};
    if (detail) args.push_back("--detail");
    bool hasWeight = weightVar.has_value() && !weightVar->empty();
    if (hasWeight) {
      args.push_back(weightFlag);
      args.push_back(*weightVar);
    }
    bool usedCobra = false;
    try {
      ojson out = runCobraJson(args);
      ojson raw = out.value("summarize", ojson::object());
      for (const auto& v : f64Vars) {
        if (raw.contains(v)) {
          stats[v] = raw[v];
        }
      }
      usedCobra = true;
    } catch (const CobraError& ce) {
      // gate-①: numpyStats below is UNWEIGHTED. Falling back to it silently
      // when a weight was requested would answer a different question than
      // the one asked (silent-wrong-number parity violation) — worse than
      // erroring. Only the no-weight path is allowed to use this fallback;
      // with a weight present, propagate the cobra failure instead.
      if (hasWeight) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      usedCobra = false;
    }
    if (!usedCobra) {
      for (const auto& v : f64Vars) {
        const cecon::Column& c = ds.col(v);
        const double* d = c.f64();
        int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        stats[v] = numpyStats(valid, detail);
      }
    }
  }

  for (const auto& v : facVars) {
    const cecon::Column& c = ds.col(v);
    const int32_t* d = c.codes();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(int32_t));
    int64_t n = 0;
    for (int64_t i = 0; i < count; ++i) {
      if (d[i] >= 0) ++n;
    }
    ojson s;
    s["N"] = n;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    stats[v] = s;
  }

  ojson out;
  out["type"] = "summarize";
  out["detail"] = detail;
  out["stats"] = stats;
  return out;
}

ojson summarizeCoreStats(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                         bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--fweight");
}

// ONESHOT-LASTFLUSH: what `summarize VARLIST if COND` needs on disk is VARLIST plus
// whatever the condition names — never the whole frame. Filtering the head instead
// meant flushing the entire resident frame first (28 of the flushes left after the
// §38 wave, every one of them a caller that only reads a handful of columns), so
// fetch the projection and filter THAT. Returns the path the row filter should run
// against; every refusal returns the head, which is the pre-existing behavior.
//
// `summarize` with no varlist reads ds.names() — every column — so it is excluded
// by the first test rather than by hoping the scanner finds nothing.
std::string summarizeFilterBase(SessionState& session,
                                const std::optional<std::vector<std::string>>& varlist,
                                const std::optional<std::string>& weightVar,
                                const std::optional<std::string>& ifExpr) {
  const std::string head = *session.currentHbin();
  if (!varlist.has_value() || varlist->empty()) return head;
  std::string named;
  for (const auto& v : *varlist) named += v + " ";
  if (weightVar.has_value()) named += *weightVar;
  auto cols = referencedColumns(session, {&named, ifExpr.has_value() ? &*ifExpr : nullptr}, "");
  if (!cols.has_value()) return head;
  std::string projPath;
  std::unique_ptr<Datastore> proj = fetchColumns(session, *cols, session.workdir(), &projPath);
  if (!proj) return head;
  // The scanner only takes names the shadow card confirms, so a varlist entry the
  // card does not know about is simply absent from the projection. Summarizing it
  // would then report "variable not found" for a column that DOES exist — a wrong
  // answer, not a slow one. Prove the projection carries the whole request instead.
  for (const auto& v : *varlist)
    if (!proj->has(v)) return head;
  if (weightVar.has_value() && !weightVar->empty() && !proj->has(*weightVar)) return head;
  return projPath;
}

// P3 weights wave: the [aw=] sibling of summarizeCoreStats above — same
// computation, --aweight instead of --fweight. NOT declared in
// piet/handlers.h (out of this batch's edit manifest); forward-declared
// locally where dispatcher.cpp's summarize arm needs it.
ojson summarizeCoreStatsAweight(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--aweight");
}

ojson runSummarize(const std::optional<std::vector<std::string>>& varlist, bool detail,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& weightVar,
                   const std::optional<std::string>& inRange) {
  // `summarize ... if COND [in RANGE]`: apply the row filter FIRST (BS-090:
  // `if`/`in` composed via AND, piet::handlers::applyIfAndInFilter) into a
  // transient hbin, then summarize the filtered data with the ordinary path
  // below. This keeps the numeric (cobra --summarize) and factor/fallback
  // paths seeing the SAME restricted sample, and reuses the validated
  // keepif rather than re-deriving a row mask natively.
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  return summarizeCoreStats(*ds, varlist, detail, weightVar);
}

// P3 weights wave: the [aw=] sibling of runSummarize above — identical if/in
// pre-materialization, routes to summarizeCoreStatsAweight instead. NOT
// declared in piet/handlers.h (out of this batch's edit manifest); forward-
// declared locally in dispatcher.cpp, which picks this vs. runSummarize off
// parsed.weightType (aw/aweight vs fw/fweight — registry.cpp's summarize row
// is AweightFweight, so those are the only two spellings that reach here).
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange) {
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  return summarizeCoreStatsAweight(*ds, varlist, detail, weightVar);
}

// ---------------------------------------------------------------------------
// regress / reg / feols (plain OLS/feols vertical slice: iid/hc1/cluster
// vcov, optional --fe via absorb(), noconstant, level(#). IV/xtreg-RE/
// multi-outcome routing is OUT of scope — M3.)

namespace {

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// BUG-102: TU-local copy of estimation_m4.cpp's substituteFactorLevelLabelM4/
// applyFactorValueLabelsM4 (handlers.cpp's cobraToEstimation is the separate
// M2/regress-only slice mentioned above — no shared header links the two
// anonymous namespaces, and strutil.h is deliberately dependency-light, so
// this follows the same duplicate-and-keep-in-sync pattern strutil.h's own
// doc comment describes for the TUs it hasn't swept yet). A factor-variable
// coefficient row's engine name is `<level>.<var>` (BUG-101, model.cpp:526),
// or `<level>.<var>#<level>.<var>` for interactions; real Stata substitutes
// the attached value label for the bare level when displaying such a row
// (verified live, StataBE 19.5). PIET's flat one-row-per-coefficient table
// keeps the ".varname" suffix (no shared group-header the way Stata's ASCII
// table has one) — only the level segment is substituted.
std::string substituteFactorLevelLabel(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

void applyFactorValueLabels(ojson& coeftable, const SessionState& session) {
  // BUG-397 (2026-09-02, the g12 oracle-first leg's FIX:BUG102-COEFTABLE-NAME-PARITY): the row's
  // `name` is Stata's e(b) colname and is INVARIANT under value labels (`1983.yearpl` stays
  // `1983.yearpl`; `_b[1983.yearpl]` must keep resolving). The label is DISPLAY: it rides on two
  // additive fields — `stub` = the BUG-102 display shape with the level segment substituted
  // (`Odd83.yearpl`), which the frontend prints in place of the name; `label` = the bare label
  // text of the (last) substituted level (`Odd83`), Stata's own stub. Rows without a label get
  // neither field, so every unlabeled fixture is byte-identical.
  if (session.valueLabels.empty() || !coeftable.is_array()) return;
  for (auto& row : coeftable) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
    const std::string name = row["name"].get<std::string>();
    std::string sub = substituteFactorLevelLabel(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

// _cobra_to_estimation (dispatcher.py) — normalise cobra JSON -> EstimationResult.
// M2 slice: does NOT port the margins AME b=null remap (margins isn't in the
// M2 handler set) — pass-through only. Field list and truthy-check semantics
// (`if raw.get("absorbed")` / `if vcov:` — Python empty-dict/empty-string are
// falsy, so an empty `{}`/`""` is DROPPED, not passed through) mirror
// dispatcher.py's "Optional fields — standard pass-through" block exactly.
// `session` optional (default nullptr) mirrors estimation_m4.cpp's M4a copy.
ojson cobraToEstimation(const ojson& raw, const std::string& cobraCmd,
                         const SessionState* session = nullptr) {
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = raw.value("estimator", cobraCmd);
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.value("n_obs", 0);
  if (raw.contains("coeftable")) block["coeftable"] = raw["coeftable"];
  if (raw.contains("coeftable") && session) applyFactorValueLabels(block["coeftable"], *session);
  if (raw.contains("outcomes")) block["outcomes"] = raw["outcomes"];

  // `if k in raw` — presence-only pass-through (no truthiness check).
  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      "adj_r2", "rmse", "mss", "rss", "df_m", "df_r", "ll", "ll_0", "r2_pseudo", "df_chi2",
      "r2_between", "r2_overall",
      // BUG-156 (wiring half, P2-PREP): poisson/fepois `chi2_df`/`r2_p` and
      // feols single-FE `sigma_u`/`sigma_e`/`rho`/`corr_u_xb`. `chi2`/`ll`/
      // `ll_0` are already above. This is the TWIN of estimation_m4.cpp's
      // kGlm/kXtreg lists — the same allowlist exists in both files and a
      // one-sided edit makes the M2 and M4a paths publish different blocks
      // for the same fit (BUG-158/BUG-161 class). Keep them in step.
      // The engine does not emit these yet; that half of BUG-156 is compute
      // work in a parallel workstream.
      "chi2_df", "r2_p", "sigma_u", "sigma_e", "rho", "corr_u_xb",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  // BUG-112(8), consumption half — and this is the M2 twin of the identical
  // carry in estimation_m4.cpp::cobraToEstimationM4. `regress` routes HERE, not
  // there, so a one-sided landing would have surfaced the engine's note for
  // some estimators and silently dropped it for the most common one (measured:
  // the M4a copy alone left `regress ... ib99.g` still silent). cobra emits
  // non-fatal model-spec notes as a top-level `warnings` array (contract A,
  // absent when empty) — Stata's `note:` line. Absent-when-empty on this side
  // too, so a clean estimation block stays byte-identical.
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  // `if raw.get("absorbed"):` — truthy check: an empty {} is dropped.
  if (raw.contains("absorbed") && !raw["absorbed"].empty()) {
    block["absorbed"] = raw["absorbed"];
  }
  // `if vcov:` — truthy check: an empty/absent string is dropped.
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

// BUG-569 (review round): the VCE options' value SHAPE. `cluster` takes a
// variable and `robust` takes none. The VCE loop in runRegress reads a bare
// `cluster` (or an empty `cluster()`) as "no cluster variable", which pushes no
// VCE, and it skips a valued `robust(x)`. The unknown-option net passes both
// names, so either one fitted the iid model at rc 0: a silent wrong answer.
// Measured on the branch binary 2026-09-29: `regress y x1, cluster`,
// `areg y x1, a(id) cl` and `areg y x1, a(id) rob(id)` each returned rc 0 with
// e(vce) iid. Stata refuses a malformed option with r(198). The wording below is
// PIET's own; no golden pins Stata's message for these two shapes. Twin:
// estimation_m4.cpp vceOptionShapeErrorM4 (piet/handlers.h, where one shared
// copy belongs, is outside this change).
std::optional<std::string> vceOptionShapeError(const eval::ParsedCommand& parsed) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && (v.isBool || strutil::trim(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// reghdfe's own spelling of the conventional VCE: `vce(unadjusted)`.
// g11_fe_header_scalars.log and reghdfe_absorb_vce.log run `reghdfe y x1 x2,
// absorb(id) vce(unadjusted)` at rc 0 with the non-robust SEs (x1 .0394148);
// runRegress answered "Unknown vce type: unadjusted" r(198). EXACT token
// (case-insensitive) only, and reghdfe only: no captured arm measures an
// abbreviation, nor the spelling under regress/areg, which are left as before.
bool isReghdfeUnadjustedVce(const std::string& raw) {
  const size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return false;
  const size_t z = raw.find_last_not_of(" \t");
  std::string tok = raw.substr(a, z - a + 1);
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return tok == "unadjusted";
}

// e(df_a) -- the absorbed degrees of freedom -- for the two absorbing words
// runRegress serves, derived from the engine's feols reply without new engine
// fields: `absorbed` ({fe name: levels}), `k_absorbed` (dof.k_fe: the rank of
// the FE dummies, redundancy removed; 1 FE = its levels, 2 FE = K1 + K2 -
// connected components), `fe_exact` (false = 3+ FE, k_fe only an upper
// bound) and `nested_fe` (the FEs nested in a cluster variable). nullopt =
// not derivable, and nothing is posted (e(df_a) stays missing).
//
//   areg:    df_a = k_absorbed - 1. areg absorbs exactly one variable and
//            keeps _cons in e(b), so one level is the constant's: w2b_estim.log
//            PREC areg_a(id)/areg_ab(id)/areg_abs(id)/areg_absorb(id): 10
//            categories -> df_a 9, df_r 48 = 60 - 2 slopes - 1 - 9. areg charges
//            every level whatever the VCE (BUG-568 keeps nested FEs in its
//            cluster dof), so nested_fe does not enter.
//   reghdfe: df_a = k_absorbed - (levels of every FE nested in the cluster),
//            reghdfe's "Categories - Redundant = Num. Coefs" column summed.
//            Redundant = the connected-components term already inside
//            k_absorbed: g11_fe_header_scalars.log absorb(id) df_a 100 (100
//            categories, 0 redundant); absorb(id t) 100 + (8 - 1) = 107;
//            aer_b4/predict-e-xbd.log:348 absorb(id f2) df_a 49 = 43 + (7 - 1).
//            A nested FE is ALL redundant: w2b_estim.log reghdfe_cluster_ctl
//            (absorb(id) vce(cluster id)): 10 categories, 10 redundant, df_a 0.
//            Not posted (no oracle arm fixes them): a nested FE alongside
//            another FE (how reghdfe then re-assigns the connected-components
//            redundancy is not measured), and 3+ FEs (fe_exact false: the
//            engine's bound skips the first pair's components, reghdfe's
//            pairwise count does not).
std::optional<double> absorbedDofA(const ojson& raw, const std::string& stataCmd) {
  if (!raw.is_object() || !raw.contains("k_absorbed") || !raw["k_absorbed"].is_number() ||
      !raw.contains("absorbed") || !raw["absorbed"].is_object() || raw["absorbed"].empty()) {
    return std::nullopt;
  }
  const double kFe = raw["k_absorbed"].get<double>();
  const ojson& absorbed = raw["absorbed"];
  if (stataCmd == "areg") {
    if (absorbed.size() != 1 || kFe < 1.0) return std::nullopt;
    return kFe - 1.0;
  }
  if (stataCmd != "reghdfe") return std::nullopt;
  if (raw.contains("fe_exact") && raw["fe_exact"].is_boolean() && !raw["fe_exact"].get<bool>()) {
    return std::nullopt;
  }
  double nestedLevels = 0.0;
  size_t nNested = 0;
  if (raw.contains("nested_fe") && raw["nested_fe"].is_array()) {
    for (const auto& name : raw["nested_fe"]) {
      if (!name.is_string()) return std::nullopt;
      const std::string n = name.get<std::string>();
      if (!absorbed.contains(n) || !absorbed[n].is_number()) return std::nullopt;
      nestedLevels += absorbed[n].get<double>();
      ++nNested;
    }
  }
  if (nNested > 0 && absorbed.size() != 1) return std::nullopt;
  return kFe - nestedLevels;
}

}  // namespace

// `assert EXP [if COND] [in RANGE]` — see handlers.h for why this delegates to
// runCount instead of evaluating the expression itself.
ojson runAssert(const std::string& expr, SessionState& session,
                const std::optional<std::string>& ifExpr,
                const std::optional<std::string>& inRange) {
  if (!session.hasData()) return eval::noDataLoaded();
  // A contradiction is a row where EXP is false. Wrapping in `!( ... )` — rather
  // than splicing a negation operator into the text — keeps the user's own
  // operator precedence intact, so `assert a == 1 | b == 2` negates the WHOLE
  // claim and not just its last term.
  std::string negated = "!(" + expr + ")";
  if (ifExpr.has_value() && !ifExpr->empty()) negated += " & (" + *ifExpr + ")";

  ojson counted = handlers::runCount(negated, session, inRange);
  // runCount surfaces a bad expression as an error block; pass it through
  // verbatim rather than reporting a contradiction count we never computed.
  if (counted.is_object() && counted.value("type", std::string()) == "error") return counted;

  const double* bad = session.eval.rValues.find("N");
  if (bad == nullptr) {
    return engineErrorBlock("assert: could not evaluate `" + expr + "`", "engine_error");
  }
  const int64_t contradictions = static_cast<int64_t>(*bad);
  // Restore r(N) to the dataset row count. `assert` is not an r-class command in
  // Stata, so leaving the internal count behind would make a following `r(N)`
  // silently answer "how many rows failed my assertion" — a wrong number with no
  // error attached, which is worse than the missing it replaces.
  Datastore* ds = session.datastore();
  if (ds != nullptr) session.eval.rValues.set("N", static_cast<double>(ds->nRows()));

  if (contradictions == 0) return ojson::array();  // Stata prints nothing on success

  const int64_t total = (ds != nullptr) ? ds->nRows() : 0;
  return eval::errorBlock(
      "assertion_false",
      std::to_string(contradictions) + " contradiction" + (contradictions == 1 ? "" : "s") +
          " in " + std::to_string(total) + " observation" + (total == 1 ? "" : "s") +
          "\nassertion is false",
      std::nullopt, std::nullopt, std::nullopt, "r(9)");
}

ojson runRegress(const eval::ParsedCommand& parsed, SessionState& session,
                  const std::string& stataCmd, const std::string* dataOverride,
                  ojson* residMutationOut) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: no variables specified", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"feols"};
  argv.push_back("--y");
  argv.push_back(y);
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinComma(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  // BS-090 (BETA Phase G): `if`/`in` used to be forwarded here as a bare
  // `--keepif` flag straight onto cobra's `feols` CLI. That still worked
  // for if-only (feols's own --keepif runs the SAME non-bysort
  // Frame::keep_if() cobra's `eval` verb uses), but a `_n`-based `in`
  // predicate can NOT go through it (op-cobra's evaluator throws "_n
  // requires a bysort context" outside --bygen) — so the row-range
  // pre-filter below (piet::handlers::applyIfAndInFilter) now runs BEFORE
  // the cobra call and points `--data` at the already-filtered copy
  // instead, and `--keepif` is never pushed to `argv` at all (removed here,
  // see the `--data` construction near the bottom of this function).

  // vce/vcov/robust/cluster (translate.py's _vcov_from_opts, iid/hc1/cluster
  // — BS-067 (BETA D1) added twoway, see splitClusterVars below).
  std::optional<std::string> vceVal;
  bool sawRobustFlag = false;
  std::optional<std::string> clusterOpt;
  bool noConstant = false;
  std::optional<int> level;
  bool keepSingletons = false;
  // BUG-539: reghdfe's `residuals[(newvar)]`. Bare -> `_reghdfe_resid`
  // (w2_smalls.log reghdfe_bare_resid/residuals/res/residual/resi, all rc 0
  // with the variable made); `NAME(newvar)` -> that name (reghdfe_named_*).
  // Only reghdfe has the option -- the reject net below still refuses these
  // keys for regress/feols/areg, which is Stata's own r(198) there.
  bool residWanted = false;
  std::string residName;
  if (auto shapeErr = vceOptionShapeError(parsed)) {
    return engineErrorBlock(*shapeErr, "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      sawRobustFlag = true;
    } else if (k == "cluster") {
      clusterOpt = v.isBool ? std::string() : v.strValue;
    } else if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      noConstant = true;
    } else if (stataCmd == "reghdfe" && kReghdfeResidSpellings.count(k)) {
      residWanted = true;
      residName = v.isBool ? std::string("_reghdfe_resid") : strutil::trim(v.strValue);
    } else if (stataCmd == "reghdfe" && kReghdfeNoAbsorbSpellings.count(k)) {
      // w2_smalls.log reghdfe_noabsorb: rc 0, _b[x] = 1.90685050483118 = the
      // constant-only OLS fit. reghdfe.ado:181 parses the option as
      // `noAbsorb2` and never reads it: `has_standard_fe` (:212) is decided
      // by absorb() ALONE, so the flag is consumed here and absorb() still
      // wins when both are typed. Nothing to push -- no absorb(), no --fe.
    } else if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    } else if (k == "keepsingletons" && v.isBool && v.boolValue) {
      // BS-068: reghdfe's own option name — real Stata has no
      // `keepsingletons` on plain regress/feols at all (that gate is below,
      // outside this loop, where stataCmd is compared).
      keepSingletons = true;
    } else if (k == "absorb") {
      if (!v.isBool) {
        std::string fe = v.strValue;
        // space/comma -> comma-joined tokens (translate.py's --fe rule)
        std::string joined;
        std::string cur;
        auto flush = [&]() {
          if (!cur.empty()) {
            if (!joined.empty()) joined += ",";
            joined += cur;
            cur.clear();
          }
        };
        for (char c : fe) {
          if (c == ' ' || c == ',') {
            flush();
          } else {
            cur += c;
          }
        }
        flush();
        if (!joined.empty()) {
          argv.push_back("--fe");
          argv.push_back(joined);
        }
      }
    }
  }

  // P4 / BUG-060: unknown-option rejection net. After the loop above has
  // consumed every option `regress`/`reg`/`feols`/`reghdfe` implements, any
  // leftover token is either a genuine typo/invalid option (r(198)-parity:
  // Stata's own "option X not allowed") or a real Stata option this
  // version of PIET hasn't implemented yet (honest "not yet supported" —
  // currently only `beta`, the oracle P4 example: standardized-coefficient
  // display column). Conservative by construction: this only fires on
  // option NAMES the loop above never reads at all; it never revisits a
  // value it already accepted, so it cannot false-reject anything that
  // worked before this fix.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster" || k == "noconstant" ||
        k == "nocons" || k == "level") {
      continue;
    }
    // BUG-539: residuals()/noabsorb are reghdfe's alone (the two spelling
    // sets at the top of this file). Gated on the Stata word typed, like
    // keepsingletons below: `regress ..., resid` stays "option `resid` not
    // allowed".
    if (stataCmd == "reghdfe" &&
        (kReghdfeResidSpellings.count(k) || kReghdfeNoAbsorbSpellings.count(k))) {
      continue;
    }
    if (k == "absorb") {
      // BUG-473 (User ruling 2026-09-09: match Stata): plain `regress`/`reg`
      // has NO `absorb()` option in any Stata edition on this machine —
      // measured StataBE r(198), message "option absorb() not allowed"
      // (tests/eval/golden/aer_b4/b5_absorb_class.log:32-36,
      // b5_misc_pins.log:29-51 incl. the cluster()/vce()/if combinations,
      // and the `ab()` abbreviation at
      // tests/eval/golden/aer_b4/milfpins_oracle_p.log:103-107, same
      // message and rc). `feols`/`reghdfe`/`areg` share this handler and DO
      // have it (b5_absorb_class.log:80-99 areg rc 0, :125-151 reghdfe rc 0;
      // freeze_extra.do:41 `feols y x1 x2, absorb(id)`) — gate on the Stata
      // word actually typed, mirroring the `keepsingletons` gate below.
      if (stataCmd == "regress") {
        return engineErrorBlock("option absorb() not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "keepsingletons") {
      // BS-068: real Stata has `keepsingletons` on reghdfe only (regress/
      // feols have no such option at all) — gate on the ACTUAL Stata word
      // the user typed, not on whether this file happens to consume it.
      if (stataCmd != "reghdfe") {
        return engineErrorBlock("option `keepsingletons` not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "beta") {
      return engineErrorBlock("Option `" + k + "` is a valid Stata option that this version of "
                              "PIET does not yet implement.",
                              "unsupported_option");
    }
    return engineErrorBlock("option `" + k + "` not allowed", "unsupported_option");
  }
  // BS-068: keepsingletons parsed above regardless of stataCmd (so the
  // reject-net check just above can compare stataCmd itself), but only
  // ACTUALLY forwarded to cobra for reghdfe — matches real Stata (regress/
  // feols reaching this point with keepSingletons=true is unreachable: the
  // net above already returned for any stataCmd != "reghdfe").
  if (keepSingletons && stataCmd == "reghdfe") {
    argv.push_back("--keep-singletons");
  }

  if (vceVal.has_value()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors: a prefix of
    // `robust` at least 1 long, of `cluster` at least 2.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (lv == "robust" || lv == "hc1") {
      argv.push_back("--vcov");
      argv.push_back("hc1");
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      if (cvar.empty()) {
        return engineErrorBlock(
            "Option `vce(cluster)` is not supported in this version of PIET. "
            "Cluster variable required: vce(cluster varname)",
            "unsupported_option");
      }
      // BS-067: one var -> --vcov cluster (unchanged); two -> --vcov
      // twoway --cluster A,B; three+ -> --vcov multiway for reghdfe, else a
      // named refusal.
      ClusterSplitResult cs = splitClusterVars(cvar, stataCmd == "reghdfe");
      if (cs.errored) {
        return engineErrorBlock(cs.errorMsg, "unsupported_option");
      }
      for (const auto& a : cs.vcovArgv) argv.push_back(a);
    } else if (lv == "iid" || (stataCmd == "reghdfe" && isReghdfeUnadjustedVce(*vceVal))) {
      argv.push_back("--vcov");
      argv.push_back("iid");
    } else if (!unimplementedVceType(*vceVal).empty()) {
      return engineErrorBlock("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
    } else {
      return engineErrorBlock("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through to
    // neither `if` nor here and silently ran iid (v wholly ignored).
    // Oracle P2_legacycluster: `regress price mpg, cluster(rep78)` must
    // come back cluster-robust (n_obs=69, n_clusters=5, se_mpg~=82.63), not
    // iid (n_obs=74, se_mpg~=53.08, rep78 ignored).
    // BS-067: same one/two/three+ split as the vce(cluster ...) branch
    // above applies to the legacy bare cluster() synonym too.
    // BUG-537 (W2 estimopts): this branch sits BEFORE the bare-`robust` one.
    // `robust cluster(id)` typed together is cluster-robust in Stata --
    // w2_pweight.log R5_pw_robust_AND_cluster_equals_R4 is byte-identical to
    // R4 (`vce(cluster id)`: N_clust 15, se_x1 +1.7f38be33c76eeX-003). With
    // robust tested first, the cluster() was parsed and then IGNORED (HC1,
    // N_clust 0): a silent wrong answer on ~29 corpus lines. Twin ordering in
    // estimation_m4.cpp's vcovFromOpts.
    ClusterSplitResult cs = splitClusterVars(*clusterOpt, stataCmd == "reghdfe");
    if (cs.errored) {
      return engineErrorBlock(cs.errorMsg, "unsupported_option");
    }
    for (const auto& a : cs.vcovArgv) argv.push_back(a);
  } else if (sawRobustFlag) {
    argv.push_back("--vcov");
    argv.push_back("hc1");
  }

  // BUG-568: areg's clustered SE charges every absorbed parameter, including an FE
  // nested in the cluster; reghdfe/xtreg,fe/feols (the engine default) exempt it.
  // w2b_estim.log areg_a_cl se[x1] .0722686736191015 (N_clust 10, df_r 9) against
  // reghdfe_cluster_ctl / xtreg_fe_cluster_ctl .0663182832 on the same fit; the
  // weighted twins w2_pweight.log A2 (0x1.cfb6746651353p-3) vs H1
  // (0x1.b315e83b84cedp-3). Only the areg word sets it, and only with a cluster
  // VCE and an absorbed FE (the engine refuses the flag anywhere else).
  if (stataCmd == "areg" && std::find(argv.begin(), argv.end(), "--fe") != argv.end()) {
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && (argv[i + 1] == "cluster" || argv[i + 1] == "twoway")) {
        argv.push_back("--keep-nested-fe-dof");
        break;
      }
    }
  }

  if (noConstant) argv.push_back("--no-intercept");
  // BS-048 (BETA A4) — session.level fallback NOT added here: verified
  // cobra's own feols CLI (cmd_feols, apps/cmds/cmds_linear.cpp) never
  // reads `opt(a, "--level", ...)` at all — the ONLY apps/cmds/*.cpp sites
  // that do are qreg/sdtest/ttest/mediate, and only ttest is wired into
  // PIET's dispatch surface today (runTtest carries the session.level
  // fallback instead). Forwarding --level to a command that never consumes
  // it trips cobra's E6 unconsumed-flag net (common.cpp::
  // check_unconsumed_flags) and turns an otherwise-successful regression
  // into a hard error — a session.level fallback here would make an
  // ordinary `set level 90` + subsequent plain `regress` newly error where
  // it worked before. The pre-existing explicit `level(#)` handling below
  // has the SAME latent gap (untested by any freeze fixture — no
  // `level(...)` usage anywhere in debug_all.do) but is OUT of this phase's
  // 4-mechanism scope to fix; flagged for the operator.
  if (level.has_value()) {
    if (*level < 1 || *level > 99) {
      return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                              "unsupported_option");
    }
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  }

  // P3 weights wave: registry.cpp's AllWeights row for regress/reghdfe/feols
  // means the tokenizer already let aw/fw/iw/pw all the way through here —
  // forward whichever one the user gave, normalized to cobra's 2-letter
  // --wtype spelling. `parsed.weightVar` only survives the tokenizer's guard
  // when `parsed.weightType` is one of the four legal spellings, so no
  // separate legality check is needed here. A weight combined with --fe
  // (already pushed to argv above, if `absorb()` was given) is fitted by the
  // engine's weighted absorption (feols_fit demeans with the weights); the
  // combinations Stata's absorbing commands refuse (iweight, pweight with an
  // iid VCE) are refused inside cobra itself and surface through the ordinary
  // CobraError catch below -- not checks this handler needs to duplicate.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    std::string wt = lower(*parsed.weightType);
    std::string wtype;
    if (wt == "aw" || wt == "aweight") wtype = "aw";
    else if (wt == "fw" || wt == "fweight") wtype = "fw";
    else if (wt == "iw" || wt == "iweight") wtype = "iw";
    else if (wt == "pw" || wt == "pweight") wtype = "pw";
    if (!wtype.empty()) {
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
      // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE.
      // w2_pweight.log R2_pw (`[pweight=w]`, no vce): e(vce)=robust,
      // e(vcetype)=Robust, se_x1 +1.a3421f4182c9fX-003 -- byte-identical to
      // R3 (`[aweight=w], vce(robust)`) and NOT to R1 (`[aweight=w]` iid,
      // +1.691bde09fd0feX-003). This handler forwarded the weight and let the
      // engine default to iid, which is exactly the R1 number under a pweight
      // header: 13 corpus lines in paper 034 ran with the wrong SE at rc 0.
      // Only when nothing chose a VCE above -- vce()/robust/cluster() all
      // pushed `--vcov` already, and R4/R5 keep their cluster.
      if (wtype == "pw" && std::find(argv.begin(), argv.end(), "--vcov") == argv.end()) {
        argv.push_back("--vcov");
        argv.push_back("hc1");
      }
    }
  }

  // TS-context injection (MI-083, CONTRACT-engine §16, M3a): when any y/x
  // token is a time-series operator expression (L./F./D./S.), inject
  // --xtset/--tsset from session.panelvar/timevar (translate.py's
  // _has_ts_token + _inject_ts_context, applied to the RAW pre-translation
  // varlist — matches build_estimation_argv's `ts_tokens = [y_raw] + xs_raw`
  // ordering; parsed.varlist IS y_raw/xs_raw here since dispatcher.cpp's
  // TODO(M3) TS-token reject was removed in favor of this real injection).
  if (translate::hasTsToken(parsed.varlist)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BS-090 (BETA Phase G): materialize the `if`/`in`-restricted sample
  // FIRST (piet::handlers::applyIfAndInFilter — same mechanism summarize/
  // count/tabulate/corr/ttest now all share) and point `--data` at it
  // instead of forwarding a raw `--keepif` to cobra's feols CLI (which
  // cannot evaluate an `in`-derived `_n` predicate — see the comment above
  // where `--keepif` used to be pushed onto `argv`).
  // dataOverride (by-group driver): the caller already materialized this group's
  // slice WITH the sub-command's own `if`/`in` ANDed in, so re-running the filter
  // here would re-apply `in` against the slice's own row numbering and silently
  // fit a sub-range of the group.
  std::string regressDataPath;
  if (dataOverride != nullptr) {
    regressDataPath = *dataOverride;
  } else {
    InRangeFilterResult regressFilter;
    try {
      regressFilter = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!regressFilter.ok) return inRangeRejectError(regressFilter, *parsed.inRange, stataCmd);
    regressDataPath = regressFilter.dataPath;
  }

  // BUG-539: everything residuals() cannot honour is refused BEFORE the fit,
  // by name, so e() is never left pointing at a model whose residual column
  // was silently not made. The column is produced by the engine's `predict
  // --resid` refit (the same call runPredict makes), which has no --weight /
  // --no-intercept / --keep-singletons / --tsset flags: forwarding those fits
  // through it would write a DIFFERENT model's residuals under the user's
  // name. An if/in restriction is NOT refused (review finding, attempt 2 --
  // paper 082 types `reghdfe ... if is_teacher >= 3, ... residuals`): see the
  // sample-masked depvar below the fit.
  bool residExisted = false;
  if (residWanted) {
    if (dataOverride != nullptr || residMutationOut == nullptr) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported under a by: prefix in this version of PIET.",
          "unsupported_option");
    }
    if (residName.empty()) {
      return engineErrorBlock("residuals() requires a new variable name, e.g. residuals(res).",
                              "syntax_error");
    }
    if (!strutil::isValidStataIdentifier(residName)) {
      return engineErrorBlock("reghdfe residuals(): invalid variable name `" + residName + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
    if (std::find(argv.begin(), argv.end(), "--weight") != argv.end() || noConstant ||
        keepSingletons || translate::hasTsToken(parsed.varlist)) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported together with weights, noconstant, "
          "keepsingletons or time-series operators in this version of PIET (the residual refit "
          "cannot carry them).",
          "unsupported_option");
    }
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      const auto& nm = ds->names();
      residExisted = std::find(nm.begin(), nm.end(), residName) != nm.end();
    }
    // w2_smalls.log reghdfe_named_exists: `residuals(myr)` with myr present is
    // r(110) "variable myr already defined" (coarseRc's "already defined"
    // rule). The BARE form is the exception -- reghdfe.ado:345 `cap drop
    // _reghdfe_resid` first, so an old `_reghdfe_resid` is replaced.
    if (residExisted && residName != "_reghdfe_resid") {
      return ojson(eval::engineError("variable " + residName + " already defined"));
    }
  }

  std::vector<std::string> cobraArgs = argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(regressDataPath);
  cobraArgs.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJson(cobraArgs);
  } catch (const CobraError& ce) {
    // dispatcher.py "4b. Cobra estimator": `except CobraError as ce:
    // blocks.append(make_engine_error(str(ce), code=ce.code))` — str(ce) is
    // the bracket-prefixed "[code] message". This is the exact site the 2
    // Eval M2-ratification TS carve-out cells exercise (`feols y L.x1`
    // without tsset -> cobra's native `ts_no_tsset` code, re-wrapped here as
    // "[ts_no_tsset] ..." with code coerced to "engine_error" by errorBlock's
    // allow-list, matching the frozen oracle cell byte-for-byte).
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // BUG-539: reghdfe's residual column. This is runPredict's engine call
  // (`predict --resid NAME`, within-residuals under --fe) pointed at the very
  // head the fit just read, so the output is that head plus one column -- no
  // column made before the fit is lost (predict's own stale-dataPath defect
  // is another lane's). It runs BEFORE e() is posted: a refit that fails, or
  // whose estimation sample is not the fit's own (a cluster variable missing
  // on rows the refit keeps, say), refuses the whole command instead of
  // leaving e() describing a model whose residual column is wrong or absent.
  // The estimation block stays the one object this handler returns; the
  // data_mutation travels through `residMutationOut` and the dispatcher
  // pushes it as its own block (ARRAY-vs-BLOCK: never hand back an array).
  //
  // Under if/in the fit read a filtered COPY (regressDataPath), which cannot
  // take the column: it has fewer rows than the live head. reghdfe fills
  // residuals() on e(sample) only, and `predict --fit-data` would evaluate
  // y - xb on EVERY row with data (and declines --fe). So the refit runs on
  // the live head with the depvar MASKED to the if/in rows: one `eval` pass
  // writes `kResidMaskY = cond((if & in), y, .)` (the in-range through a
  // materialized `_n`, the applyIfAndInFilter recipe, inrange_filter.cpp);
  // predict's listwise deletion then drops exactly the rows the filter
  // dropped, and writes NaN on every row outside md.keep; a last `eval`
  // drops the mask column. The n_obs equality check below is what proves the
  // masked refit saw the fit's own sample.
  std::string residOutPath;
  if (residWanted) {
    const bool restricted = regressDataPath != *session.currentHbin();
    std::string predictData = regressDataPath;
    std::string predictY = y;
    // Scratch column names (design decision: no existing helper exports one;
    // inrange_filter.cpp's kIdxVar is file-local). Double-underscore, like
    // kIdxVar, so a Stata user variable cannot collide in practice.
    const std::string kResidMaskY = "__piet_resid_y__";
    const std::string kResidMaskIdx = "__piet_resid_idx__";
    if (restricted) {
      const bool haveIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
      const bool haveIn = parsed.inRange.has_value() && !parsed.inRange->empty();
      std::string cond;
      std::vector<std::string> margs = {"eval", "--data", *session.currentHbin()};
      if (haveIn) {
        Datastore* hds = session.datastore();
        InRangeBounds rr = parseInRangeBounds(*parsed.inRange, hds != nullptr ? hds->nRows() : 0);
        if (!rr.ok) {
          InRangeFilterResult bad;
          bad.ok = false;
          bad.negative = rr.negative;
          return inRangeRejectError(bad, *parsed.inRange, stataCmd);
        }
        margs.push_back("--bygen");
        margs.push_back(kResidMaskIdx + "=_n");
        cond = kResidMaskIdx + ">=" + std::to_string(rr.a) + " & " + kResidMaskIdx +
               "<=" + std::to_string(rr.b);
      }
      if (haveIf) cond = haveIn ? "(" + *parsed.ifExpr + ") & (" + cond + ")" : *parsed.ifExpr;
      margs.push_back("--gen");
      margs.push_back(kResidMaskY + "=cond((" + cond + "),(" + y + "),.)");
      if (haveIn) {
        margs.push_back("--drop");
        margs.push_back(kResidMaskIdx);
      }
      predictData = nextPath(session.workdir(), "reghdfe_resid_mask");
      margs.push_back("--out");
      margs.push_back(predictData);
      try {
        runCobraJson(translate::injectTsContextForExpr(margs, cond, session));
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      predictY = kResidMaskY;
    }
    std::vector<std::string> pargs = {"predict", "--data", predictData, "--y", predictY,
                                      "--x", xs.empty() ? std::string("(intercept)") : joinComma(xs)};
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--fe") {
        pargs.push_back("--fe");
        pargs.push_back(argv[i + 1]);
      }
    }
    pargs.push_back("--resid");
    pargs.push_back(residName);
    residOutPath = nextPath(session.workdir(), "reghdfe_resid");
    pargs.push_back("--out");
    pargs.push_back(residOutPath);
    ojson praw;
    try {
      praw = runCobraJson(pargs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    const bool sameSample = praw.is_object() && raw.is_object() && praw.contains("n_obs") &&
                            raw.contains("n_obs") && praw["n_obs"].is_number() &&
                            raw["n_obs"].is_number() &&
                            praw["n_obs"].get<double>() == raw["n_obs"].get<double>();
    if (!sameSample) {
      return engineErrorBlock(
          "reghdfe residuals(): the residual refit did not reproduce the estimation sample "
          "(e.g. a cluster variable missing on some rows) -- not supported in this version of "
          "PIET.",
          "unsupported_option");
    }
    if (restricted) {
      const std::string unmasked = nextPath(session.workdir(), "reghdfe_resid");
      try {
        runCobraJson({"eval", "--data", residOutPath, "--drop", kResidMaskY, "--out", unmasked});
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      residOutPath = unmasked;
    }
  }

  ojson block = cobraToEstimation(raw, "feols", &session);
  // Label-only override: reghdfe IS feols --fe under the hood (cobra has no
  // separate reghdfe estimator), so the cobra invocation and LastModel replay
  // command (below) stay "feols" — only the displayed "estimator" field is
  // renamed to match the Stata command word the user actually typed, so
  // output parity holds (Stata shows "reghdfe", not "feols").
  if (stataCmd == "reghdfe") {
    block["estimator"] = "reghdfe";
  }

  LastModel lm;
  lm.cobraCmd = "feols";
  lm.stataCmd = stataCmd;  // BUG-458: e(cmd) reads "regress"/"reghdfe", not the engine's "feols"
  lm.y = y;
  lm.x = xs;
  // --fe
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--fe" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.fe.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.fe.push_back(cur);
    }
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
    if (argv[i] == "--weight" && i + 1 < argv.size()) lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype" && i + 1 < argv.size()) lm.fitWtype = argv[i + 1];
  }
  lm.depvar = block.value("depvar", "");
  lm.dataPath = *session.currentHbin();
  // W:P5 fit-sample fidelity (BUG-163): this handler materializes its if/in
  // restriction into a scratch file and fits THAT — record it so
  // test/lincom/margins/predict/boottest refit the same sample. Left empty
  // when no filter applied (regressDataPath == the session head), keeping
  // every unfiltered refit byte-identical to before.
  if (regressDataPath != lm.dataPath) lm.fitDataPath = regressDataPath;
  lm.result = raw;
  // BUG-570: e(N_clust) exists only after a clustered fit. The engine reports
  // n_clusters 0 for every other VCE and e(N_clust) resolves from this cached
  // reply, so `display e(N_clust)` printed 0 where Stata prints `.`
  // (w2_pweight.log R2_pw PSTR `120|117|.|robust|...`; R0/R1 and the areg/reghdfe
  // arms without a cluster alike). The key is dropped from the cached reply ONLY:
  // the emitted block keeps the engine's field, whose shape the freeze fixtures pin.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  // reghdfe's e(N_clustervars) and e(N_clust1..k) (w2b_estim.log:1410
  // reghdfe_cluster3 reads e(N_clustervars) 3). Cached reply only, like df_a
  // below: the resolver's key == name fall-through reads them. N_clustervars
  // is the number of forwarded cluster names; N_clust# are the engine's
  // per-dimension G (n_clusters_by, a JSON ARRAY read element by element --
  // never pushed as a block). Only the multiway fit is pinned by an arm; the
  // one-way and two-way values follow reghdfe's documented e() list.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway") &&
      !lm.cluster.empty()) {
    size_t nVars = 1;
    for (char c : lm.cluster.back()) {
      if (c == ',') ++nVars;
    }
    (*lm.result)["N_clustervars"] = static_cast<double>(nVars);
    if (lm.result->contains("n_clusters_by") && (*lm.result)["n_clusters_by"].is_array()) {
      size_t j = 0;
      for (const auto& g : (*lm.result)["n_clusters_by"]) {
        ++j;
        if (g.is_number()) (*lm.result)["N_clust" + std::to_string(j)] = g.get<double>();
      }
    } else if (nVars == 1 && lm.result->contains("n_clusters") &&
               (*lm.result)["n_clusters"].is_number()) {
      (*lm.result)["N_clust1"] = (*lm.result)["n_clusters"].get<double>();
    }
  }
  // e(df_a) after areg / reghdfe (derivation and oracle arms: absorbedDofA).
  // Like N_g/df_a on the xtreg,fe route, it goes into the cached reply only
  // (the e() resolver falls through key == name); the emitted block, whose
  // shape the freeze fixtures pin, is unchanged.
  if (lm.result.has_value()) {
    if (const auto dfA = absorbedDofA(*lm.result, stataCmd)) (*lm.result)["df_a"] = *dfA;
  }
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // e(vce) after a default-VCE `regress` is `ols` (w2_pweight.log R0_unweighted
  // PSTR `120|117|.|ols|||||`, R1_aw_control `...|ols|||aweight|= w|`;
  // w2h_postest_refit.log R_ctl/R_aw/R_fw `ols`). setLastModel() publishes
  // lm.vcov's engine spelling `iid`, which the replay machinery keys on, so the
  // Stata spelling is set in the macro store here, after it. areg and reghdfe
  // keep `iid`: no captured record pins their default e(vce). KNOWN GAP: a
  // later setLastModel() on the SAME model (estimation_m4c.cpp setEstHandleM4,
  // reached by test/lincom/predict) republishes `iid`; the lasting fix is a
  // stataCmd-keyed translation inside setLastModel (session_state.h), filed
  // separately. `estimates restore` is safe: it restores the stored macros.
  if (stataCmd == "regress" && lm.vcov == "iid") session.eval.setEString("vce", "ols");

  // BUG-539: e(resid) names the column made by THIS fit (w2_smalls.log
  // reghdfe_e_resid); setLastModel() just erased the previous fit's. The
  // column itself lands on the live head only now, after e() is posted, so
  // the head the fit (and lm.dataPath) read is untouched until this point.
  if (residWanted) {
    session.load(residOutPath);
    // A replaced `_reghdfe_resid` is a rewrite (mutation_result.h's third
    // slot), a fresh name is a pure add; the value is the same either way.
    *residMutationOut = residExisted ? mutationResult(session, "gen", {}, {}, {residName})
                                     : mutationResult(session, "gen", {residName});
    session.eval.setEString("resid", residName);
  }

  return block;
}

// ---------------------------------------------------------------------------
// P3 weights wave: `mean`/`proportion` — NEW commands, both real Stata
// ECLASS estimators (e(b)/_b[...] access afterward), routed to cobra's own
// `mean`/`proportion` verbs. Modeled on runRegress just above (closest
// existing handler that builds a coefficient-table estimation block from a
// cobra call) and runTtest (estimation_m4.cpp)'s level()-fallback precedent.
// registry.cpp's PweightOnly row on both means the tokenizer only lets
// [pw=]/[pweight=] through here (real Stata itself restricts both commands
// to probability weights only). NOT declared in piet/handlers.h (out of
// this batch's edit manifest — owned by a different stint); dispatcher.cpp
// forward-declares runMean/runProportion locally, the same trick this file
// uses for runSummarizeAweight above.
namespace {

ojson runMeanOrProportion(const eval::ParsedCommand& parsed, SessionState& session,
                          const std::string& stataCmd, const std::string& cobraCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: " + stataCmd + " requires at least one variable",
                            "syntax_error");
  }

  std::optional<int> level;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    }
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOpts = kVocabMeanProportion.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnownOpts)) {
    return *err;
  }
  if (level.has_value() && (*level < 1 || *level > 99)) {
    return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                            "unsupported_option");
  }

  std::vector<std::string> argv = {cobraCmd, "--vars", joinComma(parsed.varlist)};

  // registry.cpp's PweightOnly row means `parsed.weightVar` only survives the
  // tokenizer's guard here when weightType is pw/pweight — safe to forward
  // unconditionally as --wtype pw (no other weight type can reach this
  // handler at all).
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back("pw");
  }
  if (level.has_value()) {
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  } else if (session.level != 95) {
    // ttest's own session.level fallback precedent (estimation_m4.cpp,
    // BS-048): the frozen CLI surface documents --level on both mean and
    // proportion, so a prior `set level` carries through the same way.
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }

  InRangeFilterResult filt;
  try {
    filt = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!filt.ok) return inRangeRejectError(filt, *parsed.inRange, stataCmd);

  argv.push_back("--data");
  argv.push_back(filt.dataPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJson(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // The frozen mean/proportion envelope names its per-row array "coefs", not
  // "coeftable" like every other estimator this file wires — cobraToEstimation
  // AND macros.cpp's e()/_b[...]/_se[...] resolver (downstream, keyed off
  // session.lastModelResult's "coeftable" field) both look for "coeftable"
  // specifically. Alias it on a copy before handing off, so both the display
  // block and postestimation access see the SAME row shape ({"name","b","se",
  // "t","p","ci_lo","ci_hi"}, already engine-computed) every other estimator
  // uses. The rest of the envelope (n_obs, weight, level, coef, se, sum_w,
  // ...) passes through cobraToEstimation's existing field lists unchanged.
  ojson raw2 = raw;
  if (raw2.contains("coefs") && !raw2.contains("coeftable")) {
    raw2["coeftable"] = raw2["coefs"];
  }

  ojson block = cobraToEstimation(raw2, cobraCmd, &session);

  // e(b)/_b[...] wiring (same mechanism runRegress uses just above): stash
  // the RAW (coeftable-aliased) result on LastModel so macros.cpp's
  // resolveBRef/resolveEScalar can find it via session.lastModelResult, and
  // session.lastEstBlock for esttab/eststo's snapshot path.
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.y = parsed.varlist[0];
  lm.x = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
  lm.depvar = block.value("depvar", "");
  lm.dataPath = *session.currentHbin();
  lm.result = raw2;
  session.setLastModel(lm);
  session.lastEstBlock = block;

  return block;
}

}  // namespace

ojson runMean(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "mean", "mean");
}

ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "proportion", "proportion");
}

}  // namespace piet::handlers

namespace piet {

namespace {

// Same dequote rule handlers::runUse/runCd/the U7 shims all used
// independently before this consolidation (strip surrounding whitespace,
// then one layer of matching outer quotes).
std::string dequoteUserPath(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  s = s.substr(a, b - a + 1);
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool isAbsoluteUserPath(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '/') return true;
#ifdef _WIN32
  // Drive-letter ("C:\" / "C:/") or UNC/rooted-backslash forms. Dead code
  // on the current POSIX-only build of this TU (handlers.cpp has no other
  // _WIN32 branch yet) — added ahead of the eventual Windows port of the
  // native-verbs surface so the ONE resolver already knows the rule instead
  // of a second fork appearing when that port lands.
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
  if (p[0] == '\\') return true;
#endif
  return false;
}

}  // namespace

std::string resolveUserPath(const SessionState& session, const std::string& rawPath) {
  std::string path = dequoteUserPath(rawPath);

  // `~` expansion (os.path.expanduser-equivalent, HOME-relative only —
  // `~user` forms are not exercised by the corpus; matches the rule runCd
  // used before this consolidation, handlers_m3.cpp's now-shared logic).
  if (!path.empty() && path[0] == '~') {
#ifdef _WIN32
    std::string home = cecon::env_utf8("USERPROFILE");
#else
    std::string home = cecon::env_utf8("HOME");
#endif
    if (!home.empty()) path = home + path.substr(1);
  }

  if (isAbsoluteUserPath(path)) return path;

  std::string base;
  if (session.cwd.has_value() && !session.cwd->empty()) {
    base = *session.cwd;
  } else {
    // Last-resort getcwd() — headless drivers (--run-block's fresh-session-
    // per-cell harness) never seed session.cwd at all.
#ifdef _WIN32
    // No getcwd() on MinGW that answers in UTF-8 — a Korean cwd would come
    // back CP949 and every path joined onto it would miss. current_path() is
    // wide underneath; u8string() puts it back in piet's one encoding.
    std::error_code cwdEc;
    std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
    base = cwdEc ? "." : cwd.u8string();
#else
    char buf[4096];
    base = getcwd(buf, sizeof(buf)) ? buf : ".";
#endif
  }
  if (base.empty()) return path;  // no base to join against — return the
                                   // raw relative path rather than
                                   // fabricating a bogus "/path" (the old
                                   // resolvePath's base=="/" -> pop_back ->
                                   // "" -> "/"+path collapse conflated "no
                                   // base" with "root base"; see below).
  // Strip a trailing separator EXCEPT when base is exactly "/" (root) —
  // popping that back to "" and then unconditionally prepending "/" is
  // exactly the mechanism that used to build "/<name>" from a root cwd
  // (the M2-era bug this consolidation fixes); handling "/" explicitly here
  // keeps the join to a single '/' either way.
  if (base.size() > 1 && base.back() == '/') base.pop_back();
  if (base.back() == '/') return base + path;
  return base + "/" + path;
}

}  // namespace piet
