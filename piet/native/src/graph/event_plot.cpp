// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/display_hardening.h"
#include "piet/handlers/graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle -- the ONE layer-style rule
#include "graph_extension_internal.h"

namespace {

// event_plot. `alpha()` becomes coefplot's `level()`; `stub_lag`/`stub_lead`
// select and order the coefficients; `together` and the four cosmetic options
// are read to build the note. `shift`/`trimlag`/`trimlead` change WHICH
// coefficients are plotted and `savecoef`/`perturb` change what is stored --
// all five are numbers-or-content, not cosmetics, so they are refused by name.
const piet::eval::OptVocab& kVocabEventPlot = piet::eval::registerOptVocab(
    {"event_plot"}, {{"stub_lag", "stub_lead", "together", "plottype", "ciplottype", "graph_opt",
                      "noautolegend", "alpha", "name"},
                     {"shift", "trimlag", "trimlead", "savecoef", "perturb"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace graph_extension_detail;

// ===========================================================================
// event_plot
// ===========================================================================
//
// A shim on `runCoefplot`: build the ordinary coefficient plot from the stored
// estimates, then keep only the coefficients whose names match `stub_lag()` /
// `stub_lead()` and put them in RELATIVE-TIME order -- leads most-negative
// first, then lags ascending. `#` in a stub is the horizon number, so
// `stub_lag(x#)` matches x0, x1, x2 and `stub_lead(F#event)` matches
// F1event, F2event.
//
// Filtering here rather than through coefplot's own `keep()` is deliberate:
// `keep()` filters but does not REORDER (coefplot_estimates_block.cpp's loop walks the
// coeftable in its stored order), and an event plot whose horizons are in
// coeftable order is a different picture from one in event time.
namespace {

// `pattern` with `#` standing for a non-negative integer. Returns the horizon.
bool matchStub(const std::string& pattern, const std::string& name, long long* horizon) {
  const size_t hash = pattern.find('#');
  std::string prefix, suffix;
  if (hash == std::string::npos) {
    prefix = pattern;  // no `#`: the digits are whatever follows the stub
  } else {
    prefix = pattern.substr(0, hash);
    suffix = pattern.substr(hash + 1);
  }
  if (name.size() <= prefix.size() + suffix.size()) return false;
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  if (!suffix.empty() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
    return false;
  const std::string mid =
      name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (mid.empty()) return false;
  for (char c : mid) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  long long v = 0;
  if (!parseStrictInt(mid, &v)) return false;
  *horizon = v;
  return true;
}

}  // namespace

ojson runEventPlot(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "event_plot";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEventPlot.known,
                                            kVocabEventPlot.unimplemented))
    return ojson(*err);

  const std::optional<std::string> stubLag = optStr(parsed.opts, "stub_lag");
  const std::optional<std::string> stubLead = optStr(parsed.opts, "stub_lead");
  if ((!stubLag.has_value() || stubLag->empty()) && (!stubLead.has_value() || stubLead->empty())) {
    return gxSyntax(kCmd +
                    ": at least one of stub_lag() and stub_lead() is required — they name the "
                    "coefficients to plot, e.g. `stub_lag(x#) stub_lead(F#x)`.");
  }

  // `alpha(a)` is a significance level; coefplot speaks confidence levels.
  std::vector<std::pair<std::string, eval::OptValue>> fwd = parsed.opts;
  if (auto al = optStr(parsed.opts, "alpha")) {
    double a = 0.0;
    if (!parseStrictDouble(*al, &a) || !(a > 0.0) || !(a < 1.0)) {
      return gxSyntax(kCmd + ": alpha() must be between 0 and 1 (e.g. alpha(0.05)), got `" + *al +
                      "'.");
    }
    const long long level = static_cast<long long>(std::llround((1.0 - a) * 100.0));
    if (level < 1 || level > 99) {
      return gxSyntax(kCmd + ": alpha(" + *al +
                      ") gives a confidence level outside 1-99; use an alpha between 0.01 and "
                      "0.99.");
    }
    fwd.emplace_back("level", eval::OptValue::ofString(std::to_string(level)));
  }

  ojson block = ojson(runCoefplot(parsed.varlist, fwd, session));
  if (isErrorBlock(block)) return renameDelegateError(block, kCmd, "coefplot");
  if (!block.is_object() || !block.contains("models") || !block["models"].is_array())
    return gxError(kCmd + ": internal — `coefplot` returned no models.", "internal");

  // Keep + reorder, per model.
  std::vector<double> allValues;
  int64_t kept = 0;
  for (auto& model : block["models"]) {
    if (!model.is_object() || !model.contains("coefs") || !model["coefs"].is_array()) continue;
    std::vector<std::pair<long long, ojson>> picked;  // (relative time, coefficient)
    for (const auto& c : model["coefs"]) {
      const std::string name = c.value("name", std::string());
      long long h = 0;
      bool hit = false;
      long long rel = 0;
      if (stubLead.has_value() && !stubLead->empty() && matchStub(*stubLead, name, &h)) {
        rel = -h;  // a lead is BEFORE the event
        hit = true;
      } else if (stubLag.has_value() && !stubLag->empty() && matchStub(*stubLag, name, &h)) {
        rel = h;
        hit = true;
      }
      if (!hit) continue;
      picked.emplace_back(rel, c);
    }
    // Stable: two coefficients at the same relative time (a lag 0 and a lead 0,
    // say) keep the coeftable's own order rather than a sort's internals.
    std::stable_sort(picked.begin(), picked.end(),
                     [](const std::pair<long long, ojson>& a,
                        const std::pair<long long, ojson>& b) { return a.first < b.first; });
    ojson coefs = ojson::array();
    for (auto& [rel, c] : picked) {
      ojson cc = c;
      cc["rel_time"] = rel;  // what the ordering means, carried with the point
      coefs.push_back(cc);
      for (const char* key : {"b", "ci_lo", "ci_hi"}) {
        if (cc.contains(key) && cc[key].is_number()) {
          const double v = cc[key].get<double>();
          if (std::isfinite(v)) allValues.push_back(v);
        }
      }
      ++kept;
    }
    model["coefs"] = coefs;
  }

  if (kept == 0) {
    return gxError(kCmd + ": no coefficients match stub_lag()/stub_lead()");
  }

  // The x range coefplot computed covered coefficients this command dropped;
  // recompute it over what actually survives, or the plot is padded around
  // values it does not draw.
  if (block.contains("xline") && block["xline"].is_number()) {
    const double xl = block["xline"].get<double>();
    if (std::isfinite(xl)) allValues.push_back(xl);
  }
  if (!allValues.empty()) {
    double lo = *std::min_element(allValues.begin(), allValues.end());
    double hi = *std::max_element(allValues.begin(), allValues.end());
    const double span = hi - lo;
    const double pad = span > 0 ? span * 0.05 : 0.5;
    block["x_min"] = lo - pad;
    block["x_max"] = hi + pad;
  }

  // Accepted, not applied -- named, never silent (defect class 3). NOTE: the
  // coefplot renderer does not draw a block `note` today, so this reaches the
  // block JSON and not the figure; making it visible is a frontend change and
  // is reported rather than done.
  const std::vector<std::string> cosmetic =
      presentOpts(parsed.opts, {"plottype", "ciplottype", "graph_opt", "noautolegend"});
  if (!cosmetic.empty()) {
    appendBlockNote(block, "(" + kCmd + ": " + joinWith(cosmetic, ", ") +
                               " accepted and not applied — this build draws the coefficient plot "
                               "the renderer already has)");
  }
  if (!hasFlag(parsed.opts, "together")) {
    appendBlockNote(block, "(" + kCmd +
                               ": lags and leads are drawn as ONE relative-time series in this "
                               "build, as `together` asks for; separate series are not available)");
  }
  return block;
}

}  // namespace piet::handlers
