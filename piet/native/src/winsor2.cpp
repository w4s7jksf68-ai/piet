// winsor2 — C++ port of piet/backend/piet/winsor2.py's run_winsor2 (row-wise
// winsorization at the given percentiles). Each variable is winsorized via `cobra eval --winsor`;
// trim mode additionally NaNs out the clamped boundary values via a second
// `cobra eval --gen cond(...)` pass, matching the oracle's two-step pipeline.
#include "piet/handlers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

std::string nextPathW(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

ojson engineErrorBlockW(const std::string& message, const std::string& code = "engine_error") {
  ojson b;
  b["type"] = "error";
  b["code"] = code;
  b["message"] = message;
  return b;
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStrW(const CobraError& ce) { return cobraErrorMessage(ce); }

// _clamp_boundaries: compute lo_val/hi_val using the same floor(cut/100*N)
// rank method cobra --winsor uses (winsor2.py's _clamp_boundaries).
std::pair<double, double> clampBoundaries(Datastore& ds, const std::string& src, double plo,
                                           double phi) {
  const cecon::Column& c = ds.col(src);
  int64_t n = ds.nRows();
  const double* d = c.f64();
  std::vector<double> valid;
  valid.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (!std::isnan(d[i])) valid.push_back(d[i]);
  }
  if (valid.empty()) return {std::nan(""), std::nan("")};
  std::sort(valid.begin(), valid.end());
  int64_t nv = static_cast<int64_t>(valid.size());
  auto idxFor = [&](double p) -> int64_t {
    int64_t idx = static_cast<int64_t>(p / 100.0 * static_cast<double>(nv));
    return std::max<int64_t>(0, std::min(idx, nv - 1));
  };
  int64_t loIdx = idxFor(plo);
  int64_t hiIdx = idxFor(phi);
  return {valid[static_cast<size_t>(loIdx)], valid[static_cast<size_t>(hiIdx)]};
}

// Formats a double the way Python's f-string embeds it in a cobra expr
// string (e.g. "1.5", "99.0" -> matches winsor_spec's f"{plo},{phi}" and the
// trim_expr's boundary literals) — shortest round-tripping decimal.
std::string fmtNum(double v) {
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

}  // namespace

ojson runWinsor2(SessionState& session, const std::vector<std::string>& varlist, double cutLo,
                  double cutHi, bool trim, const std::string& by, const std::string& suffix,
                  bool replace) {
  if (varlist.empty()) return engineErrorBlockW("winsor2: no variables specified");
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  if (cutLo < 0 || cutHi > 100 || cutLo >= cutHi) {
    return engineErrorBlockW("winsor2: cuts(" + fmtNum(cutLo) + " " + fmtNum(cutHi) +
                              ") invalid — must have 0 <= p_lo < p_hi <= 100");
  }
  if (!by.empty()) {
    return engineErrorBlockW(
        "winsor2: by() option is not yet supported in PIET — "
        "please winsorize the full sample or split groups manually");
  }

  // C4-L4B (FIX-C4L4B-RESIDUALS ①): winsor2 already passes the head token as
  // `--data`, but the call itself was routed STATELESSLY, so a dirty virtual
  // head (the fixture's own `gen yz = ...` one line earlier) had to be
  // materialized before the engine could read it — measured on the floor row
  // as snapshot_count 1 AND datastore_constructs 1.
  //
  // The opt-in is deliberately NARROW, and the two conditions are the two
  // things that would make it WRONG rather than merely unhelpful:
  //   * varlist.size() == 1 — one variable is one engine call is ONE frame
  //     delta, and the shadow fold consumes exactly one. A multi-var winsor2
  //     issues N calls whose deltas overwrite each other, so a fold at the end
  //     would ship a card carrying only the LAST column: silently wrong, and
  //     invisible to this fixture (which winsorizes one variable).
  //   * !trim — the trim arm reads its PRE-winsorized input back AS A FILE
  //     (`Datastore preDs(currentHbin)` below) to compute the clamp
  //     boundaries, which is precisely the invariant ResidentEligible's doc
  //     comment forbids: under residency that input is a token the engine
  //     staged in worker RAM and never wrote.
  // Everything outside those two shapes keeps the pre-C4 stateless path
  // verbatim, as does the escape arm (PIET_NO_RESIDENT_READ).
  const bool residentWinsor = !residentReadDisabled() && varlist.size() == 1 && !trim;

  std::vector<std::string> newVars;
  std::string currentHbin = *session.currentHbin();

  for (const auto& src : varlist) {
    std::string outName = replace ? src : (src + suffix);

    std::string outPath = nextPathW(session.workdir(), "winsor2");
    std::string winsorSpec = outName + "=" + src + "," + fmtNum(cutLo) + "," + fmtNum(cutHi);
    std::vector<std::string> args = {"eval", "--data", currentHbin, "--winsor", winsorSpec,
                                      "--out", outPath};
    {
      std::optional<SessionState::ResidentEligible> winsorResident;
      if (residentWinsor) winsorResident.emplace(session);
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
      }
    }

    if (trim) {
      // Compute clamp boundaries from the PRE-winsorized source data
      // (currentHbin still has the original src values at this point).
      Datastore preDs(currentHbin);
      auto [loVal, hiVal] = clampBoundaries(preDs, src, cutLo, cutHi);
      if (!std::isnan(loVal) && !std::isnan(hiVal)) {
        std::string trimExpr = outName + "=cond((" + outName + ">" + fmtNum(loVal) + "&" +
                                outName + "<" + fmtNum(hiVal) + ")," + outName + ",.)";
        std::string trimPath = nextPathW(session.workdir(), "winsor2_trim");
        std::vector<std::string> trimArgs = {"eval", "--data", outPath, "--gen", trimExpr, "--out",
                                              trimPath};
        try {
          runCobraJson(trimArgs);
          outPath = trimPath;
        } catch (const CobraError& ce) {
          return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
        }
      }
    }

    currentHbin = outPath;
    newVars.push_back(outName);
  }

  session.load(currentHbin);
  // C4-L4B: on the resident shape, fold the frame delta into the shadow card
  // instead of invalidate+reopen. winsor2 KNOWS its output names up front
  // (replace: outName==src; suffix: src+suffix), so mutationResult is a
  // drop-in — same keys in the same order, and its own stateless fallback is
  // this exact rebuild (including seedNMissForPureAdd's kPureAdd branch, which
  // is why the `erase(nv)` rationale below still governs it). A fold that
  // DECLINES lands there too, so this return is never a bypass.
  if (residentWinsor) return mutationResult(session, "winsor2", newVars);
  session.invalidateDatastore();
  Datastore* ds = session.datastore();

  // C1-CARD-REBUILD-REDUCE A11 sibling: seed the unchanged columns' missing-
  // counts BEFORE the varlist rebuild below so each byte-identical column skips
  // its O(nrows) nMissing scan (and whole-column mmap fault-in — the wide-frame
  // "card tax"). `eval --winsor`/`--gen` above patched ONLY the output column(s)
  // named in newVars and left every other column — data AND dictionary — byte-
  // identical. newVars carries the winsorized target (replace: outName==src;
  // suffix: src+suffix), so seedNMissForPureAdd's kPureAdd branch (1) rescans
  // exactly those and carries the rest. Same PIET_NO_NMISS_CACHE A/B correctness
  // gate as every other seed site.
  session.seedNMissForPureAdd(ds, "winsor2", newVars, /*droppedVars=*/{}, /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "winsor2";
  ojson varlistAfter = ojson::array();
  for (const auto& v : session.varlistWithMeta(ds)) varlistAfter.push_back(v.toJson());
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  ojson nv = ojson::array();
  for (const auto& v : newVars) nv.push_back(v);
  out["new_vars"] = nv;
  return out;
}

}  // namespace piet::handlers
