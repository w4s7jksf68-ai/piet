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
// select and order the coefficients; `together` and the cosmetic options
// are read to build the note. `shift` moves the event time of every coefficient and
// `trimlag` / `trimlead` leave out the lags / leads beyond that horizon; `savecoef` writes the
// coefficients into the dataset, which is content, not cosmetics, and is refused by name.
const piet::eval::OptVocab& kVocabEventPlot = piet::eval::registerOptVocab(
    {"event_plot"}, {{"stub_lag", "stub_lead", "together", "plottype", "ciplottype", "graph_opt",
                      "noautolegend", "alpha", "name", "shift", "trimlag", "trimlead", "default_look",
                      "lag_opt", "lead_opt", "lag_ci_opt", "lead_ci_opt", "legend_opt", "perturb"},
                     {"savecoef"}});

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

  // A model name that is not a stored estimation result is r(111) before the stubs
  // are read (bank: `event_plot nosuchmodel`).
  for (const std::string& nm : parsed.varlist) {
    if (nm == "." || nm == "||" || nm.find_first_of("*?(") != std::string::npos) continue;
    const bool stored = std::any_of(session.estimates.begin(), session.estimates.end(),
                                    [&](const auto& kv) { return kv.first == nm; });
    if (!stored) {
      return ojson(eval::errorBlock("engine_error", "estimation result " + nm + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
  }
  // `plot` is not an abbreviation of plottype() and `plottype()` takes a twoway
  // plot type (bank: `plot(connected)` r(198) "option plot() incorrectly specified",
  // `plottype(zzz)` r(198) "zzz is not a twoway plot type"); inside graph_opt() the
  // twoway options are read with their own minimums: `n(`, `na(`, `nam(` are not name().
  {
    const long long comma = eval::findUnquotedComma(parsed.raw);
    if (comma >= 0) {
      std::string tail = parsed.raw.substr(static_cast<size_t>(comma) + 1);
      int depth = 0;
      std::string word;
      for (size_t i = 0; i <= tail.size(); ++i) {
        const char ch = i < tail.size() ? tail[i] : ' ';
        if (depth == 0 && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
          word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
          if (depth == 0 && word == "plot" && ch == '(') {
            return gxSyntax("option plot() incorrectly specified");
          }
          word.clear();
        }
        if (ch == '(') ++depth;
        if (ch == ')' && depth > 0) --depth;
      }
    }
    if (auto go = optStr(parsed.opts, "graph_opt")) {
      std::string w;
      int depth = 0;
      for (size_t i = 0; i <= go->size(); ++i) {
        const char ch = i < go->size() ? (*go)[i] : ' ';
        if (depth == 0 && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
          w.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
          if (depth == 0 && ch == '(' && (w == "n" || w == "na" || w == "nam")) {
            return gxSyntax("option " + w + "() not allowed");
          }
          w.clear();
        }
        if (ch == '(') ++depth;
        if (ch == ')' && depth > 0) --depth;
      }
    }
    if (auto pt = optStr(parsed.opts, "plottype")) {
      static const char* kTypes[] = {"scatter", "line", "connected", "scatteri", "area", "bar",
                                     "spike", "dropline", "dot", "rarea", "rbar", "rspike", "rcap",
                                     "rcapsym", "rscatter", "rline", "rconnected", "pcspike",
                                     "pccapsym", "pcarrow", "pcbarrow", "pcscatter", "tsline",
                                     "tsrline", "lowess", "lpoly", "lfit", "qfit", "function",
                                     "histogram", "kdensity"};
      const std::string t = trimOf(*pt);
      bool known = false;
      for (const char* k : kTypes) {
        const std::string full(k);
        if (t == full || (t.size() >= 2 && t.size() < full.size() && full.compare(0, t.size(), t) == 0))
          known = true;
      }
      if (!known) return gxSyntax(t + " is not a twoway plot type");
    }
  }

  const std::optional<std::string> stubLag = optStr(parsed.opts, "stub_lag");
  const std::optional<std::string> stubLead = optStr(parsed.opts, "stub_lead");
  if ((!stubLag.has_value() || stubLag->empty()) && (!stubLead.has_value() || stubLead->empty())) {
    return gxSyntax(kCmd +
                    ": at least one of stub_lag() and stub_lead() is required — they name the "
                    "coefficients to plot, e.g. `stub_lag(x#) stub_lead(F#x)`.");
  }

  // shift(#): event time moves by # periods; trimlag(#) / trimlead(#): lags / leads beyond # are left out
  // (each an integer; bank: `event_plot, ... together shift(-1)`, `trimlead(1) trimlag(1)` rc 0)
  auto intOpt = [&](const char* key, long long* out) -> std::optional<ojson> {
    const auto text = optStr(parsed.opts, key);
    if (!text.has_value()) return std::nullopt;
    long long v = 0;
    if (!parseStrictInt(trimOf(*text), &v)) return gxSyntax(kCmd + ": option " + key + "() incorrectly specified");
    *out = v;
    return std::nullopt;
  };
  long long shiftBy = 0, trimLag = -1, trimLead = -1;
  if (auto bad = intOpt("shift", &shiftBy)) return *bad;
  if (auto bad = intOpt("trimlag", &trimLag)) return *bad;
  if (auto bad = intOpt("trimlead", &trimLead)) return *bad;
  const bool hasTrimLag = optStr(parsed.opts, "trimlag").has_value();
  const bool hasTrimLead = optStr(parsed.opts, "trimlead").has_value();

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
  if (isErrorBlock(block)) {
    // bank: `event_plot, stub_lag(L#) stub_lead(F#)` with nothing estimated is
    // r(301) "last estimates not found".
    if (block.value("message", std::string()).find("no stored estimates") != std::string::npos) {
      return ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                    std::nullopt, std::nullopt, "r(301)"));
    }
    return renameDelegateError(block, kCmd, "coefplot");
  }
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
      if (hasTrimLag && rel > trimLag) continue;
      if (hasTrimLead && -rel > trimLead) continue;
      picked.emplace_back(rel + shiftBy, c);
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
    // bank: `event_plot, stub_lag(Z#)` r(498)
    return ojson(eval::errorBlock(
        "engine_error",
        "No estimates found. Make sure you have specified stub_lag and stub_lead correctly.", std::nullopt,
        std::nullopt, std::nullopt, "r(498)"));
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
      presentOpts(parsed.opts, {"plottype", "ciplottype", "graph_opt", "noautolegend", "default_look",
                                "lag_opt", "lead_opt", "lag_ci_opt", "lead_ci_opt", "legend_opt", "perturb"});
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
