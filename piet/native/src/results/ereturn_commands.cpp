// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2D-T01 (2026-10-11): the `ereturn` sub-verbs other than `list` --
// `ereturn clear`, `ereturn post` and `ereturn local` -- plus a NAMED refusal
// for every other sub-verb. BUG-1495 added `ereturn matrix` and a bare
// `ereturn repost`. `ereturn list` stays in scalar_results.cpp
// (runEreturnList) and `ereturn scalar NAME = EXP` inside an eclass program
// stays in program.cpp (handleReturnStmt, staged and flushed at program end);
// neither is touched here.
//
// Oracle: tests/eval/golden/aer_w2/w2d_uncovered.log.
//   * log 211-212: `w2d_mkts` runs with NOTHING in memory (after `clear all`)
//     and its first statement is `ereturn clear` -- the program goes on to
//     `set obs 12`, so `ereturn clear` is rc 0 with no data.
//   * log 2934 / 2956 (regress_absorb_cluster_052 / regress_absorb_vce_106):
//     `w2d_mkpanel` (ereturn clear first), then a refused `reg ..., absorb()`;
//     `_b[x1]`, `_se[x1]` and `e(cmd)` are all EMPTY (`198||||`). Before this
//     file the previous poisson model was still e(): `ereturn clear` was
//     unsupported_command, so nothing was ever cleared.
//   * log 4597 / 4617 / 4637 (ereturn_post_esample_068 / _tempvar_012 /
//     _noesample_ctl): `ereturn post b, depname(y) obs(#)` gives e(N) = obs()
//     and e(depvar) = depname(); without obs() e(N) is MISSING (`.`), even
//     though the program fitted a regression with N = 55 a line earlier --
//     `ereturn post` replaces the whole of e(), it does not add to it.
//     `ereturn local properties "b"` / `ereturn local cmd = "bootstrap"` are
//     read back as e(properties) / e(cmd).
// Second oracle, tests/eval/golden/aer_w2/w2_esample.log (not yet a registered
// eval group): G5 (log 1487) a TOP-LEVEL `ereturn post b` is rc 0 with e(N)
// `.` (so `post` is not confined to eclass programs), G11 (log 1634) a
// top-level `ereturn clear` after `regress` is rc 0 with e(N) `.`.
//
// WHAT "e()" IS IN THIS BUILD. A later reader can see the previous model
// through five stores, and `ereturn clear` has to empty every one of them or
// one layer keeps answering for a model that is gone:
//   session.eval.eScalars / eStrings / eMatrices  (posted e() values; the
//       e(cmd)/e(depvar)/e(vce) macros setLastModel publishes live here too)
//   SessionState::lastModel_reset()  (the LastModel the display twin reads
//       _b[]/e() from, lastEstBlock, eval.lastModelResult -- the macro layer's
//       _b[]/_se[] source -- the estadd bag and e(_estimates_name))
//   staged `ereturn scalar` values of the running eclass program
//       (session.returnStaging kind "escalar"; runProgram flushes them into
//       eScalars when the program returns, so one staged BEFORE the clear
//       would come back after it).
// This is the same set `clear results` empties (environment/workspace_commands.cpp runClear)
// minus the r() half, so `ereturn clear` leaves e() in exactly the state every
// reader already handles as "no estimates".
//
// e(sample). `ereturn post ..., esample(VAR)` moves VAR into e(sample): VAR's
// nonzero rows are installed as the hidden mask column under a fresh serial
// (installPostedMask, the scheme a fit's mask uses) and VAR is dropped. The
// posted e() has no LastModel, so SessionState::ePostedEsampleSerial carries
// the serial and SessionState::esampleReading reads it while the posting is
// live (oracle w2d_uncovered.log:4597 ereturn_post_esample_068 `55|55|111`).
//
// LIFETIME OF A POST (BUG-733, round 2). A posted e() has no fitted model
// behind it, and every e() reader prefers the stores it lives in, so it must
// end at the next model install or it answers for the new fit (posted e(N)
// 999 after a 60-obs regress). `ereturn post` sets SessionState::ePostedLive;
// setLastModel drops the stores while it is set (session_state.h). `ereturn
// local` adds to the live e() and goes with it. With no posting live, the key
// sits on top of the installed fit (or of an empty e()) and ends at the next
// model install, as in Stata (round 3: SessionState::eOnFitNames records it;
// test/lincom/nlcom/estat/erepost of the same model keep it).
//
// `ereturn local` outside an eclass program is r(152) (w2p_estim_ev.log:917),
// so nothing here writes e() from a non-eclass context; `ereturn post` and
// `ereturn clear` are rc 0 at top level (w2_esample.log G5/G11,
// w2p_estim_ev.log:931/:949).
//
// _b[] AFTER `ereturn post`. The posted b (and sqrt of V's diagonal as _se[])
// is mirrored into eval.lastModelResult as a coeftable, which is what the
// macro/arith layer and the row plane read _b[]/_se[] from -- without it
// `scalar s = _b[x]` would answer `.` at rc 0 for a coefficient that is in
// e(b). No LastModel is created: a LastModel claims a FITTED model that
// test/lincom/predict/margins replay through the engine, and a posted vector
// has no fit behind it (the same reason estpost does not call setLastModel,
// tables/table_estpost.cpp postPseudoEstimate). The display twin
// (resolveBRef in language/scalar_expression.cpp) reads _b[] from LastModel only, so `display _b[x]`
// after a post still takes its no-estimates path -- recorded as a follow-up,
// that file is not this task's.
#include "piet/handlers/io.h"
#include "piet/handlers/results.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "estimation/estimator_command_support.h"  // coeftableFromPosting (ereturn display)
#include "eval/macro_results_internal.h"  // resolveEScalar (ereturn display)

namespace piet::handlers {

namespace {

using ojson = nlohmann::ordered_json;

// `ereturn post`'s option vocabulary. `esample` is KNOWN (installPostedMask
// consumes it). buildfvinfo/findomitted are real `ereturn post` options
// (the AER corpus writes buildfvinfo 51 times) that change what a later
// `margins`/omitted-term check sees, not e(b)/e(V); this build computes
// neither, so they are refused as valid-but-unimplemented rather than
// accepted and ignored.
const eval::OptVocab& kVocabEreturn = eval::registerOptVocab(
    {"ereturn"},
    {{"esample", "obs", "depname", "dof", "properties"}, {"buildfvinfo", "findomitted"}});

// An error block with an EXPLICIT Stata rc (coarseRc honours reason "r(NNN)").
ojson rcError(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// The user matrix namespace only: `ereturn post b V` names matrices the user
// defined, never e()/r() results (EvalSession::findMatrix also searches
// eMatrices, which `ereturn post` is about to replace).
const eval::EvalMatrix* userMatrix(const SessionState& session, const std::string& name) {
  for (const auto& [k, m] : session.eval.matrices) {
    if (k == name) return &m;
  }
  return nullptr;
}

// One option's value text, or nullopt when the option was not given.
std::optional<std::string> optText(const eval::ParsedCommand& parsed, const std::string& key) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == key) return strutil::trim(v.strValue);
  }
  return std::nullopt;
}

bool hasOpt(const eval::ParsedCommand& parsed, const std::string& key) {
  return optText(parsed, key).has_value();
}

// A numeric option value (obs(#), dof(#)); nullopt when it is not one number.
std::optional<double> numberOf(const std::string& text) {
  if (text.empty()) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) return std::nullopt;
  return v;
}

// Strip ONE level of quoting: "text" or the compound `"text"'.
std::string dequoteOnce(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  if (v.size() >= 4 && v.rfind("`\"", 0) == 0 && v.compare(v.size() - 2, 2, "\"'") == 0) {
    return v.substr(2, v.size() - 4);
  }
  return v;
}

// The words after the head command and the sub-verb, raw (options included).
std::string argsAfterSubverb(const std::string& raw) {
  std::string s = strutil::trim(raw);
  for (int word = 0; word < 2; ++word) {
    const size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) return std::string();
    s = strutil::trim(s.substr(sp));
  }
  return s;
}

}  // namespace

// Empty every e() store a later reader can see (see the file header).
void clearEResults(SessionState& session) {
  session.eval.eScalars = eval::OrderedFloatDict();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();
  session.lastModel_reset();
  session.ePostedLive = false;  // nothing posted is left to supersede
  for (auto it = session.returnStaging.begin(); it != session.returnStaging.end();) {
    if (it->second.first == "escalar") {
      it = session.returnStaging.erase(it);
    } else {
      ++it;
    }
  }
}

namespace {

// `ereturn post ..., esample(VAR)`: install VAR != 0 (nonmissing) as the
// hidden e(sample) column under a fresh serial (the scheme a fit's mask uses,
// estimation/estimation_sample_install.cpp esampleInstallColumn), then drop VAR. Returns the serial, 0 on
// failure (*why says what) with the data unchanged.
unsigned long long installPostedMask(SessionState& session, const std::string& var,
                                     std::string* why) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || !session.currentHbin().has_value()) {
    *why = "no data in memory";
    return 0;
  }
  const size_t n = static_cast<size_t>(ds->nRows());
  const double* p = ds->col(var).f64();
  const bool hadColumn = ds->has(kEsampleColumn);
  const std::string head = *session.currentHbin();
  const std::string valsBase = mintScratchPath(session.workdir(), "esample_post");
  forgetScratchMint(valsBase);
  const std::string vals = valsBase.substr(0, valsBase.size() - 5) + ".txt";
  registerScratchMint(vals);
  {
    std::ofstream o(cecon::to_native_path(vals), std::ios::binary | std::ios::trunc);
    if (!o) {
      *why = "could not write the sample marker";
      return 0;
    }
    for (size_t i = 0; i < n; ++i) o << ((p != nullptr && !std::isnan(p[i]) && p[i] != 0) ? "1\n" : "0\n");
  }
  const unsigned long long serial = session.esampleColumnInstalls + 1;
  const std::string out = mintScratchPath(session.workdir(), "esample_post");
  std::vector<std::string> args = {"setcol", "--data", head, "--var", kEsampleColumn,
                                   "--values", vals, "--dtype", "f64"};
  if (!hadColumn) args.push_back("--new");
  for (const char* a : {"--ones-as"}) args.push_back(a);
  args.push_back(std::to_string(serial));
  args.push_back("--out");
  args.push_back(out);
  const CobraRunResult r = runCobra(args);
  cecon::native_unlink(vals);
  if (!r.ok || !cecon::native_exists(out)) {
    *why = "the sample marker could not be installed";
    return 0;
  }
  const bool changed = session.datasetChanged;
  session.load(out);
  session.carryCardAcrossHiddenInstall(head, out);
  session.esampleColumnInstalls = serial;
  ojson dropped;
  {
    SessionState::TempDropScope td(session);
    dropped = dispatch("quietly drop " + var, session);
  }
  for (const auto& b : dropped)
    if (b.is_object() && b.value("type", std::string()) == "error") {
      *why = "could not remove " + var + " from the data";
      return 0;
    }
  session.datasetChanged = changed;
  return serial;
}

ojson runPost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err =
          eval::rejectUnknownOptions(parsed.opts, kVocabEreturn.known, kVocabEreturn.unimplemented)) {
    return ojson(*err);
  }
  // esample(VAR): VAR's nonzero rows become e(sample) and VAR leaves the data
  // (help ereturn: "esample(varname) ... the variable is removed from the data").
  std::optional<std::string> esampleVar;
  if (auto t = optText(parsed, "esample")) {
    if (t->empty()) return rcError("option esample() required", 198, "syntax_error");
    if (!session.hasData()) return rcError("variable " + *t + " not found", 111);
    Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has(*t) || isHiddenColumn(*t))
      return rcError("variable " + *t + " not found", 111);
    if (ds->dtype(*t) != "f64") return rcError("type mismatch", 109);
    esampleVar = *t;
  }

  // `ereturn post [b [V]]`
  std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (names.size() > 2) {
    return rcError("ereturn post: too many names; the syntax is `ereturn post [b [V]] [, options]'",
                   198, "syntax_error");
  }
  const eval::EvalMatrix* b = nullptr;
  const eval::EvalMatrix* V = nullptr;
  if (!names.empty()) {
    b = userMatrix(session, names[0]);
    if (b == nullptr) return rcError("matrix " + names[0] + " not found", 111);
  }
  if (names.size() == 2) {
    V = userMatrix(session, names[1]);
    if (V == nullptr) return rcError("matrix " + names[1] + " not found", 111);
  }

  // Shape and name checks. No oracle arm records Stata's rc for these, so the
  // codes are chosen explicitly (conformability error 503, name conflict 507)
  // and listed as unpinned in the task report.
  size_t k = 0;
  if (b != nullptr) {
    if (b->data.size() != 1) {
      return rcError("ereturn post: " + names[0] + " must be a row vector (1 x k), it is " +
                         std::to_string(b->data.size()) + " x " +
                         std::to_string(b->data.empty() ? 0 : b->data[0].size()),
                     503);
    }
    k = b->data[0].size();
  }
  if (V != nullptr) {
    bool square = V->data.size() == k;
    for (const auto& row : V->data) square = square && row.size() == k;
    if (!square) {
      return rcError("ereturn post: " + names[1] + " must be " + std::to_string(k) + " x " +
                         std::to_string(k) + " to match " + names[0] + " (conformability error)",
                     503);
    }
    if (V->rownames != b->colnames || V->colnames != b->colnames) {
      return rcError("ereturn post: the row and column names of " + names[1] +
                         " must equal the column names of " + names[0] + " (name conflict)",
                     507);
    }
  }

  std::optional<double> obs;
  if (auto t = optText(parsed, "obs")) {
    obs = numberOf(*t);
    if (!obs.has_value() || *obs < 0 || std::floor(*obs) != *obs) {
      return rcError("ereturn post: obs() must be a nonnegative integer, got `" + *t + "'", 198,
                     "syntax_error");
    }
  }
  std::optional<double> dof;
  if (auto t = optText(parsed, "dof")) {
    dof = numberOf(*t);
    if (!dof.has_value() || *dof <= 0) {
      return rcError("ereturn post: dof() must be a positive number, got `" + *t + "'", 198,
                     "syntax_error");
    }
  }
  std::optional<std::string> depname = optText(parsed, "depname");
  if (depname.has_value() && !depname->empty() && !strutil::isValidStataIdentifier(*depname)) {
    return rcError("ereturn post: depname(" + *depname + ") is not a valid name", 198,
                   "syntax_error");
  }
  std::optional<std::string> properties = optText(parsed, "properties");
  if (properties.has_value()) properties = dequoteOnce(*properties);

  // Copies BEFORE the clear: b and V live in session.eval.matrices, which the
  // clear does not touch, but nothing below may depend on that.
  std::optional<eval::EvalMatrix> bCopy =
      b != nullptr ? std::optional<eval::EvalMatrix>(*b) : std::nullopt;
  // e(b)'s one row is Stata's own `y1`, whatever the posted matrix called it
  // and with or without depname() (vs-stata truth bank: `matrix b = (1,2,3)`
  // then `ereturn post b V, obs(74)` or `..., depname(price)` leaves e(b) row
  // `y1`, not b's `r1`).
  if (bCopy.has_value()) bCopy->rownames.assign(1, "y1");
  const std::optional<eval::EvalMatrix> VCopy =
      V != nullptr ? std::optional<eval::EvalMatrix>(*V) : std::nullopt;

  // The e(sample) mask first (it can still fail, and then nothing is posted).
  unsigned long long maskSerial = 0;
  if (esampleVar.has_value()) {
    std::string why;
    maskSerial = installPostedMask(session, *esampleVar, &why);
    if (maskSerial == 0) return rcError("ereturn post: esample(" + *esampleVar + "): " + why, 198);
  }

  // A NEW e(): everything the previous estimation left is gone (oracle 4617 /
  // 4637: e(N) is `.` after a post without obs(), though the program's own
  // regress had N = 55).
  clearEResults(session);

  if (bCopy.has_value()) {
    // e(b)'s row is `y1` once posted, whatever depname() says, when b carries
    // the default row name (StataBE 19.5, truth bank programs eclass_post
    // `depname(yy)`, ereturn_post_esample, ereturn: XRN|e|b|1 = y1, not r1).
    eval::EvalMatrix posted = *bCopy;
    if (posted.rownames.size() == 1 && posted.rownames[0] == "r1") posted.rownames[0] = "y1";
    session.eval.eMatrices.emplace_back("b", std::move(posted));
  }
  if (VCopy.has_value()) session.eval.eMatrices.emplace_back("V", *VCopy);
  if (obs.has_value()) session.eval.setEScalar("N", *obs);
  if (dof.has_value()) session.eval.setEScalar("df_r", *dof);
  if (depname.has_value()) session.eval.setEString("depvar", *depname);
  // Without properties(), e(properties) names what was posted (StataBE 19.5:
  // `ereturn post bb vv` lists "b V", `ereturn post bb` "b", `ereturn post,
  // obs(5)` none).
  if (!properties.has_value() && bCopy.has_value())
    properties = VCopy.has_value() ? "b V" : "b";
  if (properties.has_value()) session.eval.setEString("properties", *properties);

  // _b[]/_se[] for the macro/arith layer and the row plane (file header).
  if (bCopy.has_value()) {
    ojson coeftable = ojson::array();
    for (size_t j = 0; j < k; ++j) {
      ojson row;
      row["name"] = j < bCopy->colnames.size() ? bCopy->colnames[j] : std::string();
      const double bj = bCopy->data[0][j];
      row["b"] = std::isnan(bj) ? ojson(nullptr) : ojson(bj);
      double se = std::numeric_limits<double>::quiet_NaN();
      if (VCopy.has_value()) {
        const double vjj = VCopy->data[j][j];
        if (!std::isnan(vjj) && vjj >= 0) se = std::sqrt(vjj);
      }
      row["se"] = std::isnan(se) ? ojson(nullptr) : ojson(se);
      coeftable.push_back(row);
    }
    ojson result = ojson::object();
    result["coeftable"] = coeftable;
    session.eval.lastModelResult = result;
  }
  // BUG-733: this e() has no fitted model behind it, so the next model install
  // (SessionState::setLastModel) replaces it whole instead of letting the
  // posted e(N)/e(df_r)/e(b)/e(V) answer for the new fit.
  session.ePostedLive = true;
  session.ePostedEsampleSerial = maskSerial;
  return ojson::array();
}

// program.cpp's kProgClassMarkerKey, byte for byte: runProgram stages the
// running program's class (`eclass`/`sclass`) under this reserved local name
// on the program's own scope, and handleReturnStmt reads it the same way for
// `ereturn scalar`. Absent at top level and in a plain or rclass program.
const char* const kProgClassMarkerKey = " __piet_progclass";

bool inEclassProgram(const SessionState& session) {
  const std::string* cls = session.eval.locals.find(kProgClassMarkerKey);
  return cls != nullptr && *cls == "eclass";
}

// `ereturn local NAME [text]` / `ereturn local NAME = string-expression`,
// inside an eclass program only. Outside one Stata refuses, r(152) "non
// e-class program may not set e()" (tests/eval/golden/aer_w2/w2p_estim_ev.log:917
// C_ereturn_local `152||60|regress|`: e() is left as it was), so this handler
// never stores e() from a non-eclass context. The text is the raw rest of the
// line after NAME, as `local` takes it: a comma in it is text, not an option
// list (`ereturn local note a, b` stores `a, b`).
// After `ereturn local|matrix|scalar` on top of a fit (eclass program), the fit's
// ordered e() lists gain the name as estadd's (table_estadd.cpp): a macro first
// (an emptied one leaves), a matrix right after `b V`, a scalar last.
void noteEOnFitContract(SessionState& session, const std::string& name, char kind) {
  if (session.ePostedLive) return;
  noteEContractName(session, name, kind);
}

ojson runLocal(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!inEclassProgram(session)) {
    return rcError("non e-class program may not set e()", 152);
  }
  std::string rest = argsAfterSubverb(parsed.raw);
  size_t nameEnd = 0;
  while (nameEnd < rest.size() && rest[nameEnd] != ' ' && rest[nameEnd] != '\t' &&
         rest[nameEnd] != '=') {
    ++nameEnd;
  }
  const std::string name = rest.substr(0, nameEnd);
  if (name.empty()) {
    return rcError("ereturn local: a name is required", 198, "syntax_error");
  }
  if (!strutil::isValidStataIdentifier(name)) {
    return rcError("ereturn local: " + name + " is not a valid name", 198, "syntax_error");
  }
  std::string value = strutil::trim(rest.substr(nameEnd));
  // `ereturn local NAME : extended_fcn` stores the function's result, as
  // `local NAME : ...` does, computed by local's own definition path through a
  // scratch local (`ereturn local n : word count a b c` is 3).
  if (!value.empty() && value.front() == ':') {
    static const std::string kScratch = "__piet_eretlocal_fn";
    eval::MacroFnError fe;
    const bool consumed =
        eval::tryCaptureDefinition("local " + kScratch + " " + value, session.eval, &fe);
    const std::string got = session.eval.locals.get(kScratch, "");
    session.eval.locals.erase(kScratch);
    if (!consumed || !fe.rc.empty()) {
      int rc = 198;
      try {
        if (!fe.rc.empty()) rc = std::stoi(fe.rc);
      } catch (...) {
      }
      return rcError(fe.message.empty() ? std::string("invalid syntax") : fe.message, rc,
                     "syntax_error");
    }
    session.eval.setEString(name, got);
    if (!session.ePostedLive) session.noteEOnFit(name);
    noteEOnFitContract(session, name, 'm');
    return ojson::array();
  }
  if (!value.empty() && value.front() == '=') {
    const std::string expr = strutil::trim(value.substr(1));
    std::string raised;
    bool isString = false;
    const std::optional<std::string> v = eval::evalArith(expr, &session.eval, &raised, &isString);
    if (!v.has_value()) {
      return rcError("ereturn local " + name + " = " + expr + ": the expression could not be "
                     "evaluated" + (raised.empty() ? std::string() : " (" + raised + ")"),
                     198, "syntax_error");
    }
    if (!isString) {
      // The oracle records only a string expression (w2d_uncovered.do
      // w2d_post012: `ereturn local cmd = "bootstrap"`). How a NUMERIC result
      // is rendered into the macro text is not pinned for this surface, and
      // the macro layer's own integer rendering is an open defect (W2D-T02,
      // `19` -> `19.0`), so it is refused by name rather than guessed.
      return ojson(eval::unsupportedCommand("ereturn local NAME = <numeric expression>",
                                            std::nullopt, std::nullopt,
                                            "refused:not_implemented_sub",
                                            eval::UnsupportedCommandKind::kNotImplemented));
    }
    session.eval.setEString(name, *v);
    if (!session.ePostedLive) session.noteEOnFit(name);  // round 3: on top of the fit
    noteEOnFitContract(session, name, 'm');
    return ojson::array();
  }
  session.eval.setEString(name, dequoteOnce(value));
  if (!session.ePostedLive) session.noteEOnFit(name);  // round 3: on top of the fit
  noteEOnFitContract(session, name, 'm');
  return ojson::array();
}

// `ereturn matrix NAME [=] MATNAME [, copy]`, inside an eclass program only. As
// `ereturn local`, e(NAME) is set at once (on top of the fit or of the posting),
// and MATNAME is MOVED into it -- gone from the user's matrices -- unless `copy`
// is given. StataBE 19.5 (BUG-1495, scratch-laneA2/agents/rprog/x1495.do):
// colsof(e(mm)) is 3 on the program's next line and `matrix list M` is then
// r(111) (rc 0 with copy); the `=` is optional; NAME `b` or `V` is r(198) (ereturn
// post sets those), an invalid name r(198), an absent MATNAME r(111) "M not
// found"; outside an eclass program (an rclass one too) r(152). `ereturn list`
// shows such matrices after e(b) and e(V), newest first.
ojson runMatrix(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!inEclassProgram(session)) {
    return rcError("non e-class program may not set e()", 152);
  }
  std::string rest = argsAfterSubverb(parsed.raw);
  bool copy = false;
  const size_t comma = rest.find(',');
  if (comma != std::string::npos) {
    const std::string opt = strutil::trim(rest.substr(comma + 1));
    if (opt != "copy") return rcError("option " + opt + " not allowed", 198, "syntax_error");
    copy = true;
    rest = strutil::trim(rest.substr(0, comma));
  }
  std::string name;
  std::string src;
  const size_t eq = rest.find('=');
  if (eq != std::string::npos) {
    name = strutil::trim(rest.substr(0, eq));
    src = strutil::trim(rest.substr(eq + 1));
  } else {
    std::istringstream words(rest);
    words >> name >> src;
    std::string extra;
    if (words >> extra) return rcError("invalid syntax", 198, "syntax_error");
  }
  if (name.empty() || src.empty() || name == "b" || name == "V") {
    return rcError("invalid syntax", 198, "syntax_error");
  }
  if (!strutil::isValidStataIdentifier(name)) {
    return rcError(name + " invalid name", 198, "syntax_error");
  }
  if (!strutil::isValidStataIdentifier(src)) {
    return rcError(src + " invalid name", 198, "syntax_error");
  }
  const eval::EvalMatrix* m = userMatrix(session, src);
  if (m == nullptr) return rcError(src + " not found", 111);
  eval::EvalMatrix posted = *m;
  if (!copy) {
    auto& user = session.eval.matrices;
    user.erase(std::remove_if(user.begin(), user.end(),
                              [&](const auto& p) { return p.first == src; }),
               user.end());
  }
  auto& em = session.eval.eMatrices;
  em.erase(std::remove_if(em.begin(), em.end(), [&](const auto& p) { return p.first == name; }),
           em.end());
  size_t at = 0;
  while (at < em.size() && (em[at].first == "b" || em[at].first == "V")) ++at;
  em.insert(em.begin() + static_cast<std::ptrdiff_t>(at), {name, std::move(posted)});
  if (!session.ePostedLive) session.noteEOnFit(name);  // ends at the next fit, as `ereturn local`
  noteEOnFitContract(session, name, 'x');
  return ojson::array();
}

}  // namespace

void noteEContractName(SessionState& session, const std::string& name, char kind) {
  if (!session.eNameContract.has_value()) return;
  SessionState::ENameContract& c = *session.eNameContract;
  const auto listed = [&name](const std::vector<std::string>& v) {
    return std::find(v.begin(), v.end(), name) != v.end();
  };
  if (kind == 's' && c.scalars.has_value()) {
    if (!listed(*c.scalars)) c.scalars->push_back(name);
  } else if (kind == 'm' && c.macros.has_value()) {
    std::vector<std::string>& m = *c.macros;
    m.erase(std::remove(m.begin(), m.end(), name), m.end());
    const std::string* v = session.eval.eStrings.find(name);
    if (v != nullptr && !v->empty()) m.insert(m.begin(), name);
  } else if (kind == 'x' && c.matrices.has_value()) {
    std::vector<std::string>& x = *c.matrices;
    if (listed(x)) return;
    size_t at = 0;
    while (at < x.size() && (x[at] == "b" || x[at] == "V")) ++at;
    x.insert(x.begin() + static_cast<std::ptrdiff_t>(at), name);
  }
}

// `ereturn display [, eform(string)]`: the coefficient table of the live e(b) /
// e(V) -- t(e(df_r)) when a residual dof is posted, else the normal -- and its
// r(table) (b se z|t pvalue ll ul df crit eform over e(b)'s columns). e() is left
// alone. StataBE 19.5: bank c7bdd997 after `ereturn post b V` (z; r(mcmethod)
// "noadjust"), 38b63ecb `ereturn post b V, dof(10)` (t(10)), 10698eae after
// `regress` (t(71), no r(mcmethod)), 88509ffa after `logit`, `eform(OR)`
// (exp(b), exp(b)*se, the exponentiated interval; z and p unchanged; eform row 1).
// No e(b) is r(301) "last estimates not found". Other options are refused by name.
ojson runDisplay(const eval::ParsedCommand& parsed, SessionState& session) {
  std::optional<std::string> eformLabel;
  for (const auto& [k0, v] : parsed.opts) {
    const std::string k = strutil::lower(k0);
    if (k.size() >= 2 && std::string("eform").compare(0, k.size(), k) == 0) {
      eformLabel = v.isBool ? std::string("exp(b)") : dequoteOnce(strutil::trim(v.strValue));
      if (eformLabel->empty()) eformLabel = "exp(b)";
      continue;
    }
    return ojson(eval::errorBlock("unsupported_option",
                                  "ereturn display: option " + k0 +
                                      " is not implemented in this version of PIET; nothing was displayed.",
                                  std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  if (parsed.varlist.size() > 1) return rcError("invalid syntax", 198, "syntax_error");
  using Reader = eval::EvalSession::EReader;
  const auto eb = session.eval.resolveEMatrixFor("b", Reader::kCopy);
  if (!eb.matrix.has_value() || eb.matrix->data.size() != 1 || eb.matrix->colnames.empty())
    return rcError("last estimates not found", 301);
  const auto ev = session.eval.resolveEMatrixFor("V", Reader::kCopy);
  const std::vector<std::string>& names = eb.matrix->colnames;
  const size_t k = names.size();
  std::vector<std::vector<double>> V(k, std::vector<double>(k, std::numeric_limits<double>::quiet_NaN()));
  if (ev.matrix.has_value() && ev.matrix->data.size() == k)
    for (size_t i = 0; i < k; ++i)
      if (ev.matrix->data[i].size() == k) V[i] = ev.matrix->data[i];
  double dfR = std::numeric_limits<double>::quiet_NaN();
  {
    const std::string t = strutil::trim(eval::macro_detail::resolveEScalar("df_r", session.eval));
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (!t.empty() && t[0] != '.' && end != nullptr && *end == '\0' && std::isfinite(v) && v > 0) dfR = v;
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "ereturn_display";
  block["noheader"] = true;  // the table alone: no title, no header statistics
  const std::string* dep = session.eval.eStrings.find("depvar");
  block["depvar"] = dep != nullptr ? *dep : std::string();
  block["coeftable"] = e3::coeftableFromPosting(names, eb.matrix->data[0], V, dfR);
  if (std::isfinite(dfR)) block["df_r"] = dfR;
  const double level = static_cast<double>(session.level);
  block["level"] = level;
  if (level != 95.0) {  // the interval at c(level), as r(table) builds it
    const double alpha = 1.0 - level / 100.0;
    const double crit = std::isfinite(dfR) ? cecon::t_critical(alpha, dfR) : cecon::z_critical(alpha);
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row["b"].is_number() || !row["se"].is_number() || !row["t"].is_number()) continue;
      const double b = row["b"].get<double>(), se = row["se"].get<double>();
      row["ci_lo"] = b - crit * se;
      row["ci_hi"] = b + crit * se;
    }
  }
  // eform() exponentiates the first e(k_eform) equations of e(b) (1 when the fit
  // posts none); e(k_eform) 0 turns it off. Ancillary parameters (`/cut1`,
  // `/athrho`, the `/:` equation) and later equations stay in the coefficient
  // metric with an eform row of 0 (bank 57b46533 `ologit rep78 mpg weight, or`:
  // r(table) eform 1 1 0 0 0 0, the cutpoints unexponentiated).
  std::vector<bool> expCol(k, false);
  if (eformLabel.has_value()) {
    double kEform = 1.0;
    {
      const std::string t = strutil::trim(eval::macro_detail::resolveEScalar("k_eform", session.eval));
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (!t.empty() && t[0] != '.' && end != nullptr && *end == '\0' && std::isfinite(v)) kEform = v;
    }
    if (kEform == 0.0) eformLabel.reset();
    std::vector<std::string> eqOrder;
    for (size_t j = 0; j < k && eformLabel.has_value(); ++j) {
      const std::string& c = names[j];
      const size_t colon = c.find(':');
      const std::string eq = colon != std::string::npos ? c.substr(0, colon)
                             : (!c.empty() && c[0] == '/') ? c
                                                           : std::string();
      auto it = std::find(eqOrder.begin(), eqOrder.end(), eq);
      if (it == eqOrder.end()) it = eqOrder.insert(eqOrder.end(), eq);
      const bool ancillary = !eq.empty() && eq[0] == '/';
      expCol[j] = !ancillary && static_cast<double>(it - eqOrder.begin()) < kEform;
    }
  }
  if (eformLabel.has_value()) {
    // exp() past the double range is missing, as Stata stores it.
    const auto fin = [](double x) { return std::isfinite(x) ? x : std::numeric_limits<double>::quiet_NaN(); };
    ojson cols = ojson::array();
    size_t j = 0;
    for (auto& row : block["coeftable"]) {
      const bool ex = j < k && expCol[j];
      ++j;
      cols.push_back(ex);
      if (!ex || !row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
      const double ebj = std::exp(row["b"].get<double>());
      row["b"] = fin(ebj);
      if (row.contains("se") && row["se"].is_number()) row["se"] = fin(ebj * row["se"].get<double>());
      for (const char* key : {"ci_lo", "ci_hi"})
        if (row.contains(key) && row[key].is_number()) row[key] = fin(std::exp(row[key].get<double>()));
      row["eform"] = true;
    }
    block["eform_label"] = *eformLabel;
    block["rtable_eform"] = true;
    block["rtable_eform_cols"] = cols;
  }
  if (std::isfinite(dfR)) block["var_small_df_r"] = dfR;
  // A posting with no e(cmd) (`ereturn post`) leaves its multiple-comparison method.
  const std::string* ecmd = session.eval.eStrings.find("cmd");
  if (ecmd == nullptr || ecmd->empty()) block["rtable_mcmethod"] = "noadjust";
  eval::replaceRClass(session.eval);
  if (!postRTableFromBlock(session, block, level))
    return rcError("ereturn display: the coefficient table could not be built from e(b) and e(V)", 198);
  // _coef_table's own r() beside the table (bank c7bdd997 / 10698eae: r(k_eform) 1,
  // r(citype) "normal").
  session.eval.rValues.set("k_eform", 1.0);
  session.eval.rStrings.set("citype", "normal");
  return block;
}

ojson runEreturn(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) {
    return rcError("ereturn: a subcommand is required (list, clear, post or local)", 198,
                   "syntax_error");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);
  if (sub == "clear") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) return ojson(*err);
    if (parsed.varlist.size() > 1) {
      return rcError("ereturn clear: takes no arguments", 198, "syntax_error");
    }
    clearEResults(session);
    return ojson::array();
  }
  if (sub == "post") return runPost(parsed, session);
  if (sub == "local") return runLocal(parsed, session);
  if (sub == "matrix") return runMatrix(parsed, session);
  // `ereturn di` .. `ereturn display` (Stata's minimum abbreviation is `di`).
  if (sub.size() >= 2 && std::string("display").compare(0, sub.size(), sub) == 0)
    return runDisplay(parsed, session);
  // `ereturn repost` sets e() too, so outside an eclass program it is r(152)
  // (before and after a fit, at the top level and in an rclass program); inside
  // one a bare `ereturn repost` changes nothing at rc 0, or is r(301) with no
  // e() to repost (StataBE 19.5, BUG-1495, x1495.do / x1495b.do). Reposting b,
  // V or Cns stays a named refusal below.
  if (sub == "repost") {
    if (!inEclassProgram(session)) {
      return rcError("non e-class program may not set e()", 152);
    }
    if (argsAfterSubverb(parsed.raw).empty()) {
      if (!session.lastModel().has_value() && !session.eval.eLive()) {
        return rcError("last estimates not found", 301);
      }
      return ojson::array();
    }
  }

  // Real `ereturn` sub-verbs with no oracle arm yet: refused BY NAME (rc 199,
  // never rc 1, never accepted and ignored). `ereturn scalar NAME = EXP`
  // inside an eclass program never reaches this point (program.cpp
  // handleReturnStmt claims it first); any other shape of it lands here.
  static const std::unordered_set<std::string> kNotImplemented = {
      "repost", "scalar", "display", "hidden", "historical"};
  if (kNotImplemented.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("ereturn " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  // Any other word is not an ereturn subcommand: Stata's r(198) "invalid
  // syntax" (bank bde9c3705b3267f3 `ereturn zzz`).
  return rcError("invalid syntax", 198, "syntax_error");
}

}  // namespace piet::handlers
