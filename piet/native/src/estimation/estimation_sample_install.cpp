// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
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

#include "estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
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

// e(sample) T2 (internal note esample/SPEC.md §3.1, §4.1): the fit
// handler's half of the estimation-sample mask. The engine owns every admission
// rule (if/in, listwise deletion over y/x/fe/cluster/weight, zero weights,
// TS-operator loss, singleton drops) and writes the admitted rows on request
// (`--esample-out PATH`, apps/cmds/common.cpp emit_esample: one `1`/`0` line per
// row of the --data frame, or of an N-row frame through `--esample-index COL
// --esample-n N`, plus the reply key `esample_n` = the number of 1s). These two
// helpers are the PIET side, shared by every wired fit handler: the ones outside
// this file declare them at their call site.

// Appends `--esample-out PATH` (and `--esample-index indexVar --esample-n
// headRows` when the fit reads a filtered copy carrying the position column)
// to `argv`; returns PATH, a fresh scratch name the scratch census owns until
// esampleInstallColumn unlinks it.
std::string esampleFitArgs(SessionState& session, std::vector<std::string>& argv,
                           const std::string& indexVar, int64_t headRows) {
  const std::string hb = mintScratchPath(session.workdir(), "esample");
  forgetScratchMint(hb);
  const std::string path = hb.substr(0, hb.size() - 5) + ".txt";  // 5 == strlen(".hbin")
  registerScratchMint(path);
  argv.push_back("--esample-out");
  argv.push_back(path);
  if (!indexVar.empty()) {
    argv.push_back("--esample-index");
    argv.push_back(indexVar);
    argv.push_back("--esample-n");
    argv.push_back(std::to_string(headRows));
  }
  return path;
}

// After the fit's reply was accepted: writes the mask file as the hidden f64
// column kEsampleColumn of `basePath`'s frame (`setcol --new`, which REPLACES an
// earlier fit's mask, apps/cmds/cmds_data.cpp), admitted rows holding the
// install's serial (*serialOut, LastModel::esampleSerial), and returns the new
// frame's path,
// or "" when the mask cannot be vouched for -- the reply carries no
// `esample_n`, or it is not the fit's row count (`n_rows` when the reply has it,
// i.e. under weights, where n_obs may be the sum of fw/iw weights; else n_obs),
// or the setcol failed. "" leaves the model's e(sample) a named r(198)
// (LastModel::EsampleState::kUnavailable), never a guess. The caller adopts the
// returned frame BEFORE setLastModel (the BUG-518 order: lm.dataPath must name a
// frame that carries the column) and emits no data_mutation block for it: a fit
// is not a data change in Stata's log, so `c(changed)` is restored too.
// `residentEligible`: the returned frame becomes the head and is never read as a
// file (SessionState::ResidentEligible's contract) -- true for the plain install
// on the working frame; false when basePath is a real scratch file.
namespace {
// The reply's `esample_n` handshake: the number of admitted rows the engine wrote
// must be the fit's row count. The key is erased either way -- it is not an e()
// result: the reply becomes lm.result, whose keys the e() resolver reads by name
// (`e(esample_n)` must stay `.`).
bool esampleHandshakeOk(ojson& raw) {
  if (!raw.is_object() || !raw.contains("esample_n") || !raw["esample_n"].is_number()) return false;
  const double ones = raw["esample_n"].get<double>();
  raw.erase("esample_n");
  const char* rowsKey = raw.contains("n_rows") ? "n_rows" : "n_obs";
  if (!raw.contains(rowsKey) || !raw[rowsKey].is_number()) return false;
  return ones == raw[rowsKey].get<double>();
}
}  // namespace

std::string esampleInstallColumn(SessionState& session, const std::string& valuesPath,
                                 ojson& raw, const std::string& basePath,
                                 bool residentEligible, unsigned long long* serialOut) {
  struct Unlink {
    const std::string& p;
    ~Unlink() { cecon::native_unlink(p); }
  } unlinkValues{valuesPath};
  if (!esampleHandshakeOk(raw)) return "";
  // The engine wrote 1/0; an admitted row is stored as this install's serial
  // (LastModel::esampleSerial), so a later reading can tell this fit's mask
  // from another's. setcol maps it (`--ones-as`) and refuses any cell that is
  // not 0 or 1, so a file that is not the engine's payload is never installed.
  const unsigned long long serial = session.esampleColumnInstalls + 1;
  const std::string out = nextPath(session.workdir(), "esample_install");
  // runCobra, not runCobraJson: setcol answers on stderr, with no JSON reply.
  CobraRunResult r;
  {
    std::optional<SessionState::ResidentEligible> resident;
    if (residentEligible) resident.emplace(session);
    r = runCobra({"setcol", "--data", basePath, "--var", kEsampleColumn, "--values", valuesPath,
                  "--dtype", "f64", "--new", "--ones-as", std::to_string(serial), "--out", out});
  }
  if (!r.ok) return "";
  // BUG-124's torn-write guard (the other stateless setcol sites carry it): a
  // stateless --out that is not on disk is a failed write, not a frame. A
  // resident --out is the worker's virtual frame and has no file yet.
  if (!session.residentFrameLive() && !cecon::native_exists(out)) return "";
  session.esampleColumnInstalls = serial;
  if (serialOut != nullptr) *serialOut = serial;
  return out;
}

// The whole install for a fit that read the WORKING FRAME (its --data was the
// head, with or without --keepif/--esample-index). Call after every refusal the
// handler can still make and BEFORE setLastModel, with lm.dataPath already set
// to the head the fit read. On success the head is the install frame and `lm`
// says kColumn (serial, install path); otherwise `lm` is left kUnavailable and
// the head untouched. `valuesPath` is consumed either way.
//  * The resident install advances the worker's frame in place, so a VIRTUAL
//    fit frame would exist nowhere afterwards: it is written first (a no-op on
//    a clean head; the snapshot BUG-518's pin in setLastModel took for the same
//    model before), and lm.dataPath -- which every refit replays and `estimates
//    save` records -- stays that frame, not the transient install frame
//    (BUG-470.do's `estimates use` + `predict` round trip).
//  * The fit's frame stays pinned past the move (SessionState::
//    pinPreInstallHead): a driver may hold its path around the inner fit.
//  * No block, and c(changed) is restored: a fit is not a data change.
//  * A fit on the PREVIOUS install's frame, unmoved since (SessionState::
//    esampleHeadIsInstallOf: a second regress on the same data), reads that
//    install's on-disk base plus the hidden column, so lm.dataPath names the
//    base and nothing is written. Otherwise every wired fit wrote the whole
//    working frame to disk (review 2026-10-13: 1M rows, 56 -> 240 ms per fit).
bool esampleInstallOnHead(SessionState& session, const std::string& valuesPath, ojson& raw,
                          LastModel& lm) {
  lm.esample = LastModel::EsampleState::kUnavailable;
  lm.esampleSerial = 0;
  lm.esampleInstallPath.clear();
  if (valuesPath.empty() || !session.currentHbin().has_value() ||
      *session.currentHbin() != lm.dataPath) {
    if (!valuesPath.empty()) cecon::native_unlink(valuesPath);
    if (raw.is_object()) raw.erase("esample_n");
    return false;
  }
  const std::string fitHead = lm.dataPath;
  std::string fitFrame = fitHead;  // on disk: lm.dataPath after the install
  // The engine's 0/1 file, kept (2 bytes a row) to recognize the next fit's.
  std::string body;
  {
    std::ifstream in(cecon::to_native_path(valuesPath), std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
    if (size > 0) {
      body.assign(static_cast<size_t>(size), '\0');
      in.seekg(0);
      if (!in.read(&body[0], size)) body.clear();
    }
  }
  // A fit whose --keepif names the mask (`xtreg ... if e(sample), fe`, a TS
  // regress under `if e(sample)`) read the head's mask cells under the previous
  // serial. The base has no mask column, and this install overwrites the head's,
  // so every replay of that --keepif (lincom/test/margins/predict/boottest) must
  // read the fit's own frame: no alias, lm.dataPath stays the head the fit read
  // (review 2026-10-14: lincom r(111) `unknown variable __piet_esample__`, predict
  // rc 1 `[no_obs]`).
  // A condition mask (`if strpos(...)`, piet/datastore.h kQualifierColumnPrefix) is
  // read the same way: only the fit's own frame carries it.
  const bool fitReadsMask = namesHiddenColumn(lm.fitKeepif);
  if (!fitReadsMask && session.esampleHeadIsInstallOf(session.esampleLastInstallBase) &&
      cecon::native_exists(session.esampleLastInstallBase)) {
    fitFrame = session.esampleLastInstallBase;
    // Same rows (the head is unmoved) and byte-identical admitted rows: the
    // head already carries this sample under the last install's serial, so
    // nothing is installed (a re-fit of the same model under another vce(),
    // a second regress on the same rows).
    if (!body.empty() && body == session.esampleLastInstallBody) {
      cecon::native_unlink(valuesPath);
      if (!esampleHandshakeOk(raw)) return false;
      lm.dataPath = fitFrame;
      lm.esample = LastModel::EsampleState::kColumn;
      lm.esampleSerial = session.esampleLastInstallSerial;
      lm.esampleInstallPath = fitHead;
      return true;
    }
  } else {
    session.ensureMaterializedHead("esample-install");
    if (!cecon::native_exists(fitHead)) {  // the snapshot failed: no fit frame on disk
      cecon::native_unlink(valuesPath);
      if (raw.is_object()) raw.erase("esample_n");
      return false;
    }
  }
  unsigned long long serial = 0;
  // --data is the head itself, so the resident setcol needs no resync.
  const std::string withMask =
      esampleInstallColumn(session, valuesPath, raw, fitHead, true, &serial);
  if (withMask.empty()) return false;
  const bool changed = session.datasetChanged;
  session.pinPreInstallHead();
  session.load(withMask);
  session.carryCardAcrossHiddenInstall(fitHead, withMask);
  session.datasetChanged = changed;
  session.esampleLastInstallFrame = withMask;
  session.esampleLastInstallBase = fitFrame;
  session.esampleLastInstallLoads = session.headLoadCount;
  session.esampleLastInstallSerial = serial;
  session.esampleLastInstallRows =
      static_cast<long long>(std::count(body.begin(), body.end(), '\n'));
  session.esampleLastInstallBody = std::move(body);
  lm.dataPath = fitFrame;
  lm.esample = LastModel::EsampleState::kColumn;
  lm.esampleSerial = serial;
  lm.esampleInstallPath = withMask;
  return true;
}

}  // namespace piet::handlers
