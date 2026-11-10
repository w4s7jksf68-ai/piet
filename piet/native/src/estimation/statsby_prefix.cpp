// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `statsby _b _se, clear [double]: command` -- the ungrouped statsby prefix.
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

// `statsby _b _se, clear: command` — the UNGROUPED form (no by()): the command runs
// once on the data in memory and the dataset is replaced by one observation holding
// the collected statistics (w2d_uncovered.log:3422 xtreg_statsby_fe_028
// `|0|1|4|1.981762409210205|.3451376259326935|`).
//  - Names follow the prefix name expansion (statsby calls it with
//    stub(_stat_)): id = <stat><name>, the name prefixed with `_` unless it already
//    starts with one, so x1 -> _b_x1 and _cons -> _b_cons (never _b__cons). An id that
//    is not a valid name (a factor-level column such as 1b.g) falls back to
//    _stat_<i>, and a repeated id gets the index appended, as there.
//  - Storage: statsby posts with `double` only when asked, so every
//    statistic is a float by default — the oracle values are float(_b[x1]) and
//    float(_se[x1]) of the unprefixed fit (log:3460 1.981762433275536 /
//    .345137615018114).
// by(), saving(), total, subsets and arbitrary expressions are not implemented and
// are refused by name before the command runs; a multi-equation e(b) likewise.
ojson runStatsby(const std::string& expListRaw, const std::string& optsRaw,
                 const std::string& innerCmd, SessionState& session) {
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  auto splitWs = [](const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
  };
  if (splitWs(innerCmd).empty()) return rsError("statsby: no command found after the colon");
  bool clear = false, asDouble = false;
  for (const auto& word : splitWs(optsRaw)) {
    std::string w = word;
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "clear") clear = true;
    else if (w == "double") asDouble = true;
    else if (w == "nodots" || w == "noisily" || w == "nolegend" || w == "verbose") continue;
    else
      return rsError("statsby: option `" + word +
                         "` is a valid Stata option that this version of PIET does not implement "
                         "(only the ungrouped `statsby _b _se, clear [double]: command` form is)",
                     "unsupported_option");
  }
  std::vector<std::string> statistics;
  for (const auto& word : splitWs(expListRaw)) {
    if (word != "_b" && word != "_se")
      return rsError("statsby: `" + word +
                         "` — this version of PIET collects the _b and _se vectors only",
                     "unsupported_option");
    if (std::find(statistics.begin(), statistics.end(), word) == statistics.end())
      statistics.push_back(word);
  }
  if (statistics.empty()) statistics.push_back("_b");  // statsby's default exp_list
  // statsby.ado replaces the data in memory, which Stata refuses without `clear`
  // when the data have changed since they were last saved (r(4)).
  if (!clear)
    return ojson(eval::errorBlock("engine_error", "no; dataset in memory has changed since last saved",
                                  std::nullopt, std::nullopt, std::nullopt, "r(4)"));

  const ojson result = dispatch(innerCmd, session);
  for (const auto& block : result)
    if (block.is_object() && block.value("type", std::string()) == "error") return block;
  const auto b = session.eval.resolveEMatrix("b");
  if (!b || b->data.size() != 1 || b->data[0].size() != b->colnames.size() ||
      b->colnames.empty())
    return ojson(eval::errorBlock("engine_error", "statsby: the command did not post e(b)",
                                  std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  for (const auto& cn : b->colnames)
    if (cn.find(':') != std::string::npos)
      return rsError("statsby: a multi-equation e(b) is not implemented in this version of PIET",
                     "unsupported_option");
  std::optional<eval::EvalMatrix> v;
  if (std::find(statistics.begin(), statistics.end(), "_se") != statistics.end()) {
    v = session.eval.resolveEMatrix("V");
    if (!v || v->data.size() != b->colnames.size())
      return ojson(eval::errorBlock("engine_error", "statsby: the command did not post e(V)",
                                    std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  }
  auto validName = [](const std::string& s) { return cecon::ident::isValidName(s); };
  std::vector<std::string> names, legend;
  std::vector<double> values;
  size_t idx = 0;
  for (const auto& statistic : statistics) {
    for (size_t j = 0; j < b->colnames.size(); ++j) {
      ++idx;
      const std::string& na = b->colnames[j];
      std::string id = statistic + (na.rfind('_', 0) == 0 ? na : "_" + na);
      if (std::find(names.begin(), names.end(), id) != names.end()) id += std::to_string(idx);
      if (!validName(id)) id = "_stat_" + std::to_string(idx);
      double value = b->data[0][j];
      if (statistic == "_se") {
        const double vjj = j < v->data[j].size() ? v->data[j][j] : std::nan("");
        value = (std::isfinite(vjj) && vjj >= 0) ? std::sqrt(vjj) : std::nan("");
      }
      names.push_back(id);
      legend.push_back(statistic + "[" + na + "]");
      values.push_back(asDouble || !std::isfinite(value)
                           ? value
                           : static_cast<double>(static_cast<float>(value)));
    }
  }

  const std::string out = nextPath(session.workdir(), "statsby");
  const std::string csv = out + ".csv";
  registerScratchMint(csv);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return rsError("statsby: could not write the results file");
    f.precision(17);
    for (size_t j = 0; j < names.size(); ++j) f << (j ? "," : "") << names[j];
    f << '\n';
    for (size_t j = 0; j < values.size(); ++j) {
      if (j) f << ',';
      if (std::isfinite(values[j])) f << values[j];
    }
    f << '\n';
    if (!f) return rsError("statsby: could not write the results file");
  }
  const CobraRunResult converted = runCobra({"convert", "--data", csv, "--out", out, "--delim", ","});
  if (!converted.ok) return rsError("statsby: " + cobraErrorStr(errorFromRaw(converted)));
  session.clearMetadata();
  session.load(out);
  for (const auto& name : names) session.varMeta[name].stataType = asDouble ? "double" : "float";

  ojson lines = ojson::array();
  lines.push_back(ojson{{"type", "display"}, {"text", "      Command: " + innerCmd}});
  for (size_t j = 0; j < names.size(); ++j) {
    std::string pad = names[j].size() < 13 ? std::string(13 - names[j].size(), ' ') : std::string();
    lines.push_back(ojson{{"type", "display"}, {"text", pad + names[j] + ": " + legend[j]}});
  }
  return lines;
}

}  // namespace handlers
}  // namespace piet
