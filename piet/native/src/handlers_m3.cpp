// M3a handler-surface expansion — see piet/native/include/piet/handlers.h
// for the per-handler oracle-source mapping. Split from handlers.cpp (the
// M2 vertical slice) to keep that file's diff clean and let this module
// grow independently as M3a/M3b add more commands.
#include "piet/bcal.h"  // AER scope-2 B2-3: %tb formats + the cwd-tracked calendar search dir
#include "piet/handlers.h"
#include "piet/translate.h"
#include "piet/column_fetch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve

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
const piet::eval::OptVocab& kVocabLog = piet::eval::registerOptVocab(
    {"log"}, {{"replace", "append", "text",
                                                                   "smcl", "name"},
     {}});
const piet::eval::OptVocab& kVocabMkdir = piet::eval::registerOptVocab(
    {"mkdir"}, {{"public"},
     {}});
}  // namespace
#endif

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// B5-D14 (BUG-454): an error whose Stata rc is KNOWN but which coarseRc's
// message rules cannot recognise -- same shape as handlers_aer_util.cpp's
// utilErrorRc. `reason` is the only channel carrying an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson engineErrorBlockRc(const std::string& message, int rc) {
  return eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                           "r(" + std::to_string(rc) + ")");
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// Q1 (gate ③): snapshot an on-disk dataset file to `dst`. On APFS a copy-on-
// write clone is effectively free (the ~164MB preserve byte-copy is ~0.6s on
// the reference corpus), so attempt clonefile FIRST and fall back to the
// verbatim byte-copy on ANY failure — non-APFS / cross-volume / a dst-exists
// race, or any non-Apple build (Linux/Windows), where the #if simply compiles
// to the byte-copy alone. The fallback is byte-for-byte the pre-existing copy,
// so behavior is identical everywhere the clone can't be taken. Mirrors the
// engine's own clone-first-fallback (src/data/frame.cpp clone_file, used by
// patch_hbin). clonefile refuses to overwrite an existing dst, so unlink
// first; `dst` is a fresh nextPath() here, so that removal is normally a
// no-op and never touches a live file.
// BUG-124: returns false (and removes any partial dst) when the byte-copy
// fallback fails — an unchecked ENOSPC here left a TORN preserve snapshot
// that a later restore would load. The clonefile path is all-or-nothing.
bool cloneOrCopyFile(const std::string& src, const std::string& dst) {
#if defined(__APPLE__)
  // path::c_str() is const char* on POSIX (Apple); keep the paths alive across
  // the clonefile call so the pointers don't dangle.
  std::filesystem::path nsrc = cecon::to_native_path(src);
  std::filesystem::path ndst = cecon::to_native_path(dst);
  cecon::native_unlink(dst);
  if (::clonefile(nsrc.c_str(), ndst.c_str(), 0) == 0) return true;
  // clonefile failed (non-APFS FS, cross-device, EEXIST race, ...) -> byte-copy.
#endif
  std::ifstream in(cecon::to_native_path(src), std::ios::binary);
  std::ofstream out(cecon::to_native_path(dst), std::ios::binary);
  if (!in || !out) return false;
  // A11 (BUG-138/L5-IOSAVE class sweep): this fallback used to be
  // `out << in.rdbuf()` — libc++ implements that as a byte-at-a-time
  // sgetc/sputc virtual-call loop, ~5.5 ns/byte and *user*-CPU bound, which
  // measured 1.20-1.30 s on the 164 MB reference corpus against 0.35 s for the
  // 1 MB buffered loop below (3.5×, sha256-identical output — see
  // milf-coord/plans/GATE23-stage0-evidence/l5-iosave/). Only the cross-volume
  // / non-APFS preserve snapshot reaches here (the clone above takes the
  // same-volume case), so this is the slowest path getting the 3.5×.
  {
    std::vector<char> buf(1 << 20);
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      std::streamsize got = in.gcount();
      if (got > 0) out.write(buf.data(), got);
      if (!out) break;
    }
  }
  out.flush();
  if (!out || in.bad()) {
    out.close();
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(dst), rmEc);
    return false;
  }
  return true;
}

// BS-001: `gsort` (real cobra verb, cmds_data.cpp's cmd_gsort) prints a
// plain-text status line to STDERR and JSON only on error — runCobraJson()'s
// "parse stdout as JSON" contract doesn't fit, so runSort (below) calls the
// raw runCobra() and builds the CobraError manually from its stderr. File-
// local copy of handlers_data_verbs.cpp's identically-named helper (this
// codebase's established per-TU convention — see this file's own trim()/
// lower() forwarding-wrapper note above) rather than a new shared header.
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for runSort below, reused here by runDrop/runKeep so
// neither needs session.datastore() (a full mmap + header parse of however
// many MB the current hbin is) just to answer "does column X exist". Plain
// linear scan over VarInfo entries, mirroring Datastore::has()'s own O(cols)
// name lookup (cecon::Dataset::has) — nothing this file does needs it to be
// more than that.
// C4-L2A sibling (D1 card plane): the same card lookup handing back the ENTRY
// rather than a yes/no — runConfirm needs dtype alongside the name. File-local
// twin of handlers_data_verbs.cpp's identically-named S4a helper (the
// established per-TU convention — see that file's own note); cardHasName
// forwards to it rather than keeping a second copy of the scan.
const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// xtset / tsset

// BUG-373: tsset/xtset are r-class in Stata and published NOTHING. The declared-
// returns sweep measured 10 names on tsset and 14 on xtset — timevar, panelvar,
// tdelta, tmin/tmax, gaps, and the string twins — and every one of them is
// something a panel do-file reads back. This computes them from the SAME time
// column the delta validation already scans, so the cost is one pass that was
// mostly happening anyway.
//
// Stata's shape, from the live capture (plans/stata-capture-20260831): the
// numeric edges are r(tmin)/r(tmax) and the *s twins are their FORMATTED text.
// With no display format on the column the two read the same, which is what the
// fatalities capture shows (tmin 1982, tmax 1988).
namespace {

struct TsReturns {
  bool ok = false;
  double tmin = 0, tmax = 0;
  double gaps = 0;   // 1 if the distinct times are not a complete delta-spaced run
  int64_t imin = 0, imax = 0;
  bool havePanel = false;
  bool balanced = false;
};

TsReturns computeTsReturns(SessionState& session, const std::string& tvar,
                           const std::optional<std::string>& pvar, double delta) {
  TsReturns r;
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(tvar) || ds->dtype(tvar) != "f64") return r;
  const double* tv = ds->col(tvar).f64();
  const int64_t n = ds->nRows();
  std::vector<double> distinct;
  distinct.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i)
    if (!std::isnan(tv[i])) distinct.push_back(tv[i]);
  if (distinct.empty()) return r;
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  r.tmin = distinct.front();
  r.tmax = distinct.back();
  // "gaps" = the observed distinct times do not fill [tmin, tmax] at the
  // declared period. Stata reports 0/1, not a count.
  if (delta > 0) {
    const double span = (r.tmax - r.tmin) / delta + 1.0;
    r.gaps = (static_cast<double>(distinct.size()) < span - 1e-9) ? 1.0 : 0.0;
  }
  if (pvar.has_value() && !pvar->empty() && ds->has(*pvar) && ds->dtype(*pvar) == "f64") {
    const double* pv = ds->col(*pvar).f64();
    std::map<double, int64_t> per;
    for (int64_t i = 0; i < n; ++i)
      if (!std::isnan(pv[i]) && !std::isnan(tv[i])) ++per[pv[i]];
    if (!per.empty()) {
      r.havePanel = true;
      r.imin = static_cast<int64_t>(per.begin()->first);
      r.imax = static_cast<int64_t>(per.rbegin()->first);
      // Balanced = every panel carries the same number of observations, which is
      // the property xtset itself prints as "(balanced)" / "(unbalanced)".
      const int64_t first = per.begin()->second;
      r.balanced = true;
      for (const auto& kv : per)
        if (kv.second != first) { r.balanced = false; break; }
    }
  }
  r.ok = true;
  return r;
}

// Format a numeric time edge the way Stata's *s twins do. Without a display
// format on the column Stata prints the plain number, which is what the capture
// shows; a formatted column is deliberately NOT guessed at here.
std::string tsEdgeText(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15)
    return std::to_string(static_cast<long long>(v));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return std::string(buf);
}

void publishTsReturns(SessionState& session, const std::string& tvar,
                      const std::optional<std::string>& pvar, std::optional<double> delta) {
  const double d = delta.value_or(1.0);
  TsReturns r = computeTsReturns(session, tvar, pvar, d);
  session.eval.rStrings.set("timevar", tvar);
  session.eval.rStrings.set("tsfmt", "%tg");   // Stata's default when none is set
  session.eval.rStrings.set("unit1", "1");
  session.eval.rStrings.set("tdeltas", tsEdgeText(d));
  session.eval.rValues.set("tdelta", d);
  if (!r.ok) return;
  session.eval.rValues.set("tmin", r.tmin);
  session.eval.rValues.set("tmax", r.tmax);
  session.eval.rValues.set("gaps", r.gaps);
  session.eval.rStrings.set("tmins", tsEdgeText(r.tmin));
  session.eval.rStrings.set("tmaxs", tsEdgeText(r.tmax));
  if (pvar.has_value() && !pvar->empty()) {
    session.eval.rStrings.set("panelvar", *pvar);
    if (r.havePanel) {
      session.eval.rValues.set("imin", static_cast<double>(r.imin));
      session.eval.rValues.set("imax", static_cast<double>(r.imax));
      session.eval.rStrings.set("balanced", r.balanced ? "strongly balanced" : "unbalanced");
    }
  }
}

}  // namespace

ojson runXtset(const std::string& ivar, const std::optional<std::string>& tvar, SessionState& session,
               std::optional<double> delta) {
  // Stata's xtset is not metadata-only: it physically sorts the live data by
  // panel and time before installing the declaration. Reuse the same stable,
  // ascending gsort-backed mutation as an explicit `sort ivar tvar`, but keep
  // xtset's established echo-only output contract by discarding the internal
  // mutation card.
  std::vector<std::string> sortVars = {ivar};
  if (tvar.has_value() && !tvar->empty()) sortVars.push_back(*tvar);
  ojson sorted = runSort(sortVars, session);
  if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;

  session.panelvar = ivar;
  session.timevar = tvar;
  // BS-030 (BETA campaign C2): reset/set delta alongside panelvar/timevar on
  // every xtset call, forwarded to the engine via translate::injectTsContext
  // (--tsdelta). Unlike plain tsset, xtset's delta would need a PER-PANEL
  // minimum-gap check (grouped by ivar) to honestly validate the same way —
  // that grouped validation is not implemented this stint (not oracle-
  // tested for xtset); delta is forwarded un-validated here.
  session.delta = delta;
  // xtset-file-persistence: the declaration lives in the `_dta` char table too,
  // so `save x.dta` writes it and `use`/`restore` bring it back (Stata's model).
  session.writeTsChars();
  session.eval.cwd = session.eval.cwd;  // no-op; eval session unaffected
  if (tvar.has_value() && !tvar->empty()) publishTsReturns(session, *tvar, ivar, delta);
  std::string text = "Panel variable: " + ivar;
  if (tvar.has_value() && !tvar->empty()) text += ", time variable: " + *tvar;
  ojson e;
  e["type"] = "echo";
  e["text"] = text;
  return e;
}

ojson runTsset(const std::string& tvar, SessionState& session, std::optional<double> delta,
               const std::string& tsFmt) {
  // BS-030 (BETA campaign C2): validate delta() against the ACTUAL data —
  // real Stata rejects a declared period narrower than the true minimum gap
  // between two (sorted, distinct) time values ("time values with period
  // less than delta() found"), leaving the ts context UNSET. Live-Stata-
  // verified this stint (the shipped oracle log's own BS-030 stanza covers
  // only the plain no-delta case; this exact delta(2)-on-gap-1 scenario and
  // a delta-supported success case were independently re-probed against
  // StataBE and are pinned in the selftest leg below).
  if (delta.has_value() && *delta > 0.0) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(tvar) && ds->dtype(tvar) == "f64") {
      const double* vals = ds->col(tvar).f64();
      int64_t n = ds->nRows();
      std::vector<double> distinct;
      distinct.reserve(static_cast<size_t>(n));
      for (int64_t i = 0; i < n; ++i) {
        double v = vals[i];
        if (!std::isnan(v)) distinct.push_back(v);
      }
      std::sort(distinct.begin(), distinct.end());
      distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
      double minGap = std::numeric_limits<double>::infinity();
      for (size_t i = 1; i < distinct.size(); ++i) {
        minGap = std::min(minGap, distinct[i] - distinct[i - 1]);
      }
      if (std::isfinite(minGap) && minGap < *delta) {
        return engineErrorBlock("time values with period less than delta() found");
      }
    }
  }
  // BUG-383 (leg-1D probe 3, 2026-09-01): Stata's tsset is not metadata-only
  // either — [TS] tsset: "tsset then sorts and indexes the data appropriately"
  // / "sorts the data on timevar or (panelvar, timevar)". runXtset below has
  // done this from the start; tsset never did, and the asymmetry was invisible
  // until the off-pin sweep ran the ts diagnostics on a fixture whose rows are
  // sorted by state only (years scrambled within each state): every command
  // that walks adjacent ROWS as adjacent TIMES — wntestq's autocorrelations,
  // pperron's Newey-West sum, prais's rho/DW — answered for the file order.
  // Measured: file-order rho1 = -0.225906 reproduces our published
  // Q = 0.535853 to the last digit; Stata's time-order rho1 = 0.370 gives its
  // 1.440099. Commands that build lags time-aware (dfuller via ts_apply) were
  // right all along, which is what isolated the class to row-walkers.
  {
    ojson sorted = runSort({tvar}, session);
    if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
  }
  session.panelvar.reset();
  session.timevar = tvar;
  session.delta = delta;
  session.writeTsChars();  // xtset-file-persistence (see runXtset)
  publishTsReturns(session, tvar, std::nullopt, delta);
  // B4-W3b: the period option. publishTsReturns sets r(tsfmt) to Stata's
  // no-period default "%tg" a line above; a declared period overrides BOTH it
  // and the column's own display format (measured: `describe t` reads %tq after
  // `tsset t, quarterly`).
  if (!tsFmt.empty()) {
    session.eval.rStrings.set("tsfmt", tsFmt);
    session.varMeta[tvar].format = tsFmt;
    session.userFormattedVars.insert(tvar);
  }
  ojson e;
  e["type"] = "echo";
  e["text"] = "Time variable: " + tvar;
  return e;
}

// ---------------------------------------------------------------------------
// drop / keep (varlist form)

ojson runDrop(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix below — this varlist check only ever needs column NAMES, so
  // answer it from the resident shadow card when it is provably current for
  // the loaded hbin, instead of unconditionally forcing session.datastore()
  // to reopen the file from disk. Falls back to the exact prior behavior
  // (session.datastore() + ds->has()) whenever the card cannot be trusted —
  // see session.shadowVarlistIfCurrent()'s doc comment (session_state.h) for
  // every case that must fall back rather than risk a wrong "found"/"not
  // found" answer.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "drop");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : varlist) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): drop is a single terminal mutation whose --out
  // becomes the session head and is NEVER read back as a file (session.load()
  // records the virtual head; the card renders from the shadow/materialized
  // datastore) — opt it into the resident frame plane (in-RAM column mutation,
  // no per-verb process spawn). No-op on the fallback plane (serve down/§26-No).
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "drop", {}, varlist);
}

ojson runKeep(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runDrop just above — both of
  // keep's own name-only lookups (the full-column enumeration that builds
  // toDrop, and the has-check that validates varlist) come from the resident
  // shadow card when it is provably current, instead of unconditionally
  // forcing session.datastore() open. Falls back to the exact prior
  // behavior (session.datastore() + ds->names()/ds->has()) otherwise.
  std::vector<std::string> toDrop;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (std::find(varlist.begin(), varlist.end(), info.name) == varlist.end()) {
        toDrop.push_back(info.name);
      }
    }
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& name : ds->names()) {
      if (std::find(varlist.begin(), varlist.end(), name) == varlist.end()) toDrop.push_back(name);
    }
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "keep");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : toDrop) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): keep is a column-subset drop — same single-
  // terminal-mutation profile as runDrop; opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "keep", {}, toDrop);
}

ojson runDropIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // BS-090 (BETA Phase G): `complement=true` keeps the COMPLEMENT of the
  // combined (if AND in) condition. BUG-533: the if-only complement is
  // `--keepif "(EXPR)==0"`, not the old `"!(EXPR)"`, which kept the rows whose
  // EXPR is missing — Stata drops them (missing is true); see
  // applyIfAndInFilter.
  //
  // fix/verb-overhead-sweep S2: opt the row filter into the resident frame
  // plane — the same single-terminal-mutation profile runDrop/runKeep declare
  // above, and the assertion the guard exists to make holds here exactly:
  // `fr.dataPath` becomes the session head via session.load() and is NEVER read
  // back as a file (mutationResult renders it from the shadow card; the
  // fallback rebuild materializes the head first). This is stated HERE, not
  // inside applyIfAndInFilter, because that helper is shared with ~9 callers
  // that DO read its output as a file — summarize's `Datastore(sumifPath)`,
  // collapse's stateless `--data`, the by-group dispatch's `Datastore groupDs`
  // — and only the call site knows which it is. The `in`-range chain suspends
  // this again internally (inrange_filter.cpp), since its own intermediate
  // temps are read back as files.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange, /*complement=*/true);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "drop");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

ojson runKeepIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // fix/verb-overhead-sweep S2: same opt-in as runDropIf just above, for the
  // same reason — see its comment for why this belongs at the call site rather
  // than inside the shared applyIfAndInFilter.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "keep");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

StringKeyOrder stringKeyOrder(const std::vector<std::string>& levels) {
  StringKeyOrder o;
  std::vector<std::string> distinct = levels;
  distinct.push_back(std::string());  // a -1 code reads as ""
  // std::string's operator< is char_traits<char>::compare: byte order, each
  // byte unsigned — Stata's string order.
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  auto rankOf = [&](const std::string& v) {
    return static_cast<double>(std::lower_bound(distinct.begin(), distinct.end(), v) -
                               distinct.begin());
  };
  o.rank.reserve(levels.size());
  for (const auto& l : levels) {
    if (l.empty()) o.blankLevel = true;
    o.rank.push_back(rankOf(l));
  }
  o.blankRank = rankOf(std::string());
  // The engine keys a factor on its code with -1 first, so the code order is
  // Stata's exactly when the levels are strictly byte-ordered and none is "".
  o.codeOrderIsStata = !o.blankLevel;
  for (size_t i = 1; i < levels.size() && o.codeOrderIsStata; ++i)
    o.codeOrderIsStata = levels[i - 1] < levels[i];
  return o;
}

namespace {

// A scratch f64 column runByKeepIf adds before its engine calls and drops in
// the last one.
struct ByKeepTempCol {
  std::string name;
  std::vector<double> values;
};

bool byKeepIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// One whole-operand comparison of a string variable with a "..." literal,
// `VAR ==/!= "lit"` or `"lit" ==/!= VAR`.
struct ByKeepLitCompare {
  std::string var;
  bool eq = true;   // `==` (true) or `!=`
  std::string lit;  // the literal's text, quotes stripped
};

// BUG-532: the one string form the engine evaluates right under `by:` is a
// string variable compared with a literal, `VAR ==/!= "lit"` (frame.cpp eqne
// resolves the literal against VAR's dictionary). This scanner finds every
// such comparison that stands as a WHOLE operand -- bounded by the
// expression's start/end, `(`, `)`, `,`, `&`, `|`, with a plain name (no
// subscript, call or TS operator) -- and hands it to `fn`: a returned text
// replaces the comparison, nullopt keeps it as written. Everything else is
// copied, "..." literals whole.
template <typename IsString, typename Fn>
std::string mapStringLiteralCompares(const std::string& cond, IsString isString, Fn fn) {
  auto skipWs = [&](size_t j) {
    while (j < cond.size() && std::isspace(static_cast<unsigned char>(cond[j]))) ++j;
    return j;
  };
  auto boundedLeft = [&](size_t start) {
    size_t j = start;
    while (j > 0 && std::isspace(static_cast<unsigned char>(cond[j - 1]))) --j;
    if (j == 0) return true;
    const char c = cond[j - 1];
    // `&` / `|` but not the tail of `!=`/`==`/`<=`/`>=` (those end in '=').
    return c == '(' || c == ',' || c == '&' || c == '|';
  };
  auto boundedRight = [&](size_t end) {
    const size_t j = skipWs(end);
    if (j >= cond.size()) return true;
    const char c = cond[j];
    return c == ')' || c == ',' || c == '&' || c == '|';
  };
  // `==` or `!=` at j: 0 (none), 1 (==), 2 (!=).
  auto eqOpAt = [&](size_t j) -> int {
    if (j + 1 >= cond.size() || cond[j + 1] != '=') return 0;
    if (cond[j] == '=') return 1;
    if (cond[j] == '!') return 2;
    return 0;
  };
  // A plain name at [nb, ne) that names a string variable.
  auto plainStringName = [&](size_t nb, size_t ne) {
    return ne > nb && !std::isdigit(static_cast<unsigned char>(cond[nb])) &&
           (ne >= cond.size() || (cond[ne] != '[' && cond[ne] != '(' && cond[ne] != '.')) &&
           isString(cond.substr(nb, ne - nb));
  };

  std::string out;
  out.reserve(cond.size() + 16);
  size_t i = 0;
  while (i < cond.size()) {
    const char c = cond[i];
    if (c == '"') {
      const size_t q = cond.find('"', i + 1);
      if (q == std::string::npos) {
        out.append(cond, i, std::string::npos);
        break;
      }
      // `"lit" OP NAME`
      const size_t opAt = skipWs(q + 1);
      if (const int op = eqOpAt(opAt)) {
        const size_t nb = skipWs(opAt + 2);
        size_t ne = nb;
        while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
        if (plainStringName(nb, ne) && boundedLeft(i) && boundedRight(ne)) {
          const ByKeepLitCompare lc{cond.substr(nb, ne - nb), op == 1,
                                    cond.substr(i + 1, q - i - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = ne;
            continue;
          }
        }
      }
      out.append(cond, i, q + 1 - i);
      i = q + 1;
      continue;
    }
    // `NAME OP "lit"`
    if ((std::isalpha(static_cast<unsigned char>(c)) || c == '_') &&
        (i == 0 || (!byKeepIdentChar(cond[i - 1]) && cond[i - 1] != '.'))) {
      size_t ne = i;
      while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
      const size_t opAt = skipWs(ne);
      const int op = eqOpAt(opAt);
      const size_t lit = op != 0 ? skipWs(opAt + 2) : cond.size();
      if (op != 0 && lit < cond.size() && cond[lit] == '"' && plainStringName(i, ne)) {
        const size_t q = cond.find('"', lit + 1);
        if (q != std::string::npos && boundedLeft(i) && boundedRight(q + 1)) {
          const ByKeepLitCompare lc{cond.substr(i, ne - i), op == 1,
                                    cond.substr(lit + 1, q - lit - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = q + 1;
            continue;
          }
        }
      }
      out.append(cond, i, ne - i);
      i = ne;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// BUG-532: the engine resolves `factorvar ==/!= "literal"` against the level
// dictionary and folds a literal that is not a level to a CONSTANT (frame.cpp
// eqne). A blank read from .dta or CSV is code -1 with no "" level, so `s==""`
// was 0 on every row and `s!=""` 1 — the rows Stata calls blank were never
// seen. Every whole-operand `NAME ==/!= ""` (either operand order) naming a
// string variable is therefore answered here: a 0/1 scratch column, 1 where
// the row is blank (code -1, or the level ""), and the comparison respelled
// `(T==1)` / `(T==0)`. Any other `""` left next to a string variable
// (`(s=="")==1` spelled without the parentheses, ...) is refused by name
// rather than handed to the engine's constant fold.
std::optional<ojson> rewriteBlankStringCompares(std::string& cond, Datastore& ds,
                                                 std::vector<ByKeepTempCol>& temps) {
  auto isFactor = [&](const std::string& nm) { return ds.has(nm) && ds.dtype(nm) != "f64"; };
  std::map<std::string, std::string> tempFor;  // variable -> its blank column
  auto blankCol = [&](const std::string& var) -> std::string {
    auto it = tempFor.find(var);
    if (it != tempFor.end()) return it->second;
    std::string name = "__piet_bkblank" + std::to_string(temps.size()) + "__";
    const cecon::Column& c = ds.col(var);
    const int32_t* codes = c.codes();
    std::vector<char> blankLevel(c.levels.size(), 0);
    for (size_t l = 0; l < c.levels.size(); ++l) blankLevel[l] = c.levels[l].empty() ? 1 : 0;
    ByKeepTempCol t;
    t.name = name;
    t.values.resize(static_cast<size_t>(ds.nRows()));
    for (int64_t r = 0; r < ds.nRows(); ++r) {
      const int32_t code = codes[r];
      t.values[static_cast<size_t>(r)] =
          (code < 0 || blankLevel[static_cast<size_t>(code)]) ? 1.0 : 0.0;
    }
    temps.push_back(std::move(t));
    tempFor.emplace(var, name);
    return name;
  };
  const std::string out = mapStringLiteralCompares(
      cond, isFactor, [&](const ByKeepLitCompare& lc) -> std::optional<std::string> {
        if (!lc.lit.empty()) return std::nullopt;
        return "(" + blankCol(lc.var) + (lc.eq ? "==1)" : "==0)");
      });
  // What is left: a blank literal the respelling did not take. Next to a
  // string variable, the engine would fold it to a constant — refuse.
  bool leftover = false;
  for (size_t j = 0; j < out.size() && !leftover;) {
    if (out[j] == '"') {
      const size_t q = out.find('"', j + 1);
      if (q == j + 1) leftover = true;
      j = q == std::string::npos ? out.size() : q + 1;
    } else {
      ++j;
    }
  }
  if (leftover) {
    bool namesString = false;
    for (size_t j = 0; j < out.size() && !namesString;) {
      if (std::isalpha(static_cast<unsigned char>(out[j])) || out[j] == '_') {
        size_t e = j;
        while (e < out.size() && byKeepIdentChar(out[e])) ++e;
        namesString = (j == 0 || out[j - 1] != '.') && isFactor(out.substr(j, e - j));
        j = e;
      } else if (out[j] == '"') {
        const size_t q = out.find('"', j + 1);
        j = q == std::string::npos ? out.size() : q + 1;
      } else {
        ++j;
      }
    }
    if (namesString) {
      return ojson(eval::errorBlock(
          "unsupported_command",
          "a comparison with the blank string \"\" under `by:` is supported only as a whole "
          "operand, `VAR == \"\"` or `VAR != \"\"` (optionally in parentheses, joined by & or |); "
          "rewrite the condition in that form.",
          std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
    }
  }
  cond = out;
  return std::nullopt;
}

// Unique scratch names inside one second (mintScratchPath is second-grained on
// POSIX, and one call here mints up to three files under the same prefix).
std::string byKeepPath(SessionState& session, const char* what) {
  static unsigned long long seq = 0;
  return nextPath(session.workdir(), std::string("bykeep") + what + std::to_string(seq++));
}

// A scratch file one of these helpers made, unlinked once the next step has
// read it (never `head`, the session's own file).
void dropScratchFile(const std::string& p, const std::string& head) {
  if (p.empty() || p == head) return;
  cecon::native_unlink(p);
  forgetScratchMint(p);
}

// BUG-532: every key in `keys` that is a string variable whose dictionary is
// not in Stata's byte order (stringKeyOrder) is replaced by a scratch f64
// column holding its byte rank, appended to `temps`; the engine's sort then
// orders the rows as Stata's does (the engine orders a factor by its
// first-seen code).
void rankStringKeys(Datastore& ds, std::vector<std::string>& keys,
                    std::vector<ByKeepTempCol>& temps) {
  for (auto& k : keys) {
    if (!ds.has(k) || ds.dtype(k) == "f64") continue;
    const cecon::Column& c = ds.col(k);
    const StringKeyOrder o = stringKeyOrder(c.levels);
    if (o.codeOrderIsStata) continue;
    ByKeepTempCol t;
    t.name = "__piet_bkrank" + std::to_string(temps.size()) + "__";
    const int32_t* codes = c.codes();
    t.values.resize(static_cast<size_t>(ds.nRows()));
    for (int64_t r = 0; r < ds.nRows(); ++r)
      t.values[static_cast<size_t>(r)] =
          codes[r] < 0 ? o.blankRank : o.rank[static_cast<size_t>(codes[r])];
    k = t.name;
    temps.push_back(std::move(t));
  }
}

// Adds each of `temps` to the file `cur`, file to file (`setcol --new`), and
// leaves `cur` naming the last file. Every intermediate file (the CSV payload,
// the previous copy) is unlinked once read, and on every error exit; `head`
// is never touched.
std::optional<ojson> addTempCols(SessionState& session, const std::string& head, std::string& cur,
                                 const std::vector<ByKeepTempCol>& temps) {
  for (const auto& t : temps) {
    // mintScratchPath registers a ".hbin" name; the setcol payload is a CSV,
    // so the name that actually exists is the one registered (BUG-389).
    std::string valuesPath = byKeepPath(session, "vals");
    forgetScratchMint(valuesPath);
    valuesPath = valuesPath.substr(0, valuesPath.size() - 5) + ".csv";  // 5 == strlen(".hbin")
    registerScratchMint(valuesPath);
    {
      std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
      bool okWrite = static_cast<bool>(vf);
      if (okWrite) {
        char buf[32];
        for (double v : t.values) {
          std::snprintf(buf, sizeof(buf), "%.17g\n", v);
          vf << buf;
        }
        vf.flush();
        okWrite = static_cast<bool>(vf);
      }
      if (!okWrite) {
        vf.close();
        cecon::native_unlink(valuesPath);
        forgetScratchMint(valuesPath);
        dropScratchFile(cur, head);
        return engineErrorBlock("could not write a scratch file under " + session.workdir());
      }
    }
    const std::string next = byKeepPath(session, "col");
    CobraRunResult cr = runCobra({"setcol", "--data", cur, "--var", t.name, "--values", valuesPath,
                                  "--dtype", "f64", "--new", "--out", next});
    cecon::native_unlink(valuesPath);
    forgetScratchMint(valuesPath);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      dropScratchFile(next, head);
      dropScratchFile(cur, head);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    dropScratchFile(cur, head);
    cur = next;
  }
  return std::nullopt;
}

std::string joinCommaKeys(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

}  // namespace

ojson runByKeepIf(SessionState& session, const std::string& ifExpr,
                  const std::vector<std::string>& byVars,
                  const std::vector<std::string>& stataSortKeys, bool drop) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-532: `by g [(s)]: keep if EXPR` / `drop if EXPR`. EXPR is evaluated
  // per by-group (group-local _n/_N, x[k], sum()) into a scratch mask, the rows
  // are filtered on it and the mask is dropped again — ONE engine call, because
  // cmd_eval applies --bygen/--keepif/--drop in argv order over a single frame
  // load (the same fused shape applyIfAndInFilter's `in` chain uses).
  //
  // Row order: the rows are in by++sort key order when the mask is computed —
  // either the caller checked that, or they are sorted in place here first on
  // `stataSortKeys` (the `bysort` half). --bygen is then given by() ONLY: its
  // stable sort on the by-keys keeps every group's rows in their physical
  // order, so _n is the row's position inside its group in the CURRENT order,
  // as in Stata — also when a key in the parentheses is a string, whose
  // engine code order is not Stata's.
  //
  // Missing is TRUE in Stata's `if`. The engine's keep_if keeps a missing mask,
  // which is right for keep; for drop the complement is written `mask==0`, which
  // is false on a missing mask (missing compares above every number), so the
  // row is dropped. EXPR's own `!`, `&`, `|` are left as written: the engine
  // reads a missing OPERAND of those as true, as Stata does (BUG-620), the
  // same reading every other engine-evaluated `if` in PIET gets.
  static const char* kMask = "__piet_bykeep__";
  std::string cond = ifExpr;
  auto joinComma = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) s += ",";
      s += v[i];
    }
    return s;
  };

  // Scratch columns the engine calls need: a blank indicator per string
  // variable compared with "", and a byte-order rank per string sort key whose
  // dictionary is not in Stata's order.
  std::vector<ByKeepTempCol> temps;
  std::vector<std::string> gsortKeys = stataSortKeys;
  // A string variable, a string literal or a string function in EXPR: the
  // same predicate the global keep/drop use to leave the engine for the
  // row-wise string path (read off the varlist card, no file open).
  const bool stringCond = strmutate::needsStringCount(session, cond);
  bool stringKeyToCheck = false;
  // The Datastore is opened only when a string condition or key is involved.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (info.dtype == "f64") continue;
      for (const auto& k : byVars) stringKeyToCheck |= info.name == k;
      for (const auto& k : stataSortKeys) stringKeyToCheck |= info.name == k;
    }
  } else {
    stringKeyToCheck = true;
  }
  if (stringCond || stringKeyToCheck) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    if (stringCond) {
      if (cond.find("\"\"") != std::string::npos) {
        if (auto err = rewriteBlankStringCompares(cond, *ds, temps)) return *err;
      }
      // Under `by:` the engine evaluates EXPR, and of the string forms it gets
      // only a whole-operand `VAR ==/!= "lit"` right (a blank one was just
      // respelled). Anything else — inlist(s,...), strpos(s,...),
      // substr(s,1,1)=="a", s==s[_n-1], a string variable read as a number —
      // would die in the engine with a generic error (rc 1) or be answered
      // wrong, so it is refused by name, before any row moves.
      auto isFactor = [&](const std::string& nm) { return ds->has(nm) && ds->dtype(nm) != "f64"; };
      const std::string residual = mapStringLiteralCompares(
          cond, isFactor,
          [](const ByKeepLitCompare&) { return std::optional<std::string>("1"); });
      if (strmutate::needsStringCount(session, residual)) {
        return ojson(eval::errorBlock(
            "unsupported_command",
            "under `by:`, a condition on a string variable is supported only as a whole "
            "operand `VAR == \"text\"` or `VAR != \"text\"` (optionally in parentheses, "
            "joined by & or |); string functions (inlist, strpos, substr, ...) and other "
            "string forms in `by ...: keep|drop if` are not supported yet. Compute the "
            "condition into a numeric variable first, e.g. `generate byte t = " + ifExpr +
                "`, then `by ...: keep if t`.",
            std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
      }
    }
    // A string BY-key whose dictionary holds "" as a level AND has blank (-1)
    // rows: Stata sees one group, the engine's by() two. Refused by name.
    for (const auto& k : byVars) {
      if (!ds->has(k) || ds->dtype(k) == "f64") continue;
      const cecon::Column& c = ds->col(k);
      const StringKeyOrder o = stringKeyOrder(c.levels);
      if (!o.blankLevel) continue;
      const int32_t* codes = c.codes();
      for (int64_t r = 0; r < ds->nRows(); ++r) {
        if (codes[r] < 0) {
          return ojson(eval::errorBlock(
              "unsupported_command",
              "`by " + k + ":` is not supported yet when `" + k +
                  "` holds blank values stored two ways (as missing and as an empty "
                  "level): Stata sees one group, this engine would see two. Group on a "
                  "numeric key instead, e.g. `egen id = group(" + k + "), missing`.",
              std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
        }
      }
    }
    rankStringKeys(*ds, gsortKeys, temps);
    for (const auto& t : temps) {
      if (ds->has(t.name)) {
        return engineErrorBlock("a variable named " + t.name +
                                " exists; it is a name PIET reserves for scratch columns");
      }
    }
  }

  const std::string bygenArg =
      std::string(kMask) + "=(" + cond + "), by(" + joinComma(byVars) + ")";
  const std::string pred = drop ? (std::string(kMask) + "==0") : std::string(kMask);

  if (temps.empty()) {
    if (!stataSortKeys.empty()) {
      ojson sorted = runSort(stataSortKeys, session);
      if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
    }
    std::string outPath = byKeepPath(session, "");
    // BUG-369: a TS operator in EXPR needs the tsset metadata on the call; a
    // runiform()/runiformint() draw needs the session's seed and stream (the
    // runBysortGen pattern), or `set seed` is ignored and the stream never
    // advances.
    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", *session.currentHbin(), "--bygen", bygenArg, "--keepif", pred,
         "--drop", kMask, "--out", outPath},
        cond, session);
    args = translate::injectRngContextForExpr(args, cond, session);
    // Single terminal mutation whose --out becomes the head and is never read
    // back as a file — the runKeepIf/runDropIf opt-in, for the same reason.
    SessionState::ResidentEligible resident(session);
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    return mutationResult(session, "keepif");
  }

  // With scratch columns: file to file (the head was materialized by
  // session.datastore() above), and only the last file becomes the head, so
  // no scratch column can reach the user's frame even when a step fails.
  // Every intermediate file is this call's own and is unlinked as soon as
  // the next step has read it, and on every error exit; the head is never
  // touched.
  const std::string head = *session.currentHbin();
  std::string cur = head;
  auto dropIntermediate = [&](const std::string& p) { dropScratchFile(p, head); };
  auto fail = [&](const std::string& partial, ojson err) {
    dropIntermediate(cur);
    if (!partial.empty()) dropIntermediate(partial);
    return err;
  };
  if (std::optional<ojson> err = addTempCols(session, head, cur, temps)) return *err;
  if (!stataSortKeys.empty()) {
    // With blank-compare columns only, gsortKeys are the Stata keys, and the
    // marker this leaves is exactly them, as Stata's `bysort` sets it
    // (w2e_probes.log bo_sort_prefix_noop: a coarser sort downgrades "g x" to
    // "g"); adding or dropping the scratch columns leaves it standing.
    const std::string next = byKeepPath(session, "sort");
    CobraRunResult cr =
        runCobra({"gsort", "--data", cur, "--vars", joinComma(gsortKeys), "--out", next});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return fail(next, engineErrorBlock(cobraErrorStr(ce), ce.code));
    }
    dropIntermediate(cur);
    cur = next;
  }
  std::vector<std::string> base = {"eval", "--data", cur, "--bygen", bygenArg, "--keepif", pred,
                                   "--drop", kMask};
  for (const auto& t : temps) {
    base.push_back("--drop");
    base.push_back(t.name);
  }
  const std::string outPath = byKeepPath(session, "");
  base.push_back("--out");
  base.push_back(outPath);
  std::vector<std::string> args = translate::injectTsContextForExpr(base, cond, session);
  args = translate::injectRngContextForExpr(args, cond, session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return fail(outPath, engineErrorBlock(cobraErrorStr(ce), ce.code));
  }
  session.load(outPath);
  dropIntermediate(cur);
  return mutationResult(session, "keepif");
}

// ---------------------------------------------------------------------------
// sort

std::optional<ojson> sortInStataOrder(SessionState& session, const std::vector<std::string>& keys) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (keys.empty()) return std::nullopt;
  // A string key is spotted off the varlist card when it is current; the
  // Datastore is opened only when one is (or when the card cannot tell).
  bool stringKey = false;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (info.dtype == "f64") continue;
      for (const auto& k : keys) stringKey |= info.name == k;
    }
  } else {
    stringKey = true;
  }
  std::vector<ByKeepTempCol> temps;
  std::vector<std::string> gsortKeys = keys;
  if (stringKey) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& k : keys)
      if (!ds->has(k)) return engineErrorBlock("variable " + k + " not found");
    rankStringKeys(*ds, gsortKeys, temps);
    for (const auto& t : temps) {
      if (ds->has(t.name)) {
        return engineErrorBlock("a variable named " + t.name +
                                " exists; it is a name PIET reserves for scratch columns");
      }
    }
  }
  if (temps.empty()) {
    ojson sorted = runSort(keys, session);
    if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
    return std::nullopt;
  }
  // Rank columns in, gsort on them, rank columns out: three steps, file to
  // file, and only the last file becomes the head. Dropping the rank columns
  // clears the claim gsort minted on them (a claim naming a dropped variable
  // clears, frame.cpp), so the head carries none, as for every string key.
  const std::string head = *session.currentHbin();
  std::string cur = head;
  if (std::optional<ojson> err = addTempCols(session, head, cur, temps)) return *err;
  const std::string sortedPath = byKeepPath(session, "sort");
  CobraRunResult cr = runCobra(
      {"gsort", "--data", cur, "--vars", joinCommaKeys(gsortKeys), "--out", sortedPath});
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    dropScratchFile(sortedPath, head);
    dropScratchFile(cur, head);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  dropScratchFile(cur, head);
  std::vector<std::string> args = {"eval", "--data", sortedPath};
  for (const auto& t : temps) {
    args.push_back("--drop");
    args.push_back(t.name);
  }
  const std::string outPath = nextPath(session.workdir(), "sort");
  args.push_back("--out");
  args.push_back(outPath);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    dropScratchFile(outPath, head);
    dropScratchFile(sortedPath, head);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  dropScratchFile(sortedPath, head);
  (void)mutationResult(session, "sort");
  return std::nullopt;
}

ojson runSort(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/sort-validate-lazy: this varlist check only ever needs COLUMN NAMES,
  // never column DATA — so answer it from the resident shadow card
  // (session.shadowVarlistIfCurrent(), a plain O(columns) name scan over
  // already-cached metadata) instead of unconditionally forcing
  // session.datastore() open the current hbin from disk. That eager re-open
  // (Datastore::Datastore -> cecon::Dataset::open, a full mmap + header
  // parse of however many MB the file is) purely to answer "does column X
  // exist" measured ~700ms-1700ms per sort on a 164 MB corpus in phase-split
  // instrumentation of this function — see the fix's own report for the
  // table. shadowVarlistIfCurrent() returns nullptr for every case where the
  // card cannot be trusted (absent, a `use`/`clear`/restore swapped datasets
  // without reseeding it, a post-estimation command resynced the resident
  // worker onto a DIFFERENT file, or a respawned/rolled-back worker) — the
  // fallback below is the exact pre-existing behavior, unchanged, so a
  // stale/foreign card never turns into a wrong "not found" (or wrongly
  // "found") answer; it only ever costs the same as before.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      bool found = false;
      for (const auto& info : *card) {
        if (info.name == v) {
          found = true;
          break;
        }
      }
      if (!found) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  // BUG-532: Stata's `sort K` on data whose marker already begins with K moves
  // no row and sets the marker to K (w2e_probes.log bo_sort_prefix_noop: "g x"
  // -> `sort g` -> "g"; bo_sort_prefix_then_by). That is the engine's gsort as
  // is (Frame::gsort skips the radix pass there), so this handler neither
  // reads the marker nor opens the head's Datastore: on the resident plane
  // that read materialized the whole dataset on every sort and every bysort
  // line (review of e19291ad).
  // BS-001 fix: this used to shell out to `eval --data ... --sort <vars>`,
  // but cobra's `eval` verb (cmds_data.cpp's cmd_eval) has no `--sort` flag
  // at all — its option-parsing loop only recognizes a fixed set of flags
  // (--xtset/--gen/--egen/.../--keepif/--summarize) and silently ignores any
  // OTHER flag/value pair without error (no catch-all reject branch), so
  // `--sort <vars>` was a pure no-op: cobra ran, wrote the SAME row order
  // back out, and PIET reported success. Real row reordering lives in
  // cobra's dedicated `gsort` verb (cmds_data.cpp's cmd_gsort / Frame::gsort,
  // std::stable_sort — see runGsort in handlers_data_verbs.cpp, the already-
  // correct sibling this now mirrors). Stata's plain `sort` is that same
  // stable multi-key sort with every key ascending (no [+-] prefix), so the
  // varlist is forwarded unprefixed. `gsort` writes its output file directly
  // and prints a plain-text status line (JSON only on error), so this must
  // use runCobra (not runCobraJson, which requires a JSON stdout payload).
  std::string outPath = nextPath(session.workdir(), "sort");
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): a pure row-reorder — single terminal mutation,
  // --out never read back as a file. Opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "sort");
}

// BS-092 (BETA campaign, Phase G): a NON-MUTATING sibling of runSort/
// runGsort above — sorts `basePath` by `byVars` (ascending, stable, same
// `gsort` verb and shape as runSort) into a FRESH scratch .hbin instead of
// calling session.load() on the result. dispatcher.cpp's `by:`/`bysort:
// summarize` grouped-dispatch loop needs the PHYSICALLY contiguous
// same-group row blocks a stable sort produces (so each group can be sliced
// out via piet::handlers::applyIfAndInFilter's plain row-range mechanism),
// but summarize is a READ-ONLY command — it must never reorder the live
// session dataset a user has loaded, unlike a real `sort`/`bysort`'s own
// (documented) side effect on gen/replace's target. Throws CobraError on
// failure (same convention as applyIfAndInFilter), not an ojson error
// block, so callers can wrap it in their own try/catch around the whole
// per-group loop instead of checking a sentinel after every call.
std::string sortForGrouping(SessionState& session, const std::string& basePath,
                            const std::vector<std::string>& byVars) {
  std::string joined;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) joined += ",";
    joined += byVars[i];
  }
  std::string outPath = nextPath(session.workdir(), "bygroup_sort");
  std::vector<std::string> args = {"gsort", "--data", basePath, "--vars", joined, "--out", outPath};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) throw errorFromRaw(cr);
  return outPath;
}

// ---------------------------------------------------------------------------
// count [if COND]

namespace {

// Q2 (gate ②③): native fast path for `count if [in]`.
//
// runCount used to route every if/in through applyIfAndInFilter -> cobra
// keepif, which spawns a subprocess and writes a COMPLETE filtered .hbin to
// disk just to read back its row count (~0.66s on a 108K×192 corpus vs a
// ~µs floor). This evaluates the predicate directly over the ALREADY-mmap'd
// Datastore columns instead.
//
// CORRECTNESS-FIRST (gate ①, silent divergence forbidden): parity is with
// TODAY's cobra keepif pipeline (src/data/frame.cpp Parser+ev()+keep_if),
// which is Stata-verified. piet-native does NOT link that engine (subprocess
// only), so ev() is PORTED here 1:1 for a deliberately small whitelist, and
// ANYTHING the port can't prove identical throws CountFastBail -> runCount
// falls back to the UNCHANGED cobra path. Falling back is always safe (cobra
// is the ground truth and re-produces today's result OR today's error), so the
// only obligation is that an ACCEPTED expression computes bit-identically.
//
// Accepted grammar (mirrors frame.cpp exactly): numeric literals (same strict
// scan — a token std::stod won't fully consume, or a hex 0x.., bails), the
// bare `.` missing literal, f64 variable references, `factorvar ==/!=
// "literal"` (resolved to a level-code compare, missing code -> +inf, exactly
// like frame.cpp eqne), unary +/-/!, ( ), + - * / ^ (all wrapped in fin():
// non-finite -> missing), the six comparisons (both sides through mi():
// missing = +infinity), & | (a missing operand is TRUE, as Stata reads it),
// and missing()/inrange()/inlist(). Everything else BAILS: _n / _N
// / _pi / subscripts var[..] (the first two even ERROR in cobra's non-bysort
// keep_if — bailing reproduces that error), any other function, a factor used
// as a number or any string/number mix, and any parse error / trailing input.
//
// Missing-value semantics are the crux and are taken verbatim from frame.cpp:
// mi() maps NaN->+inf for comparisons, fin() maps non-finite->NaN for
// arithmetic, and `!`, `&`, `|` read an operand through truthy(): nonzero is
// true and a missing value is nonzero, so a missing operand is TRUE ([U]
// 13.2.4; w2c_functions.log operators_missing), and the result is always 0/1.
// Until BUG-620 both layers read a missing `&`/`|` operand as false and `!`
// passed it through as missing; they changed together, so they never disagree.
// See runCount for the keep-truthiness rule, one for if-only and if+in alike
// (BUG-534), derived from frame.cpp keep_if + the fallback's
// `((if)!=0)&(inrange)`, not guessed.
struct CountFastBail {};

constexpr double kCiNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCiInf = std::numeric_limits<double>::infinity();
inline double ciMi(double v) { return std::isnan(v) ? kCiInf : v; }        // frame.cpp mi()
inline double ciFin(double v) { return std::isfinite(v) ? v : kCiNaN; }    // frame.cpp fin()
inline bool ciTruthy(double v) { return std::isnan(v) || v != 0.0; }       // frame.cpp truthy()
// frame.cpp scmp() — Stata's total order with the extended missings `.a`-`.z`
// ordered above `.` (data/missing.h). This fast path SHADOWS the engine for
// every `count if`, so it has to carry the same semantics or PIET answers a
// different question than cobra does for the identical expression.
inline int ciCmp(double a, double b) { return cecon::stata_cmp(a, b); }

struct CiNode {
  enum K {
    Num, VarF64, FactorCmp, FactorRef, StrLit,
    Neg, Not, Add, Sub, Mul, Div, Pow,
    Lt, Gt, Le, Ge, Eq, Ne, And, Or,
    FnMissing, FnInrange, FnInlist
  } kind = Num;
  double num = 0.0;
  const double* col = nullptr;                        // VarF64
  const int32_t* codes = nullptr;                     // FactorRef / FactorCmp
  const std::vector<std::string>* levels = nullptr;   // FactorRef (dictionary)
  long factorIdx = 0;                                 // FactorCmp: level code
  bool eq = true;                                     // FactorCmp: == vs !=
  std::string sval;                                   // StrLit
  std::unique_ptr<CiNode> a, b;
  std::vector<std::unique_ptr<CiNode>> args;          // Fn args
};

// 1:1 port of frame.cpp ev(). Only reaches node kinds the parser actually
// produces; FactorRef/StrLit are guarded out at parse time (they bail).
double ciEval(const CiNode* n, int64_t row) {
  switch (n->kind) {
    case CiNode::Num: return n->num;
    case CiNode::VarF64: return n->col[row];
    case CiNode::FactorCmp: {
      double c = n->codes[row] < 0 ? kCiInf : static_cast<double>(n->codes[row]);
      double t = static_cast<double>(n->factorIdx);
      return n->eq ? (c == t ? 1.0 : 0.0) : (c != t ? 1.0 : 0.0);
    }
    case CiNode::Neg: return -ciEval(n->a.get(), row);
    case CiNode::Not: return ciTruthy(ciEval(n->a.get(), row)) ? 0.0 : 1.0;
    case CiNode::Add: return ciFin(ciEval(n->a.get(), row) + ciEval(n->b.get(), row));
    case CiNode::Sub: return ciFin(ciEval(n->a.get(), row) - ciEval(n->b.get(), row));
    case CiNode::Mul: return ciFin(ciEval(n->a.get(), row) * ciEval(n->b.get(), row));
    case CiNode::Div: return ciFin(ciEval(n->a.get(), row) / ciEval(n->b.get(), row));
    case CiNode::Pow: return ciFin(std::pow(ciEval(n->a.get(), row), ciEval(n->b.get(), row)));
    case CiNode::Lt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <  0 ? 1.0 : 0.0;
    case CiNode::Gt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >  0 ? 1.0 : 0.0;
    case CiNode::Le: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <= 0 ? 1.0 : 0.0;
    case CiNode::Ge: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >= 0 ? 1.0 : 0.0;
    case CiNode::Eq: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) == 0 ? 1.0 : 0.0;
    case CiNode::Ne: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) != 0 ? 1.0 : 0.0;
    case CiNode::And: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) && ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::Or: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) || ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::FnMissing:
      for (auto& q : n->args) if (std::isnan(ciEval(q.get(), row))) return 1.0;
      return 0.0;
    case CiNode::FnInrange: {
      double z = ciEval(n->args[0].get(), row), lo = ciEval(n->args[1].get(), row),
             hi = ciEval(n->args[2].get(), row);
      if (std::isnan(z)) return 0.0;
      bool lm = std::isnan(lo), hm = std::isnan(hi);
      if (lm && hm) return 1.0;
      if (lm) return z <= hi ? 1.0 : 0.0;
      if (hm) return z >= lo ? 1.0 : 0.0;
      return (lo <= z && z <= hi) ? 1.0 : 0.0;
    }
    case CiNode::FnInlist: {
      double z = ciEval(n->args[0].get(), row);
      for (size_t k = 1; k < n->args.size(); ++k) {
        double v = ciEval(n->args[k].get(), row);
        if ((std::isnan(z) && std::isnan(v)) || z == v) return 1.0;
      }
      return 0.0;
    }
    case CiNode::FactorRef:
    case CiNode::StrLit:
      throw CountFastBail{};  // never legal outside a factor ==/!= "literal"
  }
  return kCiNaN;
}

// Recursive-descent parser mirroring frame.cpp's grammar
// (or > and > cmp > add > mul > pow(right) > unary > primary). Every path that
// frame.cpp's Parser would err() on, or that this port can't prove identical,
// throws CountFastBail so the caller can fall back to cobra.
struct CiParser {
  const std::string& s;
  size_t p = 0;
  Datastore& ds;
  int depth = 0;
  CiParser(const std::string& src, Datastore& d) : s(src), ds(d) {}

  void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
  bool eat(const char* tok) {
    ws();
    size_t n = 0;
    while (tok[n]) ++n;
    if (s.compare(p, n, tok) == 0) { p += n; return true; }
    return false;
  }
  static std::unique_ptr<CiNode> mk(CiNode::K k) { auto n = std::make_unique<CiNode>(); n->kind = k; return n; }
  static bool isOperand(const CiNode& n) { return n.kind != CiNode::FactorRef && n.kind != CiNode::StrLit; }
  std::unique_ptr<CiNode> bin(CiNode::K k, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    // A factor/string operand is legal ONLY as a direct child of a factor
    // ==/!= "literal" comparison (handled in cmp/eqne). Anywhere else, cobra
    // ev() throws "string/factor used where a number is required" -> bail.
    if (!isOperand(*a) || !isOperand(*b)) throw CountFastBail{};
    auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
  }
  struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { if (++d > 1000) throw CountFastBail{}; } ~DepthGuard() { --d; } };

  std::unique_ptr<CiNode> parse() {
    auto n = orx();
    ws();
    if (p != s.size()) throw CountFastBail{};  // trailing input
    return n;
  }
  std::unique_ptr<CiNode> orx() { auto a = andx(); while (eat("|")) a = bin(CiNode::Or, std::move(a), andx()); return a; }
  std::unique_ptr<CiNode> andx() { auto a = cmp(); while (eat("&")) a = bin(CiNode::And, std::move(a), cmp()); return a; }
  std::unique_ptr<CiNode> cmp() {
    auto a = add();
    for (;;) {
      ws();
      if (eat("<=")) a = bin(CiNode::Le, std::move(a), add());
      else if (eat(">=")) a = bin(CiNode::Ge, std::move(a), add());
      else if (eat("==")) a = eqne(true, std::move(a), add());
      else if (eat("!=")) a = eqne(false, std::move(a), add());
      else if (eat("<")) a = bin(CiNode::Lt, std::move(a), add());
      else if (eat(">")) a = bin(CiNode::Gt, std::move(a), add());
      else break;
    }
    return a;
  }
  // frame.cpp eqne: resolve `factorvar ==/!= "literal"` (either order) to a
  // numeric compare against the 0-based level code; a literal that is not a
  // level makes == constant-false / != constant-true.
  std::unique_ptr<CiNode> eqne(bool isEq, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    CiNode* fac = nullptr;
    CiNode* lit = nullptr;
    if (a->kind == CiNode::FactorRef && b->kind == CiNode::StrLit) { fac = a.get(); lit = b.get(); }
    else if (b->kind == CiNode::FactorRef && a->kind == CiNode::StrLit) { fac = b.get(); lit = a.get(); }
    if (fac) {
      long idx = -1;
      for (size_t i = 0; i < fac->levels->size(); ++i)
        if ((*fac->levels)[i] == lit->sval) { idx = static_cast<long>(i); break; }
      if (idx < 0) { auto cst = mk(CiNode::Num); cst->num = isEq ? 0.0 : 1.0; return cst; }
      auto n = mk(CiNode::FactorCmp); n->codes = fac->codes; n->factorIdx = idx; n->eq = isEq;
      return n;
    }
    // lone factor/string, or a string-vs-number mix -> cobra errors -> bail.
    if (a->kind == CiNode::FactorRef || b->kind == CiNode::FactorRef ||
        a->kind == CiNode::StrLit || b->kind == CiNode::StrLit) throw CountFastBail{};
    return bin(isEq ? CiNode::Eq : CiNode::Ne, std::move(a), std::move(b));
  }
  std::unique_ptr<CiNode> add() {
    auto a = mul();
    for (;;) { ws(); if (eat("+")) a = bin(CiNode::Add, std::move(a), mul());
      else if (eat("-")) a = bin(CiNode::Sub, std::move(a), mul()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> mul() {
    auto a = powx();
    for (;;) { ws(); if (eat("*")) a = bin(CiNode::Mul, std::move(a), powx());
      else if (eat("/")) a = bin(CiNode::Div, std::move(a), powx()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> powx() {
    auto a = unary();
    ws();
    if (eat("^")) return bin(CiNode::Pow, std::move(a), powx());
    return a;
  }
  std::unique_ptr<CiNode> unary() {
    DepthGuard g(depth);
    ws();
    if (p < s.size() && s[p] == '!' && (p + 1 >= s.size() || s[p + 1] != '=')) {
      ++p; auto n = mk(CiNode::Not); n->a = unary();
      if (!isOperand(*n->a)) throw CountFastBail{};
      return n;
    }
    if (eat("-")) { auto n = mk(CiNode::Neg); n->a = unary(); if (!isOperand(*n->a)) throw CountFastBail{}; return n; }
    if (eat("+")) return unary();
    return primary();
  }
  std::vector<std::unique_ptr<CiNode>> arglist() {  // '(' already consumed
    std::vector<std::unique_ptr<CiNode>> v;
    ws();
    if (p < s.size() && s[p] == ')') { ++p; return v; }
    v.push_back(orx());
    while (eat(",")) v.push_back(orx());
    if (!eat(")")) throw CountFastBail{};
    return v;
  }
  std::unique_ptr<CiNode> primary() {
    ws();
    if (eat("(")) { auto n = orx(); if (!eat(")")) throw CountFastBail{}; return n; }
    if (p < s.size() && s[p] == '"') {
      ++p; size_t st = p;
      while (p < s.size() && s[p] != '"') ++p;
      if (p >= s.size()) throw CountFastBail{};  // unterminated
      auto n = mk(CiNode::StrLit); n->sval = s.substr(st, p - st); ++p; return n;
    }
    // extended missing literal `.a` … `.z` (frame.cpp) — one lowercase letter
    // after the dot that does not continue into an identifier.
    if (p + 1 < s.size() && s[p] == '.' && s[p + 1] >= 'a' && s[p + 1] <= 'z' &&
        (p + 2 >= s.size() ||
         (!std::isalnum(static_cast<unsigned char>(s[p + 2])) && s[p + 2] != '_' &&
          s[p + 2] != '.'))) {
      int kind = cecon::kMissA + (s[p + 1] - 'a');
      p += 2;
      auto n = mk(CiNode::Num);
      n->num = cecon::stata_missing(kind);
      return n;
    }
    // bare missing literal: a '.' not part of a number (frame.cpp).
    if (p < s.size() && s[p] == '.' && (p + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p + 1])))) {
      ++p; auto n = mk(CiNode::Num); n->num = kCiNaN; return n;
    }
    if (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) {
      size_t st = p;
      while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '.' ||
                              ((s[p] == '+' || s[p] == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E'))))
        ++p;
      std::string tok = s.substr(st, p - st);
      for (char c : tok) if (c == 'x' || c == 'X') throw CountFastBail{};  // hex: Stata rejects
      size_t consumed = 0;
      double val;
      try { val = std::stod(tok, &consumed); } catch (...) { throw CountFastBail{}; }
      if (consumed != tok.size()) throw CountFastBail{};  // partial consume -> reject (frame.cpp C1-F11)
      auto n = mk(CiNode::Num); n->num = val; return n;
    }
    if (p < s.size() && (std::isalpha(static_cast<unsigned char>(s[p])) || s[p] == '_')) {
      size_t st = p;
      while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
      std::string id = s.substr(st, p - st);
      // _pi / _n / _N: not fast-pathed (_n/_N even ERROR in cobra's non-bysort
      // keep_if — bailing reproduces that; _pi is just rare here).
      if (id == "_pi" || id == "_n" || id == "_N") throw CountFastBail{};
      ws();
      if (p < s.size() && s[p] == '(') {  // function call
        ++p;
        auto args = arglist();
        for (auto& q : args) if (!isOperand(*q)) throw CountFastBail{};  // no factor/string args
        int na = static_cast<int>(args.size());
        if (id == "missing" && na >= 1) { auto n = mk(CiNode::FnMissing); n->args = std::move(args); return n; }
        if (id == "inrange" && na == 3) { auto n = mk(CiNode::FnInrange); n->args = std::move(args); return n; }
        if (id == "inlist" && na >= 2) { auto n = mk(CiNode::FnInlist); n->args = std::move(args); return n; }
        throw CountFastBail{};  // any other function -> cobra handles/errors it
      }
      if (!ds.has(id)) throw CountFastBail{};  // unknown variable -> cobra errors
      const cecon::Column& c = ds.col(id);
      if (c.dtype == cecon::DType::Factor) {
        ws();
        if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
        auto fv = mk(CiNode::FactorRef); fv->codes = c.codes(); fv->levels = &c.levels;
        return fv;
      }
      auto v = mk(CiNode::VarF64); v->col = c.f64();
      ws();
      if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
      return v;
    }
    throw CountFastBail{};  // unexpected token
  }
};

}  // namespace

ojson runCount(const std::optional<std::string>& ifExpr, SessionState& session,
              const std::optional<std::string>& inRange) {
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  // C4-L2A (D1 card plane, C4-RESIDENT-READ-SCOPE-brief §1/§4): a bare
  // `count` needs only _N, which the resident card already carries
  // (residentRowsIfCurrent — same honest-currency rule as
  // shadowVarlistIfCurrent, nullopt under PIET_NO_RESIDENT_READ=1 or a
  // non-current/rows-unknown card → the unchanged datastore() path below).
  // Answering here skips the Datastore open (mmap + whole-file FNV) and, on
  // a dirty virtual head, the EMH frame snapshot that open forces. Output is
  // byte-identical to the ds->nRows() path: same r(N), same display text.
  if (!haveIf && !haveIn) {
    if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
      session.eval.rValues.set("N", static_cast<double>(*rows));
      ojson out;
      out["type"] = "display";
      out["text"] = std::to_string(*rows);
      return out;
    }
  }
  // ONESHOT-GETCOLS2: `count if` needs the columns its predicate names, not the
  // frame. session.datastore() flushes the whole resident head to disk to supply
  // them — 56 of the 98 flushes left after the mutation entrance moved. Fetch the
  // predicate's columns instead (contract_A §38); every failure falls through to
  // the ordinary path with nothing lost.
  //
  // Row COUNT is preserved by a projection, so nRows() and the `in` bounds below
  // read the same numbers either way. The no-if/no-in case never gets here — the
  // shadow card answered it above — so a fetch is only attempted when there is a
  // predicate whose columns are worth fetching.
  std::string countMiniPath;
  std::unique_ptr<Datastore> countMini;
  if (haveIf) {
    if (auto cols = referencedColumns(session, {&*ifExpr}, ""))
      countMini = fetchColumns(session, *cols, session.workdir(), &countMiniPath);
  }
  Datastore* ds = countMini ? countMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  int64_t n = 0;

  // Q2 (gate ②③) native fast path — evaluate `if`/`in` over the mmap'd columns
  // rather than spawning cobra keepif + writing a full filtered .hbin. Anything
  // the native evaluator can't prove identical to cobra keep_if throws
  // CountFastBail and we drop to the UNCHANGED applyIfAndInFilter path.
  bool fastPathDone = false;
  if (!haveIf && !haveIn) {
    n = ds->nRows();
    fastPathDone = true;
  } else {
    // Resolve `in` bounds natively (same parseInRangeBounds the fallback uses),
    // clamped to the real row set exactly like cobra's `idx>=a & idx<=b` over a
    // 1-based row index. A malformed/negative bound is the SAME user error
    // either way, surfaced identically (byte-identical InRangeFilterResult).
    int64_t lo1 = 1, hi1 = ds->nRows();
    if (haveIn) {
      InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "count");
      }
      lo1 = std::max<int64_t>(1, rr.a);
      hi1 = std::min<int64_t>(ds->nRows(), rr.b);
    }
    if (haveIf) {
      try {
        CiParser parser(*ifExpr, *ds);
        std::unique_ptr<CiNode> ast = parser.parse();
        // A bare factor/string predicate (`count if svar`) is a number-context
        // misuse cobra keep_if errors on — bail so the fallback reproduces it.
        if (ast->kind == CiNode::FactorRef || ast->kind == CiNode::StrLit) throw CountFastBail{};
        int64_t cnt = 0;
        for (int64_t i = lo1; i <= hi1; ++i) {
          double v = ciEval(ast.get(), i - 1);
          // Keep-truthiness parity with the fallback — ONE rule, if-only or
          // if+in alike: a row counts iff `(isnan(v) || v != 0)`, a missing
          // predicate being TRUE in Stata ([U] 13.2.4). If-only, that is
          // frame.cpp keep_if itself. If+in, the fallback runs
          // `((ifExpr)!=0) & (idx-range)` (BUG-534, applyIfAndInFilter);
          // before that it ran `(ifExpr) & (idx-range)`, whose engine `&`
          // then read a missing operand as false (BUG-620), and this line
          // mirrored it (`!isnan(v) && v != 0`), so `count if x in 1/5`
          // skipped the missing-x rows Stata counts.
          bool keep = ciTruthy(v);
          if (keep) ++cnt;
        }
        n = cnt;
        fastPathDone = true;
      } catch (const CountFastBail&) {
        // if-expr uses a non-fast-pathable construct -> cobra fallback below.
      }
    } else {
      // `in`-only: keep_if on the range predicate keeps exactly the in-range
      // rows — no expression to evaluate.
      n = hi1 >= lo1 ? (hi1 - lo1 + 1) : 0;
      fastPathDone = true;
    }
  }

  if (!fastPathDone) {
    // Fallback (UNCHANGED from pre-Q2). BS-090 (BETA Phase G): `in` composes
    // with `if` via AND (piet::handlers::applyIfAndInFilter).
    InRangeFilterResult fr;
    try {
      // The filter must run on the session's REAL head, never on a §38 projection:
      // the projection carries only the predicate's columns, and this fallback's
      // result is read back as a dataset. `ds->path()` is the projection whenever
      // the fetch above succeeded, so ask the session instead.
      const std::string headPath =
          countMini ? session.currentHbin().value_or(ds->path()) : ds->path();
      fr = applyIfAndInFilter(session, headPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "count");
    Datastore filtered(fr.dataPath);
    n = filtered.nRows();
  }
  // dispatcher.py's count handler: `session._r_values["N"] = float(n)` —
  // count is an r-class command (r(N) available to a following `display
  // r(N)`), and the result block is `{"type": "display", ...}`, NOT echo.
  session.eval.rValues.set("N", static_cast<double>(n));
  ojson out;
  out["type"] = "display";
  out["text"] = std::to_string(n);
  return out;
}

// ---------------------------------------------------------------------------
// list (browse.py's run_browse in read-only list mode — dispatcher.py's
// `handler == "browse"` branch; `editable` is always false for `list`)

ojson runList(const std::optional<std::vector<std::string>>& varlist,
              const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
              SessionState& session) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  int64_t browseOffset = 0;
  int64_t browseLimit = 50;
  bool haveInRange = inRange.has_value() && !inRange->empty();
  bool haveIfExpr = ifExpr.has_value() && !ifExpr->empty();
  if (haveInRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver. list's own parser clamped
    // every bound into range, so `list in -3/-1` listed row 1 and `list in
    // 1/50` on 12 rows listed all 12 with rc 0 (Stata: rows 10-12; r(198)).
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "list");
    }
    browseOffset = haveIfExpr ? 0 : rr.a - 1;
    browseLimit = rr.b - rr.a + 1;
  }

  // BUG-025: thread session.valueLabels through so value-labeled numeric
  // variables render label TEXT (mirrors browse.py's run_browse, which
  // always passes session.get_value_label_map() to Datastore.browse — list
  // and browse share the same underlying renderer in the oracle).
  auto vlMap = session.valueLabelMapForBrowse();
  // BS-005/D6: `format VAR %fmt` display-layer attribute — sparse map, see
  // formatMapForBrowse()'s own header note (empty for any session that never
  // calls `format`, so this is a no-op for every pre-existing fixture).
  auto fmtMap = session.formatMapForBrowse();

  ojson block;
  if (haveIfExpr) {
    // BUG-674: `if` and `in` compose as Stata's AND over the ORIGINAL row
    // numbers — the shared applyIfAndInFilter chain. The old path windowed the
    // `if`-filtered rows instead, so `list x if x>2 in 1/5` listed x = 3..7
    // (the first five matches) where Stata lists 3, 4, 5. An if-only call is
    // the same single --keepif this used to issue (BUG-369's ts injection
    // included).
    InRangeFilterResult fr;
    const std::string basePath = ds->path();  // a copy: the chain may touch the head
    try {
      fr = applyIfAndInFilter(session, basePath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "list");
    Datastore tmpDs(fr.dataPath);
    block = tmpDs.browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
    cecon::native_unlink(fr.dataPath);
  } else {
    block = ds->browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
  }
  block["editable"] = false;
  if (haveInRange || haveIfExpr) {
    block["windowed"] = true;
    block["n_rows_total"] = static_cast<int64_t>(block.value("rows", ojson::array()).size());
  }
  return block;
}

// ---------------------------------------------------------------------------
// scalar list / dir

ojson runScalarList(const std::vector<std::string>& names, SessionState& session) {
  // BS-038 (BETA campaign C3): real Stata's `scalar list`/`scalar dir`
  // column format — name right-justified to width 10, " = ", value
  // right-justified to width 10 (oracle: `      sca1 =         42`).
  // Values are emitted AS ALREADY STORED (session.eval.scalars' own
  // decimal-string convention, e.g. "42"/"19.5" from the arithmetic
  // evaluator) rather than re-deriving Stata's full %10.0g significant-
  // digit formatter — exact for the common case; a value longer than 10
  // chars is left un-truncated rather than silently mis-rendered (a
  // disclosed, smaller gap than Stata's own overflow/scientific-notation
  // rule, not attempted here). This handler previously existed but was
  // dead code — eval/macros.cpp's tryCaptureDefinition intercepted the
  // bare `scalar list`/`scalar dir` line earlier in the pipeline and threw
  // the result away (a bool-only capture signature has no way to carry a
  // display block) before dispatch ever reached this arm; see that file's
  // own BS-038 comment for the other half of the fix.
  auto padRight10 = [](const std::string& s) {
    return s.size() >= 10 ? s : std::string(10 - s.size(), ' ') + s;
  };
  std::vector<std::string> lines;
  const auto& items = session.eval.scalars.items();
  for (const auto& [name, val] : items) {
    if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) continue;
    lines.push_back(padRight10(name) + " = " + padRight10(val));
  }
  ojson out;
  out["type"] = "echo";
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  out["text"] = text;
  return out;
}

// ---------------------------------------------------------------------------
// ereturn list — BS-039 (BETA campaign C3): display-only dump of the e()
// values PIET actually stores after an estimation (session.lastModel() +
// its cached raw cobra --tidy JSON, session_state.h's LastModel — PIET's
// e()-equivalent). Bare `ereturn list` ONLY; `ereturn scalar`/`ereturn
// post`/eclass creation stay OUT of scope (BS-109) — dispatcher.cpp's arm
// only routes the bare "list" sub-form here, so any other `ereturn ...`
// form still falls through to the pre-existing generic unsupported_command
// reject, unchanged.
//
// Honest partial dump beats a fabricated complete one: every section below
// emits ONLY fields verified present in PIET's own regress/feols result —
// e(ll)/e(ll_0)/e(rank) and the Stata-internal plumbing macros (e(title),
// e(marginsok), e(properties), e(predict), e(model), e(estat_cmd)) are NOT
// tracked anywhere in this build and are silently omitted rather than
// invented. e(cmd) is a known, disclosed approximation: LastModel only
// tracks the INTERNAL engine command name (cobraCmd, e.g. "feols" for
// regress/reg/r/reghdfe alike) — there is no separate "what the user
// actually typed" field, so e(cmd) can read "feols" where real Stata would
// say "regress" (a pre-existing LastModel gap, not something this
// display-only fix should widen its diff to also close).
ojson runEreturnList(SessionState& session) {
  const std::optional<LastModel>& lm = session.lastModel();
  if (!lm.has_value()) {
    // Real Stata: `ereturn list` with nothing estimated yet errors ("last
    // estimates not found", r(301)) rather than printing an empty/
    // misleading dump.
    return engineErrorBlock("last estimates not found", "engine_error");
  }
  const ojson& r = lm->result.has_value() ? *lm->result : ojson::object();

  auto padLabel = [](const std::string& s) {
    return s.size() >= 22 ? s : std::string(22 - s.size(), ' ') + s;
  };
  auto fmtScalar = [](double v) {
    std::ostringstream oss;
    oss.precision(16);
    oss << v;
    return oss.str();
  };

  std::ostringstream out;
  out << "\nscalars:\n";
  static const std::vector<std::pair<std::string, std::string>> kScalarMap = {
      {"n_obs", "N"}, {"df_m", "df_m"}, {"df_r", "df_r"}, {"F", "F"},
      {"r2", "r2"},   {"rmse", "rmse"}, {"mss", "mss"},   {"rss", "rss"},
      {"adj_r2", "r2_a"},
  };
  std::unordered_set<std::string> emitted;
  for (const auto& [jsonKey, eName] : kScalarMap) {
    if (r.contains(jsonKey) && r.at(jsonKey).is_number()) {
      out << padLabel("e(" + eName + ")") << " =  " << fmtScalar(r.at(jsonKey).get<double>()) << "\n";
      emitted.insert(eName);
    }
  }
  // Anything a handler posted directly to the e-class store. Until AER bundle H
  // this function was driven ENTIRELY by the coeftable JSON plus the fixed map
  // above, so a command like pca -- which has no coeftable and whose returns are
  // e(rho)/e(trace)/e(cond)/e(f)/e(lndet) -- would have printed nothing at all.
  // Appended rather than merged so the regression commands' output is unchanged
  // byte for byte: for those, eScalars is empty and this loop adds no lines.
  for (const auto& [k, v] : session.eval.eScalars.items()) {
    if (emitted.count(k)) continue;  // the coeftable copy already spoke for it
    out << padLabel("e(" + k + ")") << " =  " << fmtScalar(v) << "\n";
  }

  out << "\nmacros:\n";
  // BUG-458 (2026-09-08): read the STORE, which setLastModel now publishes
  // from LastModel (stataCmd -> "regress" where this printed the engine's
  // "feols"; depvar; vce). One source for `e(cmd)', bare e(cmd) and this
  // listing, so they cannot drift. The LastModel fields stay the fallback for
  // a store someone cleared underneath a live model.
  const std::string eCmd = session.eval.eStrings.get("cmd", lm->cobraCmd);
  const std::string eDepvar = session.eval.eStrings.get("depvar", lm->depvar);
  const std::string eVce = session.eval.eStrings.get("vce", lm->vcov);
  out << padLabel("e(cmd)") << " : \"" << eCmd << "\"\n";
  if (!eDepvar.empty()) {
    out << padLabel("e(depvar)") << " : \"" << eDepvar << "\"\n";
  }
  out << padLabel("e(vce)") << " : \"" << eVce << "\"\n";
  // Same rule as the scalars: empty for every pre-H command, so their pins hold.
  for (const auto& [k, v] : session.eval.eStrings.items()) {
    if (k == "cmd" || k == "depvar" || k == "vce") continue;  // said above
    out << padLabel("e(" + k + ")") << " : \"" << v << "\"\n";
  }

  const bool haveCoefTable =
      r.contains("coeftable") && r.at("coeftable").is_array() && !r.at("coeftable").empty();
  if (haveCoefTable || !session.eval.eMatrices.empty()) {
    out << "\nmatrices:\n";
    if (haveCoefTable) {
      size_t k = r.at("coeftable").size();
      out << padLabel("e(b)") << " :  1 x " << k << "\n";
      out << padLabel("e(V)") << " :  " << k << " x " << k << "\n";
    }
    for (const auto& [k, m] : session.eval.eMatrices) {
      if (haveCoefTable && (k == "b" || k == "V")) continue;  // synthesised above
      const size_t nr = m.data.size();
      const size_t nc = nr > 0 ? m.data[0].size() : 0;
      out << padLabel("e(" + k + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  ojson block;
  block["type"] = "echo";
  block["text"] = out.str();
  return block;
}

// ---------------------------------------------------------------------------
// matrix — P4 sublanguage: literal define, ' / * expressions, rownames/
// colnames, list, accum. Everything downstream that READS a matrix
// (`local : colnames X`, `X[i,j]`, `colsof()`/`rowsof()`) already resolves
// against session.eval.matrices (eval/macros.cpp's substituteMatrixRefs and
// friends) — this section only has to populate that same store, never a
// second one.
namespace {

// Forward declarations for parseMatrixLiteral: the depth-aware top-level
// splitter (defined below with the B3(c) grammar) and the one scalar-cell
// evaluator (defined below, next to evalMatrixRhs).
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops);
bool evalMatrixScalarExpr(const std::string& expr, SessionState& session, double& outVal,
                          std::string& err);

// True when the bracket at `open` ('(' or '[') is closed by the LAST character
// of `t`, counting both bracket kinds on one depth (as splitTopLevel does).
bool closingBracketIsLast(const std::string& t, size_t open) {
  if (open == std::string::npos || open >= t.size()) return false;
  int depth = 0;
  for (size_t i = open; i < t.size(); ++i) {
    const char c = t[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') {
      if (--depth == 0) return i + 1 == t.size();
    }
  }
  return false;
}

// The first BARE identifier in `expr` that is a key of session.eval.matrices,
// or "" when there is none. A same-named scalar never stands in for a matrix
// (FIX-SPEC section 9, N01: the matrix wins), so a cell or term that mentions
// a stored matrix this way is not the scalar evaluator's (BUG-584). Not bare,
// and so skipped: a call head `f(`, a subscript head `M[` and everything inside
// its brackets (`M[i,j]`, `_b[x]`, scalar context), the first argument of the
// functions whose argument is itself a name -- el/colsof/rowsof/colnumb
// (substituteMatrixRefs resolves them), scalar() (the scalar by definition) and
// the class wrappers e()/r()/c()/s() -- plus string literals, numbers (`1e5`)
// and extended missing values (`.a`).
std::string firstBareMatrixName(const std::string& expr, const SessionState& session) {
  auto isIdStart = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  auto isIdChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  auto skipSpace = [&expr](size_t k) {
    while (k < expr.size() && (expr[k] == ' ' || expr[k] == '\t')) ++k;
    return k;
  };
  size_t i = 0;
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t close = expr.find('"', i + 1);
      i = close == std::string::npos ? expr.size() : close + 1;
      continue;
    }
    if (c == '[') {
      int depth = 0;
      for (; i < expr.size(); ++i) {
        if (expr[i] == '[') ++depth;
        else if (expr[i] == ']' && --depth == 0) break;
      }
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '.' && i + 1 < expr.size() &&
                                                             !isIdStart(expr[i + 1]))) {
      while (i < expr.size() && (std::isdigit(static_cast<unsigned char>(expr[i])) != 0 ||
                                 expr[i] == '.'))
        ++i;
      if (i < expr.size() && (expr[i] == 'e' || expr[i] == 'E')) {
        ++i;
        if (i < expr.size() && (expr[i] == '+' || expr[i] == '-')) ++i;
      }
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (c == '.') {  // `.a`-`.z`
      ++i;
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (!isIdStart(c)) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < expr.size() && isIdChar(expr[i])) ++i;
    const std::string id = expr.substr(start, i - start);
    const size_t next = skipSpace(i);
    if (next < expr.size() && expr[next] == '(') {
      std::string lower = id;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      static const char* const kNameArgFns[] = {"el",     "colsof", "rowsof", "colnumb", "scalar",
                                                "e",      "r",      "c",      "s"};
      bool nameArg = false;
      for (const char* f : kNameArgFns) nameArg = nameArg || lower == f;
      i = next + 1;
      if (nameArg) {
        i = skipSpace(i);
        while (i < expr.size() && isIdChar(expr[i])) ++i;
      }
      continue;
    }
    if (next < expr.size() && expr[next] == '[') continue;  // subscript head
    for (const auto& p : session.eval.matrices)
      if (p.first == id) return id;
  }
  return std::string();
}

// Splits `(1,2\3,4)`-style literals: '\' separates rows, ',' separates
// columns. nullopt on anything malformed (ragged rows, empty cells, an
// unbalanced group) or on a cell that belongs to the join grammar -- the
// caller then tries evalMatrixRhs, and only the outermost caller (runMatrixDef)
// turns a final decline into an honest refusal (Doctrine 1). The split is
// depth-aware (splitTopLevel), so a cell's own comma -- `round(1/3,.01)`,
// `el(M,2,3)` -- is never read as a column separator.
//
// Cell order, first match wins (oracle w2c_functions.log:160
// matrix_mixed_literal, w2f_residuals.log:1303 round_matrix_row):
//   1. `.` and `.a`-`.z`;
//   2. a plain signed decimal;
//   3. a constant arithmetic expression (no name, no function);
//   4. decline: a nested literal, a cell that mentions a matrix name as a bare
//      identifier (`Bcol'`, `2*Bcol`; firstBareMatrixName), a matrix subscript
//      NAME[...], a matrix-valued call I()/J()/vecdiag()/nullmat(), or an
//      e()/r() class MATRIX -- these keep their names and shapes through the
//      join grammar (or are refused there) instead of collapsing to a scalar;
//   5. any other cell is a scalar expression, evaluated by evalMatrixScalarExpr
//      (the macro/arith layer, the same as `scalar`/`local`/`display`).
std::optional<std::vector<std::vector<double>>> parseMatrixLiteral(const std::string& body,
                                                                    SessionState& session) {
  std::string s = trim(body);
  if (s.size() < 2 || s.front() != '(' || s.back() != ')') return std::nullopt;
  s = s.substr(1, s.size() - 2);
  std::vector<std::string> parts;
  std::vector<char> ops;
  if (!splitTopLevel(s, parts, ops)) return std::nullopt;  // unbalanced group

  auto namesMatrix = [&session](const std::string& n) {
    for (const auto& p : session.eval.matrices)
      if (p.first == n) return true;
    return false;
  };
  auto cellValue = [&](const std::string& t, double& out) -> bool {
    // BUG-540 (oracle arm lit_dot): a bare `.` cell is plain system missing --
    // same NaN evalJCall's `J(r,c,.)` fill already uses, so formatMatrixCell's
    // isnan() check and the existing NaN->"."->sentinel display round-trip
    // (matrixElementText, macros.cpp) render it identically to a J() cell.
    if (t == ".") {
      out = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    // BUG-540 (oracle arm lit_extmissing): `.a`-`.z` must stay DISTINCT from
    // plain `.` under `==` (E[1,2]==.a is true, E[1,2]==. is false in the
    // oracle) -- a NaN payload survives copies but isnan()-based rendering
    // collapses every NaN to the same "." text, which would lose the kind on
    // the way back through the expression layer. The eval layer's own `.a`
    // literal already resolves to the FINITE stataMissingWithKind() sentinel
    // (exprfns.h, BUG-249), so storing that same finite double here keeps
    // identity through matrixElementText's NaN-free pythonFloatRepr path.
    if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z') {
      out = eval::stataMissingWithKind(t[1] - 'a' + 1);
      return true;
    }
    try {
      size_t pos = 0;
      const double v = std::stod(t, &pos);
      if (pos == t.size()) {
        out = v;
        return true;
      }
    } catch (const std::exception&) {
    }
    // ml-getmata T4 (oracle w2c_ml_getmata.do:244, `matrix rchange = (0.1*`i'
    // + 0.05)` inside family (8)'s loop, rc 0 in Stata): a cell may be a
    // CONSTANT arithmetic expression. Only digits, `.`, `e`, + - * / ^ and
    // parentheses qualify -- no name, no function -- so nothing that could
    // mean a matrix, a scalar or a variable is reinterpreted here. The value
    // comes from the existing macro/arith layer (eval::evalArith), not a new
    // evaluator.
    static const std::regex reConstExpr(R"(^[0-9.eE+\-*/^() \t]+$)");
    if (t.find_first_of("+-*/^(") != std::string::npos && std::regex_match(t, reConstExpr)) {
      const std::optional<std::string> r = eval::evalArith(t, nullptr);
      if (!r.has_value()) return false;
      if (*r == ".") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
      }
      try {
        size_t pos = 0;
        const double v = std::stod(*r, &pos);
        if (pos != r->size()) return false;
        out = v;
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    // Decline (a): a nested literal `(1,2)` / `(S1 \ .)` -- the join grammar's.
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
      std::vector<std::string> inner;
      std::vector<char> innerOps;
      if (splitTopLevel(t.substr(1, t.size() - 2), inner, innerOps) && inner.size() > 1)
        return false;
    }
    // Decline (b): a cell that mentions a matrix NAME anywhere as a bare
    // identifier -- `Bcol`, `Bcol'`, `2*Bcol`, `-Bcol` (BUG-584). A same-named
    // scalar must not win over the matrix: the join grammar answers the matrix
    // (`2*Bcol` through its scalar-coefficient leg) or refuses at r(198).
    if (!firstBareMatrixName(t, session).empty()) return false;
    // Decline (c): a subscript of a stored matrix, `M[1,1]` / `M[1..2,1]` /
    // `M["rb",1]`; the join grammar's extraction keeps the source names.
    // Depth-aware: `M[1,1]+M[2,2]` is a scalar expression, not a subscript.
    static const std::regex reSub(R"(^([A-Za-z_]\w*)\s*\[.*\]$)");
    std::smatch nm;
    if (std::regex_match(t, nm, reSub) && closingBracketIsLast(t, t.find('[')) &&
        namesMatrix(nm.str(1)))
      return false;
    // Decline (d): a matrix-valued call.
    static const std::regex reMatFn(R"(^(I|J|vecdiag|nullmat)\s*\()");
    if (std::regex_search(t, reMatFn)) return false;
    // Decline (e): an e()/r() class MATRIX. A class SCALAR (e(N), e(r2)) is
    // not declined: evalArith resolves it below.
    static const std::regex reClass(R"(^([er])\(\s*([A-Za-z_]\w*)\s*\)$)");
    if (std::regex_match(t, nm, reClass)) {
      const char cls = nm.str(1)[0];
      const std::string which = nm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V")))
        return false;
    }
    std::string cellErr;
    return evalMatrixScalarExpr(t, session, out, cellErr);
  };

  std::vector<std::vector<double>> rows;
  std::vector<double> row;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string t = trim(parts[i]);
    if (t.empty()) return std::nullopt;
    double v = 0.0;
    if (!cellValue(t, v)) return std::nullopt;
    row.push_back(v);
    if (i + 1 < parts.size() && ops[i] == '\\') {
      rows.push_back(std::move(row));
      row.clear();
    }
  }
  rows.push_back(std::move(row));
  const size_t ncols = rows[0].size();
  for (const auto& r : rows) {
    if (r.empty() || r.size() != ncols) return std::nullopt;  // ragged
  }
  return rows;
}

// Stata's default row/col stub ("r1".."rN" / "c1".."cN") — what an
// arithmetic result (a literal, or any `*` product) gets unless the caller
// overwrites it with an explicit `matrix rownames`/`colnames` (exactly what
// the P4 fixture does immediately after `matrix P4B = P4A'*P4A`).
void applyDefaultStripe(eval::EvalMatrix& mat) {
  const size_t nrows = mat.data.size();
  const size_t ncols = nrows > 0 ? mat.data[0].size() : 0;
  mat.rownames.clear();
  for (size_t i = 1; i <= nrows; ++i) mat.rownames.push_back("r" + std::to_string(i));
  mat.colnames.clear();
  for (size_t j = 1; j <= ncols; ++j) mat.colnames.push_back("c" + std::to_string(j));
}

eval::EvalMatrix transposeMatrix(const eval::EvalMatrix& in) {
  const size_t nrows = in.data.size();
  const size_t ncols = nrows > 0 ? in.data[0].size() : 0;
  eval::EvalMatrix out;
  out.data.assign(ncols, std::vector<double>(nrows, 0.0));
  for (size_t i = 0; i < nrows; ++i) {
    for (size_t j = 0; j < ncols; ++j) out.data[j][i] = in.data[i][j];
  }
  // Transpose swaps the stripe spaces along with the data.
  out.rownames = in.colnames;
  out.colnames = in.rownames;
  return out;
}

// nullopt on nonconformable dimensions — never a partial/garbage product.
std::optional<eval::EvalMatrix> multiplyMatrices(const eval::EvalMatrix& a,
                                                   const eval::EvalMatrix& b) {
  const size_t arows = a.data.size();
  const size_t acols = arows > 0 ? a.data[0].size() : 0;
  const size_t brows = b.data.size();
  const size_t bcols = brows > 0 ? b.data[0].size() : 0;
  if (acols != brows) return std::nullopt;
  eval::EvalMatrix out;
  out.data.assign(arows, std::vector<double>(bcols, 0.0));
  for (size_t i = 0; i < arows; ++i) {
    for (size_t j = 0; j < bcols; ++j) {
      double s = 0.0;
      for (size_t k = 0; k < acols; ++k) s += a.data[i][k] * b.data[k][j];
      out.data[i][j] = s;
    }
  }
  return out;
}

// Bail type for evalMatrixExprTerms — same local-exception idiom as
// CountFastBail above, just carrying a message since this reject has to
// name the missing matrix / nonconformable shapes.
struct MatrixExprBail {
  std::string message;
};

// AER scope 2 / B3(c): J() and nullmat(), and the concatenation that joins them.
//
// The RHS language above deliberately has no parentheses -- its character class
// is [A-Za-z0-9_'*\s] -- which meant no matrix FUNCTION could ever appear on a
// `matrix X = ...` right-hand side, however it was implemented (BUG-445). The
// two the corpus actually asks for are J (24 uses, papers 015 and 049) and
// nullmat (2 uses, paper 015's `matrix b = nullmat(b), tmp` accumulator idiom).
//
// This adds the smallest grammar that covers them and nothing more:
//     rhs   := term (( ',' | '\' ) term)*        , = side by side, \ = stacked
//     term  := '(' literal ')' | NAME | J(r,c,v) | nullmat(NAME) | '(' rhs ')'
// The existing NAME'*NAME product path is untouched and still tried first, so
// nothing that parsed before parses differently now.

// Split on ',' / '\' at paren depth 0. Returns the pieces and their separators.
// BUG-542: `[' / ']' share the same depth counter as `(' / ')' -- a submatrix
// subscript like `M[1..2,1..3]` carries its OWN top-level comma (row spec vs
// col spec), which must not be mistaken for the join's own `,`/`\` separator
// when the subscript sits inside a larger expression (`(0,0,0) \ M[1..2,1..3]`).
// The two bracket kinds always nest correctly together in this grammar, so one
// counter is exact, not an approximation.
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops) {
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') { if (--depth < 0) return false; }
    else if (depth == 0 && (c == ',' || c == '\\')) {
      parts.push_back(trim(e.substr(start, i - start)));
      ops.push_back(c);
      start = i + 1;
    }
  }
  if (depth != 0) return false;
  parts.push_back(trim(e.substr(start)));
  return true;
}

// True when the whole string is one parenthesised group.
bool wrappedInParens(const std::string& e) {
  if (e.size() < 2 || e.front() != '(' || e.back() != ')') return false;
  int depth = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] == '(') ++depth;
    else if (e[i] == ')') { if (--depth == 0) return i + 1 == e.size(); }
  }
  return false;
}

// Converts eval::evalArith's text result into the double an eval::EvalMatrix
// cell stores -- the inverse of matrixElementText (macros.cpp): "." is plain
// missing, `.a`-`.z` is the extended-missing kind (BUG-249's
// stataMissingWithKind), anything else a plain decimal.
bool matrixCellDoubleFromArithResult(const std::string& s, double& out) {
  if (s == ".") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (s.size() == 2 && s[0] == '.' && s[1] >= 'a' && s[1] <= 'z') {
    out = eval::stataMissingWithKind(s[1] - 'a' + 1);
    return true;
  }
  try {
    size_t pos = 0;
    out = std::stod(s, &pos);
    return pos == s.size();
  } catch (const std::exception&) {
    return false;
  }
}

// The ONE scalar-expression evaluator of the matrix sublanguage: literal cells
// (parseMatrixLiteral), I()'s argument, a scalar coefficient, a scalar term at
// a join position (evalMatrixRhs), and element-assignment subscripts and
// right-hand sides (runMatrixElementAssign). It is the macro/arith layer
// (eval::evalArith, which substitutes M[i,j]/el()/colsof()/rowsof(), e()/_b[]
// refs and scalars itself), so a function answers in a matrix cell exactly as
// it answers in `scalar`/`local`/`display`; there is no second parser here.
//
// Every failure carries an `r(NNN): ` prefix (read back by eval::splitErrCode):
// a string result is r(109) (as main's element path), a failure the shared
// classifier claims keeps its rc (an unknown function is r(133)), a bare
// unknown identifier -- the shape the classifier leaves unclaimed -- is r(111)
// "NAME not found", and anything else unclaimed is r(198).
bool evalMatrixScalarExpr(const std::string& expr, SessionState& session, double& outVal,
                          std::string& err) {
  // A plain run of digits is its own value, read before the arith layer, which
  // rejects a leading-zero integer literal (`01`, as `local j : display %02.0f`
  // writes it, or `I(03)`): Stata reads 01 as 1 (BUG-587 subscripts, BUG-586
  // I()). The arith layer's own leading-zero rule is not changed (FU-201).
  {
    const std::string digits = trim(expr);
    if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
      try {
        outVal = std::stod(digits);
        return true;
      } catch (const std::exception&) {
        // out of double range: the arith layer answers below
      }
    }
  }
  std::string raised;
  bool isString = false;
  const std::optional<std::string> result =
      eval::evalArith(expr, &session.eval, &raised, &isString);
  if (result.has_value() && isString) {
    err = "r(109): type mismatch";
    return false;
  }
  if (!result.has_value()) {
    // A NameError on a STORED MATRIX name (`-M`, `A*A` in a scalar position) is
    // matrix arithmetic this sublanguage does not implement, not an absent
    // name (BUG-584): r(198), never r(111) "M not found".
    static const std::string kNameErr = "NameError: name '";
    std::string nameErrName;
    if (raised.rfind(kNameErr, 0) == 0) {
      const size_t end = raised.find('\'', kNameErr.size());
      if (end != std::string::npos)
        nameErrName = raised.substr(kNameErr.size(), end - kNameErr.size());
    }
    if (!nameErrName.empty()) {
      for (const auto& p : session.eval.matrices) {
        if (p.first == nameErrName) {
          err = "r(198): matrix arithmetic is not supported in this position: `" + expr + "'";
          return false;
        }
      }
    }
    eval::MacroFnError mfe;
    if (!raised.empty() && eval::classifyFailedExpr(expr, raised, &mfe) && mfe.failed()) {
      err = "r(" + mfe.rc + "): " + mfe.message;
      return false;
    }
    if (!nameErrName.empty()) {
      err = "r(111): " + nameErrName + " not found";
      return false;
    }
    err = "r(198): invalid syntax in `" + expr + "'";
    return false;
  }
  if (!matrixCellDoubleFromArithResult(*result, outVal)) {
    err = "r(109): type mismatch";
    return false;
  }
  return true;
}

// The error block for an evalMatrixScalarExpr failure: the `r(NNN): ` prefix
// becomes the exact rc through engineErrorBlockRc (the errors.h builder,
// BUG-187). A message without a prefix is r(198); this never yields rc 1.
ojson matrixExprErrorBlock(const std::string& prefix, const std::string& err) {
  std::string msg = err;
  const std::string code = eval::splitErrCode(&msg);  // "r(NNN)"; r(111) when absent
  int rc = 198;
  if (err.rfind("r(", 0) == 0 && code.size() > 3) {
    try {
      rc = std::stoi(code.substr(2, code.size() - 3));
    } catch (const std::exception&) {
      rc = 198;
    }
  }
  return engineErrorBlockRc(prefix + msg, rc);
}

// The one resolver of a class-matrix reference `e(NAME)`/`r(NAME)`: a POSTED
// class matrix (session.eval.findClassMatrix) wins; otherwise e(b)/e(V) are
// synthesised from the last estimation's coeftable (e(V) is the diagonal of
// se^2, FU-98). Used by runMatrixDef's `matrix X = e(NAME)` form, by vecdiag()'s
// argument, and by an e()/r() term at a join position. nullopt with `err` set
// (the same texts runMatrixDef has always reported) when the name is absent.
std::optional<eval::EvalMatrix> resolveClassMatrixRef(char cls, const std::string& which,
                                                       SessionState& session, std::string& err) {
  // Prefer a posted class matrix: `estat event, post` writes eMatrices
  // "b"/"V" with Stata's Pre_avg/Post_avg/Tm#/Tp# colnames + full covariance,
  // which is the Stata-faithful e(b)/e(V) analysis.do parses. pca's Ev/L/C/Psi
  // and sktest's r(table) arrive by the same route. Fall back to synthesising
  // from the last estimation's coeftable (plain reghdfe etc.).
  if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, which)) return *posted;

  // The coeftable fallback synthesises b and V and nothing else, so any other
  // name -- and every r() name -- is simply absent rather than derivable.
  if (cls != 'e' || (which != "b" && which != "V")) {
    err = std::string("matrix ") + cls + "(" + which + ") not found";
    return std::nullopt;
  }
  if (!session.lastEstBlock.has_value()) {
    err = "matrix: no estimation results are in memory for e(" + which + ")";
    return std::nullopt;
  }
  const ojson ct = session.lastEstBlock->value("coeftable", ojson::array());
  std::vector<std::string> names;
  std::vector<double> bs, ses;
  for (const auto& row : ct) {
    names.push_back(row.value("name", std::string()));
    auto num = [&row](const char* key) -> double {
      return (row.contains(key) && !row.at(key).is_null())
                 ? row.at(key).get<double>()
                 : std::numeric_limits<double>::quiet_NaN();
    };
    bs.push_back(num("b"));
    ses.push_back(num("se"));
  }
  eval::EvalMatrix mat;
  mat.colnames = names;
  if (which == "b") {
    mat.rownames = {"y1"};
    mat.data.push_back(bs);  // 1 x k row vector
  } else {                   // e(V): k x k diagonal of se^2
    mat.rownames = names;
    const size_t k = names.size();
    mat.data.assign(k, std::vector<double>(k, 0.0));
    for (size_t i = 0; i < k; ++i) mat.data[i][i] = ses[i] * ses[i];
  }
  return mat;
}

std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs,
                                              SessionState& session, std::string& err);

// The largest row/column dimension any Stata edition allows: c(max_matdim) is
// 65,534 for Stata/MP, 11,000 for SE and 800 for BE ([P] creturn); beyond it
// Stata answers r(915) "unable to allocate matrix" ([P] error). I() and J()
// check against it BEFORE casting or allocating, so a huge argument is a
// refusal and never an out-of-range cast or a whole-run allocation failure.
constexpr double kMaxMatDim = 65534;

// `J(r, c, v)`: r x c filled with v. Stata refuses a zero dimension (r(198)),
// which is why an empty result is an error here rather than a 0x0 matrix.
std::optional<eval::EvalMatrix> evalJCall(const std::string& inner, std::string& err) {
  std::vector<std::string> a;
  std::vector<char> ops;
  if (!splitTopLevel(inner, a, ops) || a.size() != 3) {
    err = "J() takes exactly three arguments";
    return std::nullopt;
  }
  for (char o : ops) if (o != ',') { err = "J() arguments are comma separated"; return std::nullopt; }
  double dim[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    try {
      size_t used = 0;
      dim[k] = std::stod(a[static_cast<size_t>(k)], &used);
      if (used != a[static_cast<size_t>(k)].size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
      err = "J() dimension `" + a[static_cast<size_t>(k)] + "' is not a number";
      return std::nullopt;
    }
    if (dim[k] < 1 || dim[k] != std::floor(dim[k])) {
      err = "J() dimensions must be positive integers";
      return std::nullopt;
    }
    if (dim[k] > kMaxMatDim) {
      err = "r(915): unable to allocate matrix";
      return std::nullopt;
    }
  }
  double fill = std::numeric_limits<double>::quiet_NaN();  // `.` is the common case
  if (a[2] != ".") {
    try {
      size_t used = 0;
      fill = std::stod(a[2], &used);
      if (used != a[2].size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
      err = "J() fill value `" + a[2] + "' is not a number";
      return std::nullopt;
    }
  }
  eval::EvalMatrix m;
  m.data.assign(static_cast<size_t>(dim[0]),
                std::vector<double>(static_cast<size_t>(dim[1]), fill));
  applyDefaultStripe(m);
  return m;
}

// Horizontal (`,`) and vertical (`\`) concatenation, with Stata's conformability
// rules. A 0x0 operand -- which is what nullmat() yields for a matrix that does
// not exist yet -- is the identity, and that is the whole point of the idiom:
// `matrix b = nullmat(b), tmp` builds a matrix up one column-block at a time
// without a special case for the first pass.
std::optional<eval::EvalMatrix> concatMatrices(const eval::EvalMatrix& a,
                                               const eval::EvalMatrix& b, char op,
                                               std::string& err) {
  if (a.data.empty()) return b;
  if (b.data.empty()) return a;
  eval::EvalMatrix out;
  if (op == ',') {
    if (a.data.size() != b.data.size()) {
      err = "matrices are not conformable for horizontal concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    for (size_t i = 0; i < out.data.size(); ++i)
      out.data[i].insert(out.data[i].end(), b.data[i].begin(), b.data[i].end());
    out.rownames = a.rownames;
    out.colnames = a.colnames;
    out.colnames.insert(out.colnames.end(), b.colnames.begin(), b.colnames.end());
  } else {
    if (a.data[0].size() != b.data[0].size()) {
      err = "matrices are not conformable for vertical concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    out.data.insert(out.data.end(), b.data.begin(), b.data.end());
    out.colnames = a.colnames;
    out.rownames = a.rownames;
    out.rownames.insert(out.rownames.end(), b.rownames.begin(), b.rownames.end());
  }
  if (out.rownames.size() != out.data.size() ||
      out.colnames.size() != (out.data.empty() ? 0 : out.data[0].size()))
    applyDefaultStripe(out);
  return out;
}

// BUG-542 (submatrix extraction): one axis spec inside `M[rowspec,colspec]` --
// a single 1-based index, an inclusive `lo..hi` range, an open `lo...` range
// (to the last row/col), or a quoted name / quoted `"lo".."hi"` name range.
// Returns the resolved 1-based [lo,hi] (inclusive, lo<=hi) against `names`
// (rownames or colnames) and `dimSize` (nrows or ncols), or nullopt with `err`
// set to the literal text "conformability error" when the spec is well-formed
// but the index doesn't exist -- runMatrixDef matches that exact string to
// report Stata's own r(503) (oracle arm sub_out_of_range), keeping a
// genuinely malformed spec (a different message, no forced rc) distinct from
// a real bounds violation.
std::optional<std::pair<size_t, size_t>> resolveSubscriptAxis(
    const std::string& spec, const std::vector<std::string>& names, size_t dimSize,
    std::string& err) {
  const std::string s = trim(spec);
  if (s.empty()) { err = "empty matrix subscript"; return std::nullopt; }

  if (s.front() == '"') {
    // Quoted name, or a quoted `"lo".."hi"` name range (the `..` sits outside
    // both quoted strings, so a plain substring search is exact -- Stata row/
    // col names never contain `"`).
    // Custom raw-string delimiter: the pattern contains `)"`, which would
    // close a plain R"( ... )" early (same trap noted at macros.cpp's reColnumb).
    static const std::regex reOne(R"RX(^"([^"]*)"$)RX");
    static const std::regex reRange(R"RX(^"([^"]*)"\s*\.\.\s*"([^"]*)"$)RX");
    std::smatch m;
    std::string loName, hiName;
    if (std::regex_match(s, m, reRange)) {
      loName = m.str(1);
      hiName = m.str(2);
    } else if (std::regex_match(s, m, reOne)) {
      loName = hiName = m.str(1);
    } else {
      err = "malformed matrix subscript `" + s + "'";
      return std::nullopt;
    }
    size_t lo = 0, hi = 0;
    bool foundLo = false, foundHi = false;
    for (size_t i = 0; i < names.size(); ++i) {
      if (!foundLo && names[i] == loName) { lo = i + 1; foundLo = true; }
      if (!foundHi && names[i] == hiName) { hi = i + 1; foundHi = true; }
    }
    if (!foundLo || !foundHi) {
      err = "matrix subscript: name `" + (!foundLo ? loName : hiName) + "' not found";
      return std::nullopt;
    }
    if (lo > hi) { err = "conformability error"; return std::nullopt; }
    return std::make_pair(lo, hi);
  }

  static const std::regex reOpen(R"(^(\d+)\.\.\.$)");    // "2..."  -> lo..dimSize
  static const std::regex reRange(R"(^(\d+)\.\.(\d+)$)");  // "1..2"
  static const std::regex reSingle(R"(^(\d+)$)");          // "2"
  std::smatch m;
  size_t lo = 0, hi = 0;
  // A digit string too long for size_t (`M[99999999999999999999999,1]`) is
  // out of range, not a crash: std::stoull's out_of_range used to escape the
  // handler and end the whole do-file even under `capture`.
  try {
    if (std::regex_match(s, m, reOpen)) {
      lo = static_cast<size_t>(std::stoull(m.str(1)));
      hi = dimSize;
    } else if (std::regex_match(s, m, reRange)) {
      lo = static_cast<size_t>(std::stoull(m.str(1)));
      hi = static_cast<size_t>(std::stoull(m.str(2)));
    } else if (std::regex_match(s, m, reSingle)) {
      lo = hi = static_cast<size_t>(std::stoull(m.str(1)));
    } else {
      err = "malformed matrix subscript `" + s + "'";
      return std::nullopt;
    }
  } catch (const std::exception&) {
    err = "conformability error";
    return std::nullopt;
  }
  if (lo < 1 || lo > hi || hi > dimSize) {
    err = "conformability error";
    return std::nullopt;
  }
  return std::make_pair(lo, hi);
}

// `M[rowspec,colspec]` -- the row/col specs are split on the FIRST comma that
// is not inside a quoted name (a name range's own `..` never contains one).
std::optional<eval::EvalMatrix> extractSubmatrix(const eval::EvalMatrix& src,
                                                  const std::string& subscriptBody,
                                                  std::string& err) {
  bool inQuote = false;
  size_t splitAt = std::string::npos;
  for (size_t i = 0; i < subscriptBody.size(); ++i) {
    const char c = subscriptBody[i];
    if (c == '"') inQuote = !inQuote;
    else if (c == ',' && !inQuote) { splitAt = i; break; }
  }
  if (splitAt == std::string::npos) {
    err = "malformed matrix subscript `" + subscriptBody + "'";
    return std::nullopt;
  }
  const std::string rowSpec = subscriptBody.substr(0, splitAt);
  const std::string colSpec = subscriptBody.substr(splitAt + 1);

  const size_t nrows = src.data.size();
  const size_t ncols = nrows > 0 ? src.data[0].size() : 0;
  auto rowRange = resolveSubscriptAxis(rowSpec, src.rownames, nrows, err);
  if (!rowRange.has_value()) return std::nullopt;
  auto colRange = resolveSubscriptAxis(colSpec, src.colnames, ncols, err);
  if (!colRange.has_value()) return std::nullopt;

  eval::EvalMatrix out;
  out.data.reserve(rowRange->second - rowRange->first + 1);
  for (size_t i = rowRange->first; i <= rowRange->second; ++i) {
    std::vector<double> row;
    row.reserve(colRange->second - colRange->first + 1);
    for (size_t j = colRange->first; j <= colRange->second; ++j) row.push_back(src.data[i - 1][j - 1]);
    out.data.push_back(std::move(row));
    out.rownames.push_back(i - 1 < src.rownames.size() ? src.rownames[i - 1] : "r" + std::to_string(i));
  }
  for (size_t j = colRange->first; j <= colRange->second; ++j)
    out.colnames.push_back(j - 1 < src.colnames.size() ? src.colnames[j - 1] : "c" + std::to_string(j));
  return out;
}

// Validates + splits a `'`/`*`-only expression on top-level '*' — this
// sublanguage has no parens/precedence, so a plain split is exact. nullopt
// if any character falls outside [A-Za-z0-9_'* \t] or a term isn't
// NAME followed by zero-or-more trailing '.
std::optional<std::vector<std::string>> splitMatrixExprTerms(const std::string& expr) {
  static const std::regex reValidChars(R"(^[A-Za-z0-9_'*\s]+$)");
  if (!std::regex_match(expr, reValidChars)) return std::nullopt;
  std::vector<std::string> terms;
  std::stringstream ss(expr);
  std::string term;
  while (std::getline(ss, term, '*')) terms.push_back(trim(term));
  static const std::regex reTerm(R"(^[A-Za-z_]\w*'*$)");
  for (const auto& t : terms) {
    if (t.empty() || !std::regex_match(t, reTerm)) return std::nullopt;
  }
  return terms;
}

// Evaluates a validated term list against session.eval.matrices — the only
// operators are postfix ' (transpose) and left-to-right *. Throws
// MatrixExprBail on an unknown matrix name or a nonconformable product; the
// caller turns that into an engineErrorBlock.
eval::EvalMatrix evalMatrixExprTerms(const std::vector<std::string>& terms,
                                      SessionState& session) {
  eval::EvalMatrix acc;
  bool first = true;
  bool multiplied = false;
  for (const auto& term : terms) {
    size_t nameEnd = term.size();
    size_t apCount = 0;
    while (nameEnd > 0 && term[nameEnd - 1] == '\'') { ++apCount; --nameEnd; }
    const std::string name = term.substr(0, nameEnd);
    const eval::EvalMatrix* src = nullptr;
    for (const auto& p : session.eval.matrices) {
      if (p.first == name) { src = &p.second; break; }
    }
    if (src == nullptr) throw MatrixExprBail{name + " not found"};
    eval::EvalMatrix cur = *src;
    if (apCount % 2 == 1) cur = transposeMatrix(cur);
    if (first) {
      acc = std::move(cur);
      first = false;
      continue;
    }
    std::optional<eval::EvalMatrix> prod = multiplyMatrices(acc, cur);
    if (!prod.has_value()) {
      const size_t aC = acc.data.empty() ? 0 : acc.data[0].size();
      const size_t cC = cur.data.empty() ? 0 : cur.data[0].size();
      throw MatrixExprBail{"nonconformable for multiplication (" +
                            std::to_string(acc.data.size()) + "x" + std::to_string(aC) +
                            " * " + std::to_string(cur.data.size()) + "x" +
                            std::to_string(cC) + ")"};
    }
    acc = std::move(*prod);
    multiplied = true;
  }
  if (multiplied) applyDefaultStripe(acc);  // arithmetic result -> fresh r#/c# stub
  return acc;
}

// The B3(c) right-hand side: terms joined by top-level ',' and '\'. Anything
// this cannot express falls back to the older NAME'*NAME path at the call site,
// so the set of expressions that worked before is unchanged.
std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs, SessionState& session,
                                              std::string& err) {
  const std::string e = trim(rhs);
  if (e.empty()) { err = "empty matrix expression"; return std::nullopt; }

  std::vector<std::string> parts;
  std::vector<char> ops;
  if (!splitTopLevel(e, parts, ops)) { err = "unbalanced parentheses"; return std::nullopt; }

  if (parts.size() > 1) {
    std::optional<eval::EvalMatrix> acc = evalMatrixRhs(parts[0], session, err);
    if (!acc) return std::nullopt;
    for (size_t i = 1; i < parts.size(); ++i) {
      std::optional<eval::EvalMatrix> nxt = evalMatrixRhs(parts[i], session, err);
      if (!nxt) return std::nullopt;
      acc = concatMatrices(*acc, *nxt, ops[i - 1], err);
      if (!acc) return std::nullopt;
    }
    return acc;
  }

  // A scalar coefficient times a matrix-valued term, `2*I(2)` (oracle
  // w2c_functions.log:84 I_scaled) or `sc*I(2)`. This function is reached only
  // from a right-hand side containing '(', '\', ',' or '[' (runMatrixDef), so
  // the NAME'*NAME product grammar (evalMatrixExprTerms) is a separate path,
  // and a bare `2*M`/`M*2` stays refused there. Both operands must be single
  // terms -- the coefficient a number, a scalar name, a call or a group (not a
  // matrix name), the right side one matrix-valued term -- so that `0.1*3+0.05`
  // at a join position is still evaluated whole as a scalar with the usual
  // precedence, never split at its `*`.
  {
    int depth = 0;
    size_t star = std::string::npos;
    for (size_t i = 0; i < e.size(); ++i) {
      const char c = e[i];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (c == '*' && depth == 0) { star = i; break; }
    }
    if (star != std::string::npos && star > 0) {
      const std::string lhs = trim(e.substr(0, star));
      const std::string rest = trim(e.substr(star + 1));
      auto isMatrixName = [&session](const std::string& t) {
        size_t nameEnd = t.size();
        while (nameEnd > 0 && t[nameEnd - 1] == '\'') --nameEnd;
        const std::string n = t.substr(0, nameEnd);
        for (const auto& p : session.eval.matrices)
          if (p.first == n) return true;
        return false;
      };
      static const std::regex reIdent(R"(^[A-Za-z_]\w*$)");
      static const std::regex reCallHead(R"(^[A-Za-z_]\w*\s*[\(\[])");
      static const std::regex reNumber(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
      std::string coeffText = lhs;
      if (!coeffText.empty() && (coeffText[0] == '-' || coeffText[0] == '+'))
        coeffText = trim(coeffText.substr(1));
      const bool lhsSingle =
          std::regex_match(lhs, reNumber) || std::regex_match(coeffText, reIdent) ||
          (std::regex_search(coeffText, reCallHead) &&
           closingBracketIsLast(coeffText, coeffText.find_first_of("(["))) ||
          (!coeffText.empty() && coeffText[0] == '(' && closingBracketIsLast(coeffText, 0));
      static const std::regex reMatFnHead(R"(^(I|J|vecdiag|nullmat)\s*\()");
      static const std::regex reSubHead(R"(^[A-Za-z_]\w*\s*\[)");
      const bool restSingleMatrix =
          !rest.empty() &&
          (isMatrixName(rest) ||
           (std::regex_search(rest, reMatFnHead) && closingBracketIsLast(rest, rest.find('('))) ||
           (std::regex_search(rest, reSubHead) && closingBracketIsLast(rest, rest.find('[')) &&
            isMatrixName(trim(rest.substr(0, rest.find('['))))) ||
           (rest[0] == '(' && closingBracketIsLast(rest, 0)));
      double coeff = 0.0;
      std::string coeffErr;
      // The coefficient mentions no matrix name (BUG-584): `(Bcol+1)*I(2)` with
      // Bcol both a scalar and a matrix is not the scalar's coefficient.
      if (lhsSingle && restSingleMatrix && firstBareMatrixName(lhs, session).empty() &&
          evalMatrixScalarExpr(lhs, session, coeff, coeffErr)) {
        std::optional<eval::EvalMatrix> mat = evalMatrixRhs(rest, session, err);
        if (!mat.has_value()) return std::nullopt;
        for (auto& row : mat->data)
          for (double& v : row) v *= coeff;
        return mat;
      }
    }
  }

  // J(...) and nullmat(...) before the paren strip, so `J(3,2,.)` is not read as
  // a name followed by a group.
  auto callArg = [&e](const char* fn, std::string& inner) {
    const size_t n = std::strlen(fn);
    if (e.size() < n + 2 || e.compare(0, n, fn) != 0) return false;
    size_t i = n;
    while (i < e.size() && (e[i] == ' ' || e[i] == '\t')) ++i;
    if (i >= e.size() || e[i] != '(' || e.back() != ')') return false;
    inner = trim(e.substr(i + 1, e.size() - i - 2));
    return true;
  };
  std::string inner;
  if (callArg("J", inner)) return evalJCall(inner, err);
  if (callArg("nullmat", inner)) {
    for (const auto& p : session.eval.matrices)
      if (p.first == inner) return p.second;
    return eval::EvalMatrix{};  // absent: the 0x0 identity for concatenation
  }
  // I(n): the n x n identity (oracle w2c_functions.log:75 I_n3), default
  // stripe. The argument is a scalar expression (`I(`K')`, `I(sc+1)`); it must
  // be a finite integer >= 1, otherwise r(198) (design decision, rc unpinned).
  if (callArg("I", inner)) {
    double nd = 0.0;
    std::string argErr;
    if (!evalMatrixScalarExpr(inner, session, nd, argErr)) {
      err = argErr;
      return std::nullopt;
    }
    if (!std::isfinite(nd) || nd >= eval::kStataMissingSentinel || nd < 1 ||
        nd != std::floor(nd)) {
      err = "r(198): I() argument `" + inner + "' must be a positive integer";
      return std::nullopt;
    }
    if (nd > kMaxMatDim) {
      err = "r(915): unable to allocate matrix";
      return std::nullopt;
    }
    const size_t n = static_cast<size_t>(nd);
    eval::EvalMatrix m;
    m.data.assign(n, std::vector<double>(n, 0.0));
    for (size_t k = 0; k < n; ++k) m.data[k][k] = 1.0;
    applyDefaultStripe(m);
    return m;
  }
  // vecdiag(M): the 1 x n row of a SQUARE M's diagonal (oracle
  // w2c_functions.log:100 vecdiag_basic: row name r1, M's column names). M is
  // a stored matrix or an e()/r() class matrix (`vecdiag(e(V))`). An unknown M
  // is r(111); a non-square M is r(505), never a truncated diagonal (design
  // decisions, rcs unpinned). vecdiag is a matrix-sublanguage function only:
  // `local x = el(vecdiag(M),1,3)` stays r(133) (w2c_functions.log:110).
  if (callArg("vecdiag", inner)) {
    const eval::EvalMatrix* src = nullptr;
    for (const auto& p : session.eval.matrices)
      if (p.first == inner) { src = &p.second; break; }
    std::optional<eval::EvalMatrix> classMat;
    if (src == nullptr) {
      static const std::regex reClassRef(R"(^([er])\(\s*([A-Za-z_]\w*)\s*\)$)");
      std::smatch cm;
      std::string classErr;
      if (std::regex_match(inner, cm, reClassRef)) {
        classMat = resolveClassMatrixRef(cm.str(1)[0], cm.str(2), session, classErr);
        if (classMat.has_value()) src = &*classMat;
      }
    }
    if (src == nullptr) {
      err = "r(111): matrix " + inner + " not found";
      return std::nullopt;
    }
    const size_t nrows = src->data.size();
    const size_t ncols = nrows > 0 ? src->data[0].size() : 0;
    if (nrows != ncols) {
      err = "vecdiag(): matrix not square";
      return std::nullopt;
    }
    eval::EvalMatrix m;
    m.data.assign(1, std::vector<double>(nrows, 0.0));
    for (size_t k = 0; k < nrows; ++k) m.data[0][k] = src->data[k][k];
    m.rownames = {"r1"};
    if (src->colnames.size() == ncols) {
      m.colnames = src->colnames;
    } else {
      for (size_t k = 1; k <= ncols; ++k) m.colnames.push_back("c" + std::to_string(k));
    }
    return m;
  }

  // BUG-542: submatrix extraction, `NAME[rowspec,colspec]` — tried before the
  // wrappedInParens/name-lookup legs below since it is neither.
  if (!e.empty() && e.back() == ']') {
    const size_t br = e.find('[');
    static const std::regex reIdent(R"(^[A-Za-z_]\w*$)");
    if (br != std::string::npos && br > 0 && std::regex_match(e.substr(0, br), reIdent) &&
        closingBracketIsLast(e, br)) {
      const std::string mname = e.substr(0, br);
      const std::string sub = e.substr(br + 1, e.size() - br - 2);
      const eval::EvalMatrix* src = nullptr;
      for (const auto& p : session.eval.matrices)
        if (p.first == mname) { src = &p.second; break; }
      if (src == nullptr) { err = "matrix `" + mname + "' not found"; return std::nullopt; }
      return extractSubmatrix(*src, sub, err);
    }
  }

  if (wrappedInParens(e)) {
    const std::string in = trim(e.substr(1, e.size() - 2));
    // Every wrapped group tries the literal first; its decline rules keep each
    // matrix-valued cell (a name, a subscript, I()/J()/vecdiag()/nullmat(), a
    // nested literal, an e()/r() matrix) on the recursive path below.
    std::optional<std::vector<std::vector<double>>> rows = parseMatrixLiteral(e, session);
    if (rows.has_value()) {
      eval::EvalMatrix m;
      m.data = std::move(*rows);
      applyDefaultStripe(m);
      return m;
    }
    // BUG-541: not a literal after all (a NAME, `A'`, or a name mixed with `.`
    // sits inside, e.g. `(S1 \ .)`) — recurse into the B3(c) grammar instead
    // of failing outright; only the OUTERMOST caller (runMatrixDef) reports
    // the failure, and only once this recursion has also declined.
    return evalMatrixRhs(in, session, err);
  }

  // BUG-541 (oracle arm mixed_name_dot / matdef_nested_literals): a bare `.`
  // or a plain number term is a 1x1 matrix — needed for joins like `S1 \ .`
  // and `CEN \ (8, 8, 8)` where a literal sits next to a name at the SAME
  // concatenation level.
  if (e == ".") {
    eval::EvalMatrix m;
    m.data = {{std::numeric_limits<double>::quiet_NaN()}};
    applyDefaultStripe(m);
    return m;
  }
  {
    size_t pos = 0;
    bool ok = false;
    double v = 0.0;
    try {
      v = std::stod(e, &pos);
      ok = (pos == e.size());
    } catch (const std::exception&) {
      ok = false;
    }
    if (ok) {
      eval::EvalMatrix m;
      m.data = {{v}};
      applyDefaultStripe(m);
      return m;
    }
  }

  // Plain matrix name, or NAME followed by one or more postfix transpose `'`
  // (odd count transposes, even count cancels out) — the same rule
  // evalMatrixExprTerms uses for the older NAME'*NAME grammar, extended here
  // so a transpose can sit inside a join (oracle arm mixed_name_dot's `A'`).
  {
    size_t nameEnd = e.size();
    size_t apCount = 0;
    while (nameEnd > 0 && e[nameEnd - 1] == '\'') { ++apCount; --nameEnd; }
    const std::string name = e.substr(0, nameEnd);
    for (const auto& p : session.eval.matrices) {
      if (p.first == name) {
        eval::EvalMatrix cur = p.second;
        if (apCount % 2 == 1) cur = transposeMatrix(cur);
        return cur;
      }
    }
  }
  // An e()/r() class MATRIX at a join position, `(e(b) \ e(b))` -- the cell
  // parseMatrixLiteral declines (rule (e)) -- through the one resolver.
  {
    static const std::regex reClassRef(R"(^([er])\(\s*([A-Za-z_]\w*)\s*\)$)");
    std::smatch cm;
    if (std::regex_match(e, cm, reClassRef)) {
      const char cls = cm.str(1)[0];
      const std::string which = cm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V"))) {
        std::string classErr;
        std::optional<eval::EvalMatrix> resolved =
            resolveClassMatrixRef(cls, which, session, classErr);
        if (resolved.has_value()) return resolved;
        // The refusal carries its rc through matrixExprErrorBlock (BUG-584),
        // never runMatrixDef's code-less block (rc 1): no estimation results
        // is r(301), Stata's "last estimates not found"; an absent name is
        // r(111). Both rcs are UNPINNED design decisions (no oracle arm).
        if (cls == 'e' && !session.lastEstBlock.has_value()) {
          err = "r(301): last estimates not found";
        } else {
          err = "r(111): " + classErr;
        }
        return std::nullopt;
      }
    }
  }
  // Last resort: a scalar expression at a join position, e.g. a function cell
  // next to a matrix name, `(A, round(2.5))`. Every matrix reading of `e` has
  // declined above; a term that still mentions a matrix name (`-Bcol`, `A*A`,
  // `M+1`) is matrix arithmetic this grammar does not implement, refused at
  // r(198) and never evaluated with a same-named scalar (BUG-584). A failure
  // carries its classified `r(NNN): ` rc to runMatrixDef.
  if (!firstBareMatrixName(e, session).empty()) {
    err = "r(198): matrix arithmetic is not supported in this position: `" + e + "'";
    return std::nullopt;
  }
  {
    double v = 0.0;
    std::string cellErr;
    if (evalMatrixScalarExpr(e, session, v, cellErr)) {
      eval::EvalMatrix m;
      m.data = {{v}};
      applyDefaultStripe(m);
      return m;
    }
    if (cellErr.rfind("r(", 0) == 0) {
      err = cellErr;
      return std::nullopt;
    }
  }
  err = "matrix `" + e + "' not found";
  return std::nullopt;
}

void storeMatrixResult(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { p.second = std::move(mat); return; }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// `matrix list`'s cell text: integer-valued cells render bare ("10", not
// "10.0000"); everything else keeps 4 decimals. Pinned against StataBE 19.5
// (tests/kitchensink/pangram/waves/_oracle/p4_stata.log, `matrix list P4B`).
std::string formatMatrixCell(double v) {
  if (std::isnan(v)) return ".";
  const double r = std::round(v);
  if (std::fabs(v - r) < 1e-8 && std::fabs(r) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(r));
    return std::string(buf);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  return std::string(buf);
}

// Both matrix forms this sublanguage can produce (A'*A, accum) are
// structurally X'X products, which are exactly symmetric in IEEE754 (same
// summation order, commutative per-term multiply) — a plain cell-equality
// scan is therefore an honest, exact test, not a heuristic.
bool isSymmetricMatrix(const eval::EvalMatrix& m) {
  const size_t n = m.data.size();
  if (n == 0 || m.data[0].size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (m.data[i].size() != n) return false;
    for (size_t j = i + 1; j < n; ++j) {
      if (std::fabs(m.data[i][j] - m.data[j][i]) > 1e-8) return false;
    }
  }
  return true;
}

// `matrix list NAME` rendering — column widths sized from only the cells
// actually printed (symmetric -> lower triangle incl. diagonal), 2-space
// gaps throughout. Pinned line-for-line against StataBE 19.5's:
//
//   symmetric P4B[2,2]
//       c1  c2
//   r1  10
//   r2  14  20
std::string renderMatrixList(const std::string& name, const eval::EvalMatrix& m) {
  const size_t nrows = m.data.size();
  const size_t ncols = nrows > 0 ? m.data[0].size() : 0;
  const bool sym = isSymmetricMatrix(m);

  std::vector<std::string> rlabels = m.rownames;
  rlabels.resize(nrows);
  for (size_t i = 0; i < nrows; ++i) {
    if (rlabels[i].empty()) rlabels[i] = "r" + std::to_string(i + 1);
  }
  std::vector<std::string> clabels = m.colnames;
  clabels.resize(ncols);
  for (size_t j = 0; j < ncols; ++j) {
    if (clabels[j].empty()) clabels[j] = "c" + std::to_string(j + 1);
  }

  std::vector<std::vector<std::string>> cells(nrows, std::vector<std::string>(ncols));
  std::vector<size_t> colWidth(ncols, 0);
  for (size_t j = 0; j < ncols; ++j) colWidth[j] = clabels[j].size();
  size_t rowLabelWidth = 0;
  for (const auto& s : rlabels) rowLabelWidth = std::max(rowLabelWidth, s.size());

  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;  // lower triangle incl. diagonal
    for (size_t j = 0; j < limit; ++j) {
      cells[i][j] = formatMatrixCell(m.data[i][j]);
      colWidth[j] = std::max(colWidth[j], cells[i][j].size());
    }
  }

  auto rjust = [](const std::string& s, size_t w) {
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
  };
  auto ljust = [](const std::string& s, size_t w) {
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
  };

  std::ostringstream out;
  out << "\n";
  if (sym) out << "symmetric ";
  out << name << "[" << nrows << "," << ncols << "]\n";

  out << std::string(rowLabelWidth, ' ');
  for (size_t j = 0; j < ncols; ++j) out << "  " << rjust(clabels[j], colWidth[j]);
  out << "\n";

  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;
    out << ljust(rlabels[i], rowLabelWidth);
    for (size_t j = 0; j < limit; ++j) out << "  " << rjust(cells[i][j], colWidth[j]);
    out << "\n";
  }
  return out.str();
}

// `matrix rownames NAME = n1 n2 ...` / `matrix colnames NAME = n1 n2 ...`
ojson runMatrixStripe(bool isRow, const std::string& name, const std::string& namesRhs,
                       SessionState& session) {
  std::vector<std::string> names;
  {
    std::istringstream iss(namesRhs);
    std::string tok;
    while (iss >> tok) names.push_back(tok);
  }
  eval::EvalMatrix* mat = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { mat = &p.second; break; }
  }
  if (mat == nullptr) return engineErrorBlock("matrix " + name + " not found");
  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  const size_t want = isRow ? nrows : ncols;
  if (names.size() != want) {
    return engineErrorBlock("matrix " + std::string(isRow ? "rownames" : "colnames") + ": " +
                             std::to_string(names.size()) + " names given for " +
                             std::to_string(want) + (isRow ? " row(s)" : " column(s)"));
  }
  if (isRow) mat->rownames = std::move(names); else mat->colnames = std::move(names);
  return ojson::array();
}

// `matrix accum NAME = v1 v2 ...` — A = X'X where X = [v1, v2, ..., 1] (a
// constant column appended LAST), over the casewise-complete rows (a row
// missing ANY listed variable is dropped entirely — same rule `regress`
// uses). Verified against the P4 wave fixture: `matrix accum P4C = x1 x2`
// gives casewise n = 4671 and P4C[1,1] = 3896.1577 (P4's pinned value); see
// tests/kitchensink/pangram/waves/_oracle/p4_stata.log. `noconstant` is real
// Stata's opt-out of that trailing column — not implemented here, so it (and
// every other `matrix accum` option) is an explicit reject: a silently wrong
// constant-column position would still pass a [1,1]-only pin (echoes
// BUG-155's `predict`-column lesson), so this errors rather than guesses.
ojson runMatrixAccum(const std::string& name, const std::string& rhs, SessionState& session) {
  const std::string body = trim(rhs);
  std::string varsPart = body;
  std::string optsPart;
  if (auto comma = body.find(','); comma != std::string::npos) {
    varsPart = trim(body.substr(0, comma));
    optsPart = trim(body.substr(comma + 1));
  }
  if (!optsPart.empty()) {
    return engineErrorBlock("matrix accum: option `" + optsPart +
                             "' not supported in this version of PIET (only the plain "
                             "X'X form — no noconstant/deviations/etc.)");
  }
  std::vector<std::string> vars;
  {
    std::istringstream iss(varsPart);
    std::string tok;
    while (iss >> tok) vars.push_back(tok);
  }
  if (vars.empty()) return engineErrorBlock("matrix accum: no variables specified");

  Datastore* ds = session.datastore();
  if (ds == nullptr) return engineErrorBlock("no data in memory");
  std::vector<const double*> cols;
  cols.reserve(vars.size());
  for (const auto& v : vars) {
    if (!ds->has(v)) return engineErrorBlock("matrix accum: variable " + v + " not found");
    if (ds->dtype(v) != "f64") {
      return engineErrorBlock("matrix accum: variable " + v +
                               " is a string/factor variable — matrix accum requires "
                               "numeric variables");
    }
    cols.push_back(ds->col(v).f64());
  }
  const int64_t n = ds->nRows();
  const size_t k = vars.size();
  std::vector<std::vector<double>> X;  // casewise-complete rows only, [v1..vk, 1]
  X.reserve(static_cast<size_t>(std::max<int64_t>(n, 0)));
  for (int64_t r = 0; r < n; ++r) {
    bool keep = true;
    for (size_t c = 0; c < k; ++c) {
      if (cecon::is_missing(cols[c][r])) { keep = false; break; }
    }
    if (!keep) continue;
    std::vector<double> row(k + 1);
    for (size_t c = 0; c < k; ++c) row[c] = cols[c][r];
    row[k] = 1.0;  // constant, appended last
    X.push_back(std::move(row));
  }

  const size_t dim = k + 1;
  eval::EvalMatrix mat;
  mat.data.assign(dim, std::vector<double>(dim, 0.0));
  for (const auto& row : X) {
    for (size_t i = 0; i < dim; ++i) {
      const double xi = row[i];
      if (xi == 0.0) continue;
      for (size_t j = 0; j < dim; ++j) mat.data[i][j] += xi * row[j];
    }
  }
  mat.rownames = vars;
  mat.rownames.push_back("_cons");
  mat.colnames = mat.rownames;
  storeMatrixResult(session, name, std::move(mat));
  return ojson::array();  // matrix define is silent
}

}  // namespace

namespace {

// `matrix A[r,c] = exp`: overwrite one cell of an EXISTING matrix with a scalar
// expression, or, when exp is a matrix, the block it covers starting at (r,c)
// (Stata's submatrix substitution). The target must exist (r(111)); a subscript
// or block that falls outside it is Stata's conformability error r(503)
// (oracle w2c_functions.log:151 matassign_oob); a string result is r(109).
//
// `subText` is the whole bracket body, found depth-aware by runMatrixDef, so
// `A[max(1,2),3]` and the 073 corpus's expanded `H[2*3+2+1,4]` arrive intact.
// Each subscript is a scalar expression (evalMatrixScalarExpr) and is range-
// checked as a double BEFORE any cast: NaN, a missing value, < 1 or above the
// dimension is r(503); a non-integer inside the range is r(198) (no
// truncation). Row/column NAMES as subscripts are not implemented and refuse
// through the same evaluator. Every refusal carries an exact rc, never rc 1.
ojson runMatrixElementAssign(const std::string& name, const std::string& subText,
                             const std::string& rhs, SessionState& session) {
  eval::EvalMatrix* target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const std::string where = "matrix " + name + "[" + subText + "]: ";
  std::vector<std::string> subParts;
  std::vector<char> subOps;
  if (!splitTopLevel(subText, subParts, subOps) || subParts.size() != 2 || subOps[0] != ',' ||
      subParts[0].empty() || subParts[1].empty()) {
    return engineErrorBlockRc(where + "invalid syntax", 198);
  }
  const size_t dims[2] = {target->data.size(),
                          target->data.empty() ? size_t{0} : target->data[0].size()};
  size_t idx[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    double v = 0.0;
    std::string subErr;
    if (!evalMatrixScalarExpr(subParts[static_cast<size_t>(k)], session, v, subErr))
      return matrixExprErrorBlock(where, subErr);
    if (std::isnan(v) || v >= eval::kStataMissingSentinel || v < 1.0 ||
        v > static_cast<double>(dims[k])) {
      return engineErrorBlockRc(where + "conformability error", 503);
    }
    if (v != std::floor(v)) return engineErrorBlockRc(where + "invalid syntax", 198);
    idx[k] = static_cast<size_t>(v);
  }
  const size_t r = idx[0], c = idx[1];
  eval::EvalMatrix block;
  double scalarValue = 0.0;
  std::string scalarErr;
  if (evalMatrixScalarExpr(rhs, session, scalarValue, scalarErr)) {
    block.data = {{scalarValue}};
  } else {
    // Not a scalar: a matrix-valued exp is Stata's block substitution (also
    // `MN["rb",1]`, which the macro layer cannot read). If that fails too, the
    // scalar attempt's classified rc is the answer (an unknown function is
    // r(133), a `[3]` r(198)), never rc 1.
    std::string cerr;
    std::optional<eval::EvalMatrix> built = evalMatrixRhs(rhs, session, cerr);
    if (!built.has_value()) return matrixExprErrorBlock(where, scalarErr);
    block = std::move(*built);
  }
  // Re-find: evalMatrixRhs does not add matrices today, but the pointer is into
  // a vector the evaluation could in principle grow.
  target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const size_t rows = target->data.size();
  const size_t cols = rows > 0 ? target->data[0].size() : 0;
  const size_t br = block.data.size();
  const size_t bc = br > 0 ? block.data[0].size() : 0;
  if (br == 0 || bc == 0 || r - 1 + br > rows || c - 1 + bc > cols) {
    return engineErrorBlockRc(where + "conformability error", 503);
  }
  for (size_t i = 0; i < br; ++i)
    for (size_t j = 0; j < bc; ++j) target->data[r - 1 + i][c - 1 + j] = block.data[i][j];
  return ojson::array();  // matrix define is silent
}

}  // namespace

// The full `matrix` command is a large sublanguage; analysis.do needs only a
// handful of forms out of it: e(b)/e(V) capture (the original, still
// byte-identical M3 path below — tried FIRST so its output can never shift),
// plus the P4 wave's five: a `(r\c,...)` literal, a `'`/`*` expression over
// existing matrices, rownames/colnames, list, and accum. Everything
// downstream (`local : colnames b`, `b[1,j]`, `scalar = sqrt(V[j,j])`)
// already resolves against session.eval.matrices, so each form here only has
// to populate/read that one store. Any other matrix form is an explicit
// error, never a silent no-op (Doctrine 1).
ojson runMatrixDef(const std::string& raw, SessionState& session) {
  // BUG-543: `matrix define NAME = ...` / `mat def NAME = ...` — `define`/
  // `def` is an OPTIONAL keyword before an assignment target in real Stata
  // (59 corpus lines spell it), but a matrix can be literally NAMED `def` or
  // `define` (`matrix define = ...`), so the keyword is stripped only when a
  // SECOND identifier and `=` follow it — never when `=` sits directly after
  // `def(ine)?` itself.
  static const std::regex reDefKeyword(
      R"(^(\s*mat(?:rix)?\s+)def(?:ine)?(\s+[A-Za-z_]\w*\s*=.*)$)");
  std::string work = raw;
  {
    std::smatch km;
    if (std::regex_search(raw, km, reDefKeyword)) work = km.str(1) + km.str(2);
  }
  static const std::regex reMat(
      R"(^\s*mat(?:rix)?\s+([A-Za-z_]\w*)\s*=\s*([er])\(\s*([A-Za-z_]\w*)\s*\)\s*$)");
  std::smatch m;
  if (!std::regex_search(work, m, reMat)) {
    // Not the e(b)/e(V) form — try the P4 sublanguage before giving up.
    static const std::regex reList(R"(^\s*mat(?:rix)?\s+list\s+([A-Za-z_]\w*)\s*$)");
    static const std::regex reStripe(
        R"(^\s*mat(?:rix)?\s+(rownames|colnames)\s+([A-Za-z_]\w*)\s*=\s*(.+)$)");
    static const std::regex reAccum(R"(^\s*mat(?:rix)?\s+accum\s+([A-Za-z_]\w*)\s*=\s*(.+)$)");
    static const std::regex reDefine(R"(^\s*mat(?:rix)?\s+([A-Za-z_]\w*)\s*=\s*(.+)$)");
    // ml-getmata T4: element (and block) assignment, `matrix A[r,c] = exp`.
    // Oracle w2c_ml_getmata.do:312-315 fills a J(3,3,.) this way before family
    // (9)'s Mata block, and paper 087 fills all_results cell by cell the same
    // way (gen_sensitivity_stats_1.do). The bracket body is found depth-aware
    // (its matching `]`), so a subscript or right-hand side with its own
    // brackets/commas -- `A[max(1,2),3] = 6`, `H[2,3] = H[3,2]` -- splits at
    // the right place; a bracket with no `= exp` after it is r(198).
    static const std::regex reElemHead(R"(^\s*mat(?:rix)?\s+([A-Za-z_]\w*)\s*\[)");
    std::smatch mm;

    if (std::regex_search(work, mm, reElemHead)) {
      const size_t open = static_cast<size_t>(mm.length(0)) - 1;
      int depth = 0;
      size_t close = std::string::npos;
      for (size_t i = open; i < work.size(); ++i) {
        const char c = work[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') {
          if (--depth == 0) { close = i; break; }
        }
      }
      static const std::regex reAssignTail(R"(^\s*=\s*(.+)$)");
      std::smatch tm;
      const std::string tail = close == std::string::npos ? std::string() : work.substr(close + 1);
      if (close == std::string::npos || !std::regex_match(tail, tm, reAssignTail)) {
        return engineErrorBlockRc("matrix " + mm.str(1) + ": invalid syntax", 198);
      }
      return runMatrixElementAssign(mm.str(1), work.substr(open + 1, close - open - 1),
                                    trim(tm.str(1)), session);
    }

    if (std::regex_search(work, mm, reList)) {
      const std::string name = mm.str(1);
      const eval::EvalMatrix* mat = nullptr;
      for (const auto& p : session.eval.matrices) {
        if (p.first == name) { mat = &p.second; break; }
      }
      if (mat == nullptr) return engineErrorBlock("matrix " + name + " not found");
      ojson block;
      block["type"] = "echo";
      block["text"] = renderMatrixList(name, *mat);
      return block;
    }
    if (std::regex_search(work, mm, reStripe)) {
      return runMatrixStripe(mm.str(1) == "rownames", mm.str(2), mm.str(3), session);
    }
    if (std::regex_search(work, mm, reAccum)) {
      return runMatrixAccum(mm.str(1), mm.str(2), session);
    }
    if (std::regex_search(work, mm, reDefine)) {
      const std::string name = mm.str(1);
      const std::string rhs = trim(mm.str(2));
      // BUG-541: a literal that STARTS with `(` is tried as a pure
      // `(r\c,...)` literal first, but a nullopt here is no longer a hard
      // stop — it falls through to the B3(c) grammar just below (a name, a
      // transpose, or a name mixed with a literal can sit inside the SAME
      // parens, e.g. `((.,.,.) \ CEN \ (8,8,8))` or `(A' , (S1 \ .))`, neither
      // of which is a pure numeric literal). "malformed literal" is reported
      // only once THAT has also declined.
      bool triedLiteral = false;
      if (!rhs.empty() && rhs.front() == '(' && rhs.find("nullmat") == std::string::npos &&
          rhs.find("J(") == std::string::npos) {
        std::optional<std::vector<std::vector<double>>> rows = parseMatrixLiteral(rhs, session);
        if (rows.has_value()) {
          eval::EvalMatrix mat;
          mat.data = std::move(*rows);
          applyDefaultStripe(mat);
          storeMatrixResult(session, name, std::move(mat));
          return ojson::array();
        }
        triedLiteral = true;
      }
      // B3(c): J()/nullmat()/concatenation/submatrix. Tried whenever the pure
      // literal parse declined or never applied, so every expression that
      // parsed before parses the same.
      if (rhs.find('(') != std::string::npos || rhs.find('\\') != std::string::npos ||
          rhs.find(',') != std::string::npos || rhs.find('[') != std::string::npos) {
        std::string cerr;
        std::optional<eval::EvalMatrix> built = evalMatrixRhs(rhs, session, cerr);
        if (built.has_value()) {
          storeMatrixResult(session, name, std::move(*built));
          return ojson::array();
        }
        // BUG-541/BUG-542 (oracle arms join_dot_nonconformable, sub_out_of_range
        // respectively): Stata's own r(503) for a nonconformable join or an
        // out-of-range submatrix — concatMatrices/resolveSubscriptAxis both
        // spell it this exact way, so the text match is exact, not a heuristic.
        if (cerr == "conformability error" ||
            cerr.find("not conformable") != std::string::npos) {
          return engineErrorBlockRc("matrix " + name + ": conformability error", 503);
        }
        // A classified `r(NNN): ` failure (the scalar-cell evaluator, I(),
        // vecdiag()'s unknown name) keeps its exact rc. The rest are design
        // decisions with unpinned rcs: a non-square vecdiag() argument r(505),
        // a malformed literal r(198), a remaining "not found" r(111).
        if (cerr.rfind("r(", 0) == 0) return matrixExprErrorBlock("matrix " + name + ": ", cerr);
        if (cerr == "vecdiag(): matrix not square") {
          return engineErrorBlockRc("matrix " + name + ": " + cerr, 505);
        }
        if (triedLiteral) {
          return engineErrorBlockRc("matrix " + name + ": malformed literal `" + rhs + "'", 198);
        }
        if (cerr.rfind("matrix `", 0) == 0 && cerr.find("' not found") != std::string::npos) {
          return engineErrorBlockRc("matrix " + name + ": " + cerr, 111);
        }
        return engineErrorBlock("matrix " + name + ": " + cerr);
      }
      std::optional<std::vector<std::string>> terms = splitMatrixExprTerms(rhs);
      if (!terms.has_value()) {
        return engineErrorBlock(
            "matrix " + name + ": unsupported expression `" + rhs +
            "' — this version of PIET's `matrix` only supports a `(r\\c,...)` literal, "
            "a NAME'*NAME-style product (postfix transpose ' and multiply *), "
            "rownames/colnames, list, and accum");
      }
      try {
        eval::EvalMatrix mat = evalMatrixExprTerms(*terms, session);
        storeMatrixResult(session, name, std::move(mat));
        return ojson::array();
      } catch (const MatrixExprBail& e) {
        return engineErrorBlock("matrix " + name + ": " + e.message);
      }
    }

    return engineErrorBlock(
        "matrix: only `matrix NAME = e(b)/e(V)`, a `(r\\c,...)` literal, a NAME'*NAME "
        "product, `matrix rownames`/`colnames`, `matrix list`, and `matrix accum` are "
        "wired in this version of PIET");
  }
  const std::string name = m.str(1);
  const char cls = m.str(2)[0];        // 'e' or 'r'
  const std::string which = m.str(3);  // was "b"/"V" only; now any posted name

  // One resolver (resolveClassMatrixRef), shared with vecdiag()'s argument.
  // An absent name keeps today's refusal text and rc (the absent-r() content
  // is a separate task, FIX-SPEC section 8).
  std::string classErr;
  std::optional<eval::EvalMatrix> resolved = resolveClassMatrixRef(cls, which, session, classErr);
  if (!resolved.has_value()) return engineErrorBlock(classErr);
  eval::EvalMatrix mat = std::move(*resolved);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { p.second = std::move(mat); return ojson::array(); }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
  return ojson::array();  // matrix define is silent
}

// ---------------------------------------------------------------------------
// label variable / label values

ojson runLabelVariable(const std::string& varname, const std::string& text, SessionState& session) {
  session.setVarLabel(varname, text);
  return ojson::array();  // silent success (Stata-faithful, mirrors label.py)
}

namespace {
// label_define.py's _refresh_value_labels_for_var: rebuild
// session.valueLabels[varname] (int-keyed) from session.labelDefs[lblname]
// (string-keyed) — called after both `label values` and `label define`
// (define refreshes every var currently attached to that lblname).
void refreshValueLabelsForVar(SessionState& session, const std::string& varname,
                              const std::string& lblname) {
  auto defIt = session.labelDefs.find(lblname);
  if (defIt != session.labelDefs.end() && !defIt->second.empty()) {
    std::map<int, std::string> numeric;
    for (const auto& [codeStr, text] : defIt->second) {
      try {
        numeric[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    session.valueLabels[varname] = numeric;
  } else {
    session.valueLabels.erase(varname);
  }
}
}  // namespace

ojson runLabelValues(const std::string& varname, const std::optional<std::string>& lblname,
                     SessionState& session) {
  if (!lblname.has_value() || lblname->empty()) {
    // `label values VAR` (no lblname) detaches.
    session.varLblname.erase(varname);
    session.valueLabels.erase(varname);
    return ojson::array();
  }
  session.varLblname[varname] = *lblname;
  refreshValueLabelsForVar(session, varname, *lblname);
  return ojson::array();
}

// ---------------------------------------------------------------------------
// format VARLIST %fmt — BS-005/D6 (BETA B-data): a DISPLAY-LAYER session
// attribute only (applies to browse/list — see runList's fmtMap threading
// above and Datastore::browse()'s conditional "format" column-meta key; NOT
// persisted into hbin — that lands with hbin v2/P1 per D6). Writes into the
// SAME session.varMeta[name].format field applyDtaMeta() (handlers.cpp)
// already populates from a .dta's own native formats — `format` is simply
// the user-facing SETTER for that pre-existing field. Unknown/malformed
// %fmt is an honest reject, never a silent accept (D6) — `display`'s own
// separate inline `%fmt EXPR` mechanism (display_hardening.cpp's
// applyStataFmtLocal) is untouched; this is the persistent per-variable
// attribute, a different mechanism.

namespace {

// Deliberately-scoped %fmt validity check (SCOPE.md BS-005: "display layer
// only... not a full typed-format engine"): the three common Stata format
// families — numeric %[-]w[.d]{f,g,e}[c] (comma variant accepted for parity
// with real Stata's own grammar, even though piet::eval::applyStataFmt's
// renderer, reused below for numeric rendering elsewhere in this codebase,
// doesn't add thousands separators — a pre-existing, separate limitation
// this phase doesn't extend), string %[-]w s, and the 6 date/time letters
// %[-]w t{d,c,w,m,q,h,y}[...] (a trailing sub-format tail like
// %tdCCYY-NN-DD is accepted permissively, unvalidated further —
// analysis.do:308's own `format date_order %td` is the bare form). Anything
// else is malformed -> honest reject, per D6.
bool isValidDisplayFormat(const std::string& fmt) {
  static const std::regex reNumeric(R"(^%-?\d{1,3}(\.\d{1,2})?[fge]c?$)");
  static const std::regex reString(R"(^%-?\d{1,3}s$)");
  static const std::regex reDate(R"(^%-?\d{0,2}t[dcwmqhy].*$)");
  // AER scope-2 B2-3: `%tbNAME[:detail]`, the business-calendar family. Stata
  // accepts it whether or not the calendar EXISTS (measured) -- and so does
  // this, because the format is a display attribute and the calendar is
  // resolved at RENDER time, where a missing file falls back to %g exactly as
  // the manual's item 5 says. But the calendar-NAME rule binds here as hard as
  // it binds `bcal create` and `bcal load`, and the FORMAT plane is the third
  // surface, not an exception (review 2026-09-09; the earlier comment claimed
  // a blanket acceptance the measurement does not support):
  //   `format x %tbabcdefghij`      rc 0    (10 characters is inside the rule)
  //   `format x %tbabcdefghij:day`  rc 0
  //   `format x %tbelevenchars`     r(120)
  //   `format x %tbelevenchars:day` r(120)
  //   `format x %tbab-cd`           r(120)  ·  `format x %tb9abc`  r(120)
  // (StataBE 19.5, /private/tmp/fix-bcal/st/cap.log keys A_*.) The predicate is
  // bcal::isValidCalName — the ONE place the rule lives — not a fourth copy.
  static const std::regex reBcal(R"(^%-?\d{0,2}tb([A-Za-z_][A-Za-z0-9_]*)(?::.*)?$)");
  std::smatch bm;
  if (std::regex_match(fmt, bm, reBcal)) return bcal::isValidCalName(bm[1].str());
  return std::regex_match(fmt, reNumeric) || std::regex_match(fmt, reString) ||
         std::regex_match(fmt, reDate);
}

}  // namespace

ojson runFormat(const std::vector<std::string>& varlist, const std::string& fmt,
                SessionState& session) {
  if (varlist.empty()) {
    return engineErrorBlock("format: variable list required (syntax: format varlist %fmt)");
  }
  if (fmt.empty() || fmt[0] != '%') {
    return engineErrorBlock("format: invalid syntax — expected a %fmt after the variable list "
                            "(e.g. `format date %td`), got `" + fmt + "'");
  }
  if (!isValidDisplayFormat(fmt)) {
    // r(120) is Stata's rc for an invalid %fmt, measured for BOTH the general
    // family and the business-calendar name rule (StataBE 19.5, review
    // 2026-09-09: `format x %zz` r(120), `format x %tbelevenchars` r(120) —
    // cap.log keys A_zz_rc / A_eleven_rc). "invalid_format" is not one of the
    // frozen error CODES, so errorBlock() coerced it to "engine_error" and the
    // refusal reached the user as rc 1; `reason` is the only channel that
    // carries an exact rc (BUG-187), so the rc travels there.
    return ojson(eval::errorBlock(
        "engine_error",
        "format: `" + fmt +
            "' is not a format PIET recognizes in this build (supported families: numeric "
            "%w.dg/%w.df/%w.de[c], string %ws, date/time %td/%tc/%tw/%tm/%tq/%th/%ty, "
            "business-calendar %tbNAME, whose NAME is a Stata name of at most 10 characters)",
        std::nullopt, std::nullopt, std::nullopt, "r(120)"));
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("format: variable '" + v + "' not found");
  }
  for (const auto& v : varlist) {
    session.varMeta[v].format = fmt;
    // browse/list expose ONLY user-set formats, never the .dta-loaded defaults
    // applyDtaMeta() also writes into varMeta[].format — else every column of a
    // freshly-loaded .dta would gain a `format` key in browse JSON (freeze break).
    session.userFormattedVars.insert(v);
  }
  return ojson::array();  // silent success (Stata-faithful, mirrors `label variable`)
}

// ---------------------------------------------------------------------------
// label define / label list / label dir / label drop — C++ port of
// label_define.py (M3b, ROADMAP M3a's own note: "define/list/dir/drop —
// lower corpus frequency — stubbed for M3b").

namespace {

bool validStataName(const std::string& name) {
  static const std::regex re(R"(^[A-Za-z_][A-Za-z0-9_]{0,31}$)");
  return std::regex_match(name, re);
}

// _find_unquoted_comma: first top-level comma outside quotes/parens.
long findUnquotedComma(const std::string& s) {
  bool inQuote = false;
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (inQuote) {
      if (ch == '"') inQuote = false;
    } else if (ch == '"') {
      inQuote = true;
    } else if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

// _read_string: quoted (until closing ") or unquoted (until whitespace).
std::pair<std::string, size_t> readString(const std::string& s, size_t pos) {
  if (pos >= s.size()) return {"", pos};
  if (s[pos] == '"') {
    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos) return {s.substr(pos + 1), s.size()};
    return {s.substr(pos + 1, end - (pos + 1)), end + 1};
  }
  size_t end = pos;
  while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
  return {s.substr(pos, end - pos), end};
}

}  // namespace

ojson runLabelDefine(const std::string& rawLine, SessionState& session) {
  std::string raw = trim(rawLine);

  bool addOpt = false, modifyOpt = false, replaceOpt = false;
  long commaIdx = findUnquotedComma(raw);
  if (commaIdx != -1) {
    std::string optStr = lower(trim(raw.substr(static_cast<size_t>(commaIdx) + 1)));
    raw = trim(raw.substr(0, static_cast<size_t>(commaIdx)));
    if (optStr.find("modify") != std::string::npos) modifyOpt = true;
    else if (optStr.find("add") != std::string::npos) addOpt = true;
    else if (optStr.find("replace") != std::string::npos) replaceOpt = true;
  }

  static const std::regex reKw(R"(^label\s+define\s+)", std::regex::icase);
  std::smatch km;
  if (!std::regex_search(raw, km, reKw)) {
    return engineErrorBlock("Expected 'label define LBLNAME ...', got: '" + raw + "'");
  }
  std::string rest = raw.substr(static_cast<size_t>(km.length(0)));

  size_t sp = rest.find_first_of(" \t");
  std::string lblname = (sp == std::string::npos) ? rest : rest.substr(0, sp);
  if (lblname.empty()) return engineErrorBlock("label define: missing label name");
  if (!validStataName(lblname)) {
    return engineErrorBlock("label define: invalid label name '" + lblname + "'");
  }
  std::string body = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp));

  std::vector<std::pair<int, std::string>> mapping;  // preserves insertion order
  size_t pos = 0;
  while (pos < body.size()) {
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) break;

    static const std::regex reNum(R"(^-?\d+)");
    std::smatch nm;
    std::string remainder = body.substr(pos);
    if (!std::regex_search(remainder, nm, reNum) || nm.position(0) != 0) {
      std::string snippet = body.substr(pos, std::min<size_t>(20, body.size() - pos));
      return engineErrorBlock("label define " + lblname + ": expected integer code, got '" +
                              snippet + "'");
    }
    int code = std::stoi(nm.str(0));
    pos += static_cast<size_t>(nm.length(0));

    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) {
      return engineErrorBlock("label define " + lblname + ": code " + std::to_string(code) +
                              " has no text");
    }
    auto [text, newPos] = readString(body, pos);
    pos = newPos;
    mapping.emplace_back(code, text);
  }

  if (mapping.empty()) {
    return engineErrorBlock("label define " + lblname + ": no code/text pairs found");
  }

  auto existingIt = session.labelDefs.find(lblname);
  bool hasOpts = addOpt || modifyOpt || replaceOpt;
  // BUG-475 (measured StataBE 19.5, B4REC|label_define|<redefine_no_replace>|110|,
  // tests/eval/golden/aer_b4/milfpins_oracle_r.log:57-61): redefining an
  // EXISTING label with none of add/modify/replace is refused verbatim
  // "label <name> already defined" (log:58), which coarseRc's "already
  // defined" rule (eval/errors.cpp) maps to r(110). Must return BEFORE any
  // mutation or the propagation loop below, so the original mapping survives.
  if (existingIt != session.labelDefs.end() && !hasOpts) {
    return engineErrorBlock("label " + lblname + " already defined");
  }
  if (existingIt == session.labelDefs.end() || replaceOpt) {
    std::map<std::string, std::string> replaced;
    for (const auto& [k, v] : mapping) replaced[std::to_string(k)] = v;
    session.labelDefs[lblname] = replaced;
  } else if (modifyOpt) {
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) merged[std::to_string(k)] = v;
    session.labelDefs[lblname] = merged;
  } else {
    // add: new codes only; duplicate -> error.
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) {
      std::string ks = std::to_string(k);
      auto dupIt = merged.find(ks);
      if (dupIt != merged.end()) {
        return engineErrorBlock("label define " + lblname + ", add: code " + std::to_string(k) +
                                " already defined as '" + dupIt->second +
                                "'. Use ,modify to change existing codes.");
      }
      merged[ks] = v;
    }
    session.labelDefs[lblname] = merged;
  }

  // Propagate to valueLabels for every variable currently attached.
  for (const auto& [varname, lname] : session.varLblname) {
    if (lname == lblname) refreshValueLabelsForVar(session, varname, lblname);
  }
  return ojson::array();  // Stata silent on success
}

ojson runLabelList(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) {
    for (const auto& [name, _] : session.labelDefs) lblnames.push_back(name);
    std::sort(lblnames.begin(), lblnames.end());
  }
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  std::vector<std::string> errors;
  for (const auto& name : lblnames) {
    auto it = session.labelDefs.find(name);
    if (it == session.labelDefs.end()) {
      errors.push_back(name);
      continue;
    }
    lines.push_back(name + ":");
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [codeStr, text] : it->second) {
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%3d", code);
      lines.push_back("           " + std::string(buf) + "  " + text);
    }
  }
  if (!errors.empty()) {
    std::string errText;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i) errText += ", ";
      errText += errors[i];
    }
    lines.push_back("warning: " + errText + ": not defined");
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  return block;
}

ojson runLabelDir(SessionState& session) {
  std::vector<std::string> names;
  for (const auto& [name, _] : session.labelDefs) names.push_back(name);
  std::sort(names.begin(), names.end());
  ojson block;
  block["type"] = "display";
  if (names.empty()) {
    block["text"] = "(no value labels defined)";
    return block;
  }
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) joined += "\n";
    joined += names[i];
  }
  block["text"] = joined;
  return block;
}

ojson runLabelbook(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) {
    for (const auto& [name, _] : session.labelDefs) lblnames.push_back(name);
    std::sort(lblnames.begin(), lblnames.end());
  }
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  std::vector<std::string> errors;
  bool first = true;
  for (const auto& name : lblnames) {
    auto it = session.labelDefs.find(name);
    if (it == session.labelDefs.end()) {
      errors.push_back(name);
      continue;
    }
    if (!first) lines.push_back("");
    first = false;
    lines.push_back(std::string(88, '-'));
    lines.push_back("Value label " + name);
    lines.push_back(std::string(88, '-'));
    lines.push_back("  Definition");
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [codeStr, text] : it->second) {
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%6d", code);
      lines.push_back(std::string(buf) + "   " + text);
    }
    // Attached variables: session.varLblname is varname -> lblname (the
    // reverse index of session.labelDefs' lblname -> {code:text}).
    std::vector<std::string> vars;
    for (const auto& [varname, lname] : session.varLblname) {
      if (lname == name) vars.push_back(varname);
    }
    std::sort(vars.begin(), vars.end());
    std::string varsJoined;
    for (size_t i = 0; i < vars.size(); ++i) {
      if (i) varsJoined += " ";
      varsJoined += vars[i];
    }
    lines.push_back("");
    lines.push_back("   Variables:  " + (vars.empty() ? std::string("(none)") : varsJoined));
  }
  if (!errors.empty()) {
    std::string errText;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i) errText += ", ";
      errText += errors[i];
    }
    lines.push_back("warning: " + errText + ": not defined");
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  return block;
}

// `datasignature` — forward to the engine verb of the same name and show the
// string it returns. The engine prints plain text on stdout (JSON only on
// error), so this uses raw runCobra + errorFromRaw like the other status-line
// verbs rather than runCobraJson.
//
// Stata-SHAPED, not Stata-comparable: see the declaration in handlers.h and
// src/data/datasignature.h's Q9 ruling. This wiring deliberately does NOT try
// to reproduce Stata's checksums — it exposes the engine's own contract-
// satisfying fingerprint, which is what `save`/round-trip verification uses.
ojson runDatasignature(SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> args = {"datasignature", "--data", *session.currentHbin()};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  std::string sig = cr.stdout_text;
  while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r' || sig.back() == ' ')) {
    sig.pop_back();
  }
  ojson block;
  block["type"] = "display";
  block["text"] = sig;
  return block;
}

ojson runLabelDrop(const std::vector<std::string>& lblnames, SessionState& session) {
  if (lblnames.empty()) {
    return engineErrorBlock("label drop: specify at least one label name");
  }
  for (const auto& name : lblnames) {
    session.labelDefs.erase(name);
    std::vector<std::string> toDetach;
    for (const auto& [varname, lname] : session.varLblname) {
      if (lname == name) toDetach.push_back(varname);
    }
    for (const auto& varname : toDetach) {
      session.varLblname.erase(varname);
      session.valueLabels.erase(varname);
    }
  }
  return ojson::array();  // Stata silent on success
}

// ---------------------------------------------------------------------------
// preserve / restore — LIFO hbin-file-copy + display-metadata snapshot
// stack (preserve_restore.py's run_preserve/run_restore, ported exactly:
// both return a real data_mutation block with a `preserve_depth` field, NOT
// silent — unlike tempfile/tempname/label).

namespace {
// B5-D14 (BUG-454): the snapshot half of runPreserve, factored out so
// runRestore's `, preserve` re-arm can call it on the JUST-RESTORED state
// instead of trying to push the frame it popped back onto the stack (which
// may be invalid: MAXPERF Stage 2's unstash can CONSUME the resident stash,
// and the scratch-reclaim exemption below is keyed on live preserveStack
// membership, so a pop-then-repush of the SAME frame would open a reclaim
// window). Re-snapshotting from a known-good, already-restored state avoids
// both hazards. Returns an error block on snapshot failure, std::nullopt on
// success (frame pushed).
std::optional<ojson> pushPreserveSnapshot(SessionState& session) {
  PreserveFrame frame;
  // MAXPERF Stage 2 (item 1): when the resident plane is active, snapshot the
  // worker's resident data-state via `stash` — an O(columns) RAM capture with
  // NO disk write and NO full-dataset byte-copy. residentPreserve() also keeps
  // the crash-recovery floor (it materializes a dirty virtual head first, which
  // is a no-op on the common clean/real head). The clonefile plane REMAINS the
  // fallback when the resident plane is unavailable (§26-only engine / serve
  // disabled / worker down) — Q1 (gate ③) clone-first (APFS CoW) with the
  // pre-existing byte-copy as the universal fallback (see cloneOrCopyFile).
  std::optional<std::string> stashName = session.residentPreserve();
  if (stashName.has_value()) {
    frame.stashName = *stashName;
    // Record the preserve-time head file alongside the stash: residentPreserve()
    // materialized the head just before stashing, so this on-disk file is
    // byte-identical to the stash by construction. restore re-adopts it as a
    // CLEAN head (zero-write card build) instead of re-materializing the frame
    // via a dirty virtual token; it also doubles as a recovery source if the
    // worker (and its RAM stash) dies before restore. While this frame is live
    // the file is exempt from scratch reclaim (reclaimSupersededScratch checks
    // preserveStack), so no extra copy or pin is needed.
    frame.hbinPath = *session.currentHbin();
    if (piet::SessionState::scratchTraceOn()) {
      std::error_code tec;
      piet::SessionState::scratchTrace(
          std::filesystem::exists(frame.hbinPath, tec) ? "PRESERVE(twin-real)"
                                                       : "PRESERVE(twin-MISSING)",
          "resident", frame.hbinPath);
    }
  } else {
    std::string snapPath = nextPath(session.workdir(), "preserve");
    // Fallback plane: a virtual resident head must be snapshotted to disk first
    // (cloneOrCopyFile bypasses datastore()). restore() below hands the snapshot
    // file straight to session.load() (zero-copy) on this plane.
    session.ensureMaterializedHead();
    if (!cloneOrCopyFile(*session.currentHbin(), snapPath)) {
      // BUG-124 sibling: fail the preserve HERE, honestly — the old void call
      // pushed a torn (or missing) snapshot onto the stack for restore to load.
      return ojson(engineErrorBlock(
          "preserve: could not snapshot the dataset (disk full or I/O error)"));
    }
    frame.hbinPath = snapPath;
  }
  frame.varMeta = session.varMeta;
  frame.labelDefs = session.labelDefs;
  frame.varLblname = session.varLblname;
  frame.valueLabels = session.valueLabels;
  frame.dataLabel = session.dataLabel;
  frame.characteristics = session.characteristics;
  session.preserveStack.push_back(std::move(frame));
  return std::nullopt;
}
}  // namespace

ojson runPreserve(SessionState& session) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  if (std::optional<ojson> err = pushPreserveSnapshot(session)) {
    return *err;
  }

  // P2-PERF ARC-B/B3: render the unchanged varlist from the resident card, the
  // exact transition runUse already made (handlers.cpp). `preserve` does not
  // change the data at all — it only stacks a copy — so calling datastore()
  // here was asking for a full open of the head purely to re-print what the
  // session already knows, and on a dirty virtual head it forced a snapshot
  // besides. Honest fallback to the old path whenever the card cannot vouch
  // for itself.
  //
  // This rung was reverted once, in session 1, because the card could not
  // answer `format` and this render printed %9.0g where the pins said %9s.
  // That was BUG-190 and it is fixed at the root now (session_state.cpp
  // seedShadowFromFrameVars), which is what lets the rung return.
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "preserve";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "preserve";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}

ojson runRestore(SessionState& session, bool rearm) {
  if (session.preserveStack.empty()) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_nopreserve|622|,
    // tests/eval/golden/aer_b4/b5_misc_pins.log:90-91): `restore, preserve`
    // on an EMPTY stack is r(622), not the generic "1" the message-only block
    // below falls through to. Scoped to `rearm` only -- bare `restore` on an
    // empty stack has no matching oracle cell here and is byte-pinned as-is
    // (tests/eval/piet2/cells/test_preserve_restore.jsonl:12, no `reason`
    // field), so its rc is left untouched.
    if (rearm) {
      return engineErrorBlockRc(
          "restore: no previous preserve — use `preserve` before `restore`", 622);
    }
    return engineErrorBlock("restore: no previous preserve — use `preserve` before `restore`");
  }
  PreserveFrame frame = std::move(session.preserveStack.back());
  session.preserveStack.pop_back();
  if (piet::SessionState::scratchTraceOn()) {
    std::error_code tec;
    piet::SessionState::scratchTrace(
        std::filesystem::exists(frame.hbinPath, tec) ? "RESTORE(twin-real)"
                                                     : "RESTORE(twin-MISSING)",
        frame.stashName.empty() ? "clone" : "stash", frame.hbinPath);
  }

  // MAXPERF Stage 2 (item 1): a stash-backed preserve restores via `unstash`
  // (O(columns) RAM, no disk read); a clone-backed preserve (fallback plane)
  // reloads its file. The restored data-state is byte-identical on both planes
  // (§27 parity law), so the rendered card below is identical either way.
  bool restored = false;
  const bool stashBacked = !frame.stashName.empty();
  if (stashBacked) {
    restored = session.residentRestore(frame.stashName, frame.hbinPath);
  }
  if (!restored) {
    // Stash-backed and the unstash failed (worker restarted — the RAM snapshot
    // is gone): the preserve-time head file recorded alongside the stash is
    // byte-identical to the snapshot by construction, so load() it below as a
    // full recovery when it still exists. Only when no on-disk twin survives
    // either: honest error, never silent stale data. Clone-backed frames
    // (fallback plane) take the load() path exactly as before.
    std::error_code restoreEc;
    if (frame.hbinPath.empty() ||
        (stashBacked && !std::filesystem::exists(cecon::to_native_path(frame.hbinPath),
                                                 restoreEc))) {
      return engineErrorBlock(
          "restore: the preserved snapshot was held in the compute worker and is no longer "
          "available (the worker restarted). Re-run from your last saved dataset.");
    }
    session.load(frame.hbinPath);
  }
  session.varMeta = frame.varMeta;
  session.labelDefs = frame.labelDefs;
  session.varLblname = frame.varLblname;
  session.valueLabels = frame.valueLabels;
  session.dataLabel = frame.dataLabel;
  session.characteristics = frame.characteristics;
  // xtset-file-persistence: the preserved char table carries the declaration
  // that was live at `preserve`; restoring the one restores the other.
  session.adoptTsFromChars();

  if (rearm) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_after_preserve|0| +
    // B5VAL|zz_exists_after|0| i.e. a following `gen` succeeds, tests/eval/
    // golden/aer_b4/b5_misc_pins.log:97-103): `restore, preserve` restores
    // AND keeps the preserved copy, so a do-file can restore repeatedly from
    // one snapshot. Re-snapshot from the state JUST restored above (not the
    // popped `frame`) -- see pushPreserveSnapshot's comment for why reusing
    // the popped frame is unsafe.
    if (std::optional<ojson> err = pushPreserveSnapshot(session)) {
      return *err;
    }
  }

  // P2-PERF ARC-B/B3, the other end of the same window (see runPreserve above).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "restore";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "restore";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}

// ---------------------------------------------------------------------------
// tempfile / tempname — S5/U4. Silent on success (empty array).

ojson runTempfile(const std::vector<std::string>& names, SessionState& session) {
  if (!session.runTempdir.has_value()) {
    std::string dir = session.workdir() + "/tmp_" + std::to_string(std::time(nullptr));
    // BUG-111(a): create the dir via the portable API on BOTH platforms. The old
    // POSIX path shelled out `std::system("mkdir -p '<dir>'")` and DISCARDED the
    // return code — a workdir/TMPDIR containing an apostrophe (macOS volume names
    // like `Nam's MacBook`) broke the single-quoting, mkdir silently failed, and
    // every subsequent tempfile save then errored cryptically. create_directories
    // handles any path; surface an honest error on failure instead of swallowing it.
    std::error_code mkec;
    std::filesystem::create_directories(cecon::to_native_path(dir), mkec);
    if (mkec)
      return ojson(eval::engineError("tempfile: could not create scratch directory '" + dir +
                                     "': " + mkec.message()));
    session.runTempdir = dir;
  }
  for (const auto& name : names) {
    // BUG-111(b): per-CALL unique filename (Stata mints a fresh file each call) —
    // the macro `name` binds to this path, so scripts using `\`name'` are unchanged.
    std::string path = *session.runTempdir + "/" + name + "_" +
                       std::to_string(session.tempSeq++) + ".tmp";
    // BUG-360: record WHO minted it. A tempfile's lifetime in Stata is the
    // program or do-file that created it, not the run — so the returning
    // program needs to know which of these are its own (program.cpp's
    // ScopeGuard). Depth 0 = do-file level, still reaped by doexec's
    // TempdirReapGuard at the end of the run, which is the same thing Stata
    // does for a do-file-level tempfile.
    session.mintedTempfiles.emplace_back(session.progCallDepth, path);
    session.eval.locals.set(name, path);
  }
  return ojson::array();
}

ojson runTempname(const std::vector<std::string>& names, SessionState& session) {
  for (const auto& name : names) {
    // BUG-111(b): epoch-SECONDS collided for two `tempname` calls in the same second;
    // use the per-session monotonic counter so each call's basename is unique.
    std::string basename = "__" + name + "_" + std::to_string(session.tempSeq++);
    session.eval.locals.set(name, basename);
  }
  return ojson::array();
}

// BS-037: tempvar -- sibling of tempfile/tempname above, previously ZERO
// implementation (the raw token "tempvar" was still listed in the
// tokenizer's early macro-statement keyword net, eval/tokenizer.cpp:80,
// so `tempvar tv` tripped the generic "does not expand macros" tripwire
// before ever reaching dispatch — see dispatcher.cpp's U4 block, which now
// intercepts tempfile/tempname/tempvar identically, BEFORE tokenize() ever
// sees the line, same as it already did for tempfile/tempname). Real
// Stata: mints a scratch-safe NEW variable name (the column itself is only
// created later by e.g. `gen `tv''`) and auto-drops it at scope end — PIET
// has no scope-tracking to hook that auto-drop into, the SAME documented
// deviation runTempfile above already makes for its own scratch files.
// Unlike tempname's arbitrary "__NAME_TIMESTAMP" basename (fine for a FILE
// path only, never validated as an identifier), a tempvar's minted name
// must be a LEGAL Stata variable name AND must not collide with any column
// already in the current dataset — real Stata's own pattern is
// `__000001`, `__000002`, ...; mirrored here via a per-process monotonic
// counter, skipping forward past any collision against the loaded dataset
// (if any — tempvar is legal even before `use`/`sysuse`).
ojson runTempvar(const std::vector<std::string>& names, SessionState& session) {
  static std::atomic<uint64_t> counter{0};
  Datastore* ds = session.datastore();
  for (const auto& name : names) {
    std::string minted;
    for (;;) {
      uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "__%06llu", static_cast<unsigned long long>(n));
      minted = buf;
      if (ds == nullptr || !ds->has(minted)) break;
    }
    session.eval.locals.set(name, minted);
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// levelsof

namespace {

// levelsof.py's numeric _fmt: integer-valued -> plain integer string, else
// Python's str(float) shortest round-trip repr.
std::string levelsofFmtNum(double v) {
  if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
    return std::to_string(static_cast<long long>(v));
  }
  return eval::pythonFloatRepr(v);
}

}  // namespace

ojson runLevelsof(const std::string& varname, const std::optional<std::string>& localName,
                   bool clean, const std::optional<std::string>& ifExpr, SessionState& session) {
  // ONESHOT-GETCOLS2: `levelsof` reads ONE column's values (plus whatever an `if`
  // names). Materializing the head for that was 14 of the flushes left after the
  // mutation entrance moved. `varname` is passed as alsoInclude because it is the
  // subject, not an expression reference.
  std::string lvlMiniPath;
  std::unique_ptr<Datastore> lvlMini;
  if (auto cols = referencedColumns(session, {ifExpr.has_value() ? &*ifExpr : nullptr}, varname))
    lvlMini = fetchColumns(session, *cols, session.workdir(), &lvlMiniPath);
  if (lvlMini && !lvlMini->has(varname)) lvlMini.reset();  // subject absent → not usable
  Datastore* ds = lvlMini ? lvlMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  // levelsof.py's run_levelsof(ds, varname, clean) operates on `target_ds` —
  // the CURRENT dataset, or a row-filtered temp copy when `if COND` is
  // present (dispatcher.py's _run_levelsof: strmutate.write_filtered_hbin,
  // keep=True). M3a numeric-only slice: string/factor `if` conditions are
  // OUT of scope (strmutate's row-wise evaluator is not ported this
  // milestone) — a pure-numeric if routes through cobra eval --keepif.
  Datastore* targetDs = ds;
  std::unique_ptr<Datastore> filteredHolder;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    std::string outPath = nextPath(session.workdir(), "levelsof");
    // BUG-381: this filtered copy is READ and abandoned — it never becomes the
    // head, so no supersede ever names it and reclaimSupersededScratch cannot
    // reach it however it is scheduled. That is what the §38 projection ledger
    // is for: register it and the outermost dispatch boundary drains it.
    session.noteProjectionScratch(outPath);
    // ONESHOT-LASTFLUSH: the earlier wave filtered the REAL head here, reasoning
    // that a projection "carries only what we fetched". It carries exactly what
    // this filter needs: the `if` expression's own columns (that is what the fetch
    // was built from) plus `varname`, which is the only column read back below. So
    // filter the projection and leave the head virtual — this was the last 14 of
    // the flushes, and the fetch above was pure overhead on this branch before.
    //
    // The fallback is not decoration. `referencedColumns` leaves identifiers the
    // shadow card does not confirm alone, so a card that is missing a real column
    // yields a projection the filter cannot evaluate. That is an ENGINE error, not
    // a distinguishable one, so it is caught and retried against the head rather
    // than reported — a miss must cost a fallback, never an answer.
    const std::string lvlHead = session.currentHbin().value_or(ds->path());
    bool filtered = false;
    if (lvlMini) {
      std::vector<std::string> tryArgs = translate::injectTsContextForExpr(
          {"eval", "--data", lvlMiniPath, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(tryArgs);
        filtered = true;
      } catch (const CobraError&) {
        cecon::native_unlink(outPath);  // never leave a torn file for the retry to open
      }
    }
    if (!filtered) {
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", lvlHead, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlock("levelsof: " + cobraErrorStr(ce), ce.code);
      }
    }
    filteredHolder = std::make_unique<Datastore>(outPath);
    targetDs = filteredHolder.get();
  }

  if (!targetDs->has(varname)) {
    // Python: `raise KeyError(f"variable '{varname}' not found in dataset")`,
    // caught as `except KeyError as e: return make_engine_error(str(e))`.
    // KeyError.__str__() wraps the message in an EXTRA layer of double
    // quotes (Python's repr-of-a-single-string-arg convention) — reproduce
    // that exact double-quoting, not just the inner message.
    return engineErrorBlock("\"variable '" + varname + "' not found in dataset\"");
  }

  std::vector<std::string> levelStrings;
  if (targetDs->dtype(varname) == "f64") {
    const double* d = targetDs->col(varname).f64();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(double));
    std::vector<double> uniq;
    for (int64_t i = 0; i < count; ++i) {
      if (!std::isnan(d[i])) uniq.push_back(d[i]);
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    for (double v : uniq) levelStrings.push_back(levelsofFmtNum(v));
    // clean has NO effect on numeric columns (Python only branches on
    // `clean` inside the `else` / factor-dtype arm) — level_strings above
    // is final for f64 regardless of `clean`.
  } else {
    const int32_t* codes = targetDs->col(varname).codes();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(int32_t));
    std::vector<int32_t> uniqCodes;
    for (int64_t i = 0; i < count; ++i) {
      if (codes[i] >= 0) uniqCodes.push_back(codes[i]);
    }
    std::sort(uniqCodes.begin(), uniqCodes.end());
    uniqCodes.erase(std::unique(uniqCodes.begin(), uniqCodes.end()), uniqCodes.end());
    const auto& levelsList = targetDs->varinfo(varname).levels;
    std::vector<std::string> rawLabels;
    for (int32_t c : uniqCodes) {
      rawLabels.push_back((c >= 0 && static_cast<size_t>(c) < levelsList.size())
                              ? levelsList[static_cast<size_t>(c)]
                              : std::to_string(c));
    }
    std::sort(rawLabels.begin(), rawLabels.end());
    for (const auto& lbl : rawLabels) {
      levelStrings.push_back(clean ? lbl : ("\"" + lbl + "\""));
    }
  }

  std::string joined;
  for (size_t i = 0; i < levelStrings.size(); ++i) {
    if (i) joined += " ";
    joined += levelStrings[i];
  }

  ojson out;
  out["type"] = "levelsof";
  out["varname"] = varname;
  ojson levelsArr = ojson::array();
  for (const auto& s : levelStrings) levelsArr.push_back(s);
  out["levels"] = levelsArr;
  out["text"] = joined;

  // `if local_name:` — ONLY set when local() was explicitly given (no
  // default local-var name).
  if (localName.has_value() && !localName->empty()) {
    session.eval.locals.set(*localName, joined);
  }
  return out;
}

// ---------------------------------------------------------------------------
// M3b: clear / set obs / cd / pwd / noop (set|version|compress) — foundational
// commands the corpus's data-construction idiom (`clear` -> `set obs N` ->
// `gen ...`) depends on. C++ port of dispatcher.py's clear/set_obs/cd/pwd
// handlers + session.py's clear_data.

// BS-044 (BETA campaign C4): `clear <subcmd>` scoping. Before this fix
// EVERY subcommand fell through to the same bare-data-wipe path (probe
// bs044: `clear results` answered "No dataset is loaded"), which is wrong
// for every subcommand except a genuinely bare `clear`/`clear all`.
//
// Ground truth here is Stata's own clear.ado (StataNow 19.5, dumped via
// `program list` while live-probing this item — not a guess):
//   ""            -> `drop _all` + `label drop _all` ONLY. Scalars,
//                     matrices, e()/r() results and est-store all SURVIVE
//                     a bare `clear` (live-probed: sca1/matrix m1/ereturn
//                     list all still present after a bare `clear`).
//   "all"/"*"     -> everything (data, labels, matrices, scalars, e()/r(),
//                     programs, panelvar/timevar).
//   "results"/    -> ONE shared branch in the real ado: `return clear` +
//   "matrix"         `ereturn clear` + `sreturn clear` + `_return drop
//                     _all` (r()/e()/s() results only — NOT scalars, NOT
//                     matrices). `matrix` ADDITIONALLY does `matrix drop
//                     _all` + `_est drop _all` (user matrices + the eststo
//                     store) — oracle-pinned (`clear results` keeps N=336
//                     + scalar list) plus this ado reading for the r()/e()
//                     split and the matrix/results relationship.
//   "programs"    -> `program drop _all` (session.programs — real store).
//   "mata"        -> empties the M0 Mata store (session.eval.mataMatrices).
//   "ado"/"rngstream"/"rngstreams"/"frames"/"collect" -> real Stata
//                     subcommands with genuinely NO backing store in this
//                     build (no frames/collect engine, no rngstream
//                     state, no ado-vs-interactive program provenance) —
//                     honest scoped no-ops with a note, replacing the old
//                     "wipe the whole dataset" mistake, not a silent
//                     no-message pass-through.
//   anything else -> real Stata hard-rejects ("`1' not allowed", r(198));
//                     matched here rather than silently no-opping OR
//                     wiping data for a typo'd subcommand.
//
// Deviation from this batch's task-brief shorthand (noted for review): the
// brief grouped "matrix" with the no-store no-op list, but PIET already
// carries a real matrix store (eval.matrices/eval.eMatrices, populated by
// the `matrix define` handler) and a real est-store (session.estimates) —
// clearing them costs no new infrastructure and matches verified Stata
// truth, so `clear matrix` is implemented as a genuine clear here, not a
// no-op. See this task's return report for the full callout.
ojson runClear(const std::string& subIn, SessionState& session) {
  std::string sub = lower(trim(subIn));

  auto silentEcho = []() {
    ojson out;
    out["type"] = "echo";
    out["text"] = "";
    return out;
  };

  if (sub.empty()) {
    // Bare `clear`: dataset (+ its label defs) ONLY.
    session.clearData();
    session.clearMetadata();
    return silentEcho();
  }

  if (sub == "all" || sub == "*") {
    session.clearData();
    session.clearMetadata();
    session.eval.locals.clear();
    session.eval.globals.clear();
    session.eval.scalars.clear();
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.matrices.clear();
    session.eval.mataMatrices.clear();  // clear.ado's `all` branch runs `mata: mata clear`
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.panelvar.reset();
    session.timevar.reset();
    session.programs.clear();       // ado's `program drop _all` under "all".
    ++session.programsEpoch;
    session.returnStaging.clear();  // ado's `_return drop _all` under "all".
    // BUG-392 (2026-09-02): the rest of clear.ado's `all` branch — `_est drop _all`,
    // `graph drop _all`, `postutil clear`, `file close _all`, `frames reset`, the
    // preserve stack — plus the session's own `use` memos and the engine's stashes
    // (BUG-390), none of which any earlier `clear all` touched.
    session.clearAllSessionObjects();
    return silentEcho();
  }

  if (sub == "results" || sub == "matrix") {
    // Shared ado branch: r()/e()/s() results + return-staging.
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.returnStaging.clear();
    if (sub == "matrix") {
      // `clear matrix` additionally: `matrix drop _all` + `_est drop _all`.
      session.eval.matrices.clear();
      session.estimatesClear();
    }
    return silentEcho();
  }

  if (sub == "programs") {
    session.programs.clear();
    ++session.programsEpoch;
    return silentEcho();
  }

  // ml-getmata T4: Mata now HAS a store (session.eval.mataMatrices, the M0
  // subset's namespace), so `clear mata` is a real clear and no longer one of
  // the scoped no-ops below. It touches nothing else -- data, Stata matrices
  // and results all survive it, which selftest's C4/BS-044 leg (d) pins.
  if (sub == "mata") {
    session.eval.mataMatrices.clear();
    return silentEcho();
  }

  static const std::unordered_set<std::string> kNoStoreSubs = {
      "ado", "rngstream", "rngstreams", "frames", "collect"};
  if (kNoStoreSubs.count(sub)) {
    ojson out;
    out["type"] = "echo";
    out["text"] = "(clear " + sub + ": no-op -- this build of PIET has no " + sub + " store)";
    return out;
  }

  return engineErrorBlock("clear: '" + sub + "' not allowed", "syntax_error");
}

ojson runSetObs(const std::string& nArg, SessionState& session) {
  // int(float(str(n))) — Python's tolerant numeric-string parse ("8" and
  // "8.0" both -> 8; "8.5" -> 8 via truncation, matching Python's int()).
  double nd;
  try {
    size_t pos = 0;
    nd = std::stod(nArg, &pos);
    // Python's `str(n).strip()` -> float() requires the WHOLE trimmed
    // string to parse; std::stod stops early on trailing garbage — reject
    // that case exactly like float() would raise ValueError.
    std::string rest = nArg.substr(pos);
    size_t a = rest.find_first_not_of(" \t");
    if (a != std::string::npos) throw std::invalid_argument("trailing garbage");
  } catch (const std::exception&) {
    return engineErrorBlock("set obs: '" + nArg + "' is not a valid number of observations");
  }
  long long nTarget = static_cast<long long>(nd);
  if (nTarget < 0) {
    return engineErrorBlock("set obs: number of observations must be >= 0");
  }

  std::string out = nextPath(session.workdir(), "setobs");
  // A dataset with NO variables (`clear` + `set obs 0`, or `set obs 3` on an
  // empty session) has nothing to pad, and the pad-and-append path below would
  // hand cobra a header-only CSV with no columns. Growing it is the same act as
  // building a blank frame from nothing, so it takes the branch that does that.
  Datastore* dsNow = session.currentHbin().has_value() ? session.datastore() : nullptr;
  const bool growNoVars =
      dsNow != nullptr && dsNow->names().empty() && nTarget > dsNow->nRows();
  if (!session.currentHbin().has_value() || growNoVars) {
    // No data in memory -> N blank observations, 0 variables. Build via a
    // throwaway 1-column CSV (a dummy value repeated nTarget times) then
    // `cobra eval --drop` the dummy column, landing on {n_rows:N,
    // columns:[]} — bit-identical to the oracle's _write_hbin(out,[],[],n).
    std::string csvPath = session.workdir() + "/setobs_seed.csv";
    registerScratchMint(csvPath);  // BUG-389
    {
      std::ofstream csv(cecon::to_native_path(csvPath));
      csv << "_z\n";
      for (long long i = 0; i < nTarget; ++i) csv << "1\n";
      // BUG-124 sibling: surface a silently-failed CSV write (ENOSPC) here
      // instead of letting convert build a short dataset from it.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(csvPath);
        return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::string seedHbin = session.workdir() + "/setobs_seed.hbin";
    registerScratchMint(seedHbin);  // BUG-389
    std::vector<std::string> convArgs = {"convert", "--data", csvPath, "--out", seedHbin,
                                          "--delim", ","};
    // BUG-124: runCobra does not throw — check the result (the old catch was
    // dead) and remove any torn partial before it can be load()ed/mmap'd.
    CobraRunResult convCr = runCobra(convArgs);
    if (!convCr.ok) {
      cecon::native_unlink(seedHbin);
      CobraError ce = errorFromRaw(convCr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (nTarget == 0) {
      // cobra convert on a header-only (0-data-row) CSV — degrade straight
      // to the seed hbin's own {n_rows:0} shape without a --drop round-trip
      // (an empty dataset has no columns to drop from safely either way).
      session.load(seedHbin);
    } else {
      std::vector<std::string> dropArgs = {"eval", "--data", seedHbin, "--drop", "_z", "--out",
                                           out};
      try {
        runCobraJson(dropArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(out);
    }
    return mutationResult(session, "set_obs");
  }

  Datastore* ds = session.datastore();
  int64_t curRows = ds ? ds->nRows() : 0;
  if (nTarget < curRows) {
    return engineErrorBlock("set obs: cannot reduce the number of observations from " +
                            std::to_string(curRows) + " to " + std::to_string(nTarget));
  }
  if (nTarget == curRows) {
    return mutationResult(session, "set_obs");
  }

  // Extend: pad every existing column to nTarget rows with system-missing.
  // cobra has no direct "append N blank rows" verb; build a blank frame of
  // (nTarget - curRows) rows with the SAME column set (each column = "."),
  // then `cobra append` it onto the current dataset.
  //
  // BUG-697: this path never worked against the current engine, three ways:
  //  1. `cobra append --out` prints a plain-text notice on stderr and NOTHING on
  //     stdout (apps/cmds/cmds_data.cpp cmd_append), and this call went through
  //     runCobraJson, so every extension of a non-empty dataset -- `set obs 8`
  //     on 6 rows, svmat/getmata with more rows than _N -- died as "cobra
  //     produced non-JSON output", rc 1. It is a PIET call-site bug, not an
  //     engine one: the verb's contract is the file, as runVerb/runCobra use it
  //     everywhere else (handlers_aer_reshape.cpp's insobs, this file's
  //     line-level note "use runCobra (not runCobraJson ...)").
  //  2. The pad cells were BLANK. cobra's CSV reader skips a blank line, so on a
  //     one-variable dataset every pad row vanished ("6 master + 0 using") and
  //     _N silently stayed put -- hidden until now behind (1).
  //  3. A blank-or-`.` column in the pad CSV reads as NUMERIC, and append's
  //     unflagged default promotes a numeric/string clash to f64 -- a string
  //     variable would have come back as numbers. `--type-clash force` makes
  //     the pad cells missing OF THE MASTER'S TYPE (BUG-550's Stata rule), which
  //     is exactly "extend with missing".
  // Measured against the frozen engine: `.` cells + --type-clash force give
  // "6 master + 2 using = 8 rows", f64 stays f64, a factor stays a factor.
  int64_t padRows = nTarget - curRows;
  std::string padCsv = session.workdir() + "/setobs_pad.csv";
  registerScratchMint(padCsv);  // BUG-389
  {
    std::ofstream csv(cecon::to_native_path(padCsv));
    const auto& names = ds->names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) csv << ",";
      csv << names[i];
    }
    csv << "\n";
    for (int64_t r = 0; r < padRows; ++r) {
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) csv << ",";
        csv << ".";  // never blank: see BUG-697 (2) above
      }
      csv << "\n";
    }
    // BUG-124 sibling: flush + check — a silently short pad CSV appends fewer
    // blank rows than `set obs` promised.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(padCsv);
      return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  std::vector<std::string> args = {"append",       "--data", *session.currentHbin(),
                                    "--using",      padCsv,   "--type-clash",
                                    "force",        "--out",  out};
  CobraRunResult appendCr = runCobra(args);
  if (!appendCr.ok) {
    CobraError ce = errorFromRaw(appendCr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BUG-124's torn-write guard, as at every other file-contract call site.
  if (!cecon::native_exists(out)) {
    return engineErrorBlock("set obs: failed writing output dataset (disk full or I/O error)");
  }
  session.load(out);
  // The count is asserted, not assumed: (2) above extended nothing at rc 0.
  if (Datastore* grown = session.datastore(); grown == nullptr || grown->nRows() != nTarget) {
    return engineErrorBlock("set obs: the dataset has " +
                            std::to_string(grown ? grown->nRows() : 0) +
                            " observations after extending, not " + std::to_string(nTarget));
  }
  return mutationResult(session, "set_obs");
}

ojson runCd(const std::string& dirArgIn, SessionState& session) {
  std::string dirArg = trim(dirArgIn);
  // dequote (routes_data.py's _dequote: strip matching outer quotes)
  if (dirArg.size() >= 2 && ((dirArg.front() == '"' && dirArg.back() == '"') ||
                             (dirArg.front() == '\'' && dirArg.back() == '\''))) {
    dirArg = dirArg.substr(1, dirArg.size() - 2);
  }
  // `~` expansion (os.path.expanduser) — HOME-relative only, matches the
  // common corpus usage; `~user` forms are not exercised.
  if (!dirArg.empty() && dirArg[0] == '~') {
    std::string home = cecon::env_utf8("HOME");
#ifdef _WIN32
    // Windows has no HOME by default; ~ resolves against USERPROFILE (matches
    // resolveUserPath's _WIN32 branch). POSIX path unchanged.
    if (home.empty()) home = cecon::env_utf8("USERPROFILE");
#endif
    if (!home.empty()) dirArg = home + dirArg.substr(1);
  }
  namespace fs = std::filesystem;
  fs::path p(dirArg);
  if (p.is_relative()) {
    std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                        : fs::current_path().u8string();
    p = cecon::to_native_path(base) / p;
  }
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(p, ec);
  if (ec) abs = p.lexically_normal();
  if (!fs::is_directory(abs)) {
    return engineErrorBlock("Directory not found: " + abs.u8string());
  }
  session.cwd = abs.u8string();
  session.eval.cwd = *session.cwd;
  // AER scope-2 B2-3: bofd()/dofb() resolve `<name>.stbcal` against the
  // session's working directory, and the expression surfaces that call them
  // (display / local = / gen) reach the loader through a process-level search
  // directory rather than a session handle -- macros.cpp's evalArith uses the
  // immutable process namespace. Keeping it in step HERE is what makes
  // `cd sub` + `bofd("cal", d)` find sub/cal.stbcal (measured: Stata finds a
  // calendar in the new cwd after cd).
  bcal::setSearchDir(*session.cwd);
  ojson out;
  out["type"] = "echo";
  out["text"] = abs.u8string();
  return out;
}

ojson runPwd(SessionState& session) {
  namespace fs = std::filesystem;
  std::string cwd = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                     : fs::current_path().u8string();
  ojson out;
  out["type"] = "echo";
  out["text"] = cwd;
  return out;
}

// BUG-028 / BS-006: `log using FILE [, replace|append] [text]` really opens
// FILE now (SessionState::openLog) — every subsequent OUTERMOST dispatch()
// call's finalized blocks get teed to it as a plain-text transcript (see
// dispatcher.cpp's dispatch()/dispatchImpl() split + SessionState::logTee's
// doc comments). `log close` flushes+closes; `log off`/`log on` pause/
// resume an already-open log (stream stays open, writes suspended while
// paused). Sub-keyword + `using` path extraction is UNCHANGED from the
// original BUG-028 shim (mirrors router/commands.py's parse: sub = first
// varlist token; path via a `using ("..."|'...'|\S+)` regex on the raw
// command text, resolved against session.cwd like shims.py's
// _resolve(session, path)) — everything below the path-resolution is new.
//
// Format: `text` is the ONLY implemented format (BS-006's stated scope).
// `smcl` — given explicitly, OR implied by Stata's own default-by-extension
// rule for a bare ".smcl" path when NEITHER `text` nor `smcl` is given — is
// an HONEST REJECT rather than silently writing plain text into a file
// that claims SMCL markup (PIET's standing "never silently wrong"
// convention; matches this build's general policy of naming a gap instead
// of mislabeling output). Documented deviation, not a bug: a real SMCL
// renderer is out of scope here.
//
// Mode: `replace` truncates; `append` (or bare with no existing file) opens
// in append mode — C++'s ios::app already creates a fresh file when none
// exists, matching Stata's own `append` semantics with no extra logic.
// Bare with an EXISTING file is Stata's real "file already exists" hard
// stop — kept honest (not silently defaulted either way) rather than
// guessing; the message text below deliberately contains "file" +
// "already exists" so errors.cpp's coarseRc() maps a `capture`-wrapped
// call's `_rc` to 602 (Stata's real code) via its existing engine_error/
// mentionsFileOrDir branch, with no new mapping rule needed there.
//
// Only one log open at a time: a second `log using` while one is already
// open is an honest reject too (matching real Stata — you must `log close`
// first). BS-117 (multi named-log bookkeeping / `name()`) stays post-beta/
// OUT; analysis.do and the eval golden fixtures never open more than one.
ojson runLog(const std::string& raw, SessionState& session) {
  // Tokenize on whitespace to find the sub-keyword (varlist[0] equivalent) —
  // "log" itself is the command word, so the first token after it is the sub.
  // BUG-251/WS-Q: was `rawLower.find("log")` + `substr(pos + 3)` — the fifth
  // member of BUG-232's family, in the same file as two of the four the entry
  // named, and missed by its grep because that grep only searched for the
  // command words the arc was widening. `log` has no Stata abbreviation today
  // (measured), so this was latent, not live; it is repaired anyway because
  // "latent" here means "one registry edit from killing the run".
  //
  // No spelling literal and no npos arithmetic: the sub-keyword is whatever
  // follows the leading token, and the leading token is the command word by
  // construction (eval::tokenize sets pc.raw = strip(line)). Kept local rather
  // than sharing dispatcher.cpp's rawArgsAfterCmd() because that helper lives
  // in dispatcher's anonymous namespace; this carries no command-word
  // knowledge, so it is not a second copy of a spelling set.
  std::string after;
  {
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = raw.find_first_of(" \t", b);
      if (e != std::string::npos) after = trim(raw.substr(e));
    }
  }
  std::string afterLower = lower(after);
  size_t subEnd = afterLower.find_first_of(" \t");
  std::string sub = lower(subEnd == std::string::npos ? after : after.substr(0, subEnd));

  if (sub == "using") {
    static const std::regex reUsing(R"re(\busing\s+("([^"]+)"|'([^']+)'|(\S+)))re",
                                     std::regex::icase);
    std::smatch m;
    std::string path;
    if (std::regex_search(raw, m, reUsing)) {
      if (m[2].matched) {
        path = m[2].str();
      } else if (m[3].matched) {
        path = m[3].str();
      } else {
        path = m[4].str();
        // BS-006: the bare/unquoted alternative's `\S+` is greedy and a
        // comma is not whitespace, so `log using file.log, replace` (no
        // space before the comma — the common style) swallows the comma
        // into `path` too. Quoted paths (m[2]/m[3] above) never hit this —
        // they stop at the closing quote — so the strip is scoped to this
        // branch only; a bare path can never legitimately end in a comma.
        if (!path.empty() && path.back() == ',') path.pop_back();
      }
    }
    if (!path.empty()) {
      // resolve relative to session.cwd, matching shims.py's _resolve(session, path).
      namespace fs = std::filesystem;
      fs::path p(path);
      if (p.is_relative()) {
        std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                            : fs::current_path().u8string();
        p = cecon::to_native_path(base) / p;
      }
      path = p.lexically_normal().u8string();
    }
    if (path.empty()) {
      return engineErrorBlock("log using: no filename given");
    }

    // BS-006: parse the post-comma option tail the SAME quote/paren-aware
    // way every other command does (BS-018's `do FILE, nostop` precedent —
    // dispatcher.cpp). The `using` regex above already extracts the
    // filename regardless of where the comma sits, so re-splitting `raw`
    // here for the options tail is safe.
    std::string optsTail;
    long long commaPos = eval::findUnquotedComma(raw);
    if (commaPos >= 0) optsTail = trim(raw.substr(static_cast<size_t>(commaPos) + 1));
    auto opts = eval::parseOpts(optsTail);
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLogKnownOpts = kVocabLog.known;
    // BUG-262 — a PER-NET exception to the dispatcher's single resolution
    // point, and the reason there has to be one. That point canonicalizes
    // `parsed.opts`; this handler does not read `parsed.opts`. It re-parses its
    // own option list out of `raw` (it has to: `log using "f, g.log", replace`
    // needs the quote-aware split), so the vector below never passed through
    // the dispatcher and arrives with raw keys. Measured: `log using f.log,
    // repl` was accepted by the net and then dropped by the loop underneath,
    // so the log was NOT replaced. Same lesson as BUG-232 — "one resolution
    // point" is a claim about ONE VECTOR, and a second parse is a second
    // vector. Uses the registered vocabulary, so still no second copy of the
    // spellings.
    eval::canonicalizeOptionKeys(opts, kLogKnownOpts);
    if (auto err = eval::rejectUnknownOptions(opts, kLogKnownOpts)) {
      return ojson(*err);
    }
    bool wantReplace = false, wantAppend = false, wantText = false, wantSmcl = false;
    std::string logName;
    for (const auto& [k, v] : opts) {
      if (k == "replace") {
        wantReplace = true;
      } else if (k == "append") {
        wantAppend = true;
      } else if (k == "text") {
        wantText = true;
      } else if (k == "smcl") {
        wantSmcl = true;
      } else if (k == "name") {
        // W:P8: `name(X)`. Stata also accepts `name(X, replace)`; only the
        // name itself is meaningful here (the file-level replace/append
        // decision is the option pair above), so take the first token.
        logName = v.isBool ? std::string() : trim(v.strValue);
        size_t comma = logName.find(',');
        if (comma != std::string::npos) logName = trim(logName.substr(0, comma));
        if (logName.empty()) {
          return engineErrorBlock("log using: option `name()' requires a log name",
                                   "syntax_error");
        }
      }
    }
    if (wantReplace && wantAppend) {
      return engineErrorBlock("log using: `replace' and `append' may not be combined",
                               "syntax_error");
    }
    if (wantText && wantSmcl) {
      return engineErrorBlock("log using: `text' and `smcl' may not be combined",
                               "syntax_error");
    }

    // Format selection (see this function's top-of-doc comment).
    bool looksSmclByExtension = false;
    {
      std::string lowerPath = lower(path);
      looksSmclByExtension = lowerPath.size() >= 5 &&
                              lowerPath.compare(lowerPath.size() - 5, 5, ".smcl") == 0;
    }
    if (wantSmcl || (!wantText && looksSmclByExtension)) {
      return engineErrorBlock(
          "log using: smcl log format is not implemented in this build -- use `log using "
          "FILE, text' for a plain-text log",
          "unsupported_option");
    }

    // W:P8: the clash is per NAME, not per session — a named log may be opened
    // while the unnamed one is running (that is the whole point of `name()`,
    // and what the P8 target does), but reopening a name that is already
    // logging is Stata's own hard stop.
    if (session.logIsOpen(logName)) {
      return engineErrorBlock(logName.empty()
                                  ? std::string("log using: log is already open -- run `log "
                                                "close' first")
                                  : "log using: log " + logName +
                                        " is already open -- run `log close " + logName +
                                        "' first");
    }

    bool appendMode = wantAppend;
    if (!wantReplace && !wantAppend && cecon::native_exists(path)) {
      return engineErrorBlock(
          "log using: file already exists (specify `replace' or `append')");
    }
    // The `log using` command is itself running inside a dispatch frame, so
    // its own depth is the depth of the sibling commands this log must record.
    if (!session.openLog(path, appendMode, logName, session.dispatchDepth())) {
      return engineErrorBlock("log using: could not open \"" + path + "\" for writing");
    }
    return ojson::array();  // Stata: silent when log opens successfully
  }

  // `close`/`off`/`on` take an optional log name (or `_all`) after the
  // sub-keyword: `log close p8named`, `log off _all`.
  std::string target;
  if (subEnd != std::string::npos) target = trim(after.substr(subEnd));
  const bool targetAll = lower(target) == "_all";
  if (targetAll) target.clear();

  if (sub == "close") {
    if (targetAll) {
      session.closeAllLogs();
      return ojson::array();
    }
    // Naming a log that is not open is Stata's r(111) "log NAME not found"
    // (live-verified). A bare `log close` with nothing open stays the silent
    // no-op it has always been.
    if (!session.closeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log closes
  }

  if (sub == "off") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.pauseLog(n);
      return ojson::array();
    }
    if (!session.pauseLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log pauses
  }

  if (sub == "on") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.resumeLog(n);
      return ojson::array();
    }
    if (!session.resumeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log resumes
  }

  // Unknown subcommand -- silent noop (unchanged from BUG-028's original shim).
  return ojson::array();
}

// ---------------------------------------------------------------------------
// U7/S9 offline shims: which / ssc / confirm / mkdir / erase — mirrors
// piet/backend/piet/shims.py (Python oracle) + router/commands.py's parse.
// All are no-data-needed (kNoDataCommands) so classroom do-files that guard
// package/file existence before `use` never halt. analysis.do's line-77
// package-presence loop (`capture which drdid` / `capture ssc install ...`)
// and its dozen `capture confirm variable/numeric/string ...` guards
// (throughout the depvar/control setup) exercise all five directly.

namespace {
// Extract a quoted-or-bare first argument after `word` in `raw` (mirrors the
// commands.py `mkdir`/`erase` regex: `\bWORD\s+("([^"]+)"|'([^']+)'|(\S+))`).
std::string firstArgAfter(const std::string& raw, const std::string& word) {
  std::regex re("\\b" + word + "\\s+(\"([^\"]+)\"|'([^']+)'|(\\S+))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(raw, m, re)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}

std::string joinSpaceLocal(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// A legal Stata identifier: starts with a letter or underscore, followed
// by letters/digits/underscores, <=32 chars. Used only by confirm's
// `names`/`name` sub-form (BS-042) — deliberately NOT Stata's full name
// grammar (no reserved-word/`str#`-collision checks — those need real
// infrastructure this handler doesn't have; see runConfirm's own doc
// comment for the honest-skip call).
bool isValidStataName(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

// Same quote-aware first-argument extraction as firstArgAfter, but
// anchored to "whatever the line's own first token is" rather than one
// specific literal word — lets a single handler (runErase) serve more than
// one Stata command spelling for the SAME command. BS-045 (BETA campaign
// C4): `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed
// `which rm` -> "built-in command: rm", not an invented alias), so
// runErase needs to extract its path argument correctly regardless of
// whether the raw line says "erase ..." or "rm ...".
std::string firstArgAfterCmd(const std::string& raw) {
  std::string t = trim(raw);
  size_t sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  std::string rest = trim(t.substr(sp));
  static const std::regex reArg(R"re(^("([^"]+)"|'([^']+)'|(\S+)))re");
  std::smatch m;
  if (std::regex_search(rest, m, reArg)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}
}  // namespace

// `which NAME` — always rc0 (offline: we say every package is installed).
ojson runWhich(const std::string& raw, const std::vector<std::string>& varlist) {
  std::string name = !varlist.empty() ? varlist[0] : trim(firstArgAfter(raw, "which"));
  ojson out;
  out["type"] = "display";
  out["text"] = "(which: '" + name + "' -- PIET: assumed installed)";
  return out;
}

// `ssc install PKG [, replace]` / other ssc subcommands — noop rc0 (offline).
ojson runSsc(const std::vector<std::string>& varlist) {
  std::string sub = !varlist.empty() ? lower(varlist[0]) : "";
  std::string name = varlist.size() > 1 ? varlist[1] : "";
  if (sub == "install") {
    ojson out;
    out["type"] = "display";
    out["text"] = "(ssc install " + name + ": PIET is offline -- skipped)";
    return out;
  }
  return ojson::array();  // describe/uninstall/... -- silent noop
}

// `confirm [new] file|variable|numeric|string|integer|number|scalar|
// existence|names ARG` — existence/type check. Error block on failure (a
// `capture` prefix sets _rc via eval::coarseRc); silent (empty array) on
// success, matching Stata.
//
// BETA campaign C4 (BS-042/BS-043): every sub-form below was live-probed
// against StataNow 19.5 to pin its exact rc/message shape (see the C4
// agent's task-return report for the full probe transcript). Two root
// causes fixed this batch:
//   - "new variable" was never recognised as its own sub-command: the old
//     regex only special-cased "new file", so "confirm new variable X"
//     fell through to the varlist-tokenized fallback with sub="new" (an
//     unmatched literal), silently succeeding regardless of X's existence
//     (BS-042). Fixed by generically stripping a "new " prefix off subRaw
//     (mirrors how "new file" was already narrowed to "file") instead of
//     the old file-only `.find("file")` special case.
//   - "confirm integer number N" fed "number N" (not just "N") to the
//     integer parser, since "number" is REQUIRED Stata syntax glue between
//     "integer" and the value, not consumed by the old regex's single
//     "integer" alternative (BS-043). Fixed by stripping an optional
//     leading "number " token off arg specifically in the integer branch
//     (same pattern as the numeric/string branch's own "variable " strip
//     immediately below).
ojson runConfirm(const std::string& raw, const std::vector<std::string>& varlist,
                  SessionState& session) {
  static const std::regex reConfirm(
      R"re(^\s*confirm\s+(new\s+variable|new\s+file|number|integer|file|variable|numeric|string|existence|names|name|scalar)\s+(.*))re",
      std::regex::icase);
  std::smatch m;
  std::string sub, arg;
  bool isNew = false;
  if (std::regex_search(raw, m, reConfirm)) {
    std::string subRaw = lower(trim(m[1].str()));
    arg = trim(m[2].str());
    isNew = subRaw.rfind("new", 0) == 0;
    if (isNew) {
      size_t p = subRaw.find_first_not_of(" \t", 3);
      subRaw = (p == std::string::npos) ? "" : subRaw.substr(p);
    }
    sub = subRaw == "name" ? "names" : subRaw;
    if (sub == "numeric" || sub == "string") {
      static const std::regex reVarKw(R"(^variable\s+)", std::regex::icase);
      arg = std::regex_replace(arg, reVarKw, "");
    }
    if (sub == "integer") {
      // BS-043: "confirm integer number 5" — "number " is required syntax
      // glue, not part of the value; strip it the same way numeric/string
      // strip their own "variable " glue two lines up. A bare "confirm
      // integer 5" (no "number") is left untouched (harmless permissive
      // superset, never exercised by the oracle either way).
      static const std::regex reNumberKw(R"(^number\s+)", std::regex::icase);
      arg = std::regex_replace(arg, reNumberKw, "");
    }
    arg = trim(arg);
    if (arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') ||
                            (arg.front() == '\'' && arg.back() == '\''))) {
      arg = arg.substr(1, arg.size() - 2);
    }
  } else {
    sub = !varlist.empty() ? lower(varlist[0]) : "";
    if (sub == "name") sub = "names";
    arg = varlist.size() > 1 ? varlist[1] : "";
  }

  if (sub == "file") {
    std::string path = resolveUserPath(session, arg);
    namespace fs = std::filesystem;
    // BUG-361: ask the SESSION before asking the filesystem. A `save` to a minted
    // tempfile is routed to the engine's in-RAM virtual-file table when the
    // resident plane is live (handlers_data_verbs.cpp's isMintedTempfile arm) and
    // writes no bytes at all, so `fs::exists` answers "no" about a file the user
    // just saved and can still `use` — and answers it with _rc 601, the SAME code
    // a genuinely absent file returns, so a script cannot tell the two apart.
    // Measured 2026-08-19: plain save -> confirm _rc 0; tempfile save -> _rc 601;
    // absent file -> _rc 601, while `use` on that middle path restores all 5 rows.
    // `use` already consults the table (isVirtualSave, handlers.cpp:363); this is
    // the same question asked from the same session, so the two agree by
    // construction rather than by inspection. Both keys come from
    // resolveUserPath(), so they are the same string. A registered entry the
    // engine has since lost still reads as present here — which is what the
    // session believes, and the following `use` is where that failure belongs.
    bool exists = fs::exists(cecon::to_native_path(path)) || session.isVirtualSave(path);
    if (isNew) {
      if (exists) return engineErrorBlock("confirm new file: file already exists: " + path);
    } else {
      if (!exists) return engineErrorBlock("confirm file: file not found: " + path);
    }
    return ojson::array();
  }

  if (sub == "variable" || sub == "numeric" || sub == "string") {
    if (isNew) {
      // BS-042: `confirm new variable NAME [NAME ...]` — multi-name form,
      // ALL names must be new; report the FIRST existing conflict (matches
      // live-probed Stata: `confirm new variable newvar1 x newvar2` with x
      // pre-existing -> "variable x already defined", rc=110, stopping at
      // the first hit even though newvar2 was never checked). With no
      // dataset loaded, every name is trivially new (nothing can conflict).
      std::istringstream iss(arg);
      std::string tok;
      while (iss >> tok) {
        if (session.hasData()) {
          // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT
          // card is authoritative for name presence either way (brief §1
          // confirm row). Falls back to the unchanged datastore() path when
          // the card is unavailable/non-current or under the
          // PIET_NO_RESIDENT_READ escape arm.
          bool exists;
          if (const std::vector<VarInfo>* card = session.residentReadCard()) {
            exists = cardHasName(*card, tok);
          } else {
            exists = session.datastore()->has(tok);
          }
          if (exists) {
            return engineErrorBlock("confirm new variable: variable '" + tok +
                                     "' already defined");
          }
        }
      }
      return ojson::array();
    }
    if (!session.hasData()) {
      return engineErrorBlock("confirm " + (sub == "variable" ? std::string("variable") : sub) +
                               ": no dataset loaded");
    }
    // BUG-429: `confirm variable A B` is a VARLIST, exactly as `confirm new
    // variable` above already treats it (BS-042). The single-name reading made
    // the whole string one name and answered `variable 'A B' not found` — rc 111
    // where Stata is rc 0 — which is how a probe written against Stata stops on
    // a dataset that has both variables. Every name is checked; the FIRST
    // failure is reported, which is Stata's own order.
    std::vector<std::string> names;
    {
      std::istringstream iss(arg);
      std::string tok;
      while (iss >> tok) names.push_back(tok);
    }
    if (names.empty()) names.push_back(arg);
    for (const std::string& one : names) {
      // C4-L2A (D1 card plane): has + dtype are exactly what the shadow card
      // carries, in the same closed "f64"|"factor" vocabulary Datastore::dtype()
      // answers (datastore.cpp:68 — so the numeric/string verdicts and every
      // error string below are byte-identical between planes). Card unavailable
      // /non-current, or the PIET_NO_RESIDENT_READ escape arm → the unchanged
      // datastore() path.
      bool found;
      std::string dtype;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        const VarInfo* vi = cardFind(*card, one);
        found = vi != nullptr;
        if (vi) dtype = vi->dtype;
      } else {
        Datastore* ds = session.datastore();
        found = ds->has(one);
        if (found) dtype = ds->dtype(one);
      }
      if (!found) {
        return engineErrorBlock("confirm " + sub + ": variable '" + one + "' not found in dataset");
      }
      if (sub == "numeric" || sub == "string") {
        bool isStrDtype = dtype.rfind("str", 0) == 0 || dtype == "factor";
        if (sub == "numeric" && isStrDtype) {
          return engineErrorBlock("confirm numeric: variable '" + one + "' is not numeric (dtype=" +
                                   dtype + ")");
        }
        if (sub == "string" && !isStrDtype) {
          return engineErrorBlock("confirm string: variable '" + one +
                                   "' is not a string variable (dtype=" + dtype + ")");
        }
      }
    }
    return ojson::array();
  }

  if (sub == "integer") {
    try {
      size_t pos = 0;
      double val = std::stod(arg, &pos);
      if (pos != arg.size() || val != std::trunc(val)) {
        return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
      }
    } catch (...) {
      return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
    }
    return ojson::array();
  }

  if (sub == "number") {
    // BS-042: `confirm number ARG` — any real number (integer or not) is
    // fine; message wording mirrors Stata's own exact text (live-probed:
    // `confirm number abc` -> "'abc' found where number expected", rc=7).
    bool ok = false;
    if (!arg.empty()) {
      try {
        size_t pos = 0;
        std::stod(arg, &pos);
        std::string rest = arg.substr(pos);
        ok = rest.find_first_not_of(" \t") == std::string::npos;
      } catch (...) {
        ok = false;
      }
    }
    if (!ok) {
      return engineErrorBlock("confirm number: '" + arg + "' found where number expected");
    }
    return ojson::array();
  }

  if (sub == "scalar") {
    // BS-042: live-probed (`confirm scalar notascalar` -> "scalar
    // notascalar not found", rc=111; existing scalar -> rc=0).
    if (!session.eval.scalars.contains(arg)) {
      return engineErrorBlock("confirm scalar: scalar '" + arg + "' not found");
    }
    return ojson::array();
  }

  if (sub == "names") {
    // BS-042: `confirm names NAME [NAME ...]` (Stata alias `confirm name`
    // for the single-name form, normalised to "names" above) — each token
    // must be a legal identifier; live-probed exact Stata wording is
    // unquoted ("9def invalid name", rc=7), matched verbatim below.
    // Reports the FIRST malformed token, matching Stata's own
    // left-to-right stop-at-first-error.
    std::istringstream iss(arg);
    std::string tok;
    while (iss >> tok) {
      if (!isValidStataName(tok)) {
        return engineErrorBlock("confirm names: " + tok + " invalid name");
      }
    }
    return ojson::array();
  }

  if (sub == "existence") {
    // BS-042: `confirm existence TEXT` — Stata's real semantics here are
    // just "is there anything to confirm" (non-blank), NOT a name-format
    // check (that's `names`/`name` above) — live-probed: blank arg ->
    // "'' found where something expected", rc=6 (no existing coarseRc
    // bucket for r(6); honest-skipped exact-rc fidelity for this specific
    // blank-arg corner case — see this batch's report). Any non-blank arg
    // always succeeds.
    if (trim(arg).empty()) {
      return engineErrorBlock("confirm existence: '' found where something expected");
    }
    return ojson::array();
  }

  // Unknown confirm sub-type -- silent noop (future-proof, mirrors shims.py).
  return ojson::array();
}

// `mkdir DIRNAME [, public]` — create ONE directory level.
//
// BS-045 (BETA campaign C4): live-probed against StataNow 19.5 — real
// Stata's `mkdir` has NO parents-creation option at all (the pre-existing
// `,p` here was an INVENTED option; `help mkdir`'s only option is
// `public`). A missing parent directory is a genuine failure in real
// Stata, not silently bridged: `mkdir /tmp/nonexistent_parent/child`
// fails with the SAME message/rc as re-mkdir'ing an already-existing
// directory: "could not create directory <path>", rc=693 (oracle-pinned
// for the already-exists case; live-probed for the missing-parent case
// too — identical wording both times). `,public` makes the new directory
// world-readable+executable (chmod 0755); omitting it leaves whatever the
// platform's own umask-derived create_directory result is. Unknown
// options are rejected via the shared net (A2 idiom), matching live-probed
// Stata (`mkdir DIR, bogusopt` -> "option bogusopt not allowed", rc=198).
ojson runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfter(raw, "mkdir"));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("mkdir: directory name required");

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMkdirKnownOpts = kVocabMkdir.known;
  if (auto err = eval::rejectUnknownOptions(opts, kMkdirKnownOpts)) {
    return ojson(*err);
  }
  bool isPublic = false;
  for (const auto& [k, v] : opts) {
    if (k == "public" && v.isBool && v.boolValue) isPublic = true;
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  std::error_code ec;
  // create_directory (NOT create_directories): single level only, exactly
  // like real Stata's own mkdir — a missing parent must fail, not be
  // silently bridged. Returns false (with ec cleared, not an error) for
  // BOTH "already exists" and most creation failures; the missing-parent
  // case sets ec (ENOENT) on some platforms/libstdc++ versions and merely
  // returns false on others, so both `ec` and `!created` are treated as
  // the same real-Stata "could not create directory" outcome.
  bool created = fs::create_directory(cecon::to_native_path(resolved), ec);
  if (ec || !created) {
    return engineErrorBlock("could not create directory " + resolved);
  }
  if (isPublic) {
    fs::permissions(cecon::to_native_path(resolved),
                     fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec,
                     fs::perm_options::replace, ec);
    // Best-effort: a chmod failure doesn't invalidate the directory that
    // was already successfully created above.
  }
  return ojson::array();  // Stata: silent on success
}

// `rmdir DIRNAME` — remove ONE empty directory.
//
// BUG-180 (P2-PREP): PIET had no `rmdir` at ALL — registry.cpp registered
// mkdir/erase/rm but never this, so the command fell through the whole
// routing table to the generic unsupported_command fallback. Measured
// consequence: the cross-engine benchmark harness clears its scratch dirs
// with `capture quietly rmdir "$BENCH_ROOT/dir_`rep'"` before each timed
// `mkdir`; under PIET that line did nothing, the directories from earlier
// runs survived, and every subsequent `mkdir` answered r(693) — the
// benchmark reported mkdir failing 10/10 for a reason that was not mkdir.
//
// Semantics live-probed against StataNow 19.5 this stint (not guessed):
//   rmdir "empty_dir"        -> rc 0, silent
//   rmdir "nonempty_dir"     -> "could not remove directory <path>", rc 693
//   rmdir "no_such_dir"      -> "could not remove directory <path>", rc 693
//   rmdir "some_file.txt"    -> "could not remove directory <path>", rc 693
//   rmdir                    -> "invalid syntax", rc 198
//   rmdir DIR, anyopt        -> "options not allowed", rc 101
// Every failure mode shares ONE message and ONE rc — hence the single
// `fail` lambda below rather than a per-case wording that real Stata does
// not actually distinguish.
//
// Two deliberate notes:
//  - The is_directory() guard is load-bearing, not defensive padding:
//    std::filesystem::remove() would happily delete a plain FILE, so
//    without it `rmdir notes.txt` would silently destroy a file that real
//    Stata refuses to touch. remove() on a NON-EMPTY directory fails on
//    its own (ENOTEMPTY), which is the "empty only" half.
//  - Options: real Stata answers r(101), a bucket this codebase has
//    nowhere else. Routed through the shared reject net instead (r(198),
//    "option `X' not allowed"), which is the uniform idiom every other
//    arm here uses — a deliberate, reported deviation rather than a new
//    one-command rc bucket.
ojson runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("rmdir: directory name required", "syntax_error");

  if (auto err = eval::rejectUnknownOptions(opts, {})) {
    return ojson(*err);
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  auto native = cecon::to_native_path(resolved);
  // "could not remove directory <path>" is Stata's answer to ALL of:
  // missing path, path is a file, directory not empty, permission denied.
  auto fail = [&resolved]() {
    return engineErrorBlock("could not remove directory " + resolved);
  };

  std::error_code ec;
  if (!fs::is_directory(native, ec) || ec) return fail();
  // remove() (NOT remove_all()): single EMPTY directory only, exactly like
  // real Stata — a non-empty directory must fail, never be recursively
  // erased. Returns false when nothing was removed; ec carries ENOTEMPTY.
  if (!fs::remove(native, ec) || ec) return fail();
  return ojson::array();  // Stata: silent on success
}

// `erase FILE` / `rm FILE` — delete a file. BS-045 (BETA campaign C4):
// `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed `which
// rm` -> "built-in command: rm"), routed to this SAME handler by the
// dispatcher; firstArgAfterCmd (unlike the old firstArgAfter(raw,
// "erase")) doesn't hardcode the command word, so it extracts the path
// correctly for either spelling.
ojson runErase(const std::string& raw, const std::vector<std::string>& varlist,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("erase: filename required");

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  if (!fs::exists(cecon::to_native_path(resolved))) {
    return engineErrorBlock("file " + resolved + " not found");
  }
  std::error_code ec;
  fs::remove(cecon::to_native_path(resolved), ec);
  if (ec) return engineErrorBlock("erase: " + ec.message());
  return ojson::array();  // Stata: silent on success
}

// `timer on|off|clear|list [N]` (BS-124, BETA campaign C4) — a session-
// scoped stopwatch bank mirroring Stata's own `timer` command: 100 slots
// (1-100), each a (running?, monotonic start, accumulated total seconds,
// stop count) tuple (SessionState::timers / TimerSlot). Previously an
// honest hard-stop: "timer" sat in dispatcher.cpp's kNoDataCommands
// allow-list (correctly — timer must work with zero data loaded, same as
// the oracle's own probe) but had NO actual `if (cmd == "timer")` dispatch
// arm, so it fell through the entire routing table to the generic
// "unsupported_command" bottom fallback. Fixed by adding the arm (below,
// dispatcher.cpp) — kNoDataCommands' "timer" entry stays (removing it
// would newly break `timer clear`/`timer on` before any `use`, which real
// Stata allows).
//
// Every on/off/clear/list state-transition rule below was live-probed
// against StataNow 19.5 (not guessed):
//   - `timer off N` when N was never started: silent no-op, rc=0 (no
//     accumulation, no count increment).
//   - `timer on N` when N is ALREADY running: silent no-op (does not
//     restart the clock or double-count).
//   - `timer list N` on a slot with no data (count==0): prints nothing.
//   - `timer list` (bare): prints ONLY slots with data (count>0), in slot
//     order — not all 100.
//   - `timer list N` sets r(tN)/r(ntN) for that slot only; an empty slot
//     retires those two stale keys. Bare `timer list` refreshes all timer
//     keys while preserving unrelated r() results.
//   - slot out of 1..100: Stata's own "invalid syntax", rc=198.
// Column format is oracle-pinned EXACTLY (c-oracle-stata.log's BS-124
// block): "%4d: %9.2f / %8.0f = %12.4f" per slot (verified char-for-char
// against `   1:      0.10 /        1 =       0.1010`).
ojson runTimer(const std::string& sub, const std::optional<int>& slotArg, SessionState& session) {
  auto slotOk = [](int n) { return n >= 1 && n <= 100; };
  auto formatSlot = [](int slot, const TimerSlot& t) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%4d: %9.2f / %8.0f = %12.4f", slot, t.totalSeconds,
                  static_cast<double>(t.count),
                  t.count > 0 ? t.totalSeconds / static_cast<double>(t.count) : 0.0);
    return std::string(buf);
  };
  auto retireReturns = [&session](int slot) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.erase("t" + suffix);
    session.eval.rValues.erase("nt" + suffix);
  };
  auto setReturns = [&session](int slot, const TimerSlot& t) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.set("t" + suffix, t.totalSeconds);
    session.eval.rValues.set("nt" + suffix, static_cast<double>(t.count));
  };

  if (sub == "on") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (!t.running) {
      t.running = true;
      t.startedAt = std::chrono::steady_clock::now();
    }
    return ojson::array();
  }

  if (sub == "off") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (t.running) {
      double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t.startedAt).count();
      t.totalSeconds += elapsed;
      t.count += 1;
      t.running = false;
    }
    return ojson::array();
  }

  if (sub == "clear") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      session.timers[static_cast<size_t>(*slotArg)] = TimerSlot{};
    } else {
      for (size_t i = 1; i < session.timers.size(); ++i) session.timers[i] = TimerSlot{};
    }
    return ojson::array();
  }

  if (sub == "list") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      const TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
      retireReturns(*slotArg);
      if (t.count == 0) return ojson::array();  // empty slot: silent, matches Stata
      setReturns(*slotArg, t);
      ojson out;
      out["type"] = "echo";
      out["text"] = formatSlot(*slotArg, t);
      return out;
    }
    // Bare `timer list`: every slot 1..100 WITH data, in slot order.
    for (int i = 1; i <= 100; ++i) retireReturns(i);
    std::string text;
    for (int i = 1; i <= 100; ++i) {
      const TimerSlot& t = session.timers[static_cast<size_t>(i)];
      if (t.count == 0) continue;
      setReturns(i, t);
      if (!text.empty()) text += "\n";
      text += formatSlot(i, t);
    }
    if (text.empty()) return ojson::array();
    ojson out;
    out["type"] = "echo";
    out["text"] = text;
    return out;
  }

  return engineErrorBlock("timer: unrecognized sub-command" + (sub.empty() ? "" : " '" + sub + "'"),
                           "syntax_error");
}

}  // namespace piet::handlers
