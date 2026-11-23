// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// winsor2 — C++ port of piet/backend/piet/winsor2.py's run_winsor2 (row-wise
// winsorization at the given percentiles). The unrestricted winsor path uses
// `cobra eval --winsor`. Trim and qualified commands derive their cutoffs
// before writing the selected rows with `cobra eval --gen`.
#include "piet/handlers/data_values.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"

#include "core/stata_pctile.h"

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

std::pair<double, double> clampBoundaries(Datastore& ds, const std::string& src, double plo,
                                         double phi, const std::string& maskVar,
                                         int64_t first, int64_t last) {
  const double* data = ds.col(src).f64();
  const double* mask = maskVar.empty() ? nullptr : ds.col(maskVar).f64();
  std::vector<double> valid;
  valid.reserve(static_cast<size_t>(last - first));
  for (int64_t i = first; i < last; ++i) {
    if ((!mask || mask[i] != 0.0) && !std::isnan(data[i])) valid.push_back(data[i]);
  }
  std::sort(valid.begin(), valid.end());
  return {cecon::stata_pctile_sorted(valid, plo), cecon::stata_pctile_sorted(valid, phi)};
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
                  bool replace, const eval::ParsedCommand* command) {
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
  const bool hasIf = command && command->ifExpr && !command->ifExpr->empty();
  const bool hasIn = command && command->inRange && !command->inRange->empty();
  const bool qualified = hasIf || hasIn;
  InRangeBounds range;
  if (hasIn) {
    range = parseInRangeBounds(*command->inRange, session.datastore()->nRows());
    if (!range.ok) {
      InRangeFilterResult refusal;
      refusal.refusal = range.refusal;
      return inRangeRejectError(refusal, *command->inRange, "winsor2");
    }
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
  const bool residentWinsor = !residentReadDisabled() && varlist.size() == 1 && !trim && !qualified;

  std::vector<std::string> newVars;
  std::string currentHbin = *session.currentHbin();
  std::optional<std::pair<double, double>> postedCuts;
  if (qualified) session.ensureMaterializedHead();

  for (const auto& src : varlist) {
    std::string outName = replace ? src : (src + suffix);

    std::string outPath = nextPathW(session.workdir(), "winsor2");
    std::string winsorSpec = outName + "=" + src + "," + fmtNum(cutLo) + "," + fmtNum(cutHi);
    std::vector<std::string> args = {"eval", "--data", currentHbin, "--winsor", winsorSpec,
                                      "--out", outPath};
    // The new variable keeps the source's storage type, as winsor2's clonevar
    // does (StataBE wq.do: float -> float, double -> double, byte -> byte, a
    // byte cutoff 2.5 stored as 2); PIET made every one a float.
    const std::string srcType = storage::resolveReplaceType(session, src);
    storage::appendStoreTypeArg(args, srcType);
    if (qualified) {
      // Work on scratch frames until all variables are ready to commit. The ADO
      // re-evaluates if for each source, including after an earlier replace.
      RowIndexCache sample;
      sample.path = currentHbin;
      if (hasIf) {
        if (auto error = addIfMaskColumn(session, sample, *command->ifExpr, {})) return *error;
      }
      Datastore read(sample.path);
      const auto [lo, hi] = clampBoundaries(read, src, cutLo, cutHi, sample.maskVar,
                                           hasIn ? range.a - 1 : 0, hasIn ? range.b : read.nRows());
      if (std::isnan(lo)) {
        return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt,
                                      std::nullopt, std::nullopt, "r(2000)"));
      }
      postedCuts = {lo, hi};
      const std::string left = fmtNum(lo);
      const std::string right = fmtNum(hi);
      const std::string expression = trim
          ? "cond(" + src + "<" + left + "|" + src + ">" + right + ",.," + src + ")"
          : "max(min(" + src + "," + right + ")," + left + ")";
      std::string mask = "!missing(" + src + ")";
      if (hasIf) mask += "&" + sample.maskVar;
      args = {"eval", "--data", sample.path, "--gen", outName + "=" + expression,
              "--gen-mask", mask};
      if (hasIn) {
        args.insert(args.end(), {"--gen-in", std::to_string(range.a) + "/" + std::to_string(range.b)});
      }
      if (replace) args.push_back("--gen-replace");
      if (hasIf) args.insert(args.end(), {"--drop", sample.maskVar});
      storage::appendStoreTypeArg(args, srcType);
      args.insert(args.end(), {"--out", outPath});
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
      }
    } else if (!trim) {
      if (command && &src == &varlist.back()) {
        Datastore* ds = session.datastore();
        postedCuts = clampBoundaries(*ds, src, cutLo, cutHi, "", 0, ds->nRows());
      }
      std::optional<SessionState::ResidentEligible> winsorResident;
      if (residentWinsor) winsorResident.emplace(session);
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
      }
    } else {
      // Only a value strictly outside the cutoffs goes missing: StataBE keeps
      // the values AT them (uni-oracle/tm.do on AER 007's expectcomm_ehai_amount,
      // `cuts(0 95) trim`: 10 trimmed, max 80 = p95, min 5 = p0), where the old
      // pass dropped every clamped value and so both cutoffs (22). Read from the
      // original values, so `replace` trims its own variable the same way.
      Datastore preDs(currentHbin);
      auto [loVal, hiVal] = clampBoundaries(preDs, src, cutLo, cutHi, "", 0, preDs.nRows());
      if (command) postedCuts = {loVal, hiVal};
      if (!std::isnan(loVal) && !std::isnan(hiVal)) {
        std::string trimExpr = outName + "=cond((" + src + ">=" + fmtNum(loVal) + "&" + src +
                                "<=" + fmtNum(hiVal) + ")," + src + ",.)";
        std::string trimPath = nextPathW(session.workdir(), "winsor2_trim");
        std::vector<std::string> trimArgs = {"eval", "--data", currentHbin, "--gen", trimExpr,
                                              "--out", trimPath};
        storage::appendStoreTypeArg(trimArgs, srcType);
        try {
          runCobraJson(trimArgs);
          outPath = trimPath;
        } catch (const CobraError& ce) {
          return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
        }
      } else {
        // No cutoffs (no nonmissing value): the winsor pass makes the copy.
        try {
          runCobraJson(args);
        } catch (const CobraError& ce) {
          return engineErrorBlockW(cobraErrorStrW(ce), ce.code);
        }
      }
    }

    currentHbin = outPath;
    newVars.push_back(outName);
  }

  if (postedCuts && !std::isnan(postedCuts->first)) {
    // Newest results are listed first, so publish r2 before r1.
    session.eval.rValues.set("r2", postedCuts->second);
    session.eval.rValues.set("r1", postedCuts->first);
  }
  if (!replace) {
    for (const auto& src : varlist) storage::carryTypeClaim(session, src, src + suffix, false);
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
