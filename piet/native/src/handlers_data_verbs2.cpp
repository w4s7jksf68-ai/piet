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
#include "piet/column_fetch.h"  // BUG-394: reshape long j-values via §38
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
const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j"},
     {}});
// B4-W3b: `generate`, `update` and `replace` graduate from UNIMPLEMENTED to
// KNOWN — the engine grew all three on main 3a5d80e2 ("merge --update and the
// egen riders, against an oracle captured first") and this is the PIET side of
// that landing. The rest stay unimplemented and keep saying so.
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate",
                 "keep", "assert", "generate", "update", "replace"},
     {"nolabel", "nonotes", "noreport", "force", "sorted"}});
}  // namespace

namespace {

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

}  // namespace

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
  if (auto err = eval::rejectUnknownOptions(opts, kKnownReshapeOpts)) {
    return *err;
  }

  auto [iVars, jVar] = parseIvarJvar(opts);
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
  const std::vector<std::string>& allNames = ds->names();
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
  std::string outPath = nextPath(session.workdir(), "reshape_" + direction);
  std::vector<std::string> args = {"reshape", direction,  "--data", ds->path(), "--stubs",
                                    joinedStubs, "--i",    joinedI,  "--j",      *jVar,
                                    "--out",   outPath};
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
    if (jDs != nullptr && jDs->has(*jVar) && jDs->dtype(*jVar) == "f64") {
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
namespace {
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
}  // namespace

// ---------------------------------------------------------------------------
// append using FILE — C++ port of append.py's run_append. cobra 0.18.0's
// native `append` verb accepts .hbin/.dta/.csv directly on --using (no
// manual pre-conversion needed, verified empirically) and already
// implements the f64/factor dtype-promotion + missing-column padding the
// oracle documents (contract §22) — this only resolves the path against
// session.cwd and pre-checks existence for the oracle's exact error text.
ojson runAppend(const std::string& filepathArg, SessionState& session,
                 const std::optional<std::string>& genVar,
                 const std::optional<std::string>& keepSpec, bool force, bool nolabel) {
  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  std::string filepath = resolveUserPath(session, filepathArg);
  session.materializeVirtualSave(filepath);  // BUG-125: a save_virtual'd tempfile becomes real on demand
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
    if (ds->has(*genVar)) {
      return engineErrorBlock("append: variable '" + *genVar + "' already defined");
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
  // message). A .csv/.hbin using without keep() goes to the engine as before.
  std::string engineUsing = filepath;
  std::string keepCsv;
  std::string usingMetaPath;
  const std::string ext = extOf(filepath);
  if (ext == ".dta") {
    std::string conv = nextPath(session.workdir(), "append_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389
    CobraRunResult ccr = runCobra({"import-dta", filepath, conv, "--out-meta", usingMetaPath});
    if (!ccr.ok) {
      CobraError ce = errorFromRaw(ccr);
      return engineErrorBlock("append using: cannot convert '" + filepath + "': " + ce.what());
    }
    engineUsing = conv;
  }
  if (keepSpec.has_value()) {
    if (ext == ".csv" || ext == ".tsv") {
      std::string conv = nextPath(session.workdir(), "append_using_csv");
      char delim = (ext == ".tsv") ? '\t' : ',';
      CobraRunResult ccr = runCobra(
          {"convert", "--data", filepath, "--out", conv, "--delim", std::string(1, delim)});
      if (!ccr.ok) {
        CobraError ce = errorFromRaw(ccr);
        return engineErrorBlock("append using: cannot convert '" + filepath + "': " + ce.what());
      }
      engineUsing = conv;
    }
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
  }

  std::string outPath = nextPath(session.workdir(), "append");
  std::vector<std::string> args = {"append", "--data", ds->path(), "--using", engineUsing, "--out",
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
  // drops its variable; app11/app12: not under nolabel). Attachments are not
  // copied: the master's `a` stays unlabelled (app0 lists 10, not "ten"), and no
  // oracle arm measures a using-only labelled variable.
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

  if (genVar.has_value()) {
    // B5-D10 (measured B4REC|append|generate|0|, tests/eval/golden/aer_b4/
    // milfpins_oracle_r.log:96-97): src marks which side each row came from —
    // 0 for the nMaster original master rows, 1 for the nUsing rows appended
    // from `using`. Same setcol --new mechanism winsor uses for its genName
    // column (handlers_aer_abbrev.cpp), applied to the head the append above
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
    session.setVarLabel(*genVar, "Dataset source");
    result = mutationResult(session, "append");
    result["n_master"] = nMaster;
    result["n_using"] = nUsing;
  }
  return result;
}

// ---------------------------------------------------------------------------
// merge 1:1|m:1|1:m KEYLIST using FILE [, keepusing() nogenerate keep()
// assert()] — C++ port of merge_op.py's run_merge. cobra 0.18.0's native
// `merge` verb performs the actual join (factor-key label-join, outer join,
// keep()/assert()/keepusing()/nogenerate all natively supported per
// CONTRACT-engine §22) but its error TEXT diverges from the oracle's
// (verified empirically: cobra's cardinality error omits the duplicate-key
// value and its column-conflict message is shorter) — pre-validate the
// oracle's own checks (key existence, column conflict, cardinality) here so
// error cells match byte-for-byte; success path + option semantics delegate
// to cobra entirely.

namespace {

// _check_cardinality's _find_dup: first duplicated key (by first appearance
// order, mirroring pandas' duplicated(keep=False).idxmax() — the FIRST row
// index where a dup exists) — read raw column values directly (f64 raw
// value or factor level string) to format `{k=v}` like the oracle's
// DataFrame cell repr.
struct DupKeyResult {
  bool found = false;
  std::vector<std::string> keyValueStrs;  // formatted per keylist entry
};

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

}  // namespace

ojson runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
               const std::string& usingPathArg,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  if (!session.currentHbin().has_value()) {
    return engineErrorBlock("No dataset loaded. Load a dataset before merge.");
  }
  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  std::string usingPath = resolveUserPath(session, usingPathArg);
  session.materializeVirtualSave(usingPath);  // BUG-125: a save_virtual'd tempfile becomes real on demand
  if (!fileExists(usingPath)) {
    return engineErrorBlock("merge using: file not found: '" + usingPath + "'");
  }

  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();

  // The using side may be .dta/.csv — pre-convert to hbin so we can
  // pre-validate keys/cardinality on it directly (cobra's own merge also
  // accepts .dta/.csv on --using, but we need a Datastore view for the
  // pre-checks below regardless).
  std::string usingHbinPath = usingPath;
  std::string ext = lower(usingPath.substr(usingPath.find_last_of('.') == std::string::npos
                                               ? usingPath.size()
                                               : usingPath.find_last_of('.')));
  std::string usingMetaPath;  // xtset-file-persistence: the using side's char table
  if (ext == ".dta") {
    std::string conv = nextPath(session.workdir(), "merge_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389 (same sidecar shape as append's)
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv, "--out-meta", usingMetaPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock("merge using: cannot convert '" + usingPath + "': " + ce.what());
    }
    usingHbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "merge_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr =
        runCobra({"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock("merge using: cannot convert '" + usingPath + "': " + ce.what());
    }
    usingHbinPath = conv;
  }

  Datastore usingDs(usingHbinPath);

  for (const auto& k : keylist) {
    if (!master->has(k)) return engineErrorBlock("merge: key variable '" + k + "' not in master dataset");
    if (!usingDs.has(k)) {
      return engineErrorBlock("merge: key variable '" + k + "' not in using dataset (" + usingPath +
                              ")");
    }
  }

  // keepusing(): restrict using-side non-key columns BEFORE the conflict
  // check (matches the oracle's ordering).
  std::optional<std::string> keepusingOpt;
  bool nogenerate = false;
  std::optional<std::string> keepOpt, assertOpt;
  // B4-W3b riders.
  std::optional<std::string> generateOpt;
  bool updateOpt = false, replaceOpt = false;
  // BUG-148: resolve Stata option abbreviations to canonical spellings BEFORE reading them.
  // `merge ..., nogen` must set nogenerate, not fall through this loop unmatched — an accepted
  // abbreviation that is then ignored would suppress nothing and silently create `_merge`.
  static const std::unordered_set<std::string> kMergeCanonical = {
      "keepusing", "nogenerate", "keep", "assert", "generate", "update", "replace"};
  std::vector<std::pair<std::string, eval::OptValue>> optsC(opts);  // `opts` is const&
  eval::canonicalizeOptionKeys(optsC, kMergeCanonical);
  for (const auto& [k, v] : optsC) {
    if (k == "keepusing" && !v.isBool) keepusingOpt = v.strValue;
    if (k == "nogenerate" && v.isBool && v.boolValue) nogenerate = true;
    if (k == "keep" && !v.isBool) keepOpt = v.strValue;
    if (k == "assert" && !v.isBool) assertOpt = v.strValue;
    if (k == "generate" && !v.isBool) generateOpt = trim(v.strValue);
    if (k == "update" && (!v.isBool || v.boolValue)) updateOpt = true;
    if (k == "replace" && (!v.isBool || v.boolValue)) replaceOpt = true;
  }
  // ------------------------------------------------------------------
  // B4-W3b: the three riders, and the two shapes Stata refuses.
  //
  // `replace` is a MODIFIER OF `update`, not an option of its own: it changes
  // what happens on a matched row where the two datasets DISAGREE (update
  // alone keeps the master's value and marks the row 5; update replace
  // overwrites it, and the CODE distribution does not move). On its own it
  // means nothing, so it is refused rather than silently ignored — an ignored
  // `replace` would keep every master value and answer rc 0.
  if (replaceOpt && !updateOpt) {
    return engineErrorBlock(
        "merge: `replace` modifies `update` — it decides what happens on a matched row where "
        "master and using DISAGREE. Without `update` there is nothing for it to modify; write "
        "`, update replace`.",
        "bad_input");
  }
  // Stata: `generate()` RENAMES the marker; `nogenerate` suppresses it. Asking
  // for both is contradictory (oracle milfpins_oracle_h.log — with
  // `generate(mm)` the name `_merge` is r(111), i.e. generate() does not add a
  // second column, it moves the one).
  if (generateOpt.has_value() && nogenerate) {
    return engineErrorBlock(
        "merge: `generate()` and `nogenerate` are alternatives — one names the marker variable, "
        "the other suppresses it.",
        "bad_input");
  }
  if (generateOpt.has_value() &&
      (generateOpt->empty() || !strutil::isValidStataIdentifier(*generateOpt))) {
    return engineErrorBlock(
        "merge: generate() requires a variable name — got `" + generateOpt.value_or("") + "'",
        "syntax_error");
  }

  // BUG-105 (옵션망): merge accepts only keepusing()/nogenerate/keep()/
  // assert() — any other residual option token (e.g. Stata's `force`) is a
  // named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownMergeOpts = kVocabMerge.known;
  // BUG-105 secondary ⑤: real Stata `merge` also has generate()/nolabel/
  // nonotes/update/replace/noreport/force/sorted ([D] merge, d.pdf p.704) —
  // none implemented by this handler, but they are documented Stata syntax,
  // not typos, so they get the honest "not yet implemented" wording instead
  // of the plain "not allowed" reserved for a genuine unrecognized token.
  const std::unordered_set<std::string>& kMergeKnownButUnimplemented = kVocabMerge.unimplemented;
  if (auto err = eval::rejectUnknownOptions(optsC, kKnownMergeOpts, kMergeKnownButUnimplemented)) {
    return *err;
  }

  // BUG-548: keepusing() is a varlist parsed against the USING names (wildcards,
  // ranges, unique abbreviations) -- it used to be matched name-for-name, so
  // `keepusing(mother*)` brought zero columns and `keepusing(zzz)` was rc 0. The
  // using names are already in hand (usingDs, opened for the key pre-checks).
  std::vector<std::string> usingNonKeyKept;
  std::string keepusingCsv;
  if (keepusingOpt.has_value()) {
    std::vector<std::string> expanded;
    if (auto err = expandUsingKeepList(*keepusingOpt, usingDs.names(), expanded)) return *err;
    for (size_t i = 0; i < expanded.size(); ++i) {
      if (i) keepusingCsv += ",";
      keepusingCsv += expanded[i];
    }
    std::set<std::string> full(keylist.begin(), keylist.end());
    full.insert(expanded.begin(), expanded.end());
    for (const auto& n : usingDs.names()) {
      if (full.count(n)) usingNonKeyKept.push_back(n);
    }
  } else {
    usingNonKeyKept = usingDs.names();
  }

  // column-conflict check (non-key columns present in BOTH datasets).
  {
    std::set<std::string> keySet(keylist.begin(), keylist.end());
    std::vector<std::string> masterNonKeys;
    for (const auto& n : master->names()) {
      if (!keySet.count(n)) masterNonKeys.push_back(n);
    }
    std::set<std::string> masterNonKeySet(masterNonKeys.begin(), masterNonKeys.end());
    std::vector<std::string> overlap;
    for (const auto& n : usingNonKeyKept) {
      if (keySet.count(n)) continue;
      if (masterNonKeySet.count(n)) overlap.push_back(n);
    }
    // Stata keeps the MASTER's copy of a shared non-key variable and does not
    // bring the using's across (its documented default without update/replace)
    // — oracle-pinned on the P1 wave fixture, where `merge m:1 id using
    // wave_lookup, nogen` shares cluster/stratum/group and succeeds. Drop the
    // shared names from the "new columns" report instead of rejecting.
    if (!overlap.empty()) {
      std::set<std::string> shared(overlap.begin(), overlap.end());
      std::vector<std::string> kept;
      kept.reserve(usingNonKeyKept.size());
      for (const auto& n : usingNonKeyKept)
        if (!shared.count(n)) kept.push_back(n);
      usingNonKeyKept = std::move(kept);
    }
  }

  // cardinality enforcement.
  if (mergeType == "1:1" || mergeType == "1:m") {
    DupKeyResult dup = findDupKey(*master, keylist);
    if (dup.found) {
      std::string keylistRepr = "[";
      for (size_t i = 0; i < keylist.size(); ++i) {
        if (i) keylistRepr += ", ";
        keylistRepr += "'" + keylist[i] + "'";
      }
      keylistRepr += "]";
      std::string keyStr;
      for (size_t i = 0; i < keylist.size(); ++i) {
        if (i) keyStr += ", ";
        keyStr += keylist[i] + "=" + dup.keyValueStrs[i];
      }
      // Honesty rule for these hints: only name a remedy THIS build actually
      // accepts. `merge m:m` exists in Stata but dispatcher.cpp's reMergeType
      // matches `1:1|m:1|1:m` only, so a user who follows an "Use m:m" hint
      // gets `merge: syntax error` — a dead end. Where no type change helps,
      // say m:m is unsupported rather than staying silent about it.
      std::string remedy =
          (mergeType == "1:1")
              ? "Use merge m:1, or drop/collapse the duplicate master rows first."
              : "This build does not support merge m:m, so drop/collapse the duplicate "
                "master rows first.";
      return engineErrorBlock("merge " + mergeType + ": master dataset is not unique on key(s) " +
                              keylistRepr + " — duplicate found: {" + keyStr + "}. " + remedy);
    }
  }
  if (mergeType == "1:1" || mergeType == "m:1") {
    DupKeyResult dup = findDupKey(usingDs, keylist);
    if (dup.found) {
      std::string keylistRepr = "[";
      for (size_t i = 0; i < keylist.size(); ++i) {
        if (i) keylistRepr += ", ";
        keylistRepr += "'" + keylist[i] + "'";
      }
      keylistRepr += "]";
      std::string keyStr;
      for (size_t i = 0; i < keylist.size(); ++i) {
        if (i) keyStr += ", ";
        keyStr += keylist[i] + "=" + dup.keyValueStrs[i];
      }
      // Same honesty rule. `1:m` IS dispatcher-accepted, but it is only the
      // right advice when the master is the unique side (i.e. from `1:1`);
      // suggesting it for an `m:1` failure flips the master's cardinality too
      // and lands the user on a second, different error.
      std::string remedy =
          (mergeType == "1:1")
              ? "Verify the using file, or use merge 1:m."
              : "This build does not support merge m:m, so verify the using file and "
                "drop/collapse the duplicate using rows first.";
      return engineErrorBlock("merge " + mergeType + ": using dataset is not unique on key(s) " +
                              keylistRepr + " — duplicate found: {" + keyStr + "}. " + remedy);
    }
  }

  // Delegate the actual join to cobra's native merge (factor-key label
  // join, outer join, keep()/assert()/keepusing()/nogenerate).
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  std::string outPath = nextPath(session.workdir(), "merge");
  std::vector<std::string> args = {"merge",       "--data", master->path(), "--using",
                                    usingHbinPath, "--type", mergeType,      "--key",
                                    joinedKeys};
  if (keepusingOpt.has_value()) {
    args.push_back("--keepusing");
    args.push_back(keepusingCsv);
  }
  if (nogenerate) args.push_back("--nogenerate");
  // B4-W3b. `--generate NAME` / `--update` / `--update --replace` are the
  // engine's own spellings (cobra merge --help, main 3a5d80e2). The marker
  // codes the update pair introduces are 4 (matched, master missing, filled
  // from using) and 5 (matched, both non-missing, disagreeing) on top of the
  // usual 1/2/3, and the engine assigns them PER ROW over all overlapping
  // variables — not per variable — which is why nothing here tries to compute
  // them column-wise.
  if (generateOpt.has_value()) {
    args.push_back("--generate");
    args.push_back(*generateOpt);
  }
  if (updateOpt) {
    args.push_back("--update");
    if (replaceOpt) args.push_back("--replace");
  }
  if (keepOpt.has_value()) {
    args.push_back("--keep");
    args.push_back(*keepOpt);
  }
  if (assertOpt.has_value()) {
    args.push_back("--assert");
    args.push_back(*assertOpt);
  }
  args.push_back("--out");
  args.push_back(outPath);

  CobraRunResult cr;
  {
    // GA sweep 2026-08-22, leg ②/④. merge was the second-largest engine coordinate
    // (122-199 s, 60% of it in the output write, 28.04 GB of scratch at 175 MB per
    // call) AND the reason joinby's own fix only relocated its bytes: a stateless
    // merge demands a real file for --data, which forces the head materialization
    // that `frame:snapshot` was then charged for. Routing it resident closes that.
    //
    // The blocker was real and is now removed rather than waived. The router's own
    // comment excluded merge because "its exact stderr status counts are thus
    // preserved" — PIET parsed master_only/using_only/matched out of the engine's
    // stderr notice, which the engine prints ONLY on the one-shot leg, so residency
    // would have silently dropped a number the user sees. The counts now travel on
    // the frame response (contract_A additive key, present only when a merge staged),
    // so the same three numbers survive the plane change.
    SessionState::ResidentEligible residentMerge(session);
    cr = runCobra(args);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  session.load(outPath);
  // xtset-file-persistence: the result keeps the MASTER's characteristics; the
  // using side's ts declaration fills in only when the master states none.
  // Oracle: w2_eststo_prefix.log:541 (B3, master un-xtset, using xtset -> xtreg
  // rc 0) and :582 (B3 CONTROL, master xtset, using not -> rc 0). Scoped to the
  // ts entries — no oracle here pins how merge folds the using side's other chars.
  const bool masterUndeclared = !(session.panelvar.has_value() && !session.panelvar->empty()) &&
                                !(session.timevar.has_value() && !session.timevar->empty());
  // BUG-659: a native checkpoint (`.hbin`/`.tmp`) this session saved states its
  // declaration beside its path rather than in a char table — same fill rule.
  const SessionState::CheckpointTsDecl* usingCk =
      (masterUndeclared && usingMetaPath.empty()) ? session.checkpointTsDecl(usingPath) : nullptr;
  if (usingCk != nullptr &&
      ((usingCk->panelvar.has_value() && !usingCk->panelvar->empty()) ||
       (usingCk->timevar.has_value() && !usingCk->timevar->empty()))) {
    session.panelvar = usingCk->panelvar;
    session.timevar = usingCk->timevar;
    session.delta = usingCk->delta;
    session.writeTsChars();
  }
  if (!usingMetaPath.empty() && masterUndeclared) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
        bool adopted = false;
        for (const auto& ch : meta["characteristics"]) {
          if (!ch.is_object()) continue;
          const std::string vn = ch.value("varname", std::string());
          const std::string cn = ch.value("charname", std::string());
          if (vn != "_dta" || !SessionState::isTsCharName(cn) ||
              !ch.contains("value") || !ch["value"].is_string())
            continue;
          if (session.dtaChar(cn) != nullptr) continue;
          session.characteristics.push_back(
              DtaCharacteristic{vn, cn, ch["value"].get<std::string>()});
          adopted = true;
        }
        if (adopted) session.adoptTsFromChars();
      }
    } catch (const std::exception&) {
      // An unreadable sidecar leaves the master's (empty) declaration as it is —
      // the merge itself already succeeded on the engine's own read of the file.
    }
  }
  ojson result = mutationResult(session, "merge");
  if (!nogenerate) {
    // Resident leg: the engine published the counts on the frame response.
    if (const CobraFrameMeta* mm = session.lastResidentMergeCounts()) {
      ojson counts;
      counts["master_only"] = mm->mergeMasterOnly;
      counts["using_only"] = mm->mergeUsingOnly;
      counts["matched"] = mm->mergeMatched;
      result["merge_counts"] = counts;
    } else {
      // Stateless leg (serve disabled, §25-only engine, or a routing fallback): the
      // counts are in the one-shot stderr notice, exactly as before.
      static const std::regex reCounts(
          R"(master_only=(\d+)\s+using_only=(\d+)\s+matched=(\d+))");
      std::smatch m;
      if (std::regex_search(cr.stderr_text, m, reCounts)) {
        ojson counts;
        counts["master_only"] = std::stoll(m.str(1));
        counts["using_only"] = std::stoll(m.str(2));
        counts["matched"] = std::stoll(m.str(3));
        result["merge_counts"] = counts;
      }
    }
  }
  return result;
}

ojson runDuplicates(const std::string& sub, const std::optional<std::vector<std::string>>& varlist,
                    const std::optional<std::string>& genvar, SessionState& session,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange) {
  std::string subLower = lower(sub);
  if (subLower == "report") return runDuplicatesReport(varlist, session, ifExpr, inRange);
  if (subLower == "drop") return runDuplicatesDrop(varlist, session, ifExpr, inRange);
  if (subLower == "tag") {
    if (!genvar.has_value() || genvar->empty()) {
      return engineErrorBlock("duplicates tag: ,gen(NEWVAR) required");
    }
    return runDuplicatesTag(varlist, *genvar, session, ifExpr, inRange);
  }
  if (subLower == "list") return runDuplicatesList(varlist, session, ifExpr, inRange);
  return engineErrorBlock("duplicates " + sub +
                          ": unknown subcommand. Use: duplicates report|drop|tag|list [VARLIST]");
}

// ---------------------------------------------------------------------------
// encode STRVAR, gen(NEWVAR) [label(LBLNAME)] — C++ port of encode_op.py's
// run_encode. Folded into M4a (ROADMAP M4a, blocking the debug_all.do depth
// boundary at line 157) even though it predates this campaign's explicit
// item list.
//
// L1/M3 (CSV-shim retirement): this calls cobra's native `encode` verb
// (`--var SRC --gen NEW`), which appends NEW as an f64 column (codes in
// alphabetical level order, 1-based, missing preserved) and byte-preserves
// the source and every other column — no whole-frame CSV dump, no
// re-factorization. NEW lands f64, not factor, exactly the oracle's contract
// (encode's output is a plain numeric column with a value-label ATTACHMENT).
// (History: this used to dump the whole dataset to CSV with the extra
// alphabetically-ranked column and `cobra convert` it back; superseded.)
ojson runEncode(const std::string& strvar, const std::string& newvar,
                const std::optional<std::string>& lblname, SessionState& session,
                const std::optional<std::string>& ifExpr) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: encode's entry validation reads a NAME, a
  // DTYPE and another NAME — no column data — so the resident shadow card
  // answers all three, and the datastore open (cecon::Dataset::open: mmap +
  // 192-column header parse) is skipped. Same mechanism as runGsort/runOrder
  // above; the dtype half is the S4a treatment (strmutate.cpp's factorColumns),
  // where the A12 argument for trusting the card's dtype is written out in
  // full: the card's vocabulary is exactly Datastore::dtype()'s ("factor" |
  // "f64"), it is the BASE type on both sides, and any other spelling is
  // refused rather than guessed. Refusal here means falling into the else
  // branch, which is the untouched prior behavior.
  //
  // SCOPE, STATED HONESTLY. This is NOT where encode's measured ~0.24 s/call
  // goes. That is dominated by mutationResult's card rebuild rescanning all 192
  // columns for n_missing, because "encode" is missing from
  // SessionState::seedNMissForPureAdd's kPureAdd set (session_state.cpp) even
  // though encode is a textbook pure add — a one-token fix in a file this stint
  // does not touch. Removing the wasted open here does not substitute for it.
  //
  // The three facts the checks below need, resolved from the card when it can
  // supply them and from the datastore otherwise. Resolution is separated from
  // checking on purpose: the three rejections must keep firing in their
  // ORIGINAL order (not-found, then not-a-string, then already-defined), since
  // an input that trips two of them must keep returning the same message it
  // always did. Collapsing the lookups into the checks is what would silently
  // reorder them.
  bool haveFacts = false;
  bool srcExists = false;
  std::string srcDtype;
  bool newExists = false;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    const VarInfo* src = cardFind(*card, strvar);
    // An unrecognized dtype spelling is refused rather than guessed (S4a's
    // rule) — haveFacts stays false and the datastore branch re-answers all
    // three, exactly as before this fix.
    if (src == nullptr || src->dtype == "factor" || src->dtype == "f64") {
      srcExists = (src != nullptr);
      if (src != nullptr) srcDtype = src->dtype;
      newExists = (cardFind(*card, newvar) != nullptr);
      haveFacts = true;
    }
  }
  if (!haveFacts) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    srcExists = ds->has(strvar);
    if (srcExists) srcDtype = ds->dtype(strvar);
    newExists = ds->has(newvar);
  }

  if (!srcExists) {
    return engineErrorBlock("encode: variable '" + strvar + "' not found.");
  }
  if (srcDtype != "factor") {
    return engineErrorBlock("encode: '" + strvar + "' is not a string variable (dtype=" +
                            srcDtype +
                            "). encode requires a string (str) variable.");
  }
  if (newExists) {
    return engineErrorBlock("encode: variable '" + newvar + "' already defined.");
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // L1/M3 (CSV-shim retirement): cobra's native `encode` produces NEWVAR as
  // f64 with codes in ALPHABETICAL level order (1-based; missing stays
  // missing), byte-preserving the source column and every other column (no
  // whole-frame CSV dump, no re-factorization). Its stdout JSON carries the
  // alphabetical "levels" array, which drives the value-label attachment
  // below — codes and labels thus come from the SAME engine sort and always
  // agree. `--gen` lands f64, not factor: exactly encode's contract.
  //
  // BUG-042 (`encode STRVAR if COND, gen(NEWVAR)`): Stata restricts which
  // observations get a real code — excluded rows come back MISSING on NEWVAR,
  // while the level MAPPING still spans the WHOLE variable (encode's
  // value-label set is dataset-wide, not tied to one invocation's sample). So
  // encode runs on the full data (all rows -> whole-variable levels) into a
  // temp column, then a cobra `cond()` mask nulls the out-of-sample rows —
  // the identical `if`-folding runClonevar's numeric fast path uses.
  std::string encGen = haveIf ? ("__encode_full_" + scratchToken()) : newvar;
  std::string encOut = nextPath(session.workdir(), "encode");
  // *session.currentHbin() rather than ds->path() — same substitution runGsort
  // and runRename make, and the reason `ds` no longer needs to exist here.
  std::vector<std::string> encArgs = {"encode", "--data", *session.currentHbin(), "--out",
                                      encOut,   "--var",  strvar,                 "--gen", encGen};
  // C4-L4: cobra's cmd_encode rides the same §26 acquire_data/commit_frame
  // bridge as setcol (apps/cmds/cmds_data.cpp:693/:698 — read cross-lane, no
  // cobra change), so the SAME resident opt-in routes it through the frame
  // plane: virtual head, vars_delta fold, no whole-Datastore reconstruct —
  // and on a dirty virtual head no EMH materialization at the stateless
  // --data site (the a2 encode window class). The `if` mask's eval below
  // rides the same scope. Escapes as at the destring site; the stateless
  // fallback (plane down ⇒ no virtual head) is byte-identical to before.
  const bool residentEncode = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::optional<SessionState::ResidentEligible> encodeResident;
  if (residentEncode) encodeResident.emplace(session);
  nlohmann::json encJson;
  try {
    encJson = runCobraJson(encArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // Alphabetical levels straight from the engine's own encode output, so the
  // value labels below describe exactly the codes it assigned.
  std::vector<std::string> sortedLevels;
  if (encJson.contains("levels") && encJson.at("levels").is_array()) {
    for (const auto& lv : encJson.at("levels")) sortedLevels.push_back(lv.get<std::string>());
  }

  if (haveIf) {
    // Mask out-of-sample rows to missing (in-sample rows keep encGen, itself
    // already missing where the source was missing), then drop the temp — the
    // same one-shot `--gen COND-cond() --drop TMP` shape collapse's proxy
    // step used.
    std::string finalOut = nextPath(session.workdir(), "encode");
    // BUG-369 follow-up: the mask cond() embeds the user's if, which can carry
    // a TS operator (`encode s if L.x>0, gen(v)`).
    std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + encGen + ",.)";
    std::vector<std::string> maskArgs = translate::injectTsContextForExpr(
        {"eval", "--data", encOut, "--gen", maskExpr, "--drop", encGen, "--out", finalOut},
        maskExpr, session);
    CobraRunResult cr = runCobra(maskArgs);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(finalOut);
  } else {
    session.load(encOut);
  }

  // Register + attach the value-label set (label_define.py's
  // apply_label_define + apply_label_values, reproduced inline: name =
  // lblname if given else strvar; mapping = {1: levels[0], 2: levels[1], ...}
  // with levels alphabetical, from the engine's encode output above).
  std::string lbl = (lblname.has_value() && !lblname->empty()) ? *lblname : strvar;
  std::map<std::string, std::string> codeMap;   // string-keyed (labelDefs)
  std::map<int, std::string> numericMap;         // int-keyed (valueLabels)
  for (size_t i = 0; i < sortedLevels.size(); ++i) {
    int code = static_cast<int>(i) + 1;
    codeMap[std::to_string(code)] = sortedLevels[i];
    numericMap[code] = sortedLevels[i];
  }
  session.labelDefs[lbl] = codeMap;
  session.varLblname[newvar] = lbl;
  session.valueLabels[newvar] = numericMap;

  return mutationResult(session, "encode", {newvar});
}

// ---------------------------------------------------------------------------
// destring VAR, {replace | gen(NEWVAR)} [ignore("chars")] [force] — BS-003/D4
// (BETA B-data). PIET-NATIVE string(factor)->numeric conversion (no cobra
// verb, no hbin-v2 dependency — D4). See handlers.h's doc comment for the
// design summary. Already-numeric VAR is a Stata-faithful benign no-op
// (replace) / plain copy (gen), mirroring runClonevar's own numeric fast
// path (`cobra eval --gen NEW=OLD`) immediately below for that sub-case; the
// actual string->numeric conversion parses each level in C++, then writes the
// whole frame out via a CSV + `cobra convert` round-trip — there is no native
// `cobra destring` verb yet (an L1/M3 follow-up candidate), and piet/native
// has no direct hbin-write API of its own.
ojson runDestring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ignoreChars, bool force,
                  SessionState& session) {
  if (!session.hasData()) return eval::noDataLoaded();
  // C4-L4 (C2(a) reopen): validation answers off the CURRENT card when it is
  // available (the D1 rule) and the datastore is opened LAZILY — the eager
  // open that used to sit here was, on a dirty virtual head, the EMH
  // whole-frame snapshot the C2 §9.2 chain blocker named (a resident setcol
  // followed by the NEXT setcol's input read re-materialized the deferred
  // write). ensureDs() is the honest flush: any read the card or the
  // computed-column cache cannot answer pays it, exactly as before.
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card) {
        if (vi.name == name) return vi.dtype;
      }
      return std::nullopt;  // a current card is authoritative for absence too
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  std::optional<std::string> srcDtype = lookupDtype(varname);
  if (!srcDtype.has_value()) {
    return engineErrorBlock("destring: variable '" + varname + "' not found");
  }
  if (doReplace == genVar.has_value()) {
    // both given or neither — Stata requires exactly one of replace/gen().
    return engineErrorBlock("destring: specify exactly one of `replace` or `gen(newvar)`");
  }
  std::string target = doReplace ? varname : *genVar;
  if (!doReplace) {
    if (lookupDtype(target).has_value()) {
      return engineErrorBlock("destring: variable '" + target + "' already defined");
    }
    if (!strutil::isValidStataIdentifier(target)) {
      return engineErrorBlock("destring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }
  // C4-L4: the setcol residency switch, hoisted above both arms. The C4
  // family escape (PIET_NO_RESIDENT_READ) disables it alongside the C2-
  // specific one so the floor RED arm reproduces the whole pre-C4 stateless
  // path with ONE env, matching every other C4 leg's escape contract.
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();

  // Already-numeric source: Stata-faithful benign no-op/copy (real destring
  // answers "already numeric; no replace" as a note, not an error).
  if (*srcDtype == "f64") {
    if (doReplace) return mutationResult(session, "destring");  // nothing to do
    std::string outPath = nextPath(session.workdir(), "destring");
    // Resident path: --data rides the current head token (the runDrop
    // convention; the frame-run channel strips it). Escape/stateless path:
    // pre-C4 verbatim — materialize via ensureDs() and pass the real file.
    std::string dataPath;
    if (residentSetcol) {
      dataPath = *session.currentHbin();
    } else {
      Datastore* d = ensureDs();
      if (d == nullptr) return eval::noDataLoaded();
      dataPath = d->path();
    }
    std::vector<std::string> args = {"eval", "--data", dataPath, "--gen",
                                      target + "=" + varname, "--out", outPath};
    std::optional<SessionState::ResidentEligible> copyResident;
    if (residentSetcol) copyResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    return mutationResult(session, "destring", {target});
  }

  // String(factor) source: parse each level string -> numeric.
  // C4-L4: the computed-column cache first — when the source IS the column
  // the previous resident setcol computed (the destring.json chain shape,
  // tostring→destring), its row strings are already in this process's RAM
  // and the datastore (EMH on a dirty head) is never opened. The cached
  // strings equal levels[codes[r]] by construction (the engine's CSV
  // factorization round-trips them losslessly). Cache miss → ensureDs().
  const SessionState::ResidentComputedCol* srcCc = session.residentComputedCol(varname);
  if (srcCc != nullptr && srcCc->dtype != "factor") srcCc = nullptr;
  int64_t n = 0;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* srcLevelsPtr = nullptr;
  if (srcCc != nullptr) {
    n = srcCc->nRows;
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    codes = d->col(varname).codes();
    srcLevelsPtr = &d->col(varname).levels;
  }
  std::vector<double> outVals(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    std::string raw;
    if (srcCc != nullptr) {
      raw = srcCc->strs[static_cast<size_t>(r)];
    } else {
      int32_t code = codes[r];
      raw = (code >= 0 && static_cast<size_t>(code) < srcLevelsPtr->size())
                ? (*srcLevelsPtr)[static_cast<size_t>(code)]
                : "";
    }
    std::string stripped;
    stripped.reserve(raw.size());
    for (char c : raw) {
      if (ignoreChars.has_value() && ignoreChars->find(c) != std::string::npos) continue;
      stripped += c;
    }
    stripped = trim(stripped);
    if (stripped.empty()) {
      outVals[static_cast<size_t>(r)] = std::nan("");
      continue;
    }
    auto parsedVal = eval::pythonFloat(stripped);
    if (!parsedVal.has_value()) {
      if (force) {
        outVals[static_cast<size_t>(r)] = std::nan("");
        continue;
      }
      return engineErrorBlock(
          "destring: " + varname + " contains nonnumeric characters at observation " +
              std::to_string(r + 1) + " (value '" + raw +
              "'); use force to convert unparseable values to missing, or ignore(\"chars\") to "
              "strip characters before parsing",
          "invalid_input");
    }
    outVals[static_cast<size_t>(r)] = *parsedVal;
  }

  // Hand the engine's single-column `cobra setcol` verb just the converted f64
  // column instead of re-serializing the WHOLE frame to CSV + `cobra convert`
  // (which re-factorizes every untouched factor column). The parse semantics
  // above are unchanged; every other column/dictionary byte is preserved.
  // `target` present (replace) => in-place factor->f64; `target` new (gen) =>
  // --new appends a fresh f64 column. Missing rows render as an empty payload
  // line (setcol --dtype f64 reads empty as missing).
  bool targetPresent =
      lookupDtype(target).has_value();  // true only for `replace` (target==varname)
  std::string valuesPath = nextPath(session.workdir(), "destring") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("destring: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) {
      double v = outVals[static_cast<size_t>(r)];
      if (!std::isnan(v)) csv << eval::pythonFloatRepr(v);  // missing -> empty line
      csv << "\n";
    }
    // BUG-124 sibling: flush + check — silent short payload = silent row loss.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("destring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h). setcol's own
  // semantic payload is metadata, so it leaves the ratio metric and is asserted
  // as an ABSOLUTE byte budget instead (A1 1.3). What that budget catches is
  // exactly this: the scratch CSV is a full rows-scale re-serialisation handed
  // to a subprocess for what is nominally a one-column edit. Only the bytes
  // THIS process wrote are billed here -- the `--out` hbin is read back through
  // Datastore, which bills it once at the COLUMNAR site (no double count).
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }
  std::string outPath = nextPath(session.workdir(), "destring");
  // C2-SETCOL-RESIDENT: route this single-column setcol through the resident
  // frame plane (as gen/replace/egen do) so the session.load(outPath) below
  // takes a VIRTUAL head instead of reconstructing the whole Datastore — the
  // ~263 ms mmap + whole-file FNV materialization tax + card rebuild that the
  // datastore_constructs floor counts. For an in-place factor->f64 change the
  // engine reports card_stable=true with a vars_delta carrying the new
  // type/n_missing and no n_levels (de-risk brief §8.2); mutationResult ->
  // tryResidentMutationCard's vars_delta fold applies it, CLEARING nLevels
  // (CobraFrameVar::nLevels is nullopt exactly when the engine omits n_levels).
  // On the stateless fallback (old binary / serve off) a real --out is still
  // written and session.load reads it, as before. Escapes: PIET_NO_RESIDENT_-
  // SETCOL=1 (C2-specific) or PIET_NO_RESIDENT_READ=1 (C4 family) force the
  // historical stateless one-shot — residentSetcol was resolved at entry.
  std::string dataPath;
  if (residentSetcol) {
    dataPath = *session.currentHbin();  // frame-run channel strips --data
  } else {
    Datastore* d = ensureDs();  // pre-C4 verbatim: materialize + real file
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }
  std::vector<std::string> setArgs = {"setcol",   "--data",    dataPath, "--var", target,
                                      "--values", valuesPath,  "--dtype", "f64",  "--out",
                                      outPath};
  if (!targetPresent) setArgs.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // A resident (virtual) head has no on-disk file; session.load records the
  // virtual chain (the gsort/sort convention, which likewise never stats --out).
  // Keep the torn-write guard only on the forced-stateless path (real --out).
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("destring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // C1-CARD-REBUILD-REDUCE A11 sibling: `replace` (targetPresent) rewrites
  // `target` IN PLACE via single-column `cobra setcol` above — every other
  // column byte-identical — so seed via rewrittenVars={target} (branch 2:
  // carry all but the rewritten target). `gen` appends `target` as a new
  // column, so pass it in newVars (branch 1). Both proven byte-identical vs a
  // forced full scan (PIET_NO_NMISS_CACHE=1).
  ojson destringResult =
      targetPresent ? mutationResult(session, "destring", /*newVars=*/{}, /*droppedVars=*/{},
                                     /*rewrittenVars=*/{target})
                    : mutationResult(session, "destring", {target});
  // C4-L4: memoize the values THIS process just computed and committed, so a
  // chained consumer (the next setcol reading `target`) never re-materializes
  // the virtual head to learn what PIET already knew. Put AFTER
  // mutationResult — the fold's own vars_delta pass erases the name first.
  if (residentSetcol && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol put;
    put.dtype = "f64";
    put.nRows = n;
    put.f64 = std::move(outVals);
    session.residentComputedColPut(target, std::move(put));
  }
  return destringResult;
}

// ---------------------------------------------------------------------------
// tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat] — BS-003/D4 (BETA
// B-data). PIET-NATIVE numeric->string(factor) conversion, the mirror of
// runDestring above.

namespace {
// Default (non-usedisplayformat) numeric->string rendering — mirrors
// strmutate.cpp's file-local coerceStr() convention (integer -> plain
// digits via eval::pythonIntStrFromDouble, non-integer -> eval::gFormat);
// kept as its own copy per this codebase's established per-TU convention
// (see errorFromRaw's own header note above) rather than exporting
// strmutate.cpp's file-local helper. Missing is handled by the caller
// (tostringMissingRender) before this is reached.
std::string tostringDefaultRender(double v) {
  if (eval::pyIsInteger(v) && std::fabs(v) < 1e15) {
    return eval::pythonIntStrFromDouble(v);
  }
  return eval::gFormat(v);
}

// BUG-207: what `tostring` writes for a MISSING numeric.
//
// This used to be `""` on both branches, with an in-tree comment asserting that
// was Stata-faithful. It is not, and the assertion is exactly how the defect
// survived: `fmt_g_oracle.log`'s section C only ever spoke to the
// `usedisplayformat` branch, and one branch's evidence was generalised to the
// other. Measured on BOTH branches, StataNow 19.5, marker-judged
// (`tests/kitchensink/fmt_oracle/tostring_missing_oracle.log`, whole-line marker
// `TOSTRING-MISSING-CAPTURE-COMPLETE`), with `strlen` reported per cell so `""`
// and `"."` cannot be confused by eye:
//
//     branch                      42        .           .a
//     plain tostring              [42] 2    [.] 1       [.a] 2
//     , usedisplayformat          [42] 2    [.] 1       [.a] 2
//     , usedisplayformat %12.4g   [42] 2    [.] 1       [.a] 2
//
// So Stata writes the missing TOKEN, and it writes the extended CODE — a
// `tostring`-ed column keeps the 27-way distinction the numeric column had. The
// field width is NOT kept (`%12.4g` still yields `42`, not a padded field), which
// is why the width row is in the capture at all.
//
// The kind is available here: PIET carries the extended missings as NaN payload
// bits (`data/missing.h`, already included by this TU), and this engine really
// does preserve them through `replace v = .a` — live-probed this stint,
// `count if v == .a` -> 1 and `count if v == .` -> 1 on the same three-row
// column. So rendering only system missing as `.` and collapsing `.a`-`.z` onto
// it would be trading one wrong byte for a different wrong byte at a site being
// edited anyway, with the oracle for the right answer sitting in the same log.
// SCOPE NOTE: BUG-207 named the extended half as a separate claim. It is
// implemented here because it is three lines off a store this file already reads
// and an oracle row already captured — not because the entry authorised it.
//
// `missing_kind` returns -1 for anything finite or infinite, so a caller that
// reaches this for a non-missing gets the honest empty string rather than a
// bogus `.`; the one call site guards with isnan() first regardless.
std::string tostringMissingRender(double v) {
  const int kind = cecon::missing_kind(v);
  if (kind < cecon::kMissSys) return std::string();  // not missing at all
  if (kind == cecon::kMissSys) return ".";           // system missing -> `.`
  return std::string(".") + static_cast<char>('a' + (kind - cecon::kMissA));
}
}  // namespace

// BUG-371: `decode NUMVAR, generate(NEWVAR)` — the inverse of `encode`, and the
// asymmetry was the finding: a user who encoded a string could not get it back.
//
// What `encode` actually leaves behind (runEncode above) is an f64 column of
// 1-based codes PLUS a session-side value-label map — not a factor column. So
// decode is not a dtype relabel: it reads session.valueLabels[var], maps each
// row's code to its label, and writes a NEW factor column. A code with no label
// becomes missing, which is what Stata does.
ojson runDecode(const std::string& varname, const std::optional<std::string>& genVar,
                SessionState& session) {
  if (!session.hasData()) return eval::noDataLoaded();
  if (!genVar.has_value() || genVar->empty())
    return engineErrorBlock("decode: generate(newvar) is required", "syntax_error");
  const std::string target = trim(*genVar);
  if (!strutil::isValidStataIdentifier(target))
    return engineErrorBlock(
        "decode: invalid variable name `" + target + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(varname))
    return engineErrorBlock("variable " + varname + " not found", "unknown_variable");
  if (ds->has(target))
    return engineErrorBlock("decode: variable " + target + " already defined", "bad_input");
  if (ds->dtype(varname) != "f64")
    return engineErrorBlock(
        "decode: " + varname + " is already a string variable — decode maps a NUMERIC variable "
        "back through its value labels.",
        "bad_input");

  auto lblIt = session.valueLabels.find(varname);
  if (lblIt == session.valueLabels.end() || lblIt->second.empty())
    return engineErrorBlock(
        "decode: " + varname + " has no value labels, so there is nothing to decode it to "
        "(only a variable produced by `encode`, or one carrying a label set, can be decoded).",
        "bad_input");
  const std::map<int, std::string>& labels = lblIt->second;

  const int64_t n = ds->nRows();
  const double* src = ds->col(varname).f64();
  std::vector<std::string> out(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    const double v = src[r];
    if (std::isnan(v) || v != std::floor(v)) continue;  // stays "" == factor-missing
    auto it = labels.find(static_cast<int>(v));
    if (it != labels.end()) out[static_cast<size_t>(r)] = it->second;
    // An unlabeled code decodes to missing, matching Stata.
  }

  // Same one-column setcol shape runTostring uses — see its comments for the
  // resident-plane reasoning and the BUG-124 flush guard.
  std::string valuesPath = nextPath(session.workdir(), "decode") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("decode: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) csv << csvEscape(out[static_cast<size_t>(r)]) << "\n";
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("decode: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::string outPath = nextPath(session.workdir(), "decode");
  std::string dataPath = residentSetcol ? *session.currentHbin() : ds->path();
  std::vector<std::string> setArgs = {"setcol",   "--data",   dataPath, "--var",   target,
                                      "--values", valuesPath, "--dtype", "factor", "--new",
                                      "--out",    outPath};
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("decode: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  return mutationResult(session, "decode", {target});
}

// BUG-371: `inspect VARNAME` — Stata's quick "what is in this column" readout:
// how many values are negative / zero / positive, how many are integers vs not,
// how many are missing, and how many distinct values there are. It is pure
// counting over one mmap'd column, so it is PIET-native for the same reason
// `isid` is (shelling out would cost a scratch .hbin to answer an arithmetic
// question). Emitted as a `display` text block: the frontend has no inspect
// renderer, and a text block renders TODAY rather than degrading to a JSON echo.
namespace {
// Stata's %9.0g rendering: 7 significant digits, trailing zeros trimmed, and the
// leading zero dropped for |v| < 1 (Stata writes .4780935, never 0.4780935).
// Checked against eight values in the live StataBE capture
// (plans/stata-capture-20260831/conventions.log): .4780935, 1.674907, 5.263916,
// -.4077221, .424569, 3.912023, .29266, 4.78367 — all exact.
std::string stataG(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.7g", v);
  std::string out(buf);
  if (out.find('.') != std::string::npos && out.find('e') == std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  if (out.rfind("0.", 0) == 0) out = out.substr(1);
  else if (out.rfind("-0.", 0) == 0) out = "-" + out.substr(2);
  return out;
}

}  // namespace

ojson runInspect(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (varlist.size() != 1)
    return engineErrorBlock("inspect: one variable at a time in this build (`inspect varname`)",
                            "syntax_error");
  const std::string& name = varlist[0];
  if (!ds->has(name))
    return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock(
        "inspect: " + name + " is a string variable; inspect reports the numeric composition of "
        "a numeric variable.",
        "bad_input");

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
  int64_t neg = 0, zero = 0, pos = 0, ints = 0, nonints = 0, miss = 0;
  int64_t negint = 0, posint = 0;
  std::set<double> uniq;
  bool uniqCapped = false;
  for (int64_t i = 0; i < n; ++i) {
    const double v = x[i];
    if (std::isnan(v)) { ++miss; continue; }
    if (v < 0) ++neg; else if (v == 0) ++zero; else ++pos;
    if (v == std::floor(v)) {
      ++ints;
      // BUG-373: Stata splits the integer count by SIGN too (r(N_negint) /
      // r(N_posint)), so track them here rather than deriving them later.
      if (v < 0) ++negint; else if (v > 0) ++posint;
    } else {
      ++nonints;
    }
    // Same 65536 cap `codebook` uses for its unique count — past that the number
    // stops being informative and starts being a memory bill.
    if (!uniqCapped) {
      uniq.insert(v);
      if (uniq.size() > 65536) uniqCapped = true;
    }
  }
  const int64_t total = n - miss;

  // BUG-377: this used to print a single count column with PERCENTAGES. Real
  // Stata prints three COUNT columns — Total / Integers / Nonintegers — with "-"
  // for a zero cell and no percentages anywhere. Captured live
  // (plans/stata-capture-20260831/conventions.log, section Q3) on `inspect mpg`:
  //
  //   mpg:  Mileage (mpg)                             Number of observations
  //   -------------------                    ---------------------------------------
  //                                                Total      Integers   Nonintegers
  //   |      #                     Negative            -             -             -
  //   ...
  //     (21 unique values)
  //
  // The ASCII histogram in Stata's left panel is deliberately NOT reproduced: its
  // binning rule is not derivable from one capture, and a plausible-looking wrong
  // histogram is worse than an absent one. The numbers — which is what a reader
  // acts on — are exact.
  auto cell = [](int64_t k) { return k == 0 ? std::string("-") : std::to_string(k); };
  auto row3 = [&cell](const char* label, int64_t tot, int64_t ints_, int64_t non) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", label, cell(tot).c_str(),
                  cell(ints_).c_str(), cell(non).c_str());
    return std::string(buf);
  };
  // Split each sign bucket by integer-ness so the three columns add up.
  const int64_t negNon = neg - negint, posNon = pos - posint;
  const int64_t zeroInt = zero, zeroNon = 0;  // 0 is an integer
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s:%*sNumber of observations", name.c_str(),
                  static_cast<int>(name.size() < 34 ? 36 - name.size() : 2), "");
    lines.push_back(buf);
    lines.push_back("  ----------------------------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", "", "Total", "Integers",
                  "Nonintegers");
    lines.push_back(buf);
  }
  lines.push_back(row3("Negative", neg, negint, negNon));
  lines.push_back(row3("Zero", zero, zeroInt, zeroNon));
  lines.push_back(row3("Positive", pos, posint, posNon));
  lines.push_back("                           -----------   -----------   -----------");
  lines.push_back(row3("Total", total, ints, nonints));
  {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s", "Missing", cell(miss).c_str());
    lines.push_back(std::string(buf));
    lines.push_back("                           -----------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11lld", "", static_cast<long long>(n));
    lines.push_back(std::string(buf));
    std::snprintf(buf, sizeof(buf), "  (%s unique values)",
                  uniqCapped ? ">65536" : std::to_string(uniq.size()).c_str());
    lines.push_back(std::string(buf));
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // BUG-373: the counts ride alongside the rendered text so the dispatcher can
  // publish them as r(). Stata's inspect is r-class and this one published
  // nothing — measured by the declared-returns sweep the day AFTER it shipped.
  // r(N_unique)/r(N_undoc) are deliberately absent: the live capture shows Stata
  // returning MISSING for both on a 302-distinct-value column, so its rule is a
  // threshold this repo has not measured, and a guess is worse than an absence.
  ojson rcls = ojson::object();
  rcls["N"] = static_cast<double>(total);
  rcls["N_neg"] = static_cast<double>(neg);
  rcls["N_0"] = static_cast<double>(zero);
  rcls["N_pos"] = static_cast<double>(pos);
  rcls["N_negint"] = static_cast<double>(negint);
  rcls["N_posint"] = static_cast<double>(posint);
  out["inspect"] = rcls;
  return out;
}

// BUG-371: `xtsum` and `xttab` — the two panel summaries the idiom probe named.
//
// Both are PIET-native for the same reason `isid` and `inspect` are: they are
// arithmetic over mmap'd columns, and shelling out would cost a scratch .hbin to
// answer a question the data in hand already answers. Neither exists engine-side.
//
// The decompositions are the textbook ones, and they are what makes xtsum worth
// having at all — an overall SD tells a panel researcher nothing about whether
// the variation is across units or across time:
//   overall  x_it                      N = non-missing rows,   n/a
//   between  x̄_i                       n = units with any obs
//   within   x_it - x̄_i + x̄            same N as overall
// T-bar is N/n, the average number of observations per unit.
namespace {

struct PanelCols {
  const double* id = nullptr;
  const double* x = nullptr;
  int64_t n = 0;
};

// Sample SD (ddof=1), matching Stata's summarize. Fewer than two values -> missing.
double sampleSdDV(const std::vector<double>& v) {
  if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0;
  for (double q : v) sum += q;
  const double mean = sum / static_cast<double>(v.size());
  double ss = 0;
  for (double q : v) ss += (q - mean) * (q - mean);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

}  // namespace

ojson runXtsum(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xtsum.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  const std::string& idvar = *session.panelvar;
  if (!ds->has(idvar))
    return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(idvar) != "f64")
    return engineErrorBlock("xtsum: panel variable " + idvar + " must be numeric", "bad_input");
  if (varlist.empty())
    return engineErrorBlock("xtsum: variable list required", "syntax_error");
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    if (ds->dtype(v) != "f64")
      return engineErrorBlock("xtsum: " + v + " is a string variable", "bad_input");
  }

  const int64_t n = ds->nRows();
  const double* id = ds->col(idvar).f64();

  // BUG-377: Stata's real layout, captured live on nlswork
  // (plans/stata-capture-20260831/conventions.log, section Q4):
  //
  //   Variable         |      Mean   Std. dev.       Min        Max |    Observations
  //   -----------------+--------------------------------------------+----------------
  //   ln_wage  overall |  1.674907   .4780935          0   5.263916 |     N =   28534
  //            between |              .424569          0   3.912023 |     n =    4711
  //            within  |               .29266  -.4077221    4.78367 | T-bar = 6.05689
  //
  // The `|` rules and the N=/n=/T-bar= labels in the Observations column are the
  // shape; the previous rendering used a plain Obs column and no rules.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | %15s", "Variable", "Mean",
                  "Std. dev.", "Min", "Max", "Observations");
    lines.push_back(buf);
    lines.push_back("-----------------+--------------------------------------------+----------------");
  }
  ojson stats = ojson::object();

  for (const auto& name : varlist) {
    const double* x = ds->col(name).f64();
    // Unit means, in first-appearance order; a unit with no non-missing value of
    // THIS variable contributes nothing (Stata's n counts units that have one).
    std::map<double, std::pair<double, int64_t>> acc;  // id -> (sum, count)
    std::vector<double> all;
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      all.push_back(x[i]);
      auto& a = acc[id[i]];
      a.first += x[i];
      a.second += 1;
    }
    const int64_t N = static_cast<int64_t>(all.size());
    const int64_t units = static_cast<int64_t>(acc.size());
    const double NA = std::numeric_limits<double>::quiet_NaN();
    double gmean = NA, omin = NA, omax = NA;
    if (N > 0) {
      double sum = 0;
      omin = all[0];
      omax = all[0];
      for (double q : all) { sum += q; omin = std::min(omin, q); omax = std::max(omax, q); }
      gmean = sum / static_cast<double>(N);
    }
    std::vector<double> means;
    means.reserve(acc.size());
    for (const auto& kv : acc) means.push_back(kv.second.first / static_cast<double>(kv.second.second));
    double bmin = NA, bmax = NA;
    if (!means.empty()) {
      bmin = *std::min_element(means.begin(), means.end());
      bmax = *std::max_element(means.begin(), means.end());
    }
    // Within: x_it - x̄_i + x̄. Stata reports the min/max of THAT quantity, which
    // is why they can look odd next to the raw range — they are deviations
    // re-centred on the grand mean, not observations.
    std::vector<double> within;
    within.reserve(all.size());
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      const auto& a = acc[id[i]];
      within.push_back(x[i] - a.first / static_cast<double>(a.second) + gmean);
    }
    double wmin = NA, wmax = NA;
    if (!within.empty()) {
      wmin = *std::min_element(within.begin(), within.end());
      wmax = *std::max_element(within.begin(), within.end());
    }
    const double osd = sampleSdDV(all), bsd = sampleSdDV(means), wsd = sampleSdDV(within);
    const double tbar = units > 0 ? static_cast<double>(N) / static_cast<double>(units) : NA;

    auto num = [](double v) { return stataG(v); };
    char buf[240];
    // Stata puts the variable name and the "overall" tag in one 16-wide stub.
    std::string stub = name + "  overall";
    if (stub.size() > 16) stub = stub.substr(0, 16);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s |   N = %9lld", stub.c_str(),
                  num(gmean).c_str(), num(osd).c_str(), num(omin).c_str(), num(omax).c_str(),
                  static_cast<long long>(N));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s |   n = %9lld", "         between",
                  "", num(bsd).c_str(), num(bmin).c_str(), num(bmax).c_str(),
                  static_cast<long long>(units));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | T-bar = %7s", "         within ",
                  "", num(wsd).c_str(), num(wmin).c_str(), num(wmax).c_str(), num(tbar).c_str());
    lines.push_back(buf);

    ojson e;
    e["N"] = N;
    e["n_units"] = units;
    e["T_bar"] = tbar;
    e["mean"] = gmean;
    e["sd_overall"] = osd;
    e["min_overall"] = omin;
    e["max_overall"] = omax;
    e["sd_between"] = bsd;
    e["min_between"] = bmin;
    e["max_between"] = bmax;
    e["sd_within"] = wsd;
    e["min_within"] = wmin;
    e["max_within"] = wmax;
    stats[name] = e;
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // The numbers ride along beside the rendered text so a consumer (and the
  // selftest lock) can assert on values rather than on formatting.
  out["xtsum"] = stats;
  return out;
}

ojson runXttab(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xttab.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (varlist.size() != 1)
    return engineErrorBlock("xttab: one variable at a time (`xttab varname`)", "syntax_error");
  const std::string& name = varlist[0];
  const std::string& idvar = *session.panelvar;
  if (!ds->has(name)) return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (!ds->has(idvar)) return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock("xttab: " + name + " is a string variable", "bad_input");

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
  const double* id = ds->col(idvar).f64();

  std::map<double, int64_t> overall;                      // value -> rows
  std::map<double, std::set<double>> everUnits;           // value -> units taking it at least once
  std::map<double, std::map<double, int64_t>> perUnitHits;  // value -> unit -> rows with it
  std::map<double, int64_t> unitRows;                     // unit -> its non-missing rows
  std::set<double> allUnits;
  int64_t N = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(x[i]) || std::isnan(id[i])) continue;
    ++N;
    ++overall[x[i]];
    everUnits[x[i]].insert(id[i]);
    ++perUnitHits[x[i]][id[i]];
    ++unitRows[id[i]];
    allUnits.insert(id[i]);
  }
  const int64_t nUnits = static_cast<int64_t>(allUnits.size());

  // BUG-377: Stata's real layout, captured live on nlswork (section Q4):
  //
  //                     Overall             Between            Within
  //        race |    Freq.  Percent      Freq.  Percent        Percent
  //   ----------+-----------------------------------------------------
  //       White |   20180     70.72      3329     70.66         100.00
  //   ----------+-----------------------------------------------------
  //       Total |   28534    100.00      4711    100.00         100.00
  //                                 (n = 4711)
  //
  // Note the BETWEEN column is headed "Freq." like the overall one — it is a
  // count of UNITS, and the previous rendering called it "Units", which reads as
  // a different quantity.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%*s%-19s%-19s%s", 12, "", "Overall", "Between", "Within");
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%10s | %8s %8s  %9s %8s %14s", name.c_str(), "Freq.",
                  "Percent", "Freq.", "Percent", "Percent");
    lines.push_back(buf);
    lines.push_back("-----------+-----------------------------------------------------");
  }
  ojson rows = ojson::array();
  for (const auto& kv : overall) {
    const double val = kv.first;
    const int64_t freq = kv.second;
    const int64_t units = static_cast<int64_t>(everUnits[val].size());
    // Within% is the AVERAGE over the units that ever take this value of the
    // share of that unit's observations holding it — not freq/N restricted, which
    // is a different (and wrong) number when units have unequal T.
    double withinPct = std::numeric_limits<double>::quiet_NaN();
    if (units > 0) {
      double acc = 0;
      for (const auto& uk : perUnitHits[val])
        acc += static_cast<double>(uk.second) / static_cast<double>(unitRows[uk.first]);
      withinPct = 100.0 * acc / static_cast<double>(units);
    }
    const double ovPct = N > 0 ? 100.0 * static_cast<double>(freq) / static_cast<double>(N) : 0.0;
    const double btPct =
        nUnits > 0 ? 100.0 * static_cast<double>(units) / static_cast<double>(nUnits) : 0.0;
    char buf[220];
    char valbuf[32];
    std::snprintf(valbuf, sizeof(valbuf), "%g", val);
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f %14.2f", valbuf,
                  static_cast<long long>(freq), ovPct, static_cast<long long>(units), btPct,
                  withinPct);
    lines.push_back(buf);
    ojson r;
    r["value"] = val;
    r["freq"] = freq;
    r["percent"] = ovPct;
    r["units"] = units;
    r["units_percent"] = btPct;
    r["within_percent"] = withinPct;
    rows.push_back(r);
  }
  {
    char buf[220];
    lines.push_back("-----------+-----------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f", "Total",
                  static_cast<long long>(N), 100.0, static_cast<long long>(nUnits), 100.0);
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%*s(n = %lld)", 30, "", static_cast<long long>(nUnits));
    lines.push_back(buf);
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  out["xttab"] = rows;
  out["n_obs"] = N;
  out["n_units"] = nUnits;
  return out;
}

ojson runTostring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar, bool useDisplayFormat,
                  SessionState& session) {
  if (!session.hasData()) return eval::noDataLoaded();
  // C4-L4: same lazy-datastore + card-first validation as runDestring above
  // (see the destring site for the full rationale — this is its mirror).
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card) {
        if (vi.name == name) return vi.dtype;
      }
      return std::nullopt;  // a current card is authoritative for absence too
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  std::optional<std::string> srcDtype = lookupDtype(varname);
  if (!srcDtype.has_value()) {
    return engineErrorBlock("tostring: variable '" + varname + "' not found");
  }
  if (doReplace == genVar.has_value()) {
    return engineErrorBlock("tostring: specify exactly one of `replace` or `gen(newvar)`");
  }
  std::string target = doReplace ? varname : *genVar;
  if (!doReplace) {
    if (lookupDtype(target).has_value()) {
      return engineErrorBlock("tostring: variable '" + target + "' already defined");
    }
    if (!strutil::isValidStataIdentifier(target)) {
      return engineErrorBlock("tostring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }
  if (*srcDtype != "f64") {
    return engineErrorBlock("tostring: '" + varname + "' is already a string variable");
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();

  // Source values: the computed-column cache first (the destring→tostring
  // chain shape), else the datastore (honest flush on a dirty head).
  const SessionState::ResidentComputedCol* srcCc = session.residentComputedCol(varname);
  if (srcCc != nullptr && srcCc->dtype != "f64") srcCc = nullptr;
  int64_t n = 0;
  const double* src = nullptr;
  if (srcCc != nullptr) {
    n = srcCc->nRows;
    src = srcCc->f64.data();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    src = d->col(varname).f64();
  }

  // usedisplayformat (D6): render via the variable's OWN session display
  // format (BS-005's session.varMeta[...].format — "%9.0g" default when
  // unset, matching Datastore/VarInfo's own declared default). Reuses
  // piet::eval::applyStataFmt — note that helper only recognizes the
  // numeric %{f,g,e} families (falls back to its own generic-number
  // rendering for anything else, e.g. a date/time %fmt); a pre-existing
  // limitation of that shared helper, not newly introduced here.
  std::string fmt = "%9.0g";
  if (useDisplayFormat) {
    auto it = session.varMeta.find(varname);
    if (it != session.varMeta.end() && it->second.format.has_value() &&
        !it->second.format->empty()) {
      fmt = *it->second.format;
    }
  }

  std::vector<std::string> outStrings(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    double v = src[r];
    if (std::isnan(v)) {
      // BUG-207: `.` / `.a`-`.z`, NOT "" — see tostringMissingRender's oracle.
      // Deliberately BEFORE the branch fork, so plain and `usedisplayformat`
      // cannot diverge here: the capture says both write the same token, and one
      // shared site is what makes that structural rather than a coincidence.
      outStrings[static_cast<size_t>(r)] = tostringMissingRender(v);
      continue;
    }
    outStrings[static_cast<size_t>(r)] =
        useDisplayFormat ? eval::applyStataFmt(v, fmt) : tostringDefaultRender(v);
  }

  // Hand the engine's single-column `cobra setcol` verb just the rendered
  // string column instead of a whole-frame CSV + `cobra convert` round-trip
  // (which re-factorizes every untouched factor column). The rendering above
  // is unchanged; every other column/dictionary byte is preserved.
  //
  // BUG-207 changed what a missing row puts on this wire, so the transport note
  // here changed with it. It used to read: a numeric missing renders "" and
  // setcol maps an empty payload line to missing (code -1), so missing stays
  // missing. It now renders `.` / `.a`-`.z`, which setcol carries as an ORDINARY
  // FACTOR LEVEL — `Frame::read_csv_column` treats only an empty cell as missing
  // and says so in its own comment ("empty -> missing; '.' etc. stay literal
  // levels", src/data/frame.cpp), so no new transport support was needed.
  //
  // The resulting card change is the CORRECT one, not a side effect to absorb:
  // the product of `tostring` is a STRING variable, and a string variable's
  // missing is `""`, not `.`. So a Stata-faithful `tostring` of a column with
  // missings yields a string column with ZERO missing cells and a `.` level —
  // which is what n_missing/n_levels now report, and what Stata's own
  // `describe` reports for the same operation.
  bool targetPresent =
      lookupDtype(target).has_value();  // true only for `replace` (target==varname)
  std::string valuesPath = nextPath(session.workdir(), "tostring") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("tostring: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) {
      csv << csvEscape(outStrings[static_cast<size_t>(r)]) << "\n";
    }
    // BUG-124 sibling: flush + check — silent short payload = silent row loss.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("tostring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h). setcol's own
  // semantic payload is metadata, so it leaves the ratio metric and is asserted
  // as an ABSOLUTE byte budget instead (A1 1.3). What that budget catches is
  // exactly this: the scratch CSV is a full rows-scale re-serialisation handed
  // to a subprocess for what is nominally a one-column edit. Only the bytes
  // THIS process wrote are billed here -- the `--out` hbin is read back through
  // Datastore, which bills it once at the COLUMNAR site (no double count).
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }
  std::string outPath = nextPath(session.workdir(), "tostring");
  // C2-SETCOL-RESIDENT (mirror of runDestring): route this setcol through the
  // resident frame plane so session.load(outPath) takes a VIRTUAL head instead
  // of reconstructing the whole Datastore. For an in-place f64->factor change the
  // engine reports card_stable=true with a vars_delta carrying the new
  // type/n_missing AND n_levels (de-risk brief §8.2); the vars_delta fold applies
  // it, SETTING nLevels (the opposite direction of destring — the two together
  // are the A/B parity gate's symmetry check). Stateless fallback + escapes
  // behave exactly as documented at the destring site (residentSetcol resolved
  // at entry, C2-specific + C4-family escapes both honored).
  std::string dataPath;
  if (residentSetcol) {
    dataPath = *session.currentHbin();  // frame-run channel strips --data
  } else {
    Datastore* d = ensureDs();  // pre-C4 verbatim: materialize + real file
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }
  std::vector<std::string> setArgs = {"setcol",   "--data",    dataPath, "--var",  target,
                                      "--values", valuesPath,  "--dtype", "factor", "--out",
                                      outPath};
  if (!targetPresent) setArgs.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // Virtual (resident) head has no on-disk file; guard the torn-write stat to the
  // forced-stateless path only (see the destring site's note).
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("tostring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // C1-CARD-REBUILD-REDUCE A11 sibling: mirror of destring — `replace`
  // (targetPresent) rewrites `target` IN PLACE via single-column `cobra setcol`,
  // seed via rewrittenVars={target} (branch 2); `gen` appends `target`, pass it
  // in newVars (branch 1). Byte-identical vs PIET_NO_NMISS_CACHE=1.
  ojson tostringResult =
      targetPresent ? mutationResult(session, "tostring", /*newVars=*/{}, /*droppedVars=*/{},
                                     /*rewrittenVars=*/{target})
                    : mutationResult(session, "tostring", {target});
  // C4-L4: memoize the committed row strings (UNESCAPED — csvEscape is
  // transport-only; the engine stores the unescaped value) for the chained
  // consumer. Put AFTER mutationResult (the fold erases the name first).
  if (residentSetcol && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol put;
    put.dtype = "factor";
    put.nRows = n;
    put.strs = std::move(outStrings);
    session.residentComputedColPut(target, std::move(put));
  }
  return tostringResult;
}

// ---------------------------------------------------------------------------
// joinby key1 [key2 ...] using FILE (bare form only) — BS-004/D5 (BETA
// B-data). See handlers.h's runJoinby doc comment for the design summary.
// L1b: the join itself is now ONE engine call (`cobra joinby --data --using
// --keys --out`) — the old in-C++ cross-product + whole-frame CSV + `cobra
// convert` round-trip is retired. PIET keeps the using-file resolution and
// all pre-validation below, so the established error messages still fire
// before the engine is reached.


}  // namespace piet::handlers
