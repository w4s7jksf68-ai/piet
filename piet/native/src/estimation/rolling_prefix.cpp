// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `rolling explist, window() saving(): command` -- the window-slice prefix.
// Shared design note of the family: the head of jackknife_permute_prefix.cpp.
#include "piet/handlers/resampling.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "bsample_draws.h"       // bsample's Stata draws (shared with the bsample command)
#include "core/ident.h"          // the shared Unicode name rule
#include "core/distributions.h"  // t_critical / t_twosided_pvalue / z_critical (shared with cobra)
#include "core/rng.h"            // Philox4x32-10, the engine's own counter-based RNG (permute)
#include "estimator_command_support.h"  // postBV / installEsampleMask / invsym / setE*OnFit
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "resampling_prefix_internal.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace resampling_detail;

// ---------------------------------------------------------------------------
// rolling  — `rolling explist, window(N) saving("FILE", replace): command`
//
// Same skeleton as the resampling prefixes: the only difference is that the
// per-"replicate" transform is a WINDOW SLICE of the time variable rather than
// a resample, so the inner command stays completely general.
//
// Requires a prior `tsset`/`xtset` (Stata's own precondition). Windows run over
// the DISTINCT observed time values, so a gap in the series shortens the span
// in calendar terms exactly as it does in Stata.
ojson runRolling(const std::string& expListRaw, const std::string& optsRaw,
                 const std::string& innerCmd, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError("rolling: no command found after the colon");
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return rsError("rolling: the data must be tsset (or xtset) first — rolling needs to know which "
                   "variable is time.");
  }
  const std::string tvar = *session.timevar;

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError("rolling: " + perr);

  int window = 0;
  std::string savingPath;
  bool savingReplace = false;
  {
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1; ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;
      }
      if (key.empty()) break;
      if (key == "window") {
        try { window = std::stoi(val); } catch (const std::exception&) {
          return rsError("rolling: window() needs a positive integer");
        }
      } else if (key == "saving") {
        // `saving("FILE", replace)`
        std::string v = val;
        size_t comma = v.find(',');
        if (comma != std::string::npos) {
          std::string tail = v.substr(comma + 1);
          for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          savingReplace = tail.find("replace") != std::string::npos;
          v = v.substr(0, comma);
        }
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                              (v.front() == '\'' && v.back() == '\''))) {
          v = v.substr(1, v.size() - 2);
        }
        savingPath = v;
      } else if (key == "nodots" || key == "dots" || key == "noisily") {
        // presentation-only
      } else {
        return rsError("rolling: option `" + key +
                       "` is not implemented in this build. Implemented: window(), saving().");
      }
    }
  }
  if (window <= 0) return rsError("rolling: window(#) is required and must be positive");
  if (savingPath.empty()) {
    // Without saving() Stata REPLACES the dataset in memory with the results.
    // That branch is not implemented here, and saying so is better than
    // quietly computing results the user never sees.
    return rsError("rolling: this build requires saving(\"FILE\") — the in-memory-replacement form "
                   "is not implemented.");
  }

  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("rolling: could not materialize the working dataset: ") + e.what());
  }
  const std::string startHbin = *session.currentHbin();
  SessionState::ScratchPin pin(session, {startHbin});

  // Distinct, sorted time values.
  std::vector<double> tvals;
  {
    Datastore d(startHbin);
    if (!d.has(tvar)) return rsError("variable " + tvar + " not found");
    const cecon::Column& c = d.col(tvar);
    if (c.dtype == cecon::DType::Factor) {
      return rsError("rolling: the time variable `" + tvar + "` must be numeric");
    }
    const double* p = c.f64();
    tvals.assign(p, p + static_cast<size_t>(d.nRows()));
    std::sort(tvals.begin(), tvals.end());
    tvals.erase(std::unique(tvals.begin(), tvals.end()), tvals.end());
  }
  if (static_cast<int>(tvals.size()) < window) {
    return rsError("rolling: the series has only " + std::to_string(tvals.size()) +
                   " periods, fewer than window(" + std::to_string(window) + ")");
  }

  std::vector<double> startCol, endCol;
  std::vector<std::vector<double>> statCols(expList.size());
  std::string failure;
  for (size_t i = 0; i + static_cast<size_t>(window) <= tvals.size() && failure.empty(); ++i) {
    const double t0 = tvals[i];
    const double t1 = tvals[i + static_cast<size_t>(window) - 1];
    const std::string slice = nextPath(session.workdir(), "roll_win");
    std::string cond = tvar + ">=" + fmtStataDefaultG(t0) + " & " + tvar +
                       "<=" + fmtStataDefaultG(t1);
    CobraRunResult cr =
        runCobra({"eval", "--data", startHbin, "--keepif", cond, "--out", slice});
    if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }

    session.load(slice);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") { innerFailed = true; break; }
    }
    std::vector<double> vals(expList.size(), std::nan(""));
    if (!innerFailed) {
      for (size_t k = 0; k < expList.size(); ++k) {
        std::optional<double> v = evalStat(expList[k].expr, session);
        if (v.has_value()) vals[k] = *v;
      }
    }
    startCol.push_back(t0);
    endCol.push_back(t1);
    for (size_t k = 0; k < expList.size(); ++k) statCols[k].push_back(vals[k]);
  }
  session.load(startHbin);
  if (!failure.empty()) return rsError("rolling: " + failure);
  if (startCol.empty()) return rsError("rolling: no windows were produced");

  // Results -> CSV -> hbin -> .dta, reproducing the shape Stata's own saved file
  // has (decoded from the oracle artifact, not re-derived from prose): start and
  // end as int %8.0g, each statistic as float %9.0g labelled with its own
  // expression, and the dataset labelled "rolling: <command word>".
  const std::string csv = session.workdir() + "/rolling_out.csv";
  registerScratchMint(csv);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return rsError("rolling: could not write the results file");
    f << "start,end";
    for (const auto& e : expList) f << "," << e.name;
    f << "\n";
    f.precision(17);
    for (size_t r = 0; r < startCol.size(); ++r) {
      f << startCol[r] << "," << endCol[r];
      for (size_t k = 0; k < expList.size(); ++k) {
        f << ",";
        // Stata stores rolling results as `float` (its default type), so the
        // saved values ARE float-rounded. We declare the same storage type in
        // the sidecar, so round to it here too — otherwise the writer sees a
        // value that a float cannot hold losslessly, promotes the column to
        // double to avoid changing it, and our file would differ from Stata's
        // in the eighth digit while claiming the same declared type.
        if (!std::isnan(statCols[k][r])) {
          f << static_cast<double>(static_cast<float>(statCols[k][r]));
        }
      }
      f << "\n";
    }
  }
  const std::string outHbin = nextPath(session.workdir(), "rolling_res");
  {
    CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", outHbin, "--delim", ","});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }
  const std::string metaPath = session.workdir() + "/rolling_meta.json";
  registerScratchMint(metaPath);  // BUG-389
  {
    std::string cmdWord = innerCmd.substr(0, innerCmd.find(' '));
    ojson meta;
    meta["data_label"] = "rolling: " + cmdWord;
    ojson vars = ojson::array();
    auto var = [&](const std::string& n, const std::string& t, const std::string& fmt,
                   const std::string& lbl) {
      vars.push_back(ojson{{"name", n}, {"stata_type", t}, {"format", fmt}, {"label", lbl}});
    };
    var("start", "int", "%8.0g", "");
    var("end", "int", "%8.0g", "");
    for (const auto& e : expList) var(e.name, "float", "%9.0g", e.expr);
    meta["variables"] = vars;
    std::ofstream mf(cecon::to_native_path(metaPath));
    if (!mf) return rsError("rolling: could not write the result metadata");
    mf << meta.dump(1);
  }
  if (!savingReplace && cecon::native_exists(savingPath)) {
    return rsError("file " + savingPath + " already exists");
  }
  {
    CobraRunResult cr = runCobra({"export-dta", "--data", outHbin, "--out", savingPath,
                                  "--in-meta", metaPath});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }

  ojson lines = ojson::array();
  lines.push_back(ojson{{"type", "display"},
                        {"text", "(running " + innerCmd.substr(0, innerCmd.find(' ')) +
                                     " on estimation sample)"}});
  lines.push_back(ojson{{"type", "display"},
                        {"text", "file " + savingPath + " saved"}});
  return lines;
}

}  // namespace handlers
}  // namespace piet
