// R2 (giant-file split, 2026-09-01): continuation TU of handlers_data_verbs.cpp
// — a MOVE-ONLY cut along pre-existing anonymous-namespace boundaries. The
// preamble is the original file head verbatim; shared helpers come from
// handlers_data_verbs_internal.h.
// M3b: MI-066 data verbs (merge/reshape/append/duplicates/gsort/order/
// recode) via cobra 0.18.0's native CLI verbs. C++ ports of the
// corresponding piet/backend/piet/{merge_op,reshape,append,duplicates,
// gsort,order,recode}.py oracle modules — bug-for-bug where cobra 0.18.0
// and the Python backend agree; where they differ, the FROZEN 1.16.0
// corpus wins (this file pre-validates the oracle's own error conditions
// in C++ BEFORE shelling out to cobra, so error text matches byte-for-byte
// even where cobra's native error text differs — see each handler's notes).
#include "piet/handlers.h"
#include "piet/translate.h"

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <iconv.h>   // AER scope-2 W3a: `import delimited, encoding()`

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

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
#include "handlers_data_verbs_internal.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

namespace {
// R2: re-binding of the option vocabularies this TU consumes. Registering the
// same token twice UNIONS the sets (documented, deliberate — errors.h), and a
// set unioned with itself is itself, so the single-registry law (BUG-233)
// holds with three TUs exactly as it did with one.
const piet::eval::OptVocab& kVocabTsfill = piet::eval::registerOptVocab(
    {"tsfill"}, {{"full"},
     {}});
}  // namespace

namespace {

// Resolve joinby's `using` file to a locally-readable hbin path. The engine's
// `cobra joinby` verb takes .hbin only (and PIET's own pre-validation reads
// the using file via Datastore), unlike
// append (which only needs cobra's OWN --using argument — cmd_append
// auto-detects .dta/.csv internally) or merge (which pre-converts ONLY for
// its own pre-validation peek, then hands cobra the original path). Mirrors
// runMerge's using-file resolution above, kept as a SEPARATE copy per this
// codebase's established per-TU/per-callsite convention (see errorFromRaw's
// own header note above) rather than refactoring runMerge to share it.
struct JoinbyUsingResult {
  bool ok = false;
  std::string hbinPath;
  ojson errBlock;
};

JoinbyUsingResult resolveJoinbyUsing(SessionState& session, const std::string& usingPathArg) {
  JoinbyUsingResult r;
  std::string usingPath = resolveUserPath(session, usingPathArg);
  session.materializeVirtualSave(usingPath);  // BUG-125: a save_virtual'd tempfile becomes real on demand
  if (!fileExists(usingPath)) {
    r.errBlock = engineErrorBlock("joinby using: file not found: '" + usingPath + "'");
    return r;
  }
  std::string ext = extOf(usingPath);
  if (ext == ".dta") {
    std::string conv = nextPath(session.workdir(), "joinby_using_dta");
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      r.errBlock =
          engineErrorBlock("joinby using: cannot convert '" + usingPath + "': " + ce.what());
      return r;
    }
    r.hbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "joinby_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr = runCobra(
        {"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      r.errBlock =
          engineErrorBlock("joinby using: cannot convert '" + usingPath + "': " + ce.what());
      return r;
    }
    r.hbinPath = conv;
  } else {
    r.hbinPath = usingPath;  // already .hbin
  }
  r.ok = true;
  return r;
}

}  // namespace

ojson runJoinby(const std::vector<std::string>& keylist, const std::string& usingPathArg,
                SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // INSPECT-2608: everything this function asks of the MASTER frame is metadata
  // — does a key exist, what are the non-key names, how many rows — and
  // `session.datastore()` was answering it with a full open of the frame. The
  // engine reads the file itself for the actual join, so that open was pure
  // overhead: measured on the acceptance corpus's controls=yes arm, joinby's
  // 34.2 s carried 19.8 s of engine and 5.5 GB of PIET-side column reads for
  // questions the resident shadow card already holds. Same idiom, and the same
  // safety rule, as runSort's varlist check (handlers_m3.cpp): the card answers
  // only when it provably describes the current head, and a nullptr falls back
  // to the exact prior behaviour.
  const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent();
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  Datastore* master = nullptr;
  if (!card || !cardRows.has_value()) {
    master = session.datastore();
    if (master == nullptr) return eval::noDataLoaded();
  }
  auto masterHas = [&](const std::string& nm) {
    if (master) return master->has(nm);
    for (const auto& info : *card)
      if (info.name == nm) return true;
    return false;
  };
  auto masterNames = [&]() {
    std::vector<std::string> out;
    if (master) return master->names();
    out.reserve(card->size());
    for (const auto& info : *card) out.push_back(info.name);
    return out;
  };
  if (keylist.empty()) return engineErrorBlock("joinby: key variable list required");
  for (const auto& k : keylist) {
    if (!masterHas(k)) return engineErrorBlock("joinby: variable '" + k + "' not found");
  }

  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;
  Datastore usingDs(ur.hbinPath);

  for (const auto& k : keylist) {
    if (!usingDs.has(k)) {
      return engineErrorBlock("joinby: key variable '" + k + "' not found in using dataset");
    }
  }

  // Column-name conflict check (non-key columns present on both sides) —
  // mirrors merge's own established rule: this build performs a bare inner
  // key-join only (D5), with no update/replace value-reconciliation, so an
  // overlapping non-key name is ambiguous and rejected honestly rather than
  // silently picking a side.
  std::set<std::string> keySet(keylist.begin(), keylist.end());
  std::set<std::string> masterNonKeys;
  const std::vector<std::string> masterNameList = masterNames();
  for (const auto& nm : masterNameList) {
    if (!keySet.count(nm)) masterNonKeys.insert(nm);
  }
  std::vector<std::string> overlap;
  for (const auto& nm : usingDs.names()) {
    if (keySet.count(nm)) continue;
    if (masterNonKeys.count(nm)) overlap.push_back(nm);
  }
  if (!overlap.empty()) {
    std::sort(overlap.begin(), overlap.end());
    std::string listStr = "[";
    for (size_t i = 0; i < overlap.size(); ++i) {
      if (i) listStr += ", ";
      listStr += "'" + overlap[i] + "'";
    }
    listStr += "]";
    // Master wins a shared non-key name ([D] joinby's default without update/
    // replace) — oracle-pinned on the P1 wave fixture. Not an error; the
    // engine drops the using's copy and the report below must match.
    (void)listStr;
  }

  int64_t nMaster = master ? master->nRows() : *cardRows;
  int64_t nUsing = usingDs.nRows();

  std::vector<std::string> usingExtra;  // using's non-key columns, appended
  for (const auto& nm : usingDs.names()) {
    // master wins a shared name, so it is NOT a newly added column
    if (!keySet.count(nm) && !masterHas(nm)) usingExtra.push_back(nm);
  }

  // One engine call (`cobra joinby`, L1b) replaces the old in-C++ within-key
  // cross-product + whole-frame CSV + `cobra convert` round-trip. The engine
  // verb implements the same inner unmatched(none) join: factor keys matched
  // by LEVEL STRING via a unified table (each hbin's private level table can
  // number the same string differently); unmatched keys DROPPED both sides;
  // output = master columns + using non-key columns, master-row-order
  // deterministic. One accepted+logged divergence: ±0.0 f64 keys now MATCH
  // (the engine-wide make_key ±0-normalize convention) where the old
  // text-repr keyer treated them as distinct.
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  std::string outPath = nextPath(session.workdir(), "joinby");
  CobraRunResult cr;
  {
    // GA sweep leg ②/④, 2026-08-22. joinby was the single largest engine
    // coordinate in the census of record — 452.6 s, 27.3% of engine wall, of
    // which the phase probe put 363.6 s (77%) in the OUTPUT WRITE, at 330.9 MB
    // of scratch per call and 52.94 GB across one 47-task run. The kernel is
    // 106.9 s; the join itself was never the cost.
    //
    // The cost is that this call was leaving the router by the stateless leg,
    // so one joinby moved the whole frame across disk THREE times: the master
    // head had to be materialized to a real file to be named as `--data`, the
    // engine wrote the full result to `--out`, and `session.load(outPath)` read
    // all of it back. The resident plane exists precisely to delete all three —
    // `acquire_data("")` hands back the session's zero-copy view and
    // `commit_frame(..., "")` stages instead of writing (apps/cmds/common.cpp),
    // and both `data_missing`/`out_missing` already tolerate the empty argument
    // in resident mode. Nothing in the engine needed changing.
    //
    // ResidentEligible asserts exactly the invariant that holds here, the same
    // one rename/gen/replace/egen/drop/keep assert: this `--out` BECOMES the
    // head and is never read back as a file. `session.load(outPath)` below then
    // records the virtual chain rather than opening anything, and
    // mutationResult renders from the resulting card. joinby is safe to opt in
    // where `merge` is not: PIET never parses joinby's stderr (it checks
    // `cr.ok` and the card, never the "wrote N rows" notice), so no status
    // count depends on the one-shot leg being taken.
    SessionState::ResidentEligible residentJoinby(session);
    cr = runCobra({"joinby", "--data", master ? master->path() : *session.currentHbin(),
                   "--using", ur.hbinPath,
                   "--keys", joinedKeys, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // The disk-full guard still applies to the STATELESS leg, where `--out` is a
  // real file and a silent short write is exactly the BUG-350 failure mode. On
  // the resident leg no file is written by design, so the check would fire on
  // every success — it is therefore asked only when a file was expected.
  if (!session.lastRunWasResident() && !cecon::native_exists(outPath))
    return engineErrorBlock("joinby: failed writing output dataset (disk full or I/O error)");

  session.load(outPath);
  ojson result = mutationResult(session, "joinby", usingExtra);
  result["n_master"] = nMaster;
  result["n_using"] = nUsing;
  return result;
}

// ---------------------------------------------------------------------------
// tsfill (BS-081, BETA campaign Phase D batch D4): `tsfill [, full]` -> cobra
// tsfill --timevar T [--panelvar ID] [--delta D] --out new.hbin. Requires a
// prior `tsset T` (or `xtset ID T`) — reads session.timevar/panelvar/delta,
// the SAME tsset/xtset spine BS-075's xtpoisson/xtlogit read
// session.panelvar from (estimation_m4.cpp's runXtPoissonLogit) and this
// batch's own newey/prais/dfuller/pperron read session.timevar from
// (estimation_m4.cpp, same file as those D4 estimators — this one lives
// here instead because it is a data-mutation verb like gsort/order/recode
// above, not an estimation command). Oracle D4f
// (milf-coord/plans/BETA-20260712-campaign/probes/d45-oracle-stata.log):
// after `drop if mod(t,7)==0` (48 of 336 rows dropped — INCLUDING t=336
// itself, since 336 is itself a multiple of 7), `tsfill` restores the full
// grid from the SURVIVING min/max t, i.e. 1..335 (not 1..336 — tsfill only
// fills gaps WITHIN the observed range, it never extrapolates past the
// highest t still present) — live-verified byte-for-byte: 335 rows out,
// matching the oracle's own `count` = 335 exactly, with every OTHER column
// (income, etc.) missing/NaN on the newly-filled rows (also live-verified).
// Real Stata's `full` option (materializes every panelvar x timevar
// combination even for panels that never had ANY observation at a given
// period) has no engine equivalent (cobra tsfill's own usage banner has no
// --full flag at all) — honest-rejected rather than silently ignored.
ojson runTsfill(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return engineErrorBlock(
        "tsfill: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows the time "
        "variable.",
        "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    if (k == "full" && v.isBool && v.boolValue) {
      return engineErrorBlock(
          "Option `full` is a valid Stata option that this version of PIET does not yet "
          "implement for `tsfill` (the engine's tsfill CLI has no equivalent — it always fills "
          "only the gaps within each panel's own observed time range, never manufactures rows "
          "for a panel/period combination that never had ANY observation at all).",
          "unsupported_option");
    }
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabTsfill.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  std::vector<std::string> argv = {"tsfill", "--data", *session.currentHbin(), "--timevar",
                                    *session.timevar};
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }
  std::string outPath = nextPath(session.workdir(), "tsfill");
  argv.push_back("--out");
  argv.push_back(outPath);

  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane. tsfill was one of eight verbs
    // still writing a whole --out file per call with no note saying why; its engine
    // command already uses the shared plane (acquire_data / commit_frame), so the
    // scope is all it needed.
    SessionState::ResidentEligible residentTsfill(session);
    cr = runCobra(argv);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "tsfill");
}

// ---------------------------------------------------------------------------
// BUG-370: xtile / ipolate / tssmooth ma.
//
// All three were named as residuals by the 08-31 wiring audit and share one
// shape: the ENGINE half already ships and is already Stata-parity tested
// (`cobra eval --xtile/--ipolate/--tsma`; tests/unit/test_xtipo.cpp and
// test_tsma.cpp), and PIET simply had no command table row pointing at it. So
// these are not new estimators — they are the missing user-facing door on a room
// that was already furnished. Each handler is argument validation plus one
// engine call.
//
// What each one REFUSES matters as much as what it does, because Stata's option
// surface is wider than the engine's here and a silently-dropped option is a
// wrong answer wearing a right one's clothes:
//   xtile     — cutpoints()/altdef/weights have no engine equivalent at all
//   ipolate   — by() exists engine-side but PIET's by-prefix plumbing does not
//   tssmooth  — only `ma`; and only the full window(#l #c #f) spelling
namespace {

// Stata's xtile defaults to nquantiles(2); the engine's --xtile flag defaults to
// 4 when the option is absent. Those disagree, so the handler ALWAYS sends an
// explicit nq and never lets the engine's default show through. A missing
// `nq(...)` here would be a silently wrong answer, not an error.
constexpr int kXtileDefaultNq = 2;

bool parseIntOptDV(const std::string& text, int& out) {
  if (text.empty()) return false;
  size_t pos = 0;
  long v = 0;
  try {
    v = std::stol(text, &pos);
  } catch (...) {
    return false;
  }
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos != text.size()) return false;
  out = static_cast<int>(v);
  return true;
}

// Does the live shadow card (or, failing that, the datastore) already carry this
// name? Used to refuse overwriting an existing column, matching Stata's r(110).
bool columnExistsDV(SessionState& session, const std::string& name) {
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card)
      if (info.name == name) return true;
    return false;
  }
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

ojson newVarGuardDV(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return engineErrorBlock(
        verb + ": invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  if (columnExistsDV(session, newvar))
    return engineErrorBlock(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // empty => no objection
}

}  // namespace

ojson runXtile(const std::string& newvar, const std::string& srcvar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock("xtile: syntax is `xtile newvar = varname, nquantiles(#)`",
                            "syntax_error");
  ojson guard = newVarGuardDV(session, "xtile", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // NOT a wiring shortcut: with `if`, Stata computes the CUTPOINTS from the
    // restricted sample only. Forwarding a --keepif would instead compute them
    // from every row and then blank the rest — the same bins with a different
    // name, which is a wrong number. Refused with the recipe until the engine
    // grows a restricted-sample form.
    return engineErrorBlock(
        "xtile with `if` is not implemented: Stata derives the quantile cutpoints from the "
        "restricted sample, and this engine computes them over all rows. Restrict first "
        "(`preserve` + `keep if ...`, or a materialized subset frame), then xtile.",
        "unimplemented");
  }
  int nq = kXtileDefaultNq;
  bool nqGiven = false;
  std::string cutvar;
  for (const auto& [k, v] : opts) {
    if (k == "nquantiles") {
      if (v.isBool || !parseIntOptDV(v.strValue, nq))
        return engineErrorBlock("xtile: nquantiles() requires a whole number", "syntax_error");
      nqGiven = true;
    } else if (k == "cutpoints") {
      // BUG-371: the bin edges come from another variable's values instead of from
      // quantiles of srcvar. A different SOURCE of edges, not a modifier — Stata
      // treats the two as mutually exclusive and so does this.
      if (v.isBool)
        return engineErrorBlock("xtile: cutpoints() requires a variable name", "syntax_error");
      cutvar = trim(v.strValue);
    }
  }
  if (!cutvar.empty() && nqGiven)
    return engineErrorBlock(
        "xtile: nquantiles() and cutpoints() are alternatives — cutpoints() supplies the bin "
        "edges directly, so there are no quantiles left to ask for.",
        "bad_input");
  if (cutvar.empty() && nq < 2)
    return engineErrorBlock("xtile: nquantiles() must be at least 2", "bad_input");
  if (!cutvar.empty() && !columnExistsDV(session, cutvar))
    return engineErrorBlock("variable " + cutvar + " not found", "unknown_variable");

  std::string outPath = nextPath(session.workdir(), "xtile");
  const std::string spec =
      cutvar.empty() ? (newvar + "=" + srcvar + ", nq(" + std::to_string(nq) + ")")
                     : (newvar + "=" + srcvar + ", cutpoints(" + cutvar + ")");
  std::vector<std::string> argv = {"eval",  "--data", *session.currentHbin(),
                                   "--xtile", spec,
                                   "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

ojson runIpolate(const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.size() != 2)
    return engineErrorBlock("ipolate: syntax is `ipolate yvar xvar, generate(newvar)`",
                            "syntax_error");
  const std::string& yvar = varlist[0];
  const std::string& xvar = varlist[1];
  std::string newvar;
  std::string byVars;
  bool epolate = false;
  for (const auto& [k, v] : opts) {
    if (k == "generate") {
      if (v.isBool)
        return engineErrorBlock("ipolate: generate() requires a variable name", "syntax_error");
      newvar = trim(v.strValue);
    } else if (k == "epolate") {
      epolate = !v.isBool || v.boolValue;
    } else if (k == "by") {
      // BUG-371: `by()` was refused on the grounds that "PIET's by-prefix plumbing
      // does not exist". That was a misreading of the syntax — Stata's ipolate
      // takes by() as an OPTION, not a by: prefix, and the engine's --ipolate has
      // parsed `by(g..)` since it was written. So this was a wiring gap after all,
      // and one line of pass-through closes it.
      if (v.isBool)
        return engineErrorBlock("ipolate: by() requires a variable list", "syntax_error");
      byVars = trim(v.strValue);
    }
  }
  if (newvar.empty())
    return engineErrorBlock("ipolate: generate(newvar) is required", "syntax_error");
  ojson guard = newVarGuardDV(session, "ipolate", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // Same class as xtile's refusal: the interpolation ANCHORS would come from
    // rows the user excluded, so the interpolated values would not be the ones
    // Stata reports for that sample.
    return engineErrorBlock(
        "ipolate with `if` is not implemented: the interpolation anchors would still be drawn "
        "from excluded rows. Restrict first (`preserve` + `keep if ...`), then ipolate.",
        "unimplemented");
  }
  std::string spec = newvar + "=" + yvar + " " + xvar;
  // The engine's --ipolate reads by() and epolate out of ONE options tail, so both
  // ride the same comma-separated suffix.
  if (!byVars.empty()) spec += ", by(" + byVars + ")";
  if (epolate) spec += ", epolate";
  std::string outPath = nextPath(session.workdir(), "ipolate");
  std::vector<std::string> argv = {"eval",       "--data", *session.currentHbin(),
                                   "--ipolate",  spec,     "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

ojson runTssmoothMa(const std::string& newvar, const std::string& srcvar,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock(
        "tssmooth ma: syntax is `tssmooth ma newvar = varname, window(#l #c #f)`", "syntax_error");
  ojson guard = newVarGuardDV(session, "tssmooth ma", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (!session.timevar.has_value() || session.timevar->empty())
    return engineErrorBlock(
        "tssmooth ma: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows the "
        "time variable.",
        "unsupported_option");

  std::string windowSpec;
  bool haveWindow = false;
  for (const auto& [k, v] : opts) {
    if (k == "window") {
      if (v.isBool)
        return engineErrorBlock("tssmooth ma: window() requires #l #c #f", "syntax_error");
      windowSpec = v.strValue;
      haveWindow = true;
    }
  }
  if (!haveWindow)
    return engineErrorBlock("tssmooth ma: window(#l #c #f) is required", "syntax_error");
  std::vector<int> w;
  {
    std::string cur;
    auto flush = [&]() {
      if (cur.empty()) return true;
      int v = 0;
      if (!parseIntOptDV(cur, v)) return false;
      w.push_back(v);
      cur.clear();
      return true;
    };
    bool ok = true;
    for (char c : windowSpec) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!flush()) { ok = false; break; }
      } else {
        cur += c;
      }
    }
    if (ok) ok = flush();
    if (!ok)
      return engineErrorBlock("tssmooth ma: window() takes whole numbers", "syntax_error");
  }
  // BUG-378: the short forms are implemented now, with the semantics MEASURED
  // against real StataBE rather than guessed (the earlier refusal existed because
  // the omitted centre weight silently changes the window width, and my own
  // refusal text had the 2-number form wrong — it called it `#l #f`):
  //   window(1 1 1) -> (1/3)*[x(t-1) + 1*x(t) + x(t+1)]
  //   window(1 1)   -> (1/2)*[x(t-1) + 1*x(t)]      l=1, c=1, f=0
  //   window(1)     -> (1/1)*[x(t-1) + 0*x(t)]      l=1, c=0, f=0 — focal EXCLUDED
  // So the SECOND number is the centre, the third the leads, and both default to
  // 0 when omitted.
  if (w.empty() || w.size() > 3)
    return engineErrorBlock(
        "tssmooth ma: window() takes one to three whole numbers (#l [#c [#f]]); got " +
            std::to_string(w.size()),
        "syntax_error");
  const int wl = w[0];
  const int wc = w.size() >= 2 ? w[1] : 0;
  const int wf = w.size() >= 3 ? w[2] : 0;
  if (wl < 0 || wf < 0)
    return engineErrorBlock("tssmooth ma: window() lags and leads must not be negative",
                            "bad_input");
  if (wc != 0 && wc != 1)
    return engineErrorBlock("tssmooth ma: window()'s centre weight is 0 or 1", "bad_input");
  if (wl == 0 && wc == 0 && wf == 0)
    return engineErrorBlock("tssmooth ma: window() would average nothing", "bad_input");

  std::string outPath = nextPath(session.workdir(), "tsma");
  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--tsma",
                                   newvar + "=" + srcvar + "," + std::to_string(wl) + "," +
                                       std::to_string(wc) + "," + std::to_string(wf),
                                   "--out", outPath};
  // Unconditional, unlike the expression surfaces' gated helper: this command is
  // meaningless without a time variable, and the guard above already proved one.
  argv = translate::injectTsContext(argv, session);
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

// ---------------------------------------------------------------------------
// `isid VARLIST` — uniqueness assertion (P1 wave). PIET-native: the engine has
// no isid verb, and the check is a single pass over the key columns' mmap'd
// payloads, so shelling out would cost a scratch .hbin for nothing.
//
// Oracle (StataBE 19.5, auto.dta): `isid make` → silent, rc 0; `isid foreign` →
// "variable foreign does not uniquely identify the observations", r(459);
// `isid nosuchvar` → r(111). Stata names ONE offending variable list in the
// message and pluralizes on arity, which is reproduced below.
//
// Missing values are ordinary key values here (Stata's isid does not exempt
// them), and each key cell is compared by its exact f64 bits so the extended
// missings `.a`-`.z` (data/missing.h) stay distinct from `.` — the same
// kind-exact identity the expression layer uses.
ojson runIsid(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlock("isid: variable list required");

  for (const auto& v : varlist) {
    if (!ds->has(v)) {
      // r(111) class — same wording shape as Stata's "variable X not found".
      return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    }
  }

  const int64_t n = ds->nRows();
  if (n <= 1) {  // 0 or 1 observation is trivially unique
    return ojson::array();
  }

  // Build one 64-bit hash per row over the key tuple, then find duplicates by
  // hashing rows into buckets and confirming byte equality inside a bucket
  // (a hash collision must never be reported as a duplicate).
  struct KeyCol {
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
  };
  std::vector<KeyCol> keys;
  keys.reserve(varlist.size());
  for (const auto& v : varlist) {
    const cecon::Column& c = ds->col(v);
    KeyCol kc;
    if (c.dtype == cecon::DType::Factor) {
      kc.codes = c.codes();
    } else {
      kc.f64 = c.f64();
    }
    keys.push_back(kc);
  }

  auto cellBits = [&keys](size_t k, int64_t row) -> uint64_t {
    if (keys[k].codes != nullptr) return static_cast<uint64_t>(
        static_cast<uint32_t>(keys[k].codes[row]));
    uint64_t bits;
    std::memcpy(&bits, keys[k].f64 + row, sizeof bits);
    return bits;
  };
  auto rowsEqual = [&](int64_t a, int64_t b) {
    for (size_t k = 0; k < keys.size(); ++k)
      if (cellBits(k, a) != cellBits(k, b)) return false;
    return true;
  };

  std::unordered_map<uint64_t, std::vector<int64_t>> buckets;
  buckets.reserve(static_cast<size_t>(n) * 2);
  for (int64_t i = 0; i < n; ++i) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (size_t k = 0; k < keys.size(); ++k) {
      h ^= cellBits(k, i);
      h *= 1099511628211ull;
    }
    auto& bucket = buckets[h];
    for (int64_t prior : bucket) {
      if (rowsEqual(prior, i)) {
        std::string joined;
        for (size_t j = 0; j < varlist.size(); ++j) {
          if (j) joined += " ";
          joined += varlist[j];
        }
        return engineErrorBlock(
            (varlist.size() == 1 ? "variable " : "variables ") + joined +
                " do" + (varlist.size() == 1 ? "es" : "") +
                " not uniquely identify the observations",
            "engine_error");
      }
    }
    bucket.push_back(i);
  }
  return ojson::array();  // unique: silent, rc 0 (Stata prints nothing)
}

// ---------------------------------------------------------------------------
// P1 wave data verbs — expand / fillin / cross / contract. Each is a thin
// forwarder onto the engine subcommand of the same name (the joinby shape:
// validate here, transform there). The engine's own doc banners carry the
// StataBE 19.5 oracle pins.

ojson runExpand(const std::string& countArg, const std::optional<std::string>& genVar,
                SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  std::string n = trim(countArg);
  if (n.empty()) {
    return engineErrorBlock("expand: specify how many copies, e.g. `expand 2`");
  }
  // Stata accepts an EXPRESSION here (a per-observation count). This build
  // implements the constant form only and says so rather than guessing.
  for (char ch : n) {
    if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '-' && ch != '+') {
      return engineErrorBlock(
          "expand: this build supports a constant count only (e.g. `expand 2`); a per-observation "
          "expression is not implemented.");
    }
  }
  if (genVar.has_value() && !genVar->empty()) {
    ojson guard = newVarGuardDV(session, "expand", *genVar);
    if (!guard.is_null() && !guard.empty()) return guard;
  }
  const int64_t rowsBefore = session.datastore()->nRows();
  std::string outPath = nextPath(session.workdir(), "expand");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentExpand(session);  // BUG-394
    cr = runCobra({"expand", "--data", *session.currentHbin(), "--n", n, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  // B4-W3b `generate(newvar)`. Built from `_n > <rows before>` AFTER the
  // expansion, because that is the only fact that distinguishes an original
  // from a copy: the copies are byte-identical to their sources in every other
  // column. Checked BEFORE the engine call (above) so a name collision cannot
  // leave the dataset expanded but unmarked.
  if (genVar.has_value() && !genVar->empty()) {
    ojson r = runGen(*genVar + " = _n > " + std::to_string(rowsBefore), std::nullopt, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
    session.varMeta[*genVar].stataType = "byte";
    session.setVarLabel(*genVar, "Expanded observation type");
  }
  return mutationResult(session, "expand", genVar.has_value() && !genVar->empty()
                                               ? std::vector<std::string>{*genVar}
                                               : std::vector<std::string>{});
}

ojson runFillin(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("fillin: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "fillin");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentFillin(session);  // BUG-394
    cr = runCobra({"fillin", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "fillin", {"_fillin"});
}

ojson runCross(const std::string& usingPathArg, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  // Reuse joinby's `using` resolution (.dta/.csv pre-convert + virtual-save
  // materialization) so `cross using x.dta` works exactly like `joinby ... using x.dta`.
  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;

  std::vector<std::string> newCols;
  {
    Datastore usingDs(ur.hbinPath);
    Datastore* master = session.datastore();
    for (const auto& nm : usingDs.names())
      if (!master->has(nm)) newCols.push_back(nm);
  }
  std::string outPath = nextPath(session.workdir(), "cross");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentCross(session);  // BUG-394
    cr = runCobra({"cross", "--data", *session.currentHbin(), "--using", ur.hbinPath, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "cross", newCols);
}

ojson runContract(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("contract: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "contract");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentContract(session);  // BUG-394
    cr = runCobra({"contract", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "contract", {"_freq"});
}

// ---------------------------------------------------------------------------
// export / import delimited (P1 wave). PIET-native: the writer needs the
// session's value labels, which live here and not in the .hbin.

namespace {

// One CSV field, quoted only when it must be (delimiter, quote, CR or LF).
std::string csvField(const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') { needQuote = true; break; }
  }
  if (!needQuote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
  return out;
}

// A numeric cell the way Stata's `export delimited` writes it. A missing is
// `.` or its extended code `.a`..`.z` (oracle: Stata's own p1_roundtrip.csv
// carries literal `.a`).
//
// W:P8. The numeric spelling used to be the shortest round-trip decimal
// (`pythonFloatRepr`), which is NOT what Stata writes — measured cell-by-cell
// against Stata's own export of the same fixture, every one of 4,800 data rows
// differed. Two separate deviations: an integral value came out `1.0` where
// Stata writes `1` (so every id/year/region column in every exported CSV
// carried a spurious `.0` for whatever read it next), and a fractional value
// came out at 17 significant digits with its leading zero where Stata writes
// 16 without it. Stata's real rule, pinned by probing StataBE 19.5 with
// exact-valued doubles rather than inferred: `%.16g`, leading zero dropped
// for |x|<1, and the exponent branch re-spelled `%.11e` (`1e-20` ->
// `1.00000000000e-20`, `123456789012345678` -> `1.23456789012e+17`).
//
// Deliberate consequence, not an oversight: 16 digits is one short of a
// guaranteed round trip for a double, so a value can lose its last bit
// through a CSV. That is Stata's own behaviour and gate ① puts matching it
// first; PIET's own lossless path is `.dta`/`.hbin`.
//
// Known remaining divergence, out of reach here rather than unnoticed: Stata
// writes a `float` column at 8 significant digits, and PIET's in-memory
// datastore is all-f64, so a float column loaded from a .dta exports at
// double width. Closing that needs per-column storage types the datastore
// does not carry.
// `v` written with `dec` decimals, rounded HALF AWAY FROM ZERO (printf rounds
// half to even, which is what put 22 of 28,649 measured cells one unit below
// Stata in the 16th digit), then tidied Stata's way: trailing zeros and a bare
// trailing point dropped, and no leading zero on |x|<1.
// `v` written with `dec` decimals, then tidied Stata's way: trailing zeros and
// a bare trailing point dropped, and no leading zero on |x|<1.
std::string fixedDecimals(double v, int dec) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  if (out.find('.') != std::string::npos) {
    out.erase(out.find_last_not_of('0') + 1);
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  if (out.rfind("0.", 0) == 0) out.erase(0, 1);
  else if (out.rfind("-0.", 0) == 0) out.erase(1, 1);
  return out;
}

// P2-PERF ARC-B/S08. The same spelling as fixedDecimals(), written straight
// into a caller-owned buffer instead of a fresh std::string, and formatted from
// the MAGNITUDE so the "drop the leading zero of |x|<1" rule is a returned
// offset rather than an erase-in-the-middle. The caller prepends the sign;
// printf rounds on magnitude, so |round(v)| == round(|v|) and the bytes are
// identical either way. Returns the trimmed span inside `buf`.
struct FixedSpan {
  const char* p = nullptr;
  size_t len = 0;
};
FixedSpan fixedDecimalsAbs(char* buf, size_t cap, double av, int dec) {
  FixedSpan s;
  // std::to_chars(fixed, precision) and snprintf("%.*f") are both required to
  // be correctly rounded, so they emit the same digits; to_chars is simply not
  // obliged to consult the locale or parse a format string on the way, which is
  // most of what snprintf was costing per cell. The byte-identity diff of an
  // exported CSV is what certifies the equality, not this note.
  std::to_chars_result r = std::to_chars(buf, buf + cap, av, std::chars_format::fixed, dec);
  if (r.ec != std::errc()) return s;
  size_t len = static_cast<size_t>(r.ptr - buf);
  if (std::memchr(buf, '.', len) != nullptr) {
    while (len > 0 && buf[len - 1] == '0') --len;
    if (len > 0 && buf[len - 1] == '.') --len;
  }
  size_t off = 0;
  if (len >= 2 && buf[0] == '0' && buf[1] == '.') { off = 1; --len; }
  s.p = buf + off;
  s.len = len;
  return s;
}

std::string csvNumeric(double v) {
  if (std::isnan(v)) {
    // W:P8: an EXTENDED missing keeps its code (`.a`..`.z`, as Stata's own
    // p1_roundtrip.csv shows), but a plain system missing is written as an
    // EMPTY field — live-verified. Emitting `.` for it made 131 cells of the
    // P8 fixture read as the literal string "." to anything that opened the
    // file expecting a blank.
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    return m == "." ? std::string() : m;
  }
  if (v == 0.0) return "0";
  // Stata spells the value as the WIDEST plain decimal that fits 17 characters
  // of magnitude (sign excluded) — not as a fixed count of significant digits,
  // which is why `.1473717297922637` carries 16 of them and
  // `.0000617833399588` only 12. Exponent form appears only when nothing plain
  // fits, and is `%.11e` exactly (`1e-20` -> `1.00000000000e-20`).
  //
  // MEASURED RESIDUAL, stated rather than hidden: against Stata's own export of
  // the 4,800-row P8 fixture, 139,185 of 139,200 cells are identical. The
  // remaining FIFTEEN differ by one unit in the last of 16-17 digits, and only
  // where the shortest round-trip decimal needs all 17 — Stata's own decimal
  // conversion is not correctly rounded there. Three candidate reconstructions
  // of its exact behaviour (round the 17-digit decimal half away from zero;
  // scale by a power of ten in double arithmetic; truncate the shortest
  // representation) were each implemented and measured, each matched FEWER
  // cells than the plain correctly-rounded conversion kept here, and they
  // contradicted one another on the same cells. Chasing a vendor float-printer
  // quirk below 1e-16 relative is not worth a bespoke decimal engine; the
  // format itself — width budget, integer spelling, dropped leading zero,
  // missing and date spelling, exponent form — is exact.
  for (int dec = 17; dec >= 0; --dec) {
    std::string s = fixedDecimals(v, dec);
    if (s == "0" || s == "-0") continue;  // rounded away to nothing; keep looking
    const size_t visible = s.size() - (s[0] == '-' ? 1 : 0);
    if (visible <= 17) return s;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.11e", v);
  return std::string(buf);
}

// P2-PERF ARC-B/S08 — the same function, appending instead of returning, with
// one shortcut. `export delimited` on a 500k-cell frame called csvNumeric once
// per cell and csvNumeric allocated a std::string per snprintf attempt; the
// writer measured 21 MB/s, which is not a formatting rate, it is an allocation
// rate.
//
// The shortcut is the INTEGRAL case (`id`, `year`, `region`, any coded
// variable — most columns of most datasets): the general loop reaches it by
// asking snprintf for "%.17f" and then deleting the seventeen zeros it just
// asked for. An integral double under 1e17 spells identically through
// std::to_chars, in one pass, with no allocation and no trimming. Above 1e17
// the integer part alone exceeds the 17-character budget, so that case still
// belongs to the loop (which sends it to the `%.11e` branch, unchanged).
//
// Everything else is byte-for-byte the old path: same widest-plain-decimal
// search from dec=17 down, same "rounded away to nothing" skip, same `%.11e`
// fallback. The gate is a byte-identity diff of an exported CSV, not a reading
// of this comment.
void appendCsvNumeric(std::string& out, double v) {
  if (std::isnan(v)) {
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    if (m != ".") out += m;
    return;
  }
  if (v == 0.0) { out += '0'; return; }

  if (v == std::floor(v) && std::fabs(v) < 1e17) {
    char ib[32];
    const long long iv = static_cast<long long>(v);
    std::to_chars_result r = std::to_chars(ib, ib + sizeof(ib), iv);
    if (r.ec == std::errc()) {
      const size_t len = static_cast<size_t>(r.ptr - ib);
      if (len - (iv < 0 ? 1u : 0u) <= 17) { out.append(ib, len); return; }
    }
  }

  const bool neg = v < 0;
  const double av = neg ? -v : v;
  char buf[512];
  for (int dec = 17; dec >= 0; --dec) {
    FixedSpan s = fixedDecimalsAbs(buf, sizeof(buf), av, dec);
    if (s.p == nullptr) break;
    if (s.len == 1 && s.p[0] == '0') continue;  // rounded away to nothing
    if (s.len <= 17) {
      if (neg) out += '-';
      out.append(s.p, s.len);
      return;
    }
  }
  char eb[64];
  std::snprintf(eb, sizeof(eb), "%.11e", v);
  out += eb;
}

// How much CSV to accumulate before one write() (P2-PERF ARC-B/S08).
constexpr size_t kCsvBlockBytes = 1u << 20;

// P2-PERF ARC-B/S08: csvField's no-quoting-needed case returned a COPY of the
// level string for every cell. Appending skips it.
void appendCsvField(std::string& out, const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') { needQuote = true; break; }
  }
  if (!needQuote) { out += s; return; }
  out += '"';
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
}

// W:P8. Stata's `export delimited` writes a DATE-formatted variable as its
// formatted string, not as the stored index: measured against Stata's own
// export of the P8 fixture, all 4,800 rows of the `%td` column read `15jan2000`
// there and `14624` here, so every exported CSV handed a raw day count to
// whatever opened it next. (A plain numeric format like `%9.2f` is NOT applied
// — live-verified — so this is specific to the date families.)
//
// The decode rules are the frontend's, mirrored rather than re-derived: this
// is a C++ port of `fmtStataDate` in piet/frontend/js/grid.js (which owns the
// same job for the data grid and carries the design rationale, incl. the
// fixed-52-week year and %ty's un-offset storage). Any change to the families
// or their spelling has to land in BOTH — they are two renderers of one rule.
// `%tC` and `%td` detail strings (grid.js's own FIX:GRID-TFMT-DETAIL surface)
// are deliberately NOT claimed here; those fall through to the numeric path
// exactly as before.
constexpr int64_t kDaysTdEpochToUnix = 3653;  // 01jan1960 -> 01jan1970
constexpr int64_t kMsPerDay = 86400000;

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// days since 1970-01-01 -> civil y/m/d (Hinnant's civil_from_days).
void civilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  *y = yr + (*m <= 2);
}

std::string tdDayString(int64_t dayIndex) {
  static const char* const kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  int64_t y; unsigned m, d;
  civilFromDays(dayIndex - kDaysTdEpochToUnix, &y, &m, &d);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02u%s%lld", d, kMon[m - 1], static_cast<long long>(y));
  return std::string(buf);
}

// The letter after `%t` for a date-family format, or 0 when `fmt` is not one
// this renderer owns. A detail suffix (`%tdNN/DD/CCYY`) yields 0 on purpose.
char dateFamilyOf(const std::string& fmt) {
  if (fmt.size() != 3 || fmt[0] != '%' || fmt[1] != 't') return 0;
  switch (fmt[2]) {
    case 'd': case 'w': case 'm': case 'q': case 'h': case 'y': case 'c':
      return fmt[2];
    default: return 0;
  }
}

std::string formatStataDate(double v, char family) {
  if (!std::isfinite(v)) return std::string();
  const int64_t i = static_cast<int64_t>(std::floor(v));
  char buf[48];
  switch (family) {
    case 'd': return tdDayString(i);
    case 'w': std::snprintf(buf, sizeof(buf), "%lldw%lld",
                            static_cast<long long>(1960 + floorDiv(i, 52)),
                            static_cast<long long>(floorMod(i, 52) + 1)); return buf;
    case 'm': std::snprintf(buf, sizeof(buf), "%lldm%lld",
                            static_cast<long long>(1960 + floorDiv(i, 12)),
                            static_cast<long long>(floorMod(i, 12) + 1)); return buf;
    case 'q': std::snprintf(buf, sizeof(buf), "%lldq%lld",
                            static_cast<long long>(1960 + floorDiv(i, 4)),
                            static_cast<long long>(floorMod(i, 4) + 1)); return buf;
    case 'h': std::snprintf(buf, sizeof(buf), "%lldh%lld",
                            static_cast<long long>(1960 + floorDiv(i, 2)),
                            static_cast<long long>(floorMod(i, 2) + 1)); return buf;
    case 'y': std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i)); return buf;
    case 'c': {
      const int64_t days = floorDiv(i, kMsPerDay);
      const int64_t rem = floorMod(i, kMsPerDay);
      std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld", tdDayString(days).c_str(),
                    static_cast<long long>(floorDiv(rem, 3600000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 3600000), 60000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 60000), 1000)));
      return buf;
    }
    default: return std::string();
  }
}

// The delimiter named by `delimiter(D)`; Stata spells tab/comma/semicolon out.
bool parseDelimiterOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       char* out, std::string* err) {
  for (const auto& kv : opts) {
    if (kv.first != "delimiter" && kv.first != "delim") continue;
    if (kv.second.isBool) continue;
    std::string v = trim(kv.second.strValue);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    std::string lower = strutil::lower(v);
    if (lower == "tab" || v == "\\t") { *out = '\t'; return true; }
    if (lower == "comma") { *out = ','; return true; }
    if (lower == "semicolon") { *out = ';'; return true; }
    if (v.size() == 1) { *out = v[0]; return true; }
    *err = "delimiter(): expected a single character, `tab`, `comma` or `semicolon` (got '" + v + "')";
    return false;
  }
  return true;  // absent -> caller's default
}

bool hasFlagOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::string& name) {
  for (const auto& kv : opts) if (kv.first == name) return true;
  return false;
}

// An option's value with its surrounding double quotes off. `sheet("Alpha")`
// and `sheet(Alpha)` are the same request.
std::string optDequote(const std::string& raw) {
  std::string v = trim(raw);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return trim(v);
}

// The value of a named option, or nullopt when it is absent or was given as a
// bare flag.
std::optional<std::string> optStrOf(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& name) {
  for (const auto& kv : opts) {
    if (kv.first != name) continue;
    if (kv.second.isBool) return std::nullopt;
    return optDequote(kv.second.strValue);
  }
  return std::nullopt;
}

// --- AER scope-2 W3a: `import delimited`'s subsetting options ---------------
//
// These five options are implemented by STAGING a rewritten .csv and handing it
// to the same loader the option-free path already uses, rather than by teaching
// the loader five new arguments. The reason is containment: the option-free
// path is byte-for-byte the one that captured every existing fixture, and it
// stays untouched — `needsDelimPreprocess()` is false unless one of the new
// options is actually present.

// One delimited document -> rows of fields. Quoting is RFC 4180's: a field may
// be double-quoted, a doubled quote inside one is a literal quote, and a quoted
// field may contain the delimiter AND newlines. Handling the embedded newline
// is not decoration — a row-window (`rowrange`) computed by splitting on '\n'
// would cut such a record in half and stage a corrupt file at rc 0.
std::vector<std::vector<std::string>> splitDelimitedRows(const std::string& text, char delim) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool inQuotes = false;
  bool sawAny = false;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const char c = text[i];
    if (inQuotes) {
      if (c == '"') {
        if (i + 1 < n && text[i + 1] == '"') { field.push_back('"'); i += 2; continue; }
        inQuotes = false; ++i; continue;
      }
      field.push_back(c); ++i; continue;
    }
    if (c == '"') { inQuotes = true; sawAny = true; ++i; continue; }
    if (c == delim) { row.push_back(field); field.clear(); sawAny = true; ++i; continue; }
    if (c == '\r') { ++i; continue; }          // CRLF and lone CR both end the line at '\n'
    if (c == '\n') {
      row.push_back(field); field.clear();
      rows.push_back(std::move(row));
      row.clear();
      sawAny = false;
      ++i; continue;
    }
    field.push_back(c); sawAny = true; ++i;
  }
  if (!field.empty() || sawAny || !row.empty()) {
    row.push_back(field);
    rows.push_back(std::move(row));
  }
  return rows;
}

// `rowrange(a:b)` / `colrange(a:b)` — both ends optional, 1-based inclusive.
// 0 comes back for an end the user left out.
bool parseIndexRangeOpt(const std::string& what, const std::string& spec, long long* lo,
                        long long* hi, std::string* err) {
  const std::string s = trim(spec);
  const size_t colon = s.find(':');
  const std::string a = colon == std::string::npos ? s : trim(s.substr(0, colon));
  const std::string b = colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
  auto num = [&](const std::string& t, long long* out) {
    if (t.empty()) { *out = 0; return true; }
    if (t.find_first_not_of("0123456789") != std::string::npos) return false;
    try { *out = std::stoll(t); } catch (...) { return false; }
    return *out > 0;
  };
  if (!num(a, lo) || !num(b, hi)) {
    *err = what + "(" + s + "): expected first:last, both 1-based (either side may be omitted)";
    return false;
  }
  if (*hi > 0 && *lo > *hi) {
    *err = what + "(" + s + "): the range ends before it starts";
    return false;
  }
  return true;
}

// `numericcols(numlist)` — Stata's numlist, in the two shapes this option is
// ever written in: space/comma-separated indices and `a/b` runs, plus `_all`.
bool parseColumnNumlist(const std::string& spec, std::set<long long>* out, bool* all,
                        std::string* err) {
  *all = false;
  const std::string s = trim(spec);
  if (strutil::lower(s) == "_all") { *all = true; return true; }
  std::string tok;
  std::istringstream iss(s);
  bool any = false;
  while (iss >> tok) {
    while (!tok.empty() && tok.back() == ',') tok.pop_back();
    if (tok.empty()) continue;
    const size_t slash = tok.find('/');
    auto num = [&](const std::string& t, long long* v) {
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos) return false;
      try { *v = std::stoll(t); } catch (...) { return false; }
      return *v > 0;
    };
    if (slash == std::string::npos) {
      long long v = 0;
      if (!num(tok, &v)) { *err = "numericcols(" + s + "): `" + tok + "` is not a column number"; return false; }
      out->insert(v);
    } else {
      long long a = 0, b = 0;
      if (!num(tok.substr(0, slash), &a) || !num(tok.substr(slash + 1), &b) || a > b) {
        *err = "numericcols(" + s + "): `" + tok + "` is not a column run";
        return false;
      }
      for (long long v = a; v <= b; ++v) out->insert(v);
    }
    any = true;
  }
  if (!any) { *err = "numericcols(): expected column numbers, for example numericcols(2 4/6)"; return false; }
  return true;
}

// A field that `numericcols()` names but that is not a number becomes MISSING —
// which is what forcing a column numeric means. Stata's own missing tokens are
// left alone so the loader still reads them as missing.
bool looksNumericField(const std::string& raw) {
  const std::string t = trim(raw);
  if (t.empty()) return true;
  if (t == "." ) return true;
  if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z') return true;
  size_t used = 0;
  try { (void)std::stod(t, &used); } catch (...) { return false; }
  return used == t.size();
}

// iconv(3) to UTF-8. `fromCode` is Stata's `encoding()` argument passed through
// verbatim — iconv speaks the same names ("latin1", "ISO-8859-1",
// "windows-1252", ...), so a name it rejects is reported rather than guessed at.
bool iconvToUtf8(const std::string& in, const std::string& fromCode, std::string* out,
                 std::string* err) {
  const std::string from = trim(fromCode);
  const std::string lowered = strutil::lower(from);
  if (lowered == "utf-8" || lowered == "utf8") { *out = in; return true; }
  iconv_t cd = ::iconv_open("UTF-8", from.c_str());
  if (cd == reinterpret_cast<iconv_t>(-1)) {
    *err = "encoding(" + from + "): this system's iconv does not know that character encoding";
    return false;
  }
  std::string result;
  result.reserve(in.size() + in.size() / 4 + 16);
  std::vector<char> inBuf(in.begin(), in.end());
  char* inPtr = inBuf.empty() ? nullptr : inBuf.data();
  size_t inLeft = inBuf.size();
  std::vector<char> chunk(65536);
  bool ok = true;
  while (inLeft > 0) {
    char* outPtr = chunk.data();
    size_t outLeft = chunk.size();
    const size_t rc = ::iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    result.append(chunk.data(), chunk.size() - outLeft);
    if (rc == static_cast<size_t>(-1)) {
      if (errno == E2BIG) continue;            // output chunk full — drain and go on
      *err = "encoding(" + from + "): the file is not valid " + from +
             " (iconv stopped after " + std::to_string(result.size()) + " converted bytes)";
      ok = false;
      break;
    }
  }
  ::iconv_close(cd);
  if (!ok) return false;
  *out = std::move(result);
  return true;
}

}  // namespace

ojson runExportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  Datastore* ds = session.datastore();

  char delim = ',';
  std::string derr;
  if (!parseDelimiterOpt(opts, &delim, &derr)) return engineErrorBlock(derr, "unsupported_option");
  const bool noLabel = hasFlagOpt(opts, "nolabel");
  const bool replace = hasFlagOpt(opts, "replace");

  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = ds->names();
  for (const auto& v : cols) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  std::string path = resolveUserPath(session, pathArg);
  if (cecon::native_exists(path) && !replace) {
    return engineErrorBlock("export delimited: file " + path +
                            " already exists (add `replace` to overwrite)");
  }

  // Write to a sibling temp then rename, so a failure never leaves a truncated
  // file where a readable one used to be (and dest==source is safe).
  std::string tmpPath = path + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmpPath), std::ios::binary);
    if (!f) return engineErrorBlock("export delimited: cannot write " + path, "data_io");

    for (size_t c = 0; c < cols.size(); ++c) {
      if (c) f << delim;
      f << csvField(cols[c], delim);
    }
    f << "\n";

    // Per-column accessors resolved once, outside the row loop.
    struct OutCol {
      const cecon::Column* col = nullptr;
      const std::map<std::string, std::string>* labels = nullptr;  // value labels, if any
      char dateFamily = 0;  // W:P8: letter after `%t`, 0 for a non-date column
    };
    std::vector<OutCol> oc;
    oc.reserve(cols.size());
    for (const auto& name : cols) {
      OutCol o;
      o.col = &ds->col(name);
      if (!noLabel) {
        auto lv = session.varLblname.find(name);
        if (lv != session.varLblname.end()) {
          auto ld = session.labelDefs.find(lv->second);
          if (ld != session.labelDefs.end()) o.labels = &ld->second;
        }
      }
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.format.has_value()) {
        o.dateFamily = dateFamilyOf(*vm->second.format);
      }
      oc.push_back(o);
    }

    // P2-PERF ARC-B/S08: rows accumulate into a ~1 MB block and go out with one
    // write() each, instead of one operator<< per row through the stream's
    // default buffer. Same bytes, two orders of magnitude fewer stream calls.
    const int64_t n = ds->nRows();
    std::string block;
    block.reserve(kCsvBlockBytes + 4096);
    for (int64_t r = 0; r < n; ++r) {
      for (size_t c = 0; c < oc.size(); ++c) {
        if (c) block += delim;
        const cecon::Column& col = *oc[c].col;
        if (col.dtype == cecon::DType::Factor) {
          int32_t code = col.codes()[r];
          if (code >= 0) appendCsvField(block, col.levels[static_cast<size_t>(code)], delim);
        } else {
          double v = col.f64()[r];
          // A value-labeled numeric exports as its LABEL (Stata's default;
          // `nolabel` gives the code) — the reason this writer lives in PIET.
          if (oc[c].labels != nullptr && !std::isnan(v)) {
            std::string key = eval::pythonFloatRepr(v);
            auto it = oc[c].labels->find(key);
            if (it == oc[c].labels->end() && v == std::floor(v)) {
              it = oc[c].labels->find(std::to_string(static_cast<long long>(v)));
            }
            if (it != oc[c].labels->end()) { appendCsvField(block, it->second, delim); continue; }
          }
          if (oc[c].dateFamily != 0 && !std::isnan(v)) {
            std::string ds8 = formatStataDate(v, oc[c].dateFamily);
            if (!ds8.empty()) { appendCsvField(block, ds8, delim); continue; }
          }
          appendCsvNumeric(block, v);
        }
      }
      block += '\n';
      if (block.size() >= kCsvBlockBytes) {
        f.write(block.data(), static_cast<std::streamsize>(block.size()));
        block.clear();
        if (!f) break;
      }
    }
    if (!block.empty()) f.write(block.data(), static_cast<std::streamsize>(block.size()));
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
      return engineErrorBlock("export delimited: failed writing " + path + " (disk full?)",
                              "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmpPath), cecon::to_native_path(path), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
    return engineErrorBlock("export delimited: failed writing " + path, "data_io");
  }

  ojson block;
  block["type"] = "display";
  block["text"] = "file " + path + " saved";
  return block;
}

namespace {

// `import delimited ..., case(preserve|lower|upper)` -- BUG-477. Unlike
// import excel (default preserve, [D] import excel p.495), import
// delimited's OWN default is to LOWER-CASE the header -- measured against
// StataBE 19.5 on a `make,price,MPG,Foreign` header (tests/eval/golden/
// aer_b4/b5_importcase_pins.log):
//   B5REC|import_delimited|no_case_default|0|
//   B5VAL|vars_default|make price mpg foreign|x|0     <- no option at all
//   B5VAL|vars_preserve|make price MPG Foreign|x|0    <- case(preserve)
//   B5VAL|vars_lower|make price mpg foreign|x|0
//   B5VAL|vars_upper|MAKE PRICE MPG FOREIGN|x|0
// An unrecognised value is refused, not silently ignored (case_bogus|198).
// Scope: `import delimited` only -- `import excel`'s case() and its
// (different) default are a separate, already-measured option
// (handlers_data_verbs3.cpp's NameCase/sanitizeVarName above) and this item
// does not touch it.
enum class DelimCase { kPreserve, kLower, kUpper };

// Only the ASCII letters change case -- the header cells reaching this point
// are already legal Stata identifiers (the loader would have rejected an
// illegal one), so this is a pure letter-case transform, never a re-legalize.
std::string asciiCaseFold(const std::string& s, DelimCase c) {
  if (c == DelimCase::kPreserve) return s;
  std::string out = s;
  for (char& ch : out) {
    if (c == DelimCase::kLower && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    else if (c == DelimCase::kUpper && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  return out;
}

// Applied AFTER the frame is already loaded (by either the fast path's bare
// `runUse` or the preprocess path's staged-CSV `runUse`), so this is one
// rename pass over whatever names actually landed -- never a second parse of
// the file. A header that collides after folding (two cells differing only
// in case) surfaces as runRename's own "already exists" error, same as Stata.
// The same rule `rename` enforces: a letter or underscore, then letters, digits and
// underscores, at most 32 bytes. Used only to SKIP a column this loader already named
// something `rename` would refuse (BUG-480); it is not a second name adjudicator.
bool isValidStataVarName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  if (!(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
  for (char ch : n)
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return false;
  return true;
}

ojson applyImportDelimCase(SessionState& session, DelimCase c, ojson loadBlock) {
  if (c == DelimCase::kPreserve) return loadBlock;
  if (loadBlock.is_object() && loadBlock.value("type", std::string()) == "error") return loadBlock;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return loadBlock;
  const std::vector<std::string> names = ds->names();  // snapshot: rename below mutates the datastore
  for (const auto& name : names) {
    const std::string want = asciiCaseFold(name, c);
    if (want == name) continue;
    // A name this loader already created but `rename` will not accept is left
    // alone. `import delimited ..., varnames(9)` on a file whose ninth row is
    // DATA makes a column literally named `buick regal` -- space and all -- and
    // that is a pre-existing loader defect (BUG-480), not something case() can
    // repair: folding an invalid name leaves it invalid. Failing the whole
    // import here would turn one bad column name into a dead statement, and it
    // is how this change first went RED against the AER b4 probe and the
    // registry pangram. Case-folding is not the place to adjudicate names.
    if (!isValidStataVarName(name) || !isValidStataVarName(want)) continue;
    ojson r = runRename(name, want, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
  }
  return loadBlock;
}

}  // namespace

ojson runImportDelimited(const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  // `delimiter()` is accepted and honored by the shared loader's own sniffing
  // for tab/comma; an explicit exotic delimiter is not plumbed through yet, so
  // say so rather than silently reading the file with the wrong separator.
  char delim = ',';
  bool delimGiven = false;
  {
    std::string derr;
    char probe = ',';
    if (!parseDelimiterOpt(opts, &probe, &derr)) {
      return engineErrorBlock(derr, "unsupported_option");
    }
    for (const auto& kv : opts) {
      if (kv.first == "delimiter" || kv.first == "delim") { delimGiven = true; break; }
    }
    delim = probe;
  }

  // BUG-477: case() is parsed and validated up front, unconditionally --
  // every import delimited call has a case (the default IS one, `lower`),
  // not just the ones that also pass varnames()/rowrange()/etc.
  DelimCase nameCase = DelimCase::kLower;
  if (auto c = optStrOf(opts, "case")) {
    const std::string v = strutil::lower(*c);
    if (v == "preserve") nameCase = DelimCase::kPreserve;
    else if (v == "lower") nameCase = DelimCase::kLower;
    else if (v == "upper") nameCase = DelimCase::kUpper;
    else {
      return engineErrorBlock(
          "import delimited: case(" + *c + ") is not one of preserve, lower or upper",
          "unsupported_option");
    }
  }

  // AER scope-2 W3a. Absent every one of these OTHER four, nothing below
  // runs and the option-free path is the byte-identical one that has always
  // shipped -- case() is handled separately, as a post-load rename pass, so
  // it does not have to route through the CSV-restaging machinery below.
  const std::optional<std::string> varnamesOpt = optStrOf(opts, "varnames");
  const std::optional<std::string> rowrangeOpt = optStrOf(opts, "rowrange");
  const std::optional<std::string> colrangeOpt = optStrOf(opts, "colrange");
  const std::optional<std::string> numericcolsOpt = optStrOf(opts, "numericcols");
  const std::optional<std::string> encodingOpt = optStrOf(opts, "encoding");
  const bool preprocess = varnamesOpt.has_value() || rowrangeOpt.has_value() ||
                          colrangeOpt.has_value() || numericcolsOpt.has_value() ||
                          encodingOpt.has_value();
  // Each of the five takes an argument; a bare spelling is a syntax error in
  // Stata, and accepting it here would mean silently ignoring the request.
  for (const char* k : {"varnames", "rowrange", "colrange", "numericcols", "encoding"}) {
    for (const auto& kv : opts) {
      if (kv.first == k && kv.second.isBool) {
        return engineErrorBlock(std::string("import delimited: ") + k +
                                    " takes an argument, for example " +
                                    (std::string(k) == "varnames" ? "varnames(1)" : std::string(k) + "(...)"),
                                "unsupported_option");
      }
    }
  }

  if (!preprocess) {
    if (delim != ',' && delim != '\t') {
      return engineErrorBlock(
          "import delimited: this build reads comma- and tab-separated files (delimiter(" +
          std::string(1, delim) + ") is not implemented)");
    }
    // Everything else — path resolution, content sniffing, factor detection — is
    // the SAME loader `use` runs, so a .csv reads identically either way.
    return applyImportDelimCase(session, nameCase, runUse(pathArg, session));
  }

  const std::string path = resolveUserPath(session, pathArg);
  if (!cecon::native_exists(path)) {
    return engineErrorBlock("import delimited: file " + path + " not found", "data_io");
  }
  std::string text;
  {
    std::ifstream f(cecon::to_native_path(path), std::ios::binary);
    if (!f) return engineErrorBlock("import delimited: cannot read " + path, "data_io");
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }
  if (encodingOpt.has_value()) {
    std::string converted, err;
    if (!iconvToUtf8(text, *encodingOpt, &converted, &err)) {
      return engineErrorBlock("import delimited: " + err, "data_io");
    }
    text = std::move(converted);
  }
  if (!delimGiven) {
    // "import delimited will automatically detect either a comma or a tab as
    // the delimiter" ([D] import delimited). The loader does this for itself on
    // the option-free path; when this arm re-splits the file it has to make the
    // same call, and splitting a tab file on commas would stage ONE column.
    const size_t eol = text.find('\n');
    const std::string first = text.substr(0, eol == std::string::npos ? text.size() : eol);
    delim = std::count(first.begin(), first.end(), '\t') >
                    std::count(first.begin(), first.end(), ',')
                ? '\t'
                : ',';
  }

  std::vector<std::vector<std::string>> rows = splitDelimitedRows(text, delim);
  if (rows.empty()) return engineErrorBlock("import delimited: " + path + " has no rows to read");

  // varnames(# | nonames): WHICH PHYSICAL ROW holds the names. Stata reads it
  // independently of rowrange -- "import delimited still used the first line of
  // the file to obtain the variable names even though we did not start our
  // rowrange() specification with 1" ([D] import delimited p.488).
  long long namesRow = 1;  // 0 == nonames
  if (varnamesOpt.has_value()) {
    const std::string v = strutil::lower(trim(*varnamesOpt));
    if (v == "nonames") {
      namesRow = 0;
    } else if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
      try { namesRow = std::stoll(v); } catch (...) { namesRow = -1; }
      if (namesRow <= 0) namesRow = -1;
    } else {
      namesRow = -1;
    }
    if (namesRow < 0) {
      return engineErrorBlock(
          "import delimited: varnames(" + *varnamesOpt + ") is not a row number or `nonames`",
          "unsupported_option");
    }
    if (namesRow > 0 && static_cast<size_t>(namesRow) > rows.size()) {
      return engineErrorBlock("import delimited: varnames(" + std::to_string(namesRow) +
                                  ") — the file has only " + std::to_string(rows.size()) + " rows",
                              "unsupported_option");
    }
  }

  long long rowLo = 0, rowHi = 0, colLo = 0, colHi = 0;
  if (rowrangeOpt.has_value()) {
    std::string err;
    if (!parseIndexRangeOpt("rowrange", *rowrangeOpt, &rowLo, &rowHi, &err)) {
      return engineErrorBlock("import delimited: " + err, "unsupported_option");
    }
  }
  if (colrangeOpt.has_value()) {
    std::string err;
    if (!parseIndexRangeOpt("colrange", *colrangeOpt, &colLo, &colHi, &err)) {
      return engineErrorBlock("import delimited: " + err, "unsupported_option");
    }
  }

  // The header, sliced by colrange exactly as the data rows are.
  size_t width = 0;
  for (const auto& r : rows) width = std::max(width, r.size());
  const size_t cLo = colLo > 0 ? static_cast<size_t>(colLo) - 1 : 0;
  size_t cHi = colHi > 0 ? static_cast<size_t>(colHi) : width;   // exclusive
  if (cHi > width) cHi = width;
  if (cLo >= cHi) {
    return engineErrorBlock("import delimited: colrange(" + colrangeOpt.value_or(std::string()) +
                                ") selects no columns — the file has " + std::to_string(width),
                            "unsupported_option");
  }
  const size_t outWidth = cHi - cLo;

  std::vector<std::string> header;
  if (namesRow > 0) {
    const std::vector<std::string>& src = rows[static_cast<size_t>(namesRow) - 1];
    for (size_t c = cLo; c < cHi; ++c) header.push_back(c < src.size() ? src[c] : std::string());
  } else {
    // `nonames`: Stata's own generated names.
    for (size_t c = 0; c < outWidth; ++c) header.push_back("v" + std::to_string(c + 1));
  }

  std::set<long long> numericCols;
  bool numericAll = false;
  if (numericcolsOpt.has_value()) {
    std::string err;
    if (!parseColumnNumlist(*numericcolsOpt, &numericCols, &numericAll, &err)) {
      return engineErrorBlock("import delimited: " + err, "unsupported_option");
    }
  }

  const size_t rLo = rowLo > 0 ? static_cast<size_t>(rowLo) - 1 : 0;
  size_t rHi = rowHi > 0 ? static_cast<size_t>(rowHi) : rows.size();   // exclusive
  if (rHi > rows.size()) rHi = rows.size();

  std::string csvPath = nextPath(session.workdir(), "import_delim") + ".csv";
  registerScratchMint(csvPath);   // BUG-389 class: the staging file joins the ledger
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return engineErrorBlock("import delimited: cannot stage " + csvPath, "data_io");
    for (size_t c = 0; c < header.size(); ++c) {
      if (c) f << ',';
      f << csvField(header[c], ',');
    }
    f << "\n";
    for (size_t r = rLo; r < rHi; ++r) {
      // The names row is never also a data row: rowrange picks rows to import,
      // and Stata has already spent that one on the variable names.
      if (namesRow > 0 && r == static_cast<size_t>(namesRow) - 1) continue;
      const std::vector<std::string>& src = rows[r];
      for (size_t c = cLo; c < cHi; ++c) {
        if (c > cLo) f << ',';
        if (c >= src.size()) continue;
        const long long outCol = static_cast<long long>(c - cLo) + 1;
        if ((numericAll || numericCols.count(outCol) > 0) && !looksNumericField(src[c])) {
          continue;   // forced numeric, and this field is not a number -> missing
        }
        f << csvField(src[c], ',');
      }
      f << "\n";
    }
    if (!f) return engineErrorBlock("import delimited: failed staging " + csvPath, "data_io");
  }
  return applyImportDelimCase(session, nameCase, runUse(csvPath, session));
}

// ---------------------------------------------------------------------------
// export / import excel (P1 wave). The .xlsx container work lives in
// piet/native/src/xlsx.{h,cpp}; this layer is the Stata surface.

namespace {

// `case(preserve|lower|upper)` — [D] import excel p.495. The case rule applies
// to the names read by `firstrow`, and Stata's default is preserve.
enum class NameCase { kPreserve, kLower, kUpper };

// A header cell -> a legal Stata variable name: non-alphanumerics become '_',
// a leading digit gains a '_' prefix, empty becomes v<N>, and a duplicate gains
// a numeric suffix. Mirrors what `import excel, firstrow` has to do.
//
// AER scope-2 W3a: the case is now a PARAMETER. It used to be a hard
// `strutil::lower`, which is `case(lower)` — one of the three spellings, wired
// as if it were the only one, and not the one Stata defaults to.
std::string sanitizeVarName(const std::string& raw, size_t idx,
                            std::set<std::string>& taken, NameCase nameCase) {
  std::string s;
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') s.push_back(c);
    else s.push_back('_');
  }
  while (!s.empty() && s.front() == '_') s.erase(s.begin());
  if (s.empty()) s = "v" + std::to_string(idx + 1);
  if (std::isdigit(static_cast<unsigned char>(s.front()))) s.insert(s.begin(), '_');
  // "Only the ASCII letters in names are changed to lowercase or uppercase.
  // Unicode characters beyond ASCII range are not changed." ([D] import excel)
  if (nameCase == NameCase::kLower) s = strutil::lower(s);
  else if (nameCase == NameCase::kUpper) {
    for (char& c : s) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
  }
  std::string cand = s;
  int suffix = 1;
  while (taken.count(cand)) cand = s + std::to_string(++suffix);
  taken.insert(cand);
  return cand;
}

// One A1-style cell address. `col`/`row` come back 0 for a side the address
// leaves out, so "A" is column 1 with an unbounded row and "8" is row 8 with an
// unbounded column.
bool parseCellAddr(const std::string& s, int* col, int* row) {
  size_t i = 0;
  long long c = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
    c = c * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    if (c > 1000000) return false;
    ++i;
  }
  long long r = 0;
  bool anyDigit = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    r = r * 10 + (s[i] - '0');
    if (r > 100000000LL) return false;
    anyDigit = true;
    ++i;
  }
  if (i != s.size()) return false;
  if (c == 0 && !anyDigit) return false;
  *col = static_cast<int>(c);
  *row = anyDigit ? static_cast<int>(r) : 0;
  return true;
}

// `cellrange([start][:end])` — [D] import excel p.495. Both sides are optional,
// so `A8`, `A3:AG281`, `:D70` and `A3:` are all legal spellings.
bool parseCellRangeOpt(const std::string& spec, xlsx::ReadOptions* out, std::string* err) {
  const std::string s = trim(spec);
  if (s.empty()) {
    *err = "cellrange(): expected a cell or a cell range, for example cellrange(A3:AG281)";
    return false;
  }
  const size_t colon = s.find(':');
  const std::string start = colon == std::string::npos ? s : trim(s.substr(0, colon));
  const std::string end = colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
  if (!start.empty() && !parseCellAddr(start, &out->firstCol, &out->firstRow)) {
    *err = "cellrange(): `" + start + "` is not a cell reference";
    return false;
  }
  if (!end.empty() && !parseCellAddr(end, &out->lastCol, &out->lastRow)) {
    *err = "cellrange(): `" + end + "` is not a cell reference";
    return false;
  }
  if (out->lastRow > 0 && out->firstRow > out->lastRow) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  if (out->lastCol > 0 && out->firstCol > out->lastCol) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  return true;
}

// Stata's own default variable names when there is no `firstrow`: the SHEET's
// column letters, upper case (BUG-425). `idx0` is 0-based from column A.
std::string excelColumnName(size_t idx0) {
  std::string s;
  for (size_t x = idx0 + 1; x > 0;) {
    const size_t rem = (x - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    x = (x - 1) / 26;
  }
  return s;
}

}  // namespace

ojson runExportExcel(const std::vector<std::string>& varlist, const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  Datastore* ds = session.datastore();
  const bool replace = hasFlagOpt(opts, "replace");
  const bool noLabel = hasFlagOpt(opts, "nolabel");
  // `firstrow(variables)` writes the names as row 1 (Stata's spelling).
  bool header = false;
  for (const auto& kv : opts) {
    if (kv.first != "firstrow") continue;
    header = true;
    if (!kv.second.isBool) {
      std::string v = strutil::lower(trim(kv.second.strValue));
      if (!v.empty() && v != "variables" && v != "varl" && v != "variable") {
        return engineErrorBlock(
            "export excel: firstrow(" + v + ") is not supported — use firstrow(variables)",
            "unsupported_option");
      }
    }
  }

  // `sheet("Alpha")` names the worksheet. Stata's full grammar is
  // `sheet("name"[, modify|replace])`, with the sub-option INSIDE the
  // parentheses; `sheetreplace` / `sheetmodify` are the same request spelled as
  // top-level flags (the spelling the corpus actually uses — 020, 038, 091, 108).
  std::string sheetName;
  bool sheetMerge = false;      // the sub-option asks to keep the rest of the workbook
  std::string sheetMergeWord;
  if (auto sh = optStrOf(opts, "sheet")) {
    std::string v = *sh;
    const size_t comma = v.find(',');
    if (comma != std::string::npos) {
      sheetMergeWord = strutil::lower(trim(v.substr(comma + 1)));
      v = optDequote(v.substr(0, comma));
      if (sheetMergeWord != "modify" && sheetMergeWord != "replace") {
        return engineErrorBlock(
            "export excel: sheet(\"" + v + "\", " + sheetMergeWord +
                ") — the sub-option is `modify` or `replace`",
            "unsupported_option");
      }
      sheetMerge = true;
    }
    sheetName = v;
  }
  if (hasFlagOpt(opts, "sheetreplace")) { sheetMerge = true; sheetMergeWord = "sheetreplace"; }
  if (hasFlagOpt(opts, "sheetmodify")) { sheetMerge = true; sheetMergeWord = "sheetmodify"; }
  if (sheetMerge && sheetName.empty()) {
    return engineErrorBlock(
        "export excel: " + sheetMergeWord + " needs the sheet it applies to — add sheet(\"name\")",
        "unsupported_option");
  }

  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = ds->names();
  for (const auto& v : cols) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  std::string path = resolveUserPath(session, pathArg);
  const bool workbookExists = cecon::native_exists(path);
  // "replace cannot be specified when modifying or replacing a given
  // worksheet" ([D] import excel p.496) — the two mean opposite things about
  // the rest of the workbook, so accepting both would have to pick one
  // silently.
  if (sheetMerge && replace) {
    return engineErrorBlock(
        "export excel: `replace` overwrites the whole workbook and cannot be combined with " +
            sheetMergeWord,
        "unsupported_option");
  }
  if (workbookExists && !replace && !sheetMerge) {
    return engineErrorBlock("export excel: file " + path +
                            " already exists (add `replace` to overwrite)");
  }

  const int64_t n = ds->nRows();
  std::vector<xlsx::OutColumn> out;
  out.reserve(cols.size());
  for (const auto& name : cols) {
    const cecon::Column& col = ds->col(name);
    xlsx::OutColumn oc;
    oc.name = name;
    oc.isNumber.assign(static_cast<size_t>(n), false);
    oc.isBlank.assign(static_cast<size_t>(n), false);
    oc.numbers.assign(static_cast<size_t>(n), 0.0);
    oc.strings.assign(static_cast<size_t>(n), std::string());

    const std::map<std::string, std::string>* labels = nullptr;
    if (!noLabel) {
      auto lv = session.varLblname.find(name);
      if (lv != session.varLblname.end()) {
        auto ld = session.labelDefs.find(lv->second);
        if (ld != session.labelDefs.end()) labels = &ld->second;
      }
    }

    for (int64_t r = 0; r < n; ++r) {
      size_t i = static_cast<size_t>(r);
      if (col.dtype == cecon::DType::Factor) {
        int32_t code = col.codes()[r];
        if (code < 0) { oc.isBlank[i] = true; continue; }
        oc.strings[i] = col.levels[static_cast<size_t>(code)];
      } else {
        double v = col.f64()[r];
        if (std::isnan(v)) {
          // Excel has no missing sentinel; Stata writes the cell EMPTY. The
          // extended code is not representable here — a named limitation, not
          // a silent substitution (the delimited writer does carry it).
          oc.isBlank[i] = true;
          continue;
        }
        if (labels != nullptr) {
          std::string key = eval::pythonFloatRepr(v);
          auto it = labels->find(key);
          if (it == labels->end() && v == std::floor(v))
            it = labels->find(std::to_string(static_cast<long long>(v)));
          if (it != labels->end()) { oc.strings[i] = it->second; continue; }
        }
        oc.isNumber[i] = true;
        oc.numbers[i] = v;
      }
    }
    out.push_back(std::move(oc));
  }

  try {
    // A sheetreplace against a workbook that is not there yet is just a write:
    // "If there is no worksheet named sheetname in the workbook, a new sheet
    // named sheetname is created" ([D] import excel p.496), and there is no
    // workbook to preserve.
    if (sheetMerge && workbookExists) xlsx::writeSheetInto(path, sheetName, out, header);
    else xlsx::write(path, out, header, sheetName);
  } catch (const std::exception& e) {
    return engineErrorBlock(std::string("export excel: ") + e.what(), "data_io");
  }
  ojson block;
  block["type"] = "display";
  block["text"] = "file " + path + " saved";
  return block;
}

// `import excel [using] FILE [, sheet("S") cellrange(R) firstrow case(C)
//                              allstring describe clear]` — AER scope-2 W3a.
//
// The option cluster this build was missing is the single most frequent one in
// the twelve-paper corpus (`import excel` appears 273 times, 243 of them in
// paper 015), and every one of those calls names a sheet.
ojson runImportExcel(const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const bool firstrow = hasFlagOpt(opts, "firstrow");
  const bool wantDescribe = hasFlagOpt(opts, "describe");
  const bool allstring = hasFlagOpt(opts, "allstring");

  // case(preserve|lower|upper). Stata's default is preserve ([D] import excel
  // p.495); this build hard-lowercased until W3a, which is case(lower) wired as
  // if it were the only rule.
  NameCase nameCase = NameCase::kPreserve;
  if (auto c = optStrOf(opts, "case")) {
    const std::string v = strutil::lower(*c);
    if (v == "preserve") nameCase = NameCase::kPreserve;
    else if (v == "lower") nameCase = NameCase::kLower;
    else if (v == "upper") nameCase = NameCase::kUpper;
    else {
      return engineErrorBlock(
          "import excel: case(" + *c + ") is not one of preserve, lower or upper",
          "unsupported_option");
    }
  }

  xlsx::ReadOptions ro;
  ro.firstRowIsHeader = firstrow;
  if (auto sh = optStrOf(opts, "sheet")) ro.sheetName = *sh;
  if (auto cr = optStrOf(opts, "cellrange")) {
    std::string err;
    if (!parseCellRangeOpt(*cr, &ro, &err)) {
      return engineErrorBlock("import excel: " + err, "unsupported_option");
    }
  }
  // `allstring("fmt")` names the display format used to render the numbers.
  // This build renders through `tostring`'s default, so an explicit format
  // would be silently ignored — say so instead.
  if (auto af = optStrOf(opts, "allstring")) {
    if (!af->empty()) {
      return engineErrorBlock(
          "import excel: allstring(" + *af +
              ") is not supported — this build's allstring renders through tostring's default "
              "format (bare `allstring` works)",
          "unsupported_option");
    }
  }

  std::string path = resolveUserPath(session, pathArg);
  if (!cecon::native_exists(path)) {
    return engineErrorBlock("import excel: file " + path + " not found", "data_io");
  }

  // ---- `describe`: a pure QUERY. It must not touch the dataset, so it returns
  // before anything below can. Stored results are [D] import excel's own
  // (p.499): r(N_worksheet), r(worksheet_#), r(range_#).
  if (wantDescribe) {
    std::vector<xlsx::SheetInfo> info;
    try {
      info = xlsx::sheets(path);
    } catch (const std::exception& e) {
      return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
    }
    session.eval.rValues.set("N_worksheet", static_cast<double>(info.size()));
    for (size_t i = 0; i < info.size(); ++i) {
      const std::string n = std::to_string(i + 1);
      session.eval.rStrings.set("worksheet_" + n, info[i].name);
      session.eval.rStrings.set("range_" + n, info[i].range);
    }
    size_t nameWidth = 5;  // "Sheet"
    for (const auto& si : info) nameWidth = std::max(nameWidth, si.name.size());
    std::string text = "\n";
    text += "Sheet" + std::string(nameWidth - 5 + 3, ' ') + "Range\n";
    text += std::string(nameWidth + 3 + 20, '-') + "\n";
    for (const auto& si : info) {
      text += si.name + std::string(nameWidth - si.name.size() + 3, ' ') + si.range + "\n";
    }
    ojson block;
    block["type"] = "display";
    block["text"] = text;
    return block;
  }

  xlsx::Sheet sheet;
  try {
    sheet = xlsx::read(path, ro);
  } catch (const xlsx::SheetNotFound& e) {
    // Stata's own code and message, both captured (aer_b4
    // milfpins_oracle_g.log:136-139: `worksheet NoSuchSheet not found`, r(601)).
    // The rc travels as an explicit `reason`, because coarseRc's file/directory
    // rule would otherwise never see a message that says "worksheet".
    return eval::errorBlock("engine_error", e.what(), std::nullopt, std::nullopt, std::nullopt,
                            "r(601)");
  } catch (const xlsx::CellRangeOutsideData& e) {
    // B4 P2: Stata's own two messages, verbatim and unprefixed, under
    // `unsupported_option` -> r(198) (oracle_q.log:74-79 column, :84-89 row).
    // W3a clamped instead, and said in its own comment that it had no capture
    // to settle it against; oracle_q is that capture. Thrown BEFORE any data is
    // installed, which is also what Stata does — it leaves c(k)==0 and _N==0
    // (:81-82, :91-92).
    return engineErrorBlock(e.what(), "unsupported_option");
  } catch (const std::exception& e) {
    return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
  }

  size_t nCols = sheet.header.size();
  for (const auto& r : sheet.rows) nCols = std::max(nCols, r.size());
  if (nCols == 0) return engineErrorBlock("import excel: " + path + " has no cells to read");

  // BUG-425: without `firstrow`, Stata names the variables after the SHEET's own
  // column letters — A, B, ... Z, AA, AB — not v1, v2. It is a documented naming
  // rule ([D] import excel) and the reason `summarize B` is what a do-file
  // written against Stata says; `v2` made those stop at rc 111.
  //
  // W3a: those letters are the ORIGINAL spreadsheet's, not the window's. With
  // `cellrange(C3:D9)` the two columns are still C and D — which is why
  // xlsx::Sheet carries firstCol at all.
  const size_t colBase = sheet.firstCol > 0 ? static_cast<size_t>(sheet.firstCol) - 1 : 0;
  std::vector<std::string> names;
  {
    std::set<std::string> taken;
    for (size_t c = 0; c < nCols; ++c) {
      std::string raw = (firstrow && c < sheet.header.size()) ? sheet.header[c] : std::string();
      if (!raw.empty()) {
        names.push_back(sanitizeVarName(raw, c, taken, nameCase));
      } else if (firstrow) {
        // A header row with an EMPTY cell: Stata falls back to its positional
        // name there, which is what sanitizeVarName("") already produces.
        names.push_back(sanitizeVarName("", c, taken, nameCase));
      } else {
        // The letter goes in VERBATIM, not through sanitizeVarName: Stata's
        // generated names are upper case — `A`, not `a` — and they are not
        // user-supplied headers, so case() has no say over them. Legality is
        // not in question for A..ZZ, so the only thing left to guard is a
        // collision with a name an earlier column already took.
        std::string nm = excelColumnName(colBase + c);
        while (taken.count(nm) > 0) nm += "_";
        taken.insert(nm);
        names.push_back(nm);
      }
    }
  }

  // Materialize as CSV in the session workdir and hand it to the SAME loader
  // `use`/`import delimited` run, so type inference, factor detection and
  // missing handling are one implementation, not three.
  std::string csvPath = nextPath(session.workdir(), "import_excel") + ".csv";
  // BUG-389 class: register the mint. Without this the staging CSV belongs to no
  // ledger, so the moment it stops being the session head -- an `import excel`
  // followed by `clear`, or by any other load -- it survives to exit as an
  // ORPHAN. Every sibling stager in the tree carries this line; this one was
  // missed. Found by AER-SCOPE1 bundle G's corpus probe, which is the only leg
  // that looks at the exit census.
  registerScratchMint(csvPath);
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return engineErrorBlock("import excel: cannot stage " + csvPath, "data_io");
    for (size_t c = 0; c < names.size(); ++c) {
      if (c) f << ',';
      f << csvField(names[c], ',');
    }
    f << "\n";
    for (const auto& row : sheet.rows) {
      for (size_t c = 0; c < names.size(); ++c) {
        if (c) f << ',';
        if (c >= row.size() || row[c].blank) continue;   // empty field = missing
        if (row[c].isNumber) f << eval::pythonFloatRepr(row[c].number);
        else f << csvField(row[c].text, ',');
      }
      f << "\n";
    }
    if (!f) return engineErrorBlock("import excel: failed staging " + csvPath, "data_io");
  }
  ojson block = runUse(csvPath, session);

  // `allstring` — every column as string. Reuses `tostring`'s own converter
  // rather than adding a second numeric->string rendering: BUG-207 measured
  // what Stata writes for a missing (the `.`/`.a` token, not ""), and a private
  // copy here would be the place that quietly disagrees with it.
  if (allstring && block.is_object() && block.value("type", std::string()) == "data_mutation") {
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      std::vector<std::string> numeric;
      for (const auto& nm : ds->names()) {
        if (ds->dtype(nm) == "f64") numeric.push_back(nm);
      }
      for (const auto& nm : numeric) {
        ojson r = runTostring(nm, /*doReplace=*/true, std::nullopt, /*useDisplayFormat=*/false,
                              session);
        if (r.is_object() && r.value("type", std::string()) == "error") return r;
      }
      // The open block was rendered BEFORE the conversions, so its varlist_after
      // still says f64. Re-render it from the datastore the conversions left
      // behind; the shape is runUse's own (session_state's fold), not a second
      // description of a variable.
      ds = session.datastore();
      if (ds != nullptr) {
        ojson varlist = ojson::array();
        auto vlist = session.varlistWithMeta(ds);
        for (const auto& v : vlist) varlist.push_back(v.toJson());
        session.updateNMissCache(vlist);
        block["varlist_after"] = varlist;
        block["n_rows_after"] = ds->nRows();
      }
    }
  }
  return block;
}


}  // namespace piet::handlers
