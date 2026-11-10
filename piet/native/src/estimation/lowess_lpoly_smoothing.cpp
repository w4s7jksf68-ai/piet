// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// lowess and lpoly: the smoother command layer around the engine's fits.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// ---------------------------------------------------------------------------
// lpoly / lowess (goal gfx-lpoly-lowess, oracle aer_w2:w2ae_lpoly_lowess)
//
// The numbers come from the engine; what lives here is the command layer: option
// validation with Stata's return codes, the if/in/weight sample handed to the
// engine as a MASK (rows keep their positions), generate() written back by row,
// r(), and the graph block when the graph is drawn.

namespace {

// A title-like option's text: `"a" "b", size(small)` -> "a b". The part after
// the first top-level comma is suboptions (size, color, ...), which the block
// cannot express; several quoted strings are Stata's multi-line form.
std::string smoothUnquote(std::string s) {
  s = strutil::trim(s);
  std::string head;
  int depth = 0;
  bool inQ = false;
  for (char c : s) {
    if (c == '"') inQ = !inQ;
    else if (!inQ && c == '(') ++depth;
    else if (!inQ && c == ')') --depth;
    else if (!inQ && depth == 0 && c == ',') break;
    head += c;
  }
  head = strutil::trim(head);
  if (head.find('"') == std::string::npos) return head;
  std::string out;
  std::size_t i = 0;
  while ((i = head.find('"', i)) != std::string::npos) {
    const std::size_t j = head.find('"', i + 1);
    if (j == std::string::npos) break;
    if (!out.empty()) out += " ";
    out += head.substr(i + 1, j - i - 1);
    i = j + 1;
  }
  return out;
}

// `confirm new var NAME`: r(110) when it exists, r(198) when it is not a name.
std::optional<ojson> smoothConfirmNew(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name)) return aerErrorReason(name + " invalid name", 198);
  // BUG-1120 review: `lowess y x, gen(_cons)', `lpoly y x, gen(int g)' are "NAME
  // invalid varname" r(198) (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(name)) return aerErrorReason(r->message, r->rc);
  Datastore* ds = session.datastore();
  if (ds != nullptr && ds->has(name)) return aerErrorReason("variable " + name + " already defined", 110);
  return std::nullopt;
}

// The if/in qualifiers as engine flags. `in` is resolved here to literal rows.
std::optional<ojson> smoothSampleArgs(const eval::ParsedCommand& parsed, SessionState& session,
                                      const std::string& cmd, std::vector<std::string>* args) {
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args->push_back("--keepif");
    args->push_back(*parsed.ifExpr);
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = session.datastore();
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return inRangeRejectError(bad, *parsed.inRange, cmd);
    }
    args->push_back("--inrange");
    args->push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
  }
  return std::nullopt;
}

// An engine refusal carrying one of the return codes Stata's lpoly gives.
ojson smoothEngineError(const std::string& cmd, const CobraError& ce) {
  const std::string msg = cobraErrorMessage(ce);
  if (msg.find("insufficient observations") != std::string::npos)
    return aerErrorReason("insufficient observations", 2001);
  if (msg.find("no observations") != std::string::npos) return aerErrorReason("no observations", 2000);
  if (msg.find("negative weights encountered") != std::string::npos)
    return aerErrorReason("negative weights encountered", 402);
  return aerError(cmd + ": " + msg, ce.code);
}

// Write a generated column as Stata's lpoly/lowess create it: `gen double`,
// with its variable label (empty = none).
bool smoothWriteColumn(SessionState& session, const std::string& name, const std::vector<double>& v,
                       const std::string& label, ojson* err) {
  if (!estimWriteColumn(session, name, v, err)) return false;
  session.implicitFloatVars.erase(name);
  session.varMeta[name].stataType = "double";
  if (!label.empty()) session.setVarLabel(name, label);
  return true;
}

std::string smoothVarTitle(SessionState& session, const std::string& v) {
  session.syncEvalVarLabels();
  std::string l = session.eval.varLabels.get(v, std::string());
  return l.empty() ? v : l;
}

// The RIGHTMOST occurrence: a repeated option is Stata's "last one wins".
const eval::OptValue* smoothOpt(const eval::ParsedCommand& parsed, const std::string& key) {
  const eval::OptValue* found = nullptr;
  for (const auto& kv : parsed.opts)
    if (kv.first == key) found = &kv.second;
  return found;
}

// Stata's string(round(x, 0.01)), the form of lpoly's graph note.
std::string smoothRound2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", std::round(v * 100.0) / 100.0);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s.rfind("0.", 0) == 0) s = s.substr(1);
  if (s.rfind("-0.", 0) == 0) s = "-" + s.substr(2);
  return s;
}

// The drawn graph: the scatter of the sample (unless noscatter), the CI band
// (lpoly ci) and the smooth line, with Stata's default title/axis titles/
// note, each replaced by the user's option. Every option this does not read is
// named by the dispatcher's graphAcceptedNotApplied note.
ojson smoothGraphBlock(const eval::ParsedCommand& parsed, SessionState& session,
                       const std::string& defTitle, const std::string& defNote,
                       const std::string& yname, const std::string& xname,
                       const std::vector<std::pair<double, double>>& scatterPts,
                       const std::vector<std::array<double, 3>>& band,
                       const std::vector<std::pair<double, double>>& linePts) {
  static const std::vector<std::pair<std::string, eval::OptValue>> kNoLayerOpts;
  ojson layers = ojson::array();
  double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin;
  double yMin = xMin, yMax = -xMin;
  auto see = [&](double x, double y) {
    xMin = std::min(xMin, x);
    xMax = std::max(xMax, x);
    yMin = std::min(yMin, y);
    yMax = std::max(yMax, y);
  };
  if (!band.empty()) {
    ojson b = ojson::array();
    for (const auto& r : band) {
      b.push_back(ojson::array({r[0], r[1], r[2]}));
      see(r[0], r[1]);
      see(r[0], r[2]);
    }
    ojson lyr;
    lyr["kind"] = "rarea";
    lyr["band"] = b;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "rarea");
    lyr["n"] = static_cast<int64_t>(band.size());
    layers.push_back(lyr);
  }
  if (!scatterPts.empty()) {
    ojson p = ojson::array();
    for (const auto& [x, y] : scatterPts) {
      p.push_back(ojson::array({x, y}));
      see(x, y);
    }
    ojson lyr;
    lyr["kind"] = "scatter";
    lyr["points"] = p;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "scatter");
    lyr["n"] = static_cast<int64_t>(scatterPts.size());
    layers.push_back(lyr);
  }
  {
    ojson p = ojson::array();
    for (const auto& [x, y] : linePts) {
      p.push_back(ojson::array({x, y}));
      see(x, y);
    }
    ojson lyr;
    lyr["kind"] = "line";
    lyr["points"] = p;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "line");
    lyr["n"] = static_cast<int64_t>(linePts.size());
    layers.push_back(lyr);
  }
  if (!std::isfinite(xMin)) {
    xMin = 0.0;
    xMax = 1.0;
    yMin = 0.0;
    yMax = 1.0;
  }
  auto optText = [&](const std::string& key, const std::string& dflt) {
    const eval::OptValue* v = smoothOpt(parsed, key);
    if (v == nullptr || v->isBool) return dflt;
    return smoothUnquote(v->strValue);
  };
  ojson block;
  block["type"] = "graph";
  block["kind"] = "twoway";
  block["title"] = optText("title", defTitle);
  block["subtitle"] = std::string();
  block["x_label"] = optText("xtitle", smoothVarTitle(session, xname));
  block["y_label"] = optText("ytitle", smoothVarTitle(session, yname));
  block["note"] = optText("note", defNote);
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  block["y_min"] = yMin;
  block["y_max"] = yMax;
  block["layers"] = layers;
  ojson axes;
  axes["xline"] = ojson::array();
  axes["yline"] = ojson::array();
  axes["xscale_noline"] = false;
  axes["yscale_noline"] = false;
  axes["xlabel_none"] = false;
  axes["ylabel_none"] = false;
  block["axes"] = axes;
  ojson legend;
  legend["order"] = ojson::array();
  legend["cols"] = nullptr;
  legend["size"] = nullptr;
  legend["off"] = true;  // Stata draws no legend unless ci or legend() (noted, not applied)
  block["legend"] = legend;
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011)
  block["n_obs"] = static_cast<int64_t>(scatterPts.size());
  block["suppressed"] = session.graphicsOff;
  return block;
}

}  // namespace

namespace {

bool smoothCellMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

// The option spelled as at least `atLeast` leading characters of `full`; a repeated
// option answers with its FIRST occurrence (what both commands' computations use,
// measured: `lowess y x, bw(0.5) bw(0.4)` smooths with 0.5).
const eval::OptValue* smoothFirstOpt(const eval::ParsedCommand& parsed, const std::string& full, size_t atLeast) {
  for (const auto& [key, value] : parsed.opts)
    if (key.size() >= atLeast && key.size() <= full.size() && full.compare(0, key.size(), key) == 0) return &value;
  return nullptr;
}

std::string smoothOptText(const eval::OptValue* option) {
  return option == nullptr || option->isBool ? std::string() : strutil::trim(option->strValue);
}

std::vector<std::string> smoothBlankWords(const std::string& text) {
  std::istringstream in(text);
  std::vector<std::string> words;
  std::string word;
  while (in >> word) words.push_back(word);
  return words;
}

// A number written as a number: digits, sign, point and exponent only, so `abc`,
// `.`, `0.5+0.1`, `inf` and hex forms are not numbers here.
bool smoothPlainNumber(const std::string& text, double* value) {
  if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string::npos) return false;
  char* stop = nullptr;
  *value = std::strtod(text.c_str(), &stop);
  return stop != text.c_str() && *stop == '\0';
}

bool smoothHasOperator(const std::string& token) { return token.find('.') != std::string::npos; }

bool smoothColumnExists(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

std::optional<ojson> smoothBindYorX(SessionState& session, const std::string& token, std::string* bound) {
  bool ambiguous = false;
  const std::optional<std::string> full = resolveVarName(token, session, &ambiguous);
  if (ambiguous) return aerErrorReason(token + " ambiguous abbreviation", 111);
  if (!full) return aerErrorReason("variable " + token + " not found", 111);
  if (!smoothIsNumeric(session, *full))
    return aerErrorReason("string variables not allowed in varlist;\n" + *full + " is a string variable", 109);
  *bound = *full;
  return std::nullopt;
}

std::optional<ojson> smoothDrawnNet(const eval::ParsedCommand& parsed, const std::string& cmd) {
  const eval::OptVocab* vocab = eval::findOptVocab(cmd);
  if (vocab == nullptr) return std::nullopt;
  if (auto bad = eval::rejectUnknownOptions(parsed.opts, vocab->known, vocab->unimplemented)) return ojson(*bad);
  return std::nullopt;
}

// N, sum_w, sum, mean, min, max: what `summarize, meanonly` leaves, posted over the
// rows `keep` admits (an empty mask admits every row).
std::vector<std::string> smoothPostSummary(SessionState& session, const std::vector<double>& values,
                                           const std::vector<int>& keep) {
  const double absent = std::numeric_limits<double>::quiet_NaN();
  cecon::ExactSum total;
  double count = 0.0;
  double lowest = absent, highest = absent;
  for (size_t r = 0; r < values.size(); ++r) {
    if ((!keep.empty() && keep[r] != 1) || smoothCellMissing(values[r])) continue;
    total.add(values[r]);
    lowest = count == 0.0 ? values[r] : std::min(lowest, values[r]);
    highest = count == 0.0 ? values[r] : std::max(highest, values[r]);
    count += 1.0;
  }
  auto& out = session.eval.rValues;
  out.set("N", count);
  out.set("sum_w", count);
  out.set("sum", total.value());
  out.set("mean", count > 0.0 ? total.value() / count : absent);
  out.set("min", lowest);
  out.set("max", highest);
  return {"N", "sum_w", "sum", "mean", "min", "max"};
}

std::vector<int> smoothSampleMask(const nlohmann::json& reply) {
  std::vector<int> mask;
  if (reply.contains("touse") && reply["touse"].is_array())
    for (const auto& cell : reply["touse"]) mask.push_back(cell.is_number() ? cell.get<int>() : 0);
  return mask;
}

std::vector<std::pair<double, double>> smoothPairsSortedByX(std::vector<std::pair<double, double>> pairs) {
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const auto& left, const auto& right) { return left.first < right.first; });
  return pairs;
}

// The sample rows as (x, y) points.
bool smoothSamplePoints(SessionState& session, const std::string& yVar, const std::string& xVar,
                        const std::vector<int>& mask, std::vector<std::pair<double, double>>* points, ojson* failure) {
  Datastore* ds = session.datastore();
  std::vector<double> ys, xs;
  std::string why;
  if (ds == nullptr || !numericColumn(*ds, yVar, &ys, &why) || !numericColumn(*ds, xVar, &xs, &why)) {
    *failure = aerError(why.empty() ? std::string("no dataset") : why);
    return false;
  }
  for (size_t r = 0; r < ys.size() && r < mask.size(); ++r)
    if (mask[r] == 1) points->emplace_back(xs[r], ys[r]);
  return true;
}

// generate() as lowess reads it. One word must be new and a name; a reserved word
// gets past this and fails once the smooth is done. More than one word is refused
// here: on the first existing word, else on the second.
std::optional<ojson> lowessGenerateRefusal(SessionState& session, const std::vector<std::string>& words) {
  if (words.size() == 1) {
    if (!strutil::isValidStataIdentifier(words[0])) return aerErrorReason(words[0] + " invalid name", 198);
    if (smoothColumnExists(session, words[0])) return aerErrorReason("variable " + words[0] + " already defined", 110);
    return std::nullopt;
  }
  for (const std::string& word : words)
    if (smoothColumnExists(session, word)) return aerErrorReason("variable " + word + " already defined", 110);
  if (words.size() >= 2) return aerErrorReason(words[1] + " not allowed", 101);
  return std::nullopt;
}

// A smooth with no neighbours is left missing: [R] lowess, Methods and formulas, has
// the half-window k = int((N * bwidth - 0.5) / 2), and Stata returns all missing below 1.
void lowessBlankThinWindow(double rows, double bandwidth, std::vector<double>* smooth) {
  if (std::floor((rows * bandwidth - 0.5) / 2.0) < 1.0)
    std::fill(smooth->begin(), smooth->end(), std::numeric_limits<double>::quiet_NaN());
}

// `adjust` rescales to the mean of every nonmissing y in the dataset (if/in play no
// part), then `logit` moves the extreme proportions to 1/N and 1-1/N before taking the
// log odds.
void lowessRescale(const std::vector<double>& yAll, double rows, bool adjust, bool logit,
                   std::vector<double>* smooth) {
  if (adjust) {
    cecon::ExactSum yTotal, smoothTotal;
    double yCount = 0.0, smoothCount = 0.0;
    for (double v : yAll)
      if (!smoothCellMissing(v)) {
        yTotal.add(v);
        yCount += 1.0;
      }
    for (double v : *smooth)
      if (!smoothCellMissing(v)) {
        smoothTotal.add(v);
        smoothCount += 1.0;
      }
    const double ratio = (yTotal.value() / yCount) / (smoothTotal.value() / smoothCount);
    for (double& v : *smooth)
      if (!smoothCellMissing(v)) v *= ratio;
  }
  if (logit) {
    for (double& v : *smooth) {
      if (smoothCellMissing(v)) continue;
      const double p = v < 0.0001 ? 1.0 / rows : (v > 0.9999 ? 1.0 - 1.0 / rows : v);
      v = std::log(p / (1.0 - p));
    }
  }
}

// One entry of the lpoly kernel table: how few leading characters name it.
struct LpolyKernelSpelling {
  const char* full;
  size_t atLeast;
};
const LpolyKernelSpelling kLpolyKernelTable[] = {{"epanechnikov", 2}, {"epan2", 5},  {"biweight", 2},
                                                 {"cosine", 3},       {"gaussian", 3}, {"parzen", 3},
                                                 {"rectangle", 3},    {"triangle", 3}};

std::string lpolyKernelSpelled(const std::string& word) {
  for (const LpolyKernelSpelling& entry : kLpolyKernelTable) {
    const std::string full = entry.full;
    if (word.size() >= entry.atLeast && word.size() <= full.size() && full.compare(0, word.size(), word) == 0)
      return full;
  }
  return std::string();
}

// kernel(name) and the older bare-word spelling, which may not be combined.
std::optional<ojson> lpolyChooseKernel(const eval::ParsedCommand& parsed, std::string* kernel) {
  std::vector<std::string> bareWords;
  for (const auto& [key, value] : parsed.opts) {
    if (!value.isBool) continue;
    const std::string named = lpolyKernelSpelled(key);
    if (!named.empty()) bareWords.push_back(named);
  }
  const std::string typed = smoothOptText(smoothFirstOpt(parsed, "kernel", 1));
  if (bareWords.size() > 1) return aerErrorReason("only one kernel may be specified", 198);
  if (bareWords.size() == 1 && !typed.empty())
    return aerErrorReason("kernel(): old syntax may not be combined with new syntax", 198);
  *kernel = bareWords.empty() ? std::string("epanechnikov") : bareWords[0];
  if (typed.empty()) return std::nullopt;
  if (typed.find_first_of(" \t") != std::string::npos)
    return aerErrorReason("only one kernel may be specified", 198);
  *kernel = lpolyKernelSpelled(typed);
  if (kernel->empty()) return aerErrorReason("invalid kernel function", 198);
  return std::nullopt;
}

std::optional<ojson> lpolyNewNameRefusal(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name)) return aerErrorReason(name + " invalid name", 198);
  if (smoothColumnExists(session, name)) return aerErrorReason("variable " + name + " already defined", 110);
  return std::nullopt;
}

// A bwidth() that is not a number may only name a numeric at()-companion variable,
// and that form itself is still PIET's named gap (BUG-1313).
std::optional<ojson> lpolyBandwidthRefusal(SessionState& session, const std::string& text, bool hasAt) {
  if (text.empty()) return std::nullopt;
  double width = 0.0;
  if (smoothPlainNumber(text, &width)) {
    if (width <= 0.0) return aerErrorReason("bwidth() must be a positive number", 198);
    return std::nullopt;
  }
  bool ambiguous = false;
  const std::optional<std::string> variable = resolveVarName(text, session, &ambiguous);
  if (!variable) return aerErrorReason("variable " + text + " not found", 111);
  if (!hasAt) return aerErrorReason("variable name in bwidth() is allowed only with at() option", 198);
  if (!smoothIsNumeric(session, *variable))
    return aerErrorReason("bwidth(): " + text + " is not a numeric variable", 198);
  return aerErrorReason("lpoly: a variable in bwidth() is not implemented in this build", 198);
}

// A column lpoly will add, once the fit is in hand.
struct LpolyOutput {
  std::string name;
  std::string label;
  std::vector<double> values;
  int addOrder;
};

std::vector<double> lpolyRowLength(std::vector<double> values, size_t rows) {
  values.resize(rows, std::numeric_limits<double>::quiet_NaN());
  return values;
}

}  // namespace

// `lowess yvar xvar [if] [in] [, options]`. The checks, the bandwidth reading, the r()
// that remains and the graph text were measured on StataBE 19.5; the fit is the
// engine's.
ojson runLowess(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lowess")) return engineTooOld("lowess", "lowess");
  if (parsed.weightType.has_value()) return aerErrorReason("weights not allowed", 101);
  if (parsed.varlist.size() < 2) return aerErrorReason("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return aerErrorReason("too many variables specified", 103);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string bound[2];
  for (int i = 0; i < 2; ++i)
    if (!smoothHasOperator(parsed.varlist[static_cast<size_t>(i)]))
      if (auto bad = smoothBindYorX(session, parsed.varlist[static_cast<size_t>(i)], &bound[i])) return *bad;
  if (bound[0].empty() || bound[1].empty()) {
    if (!session.timevar.has_value() || session.timevar->empty())
      return aerErrorReason("time variable not set", 111);
    return aerErrorReason("lowess: time-series operators are not implemented in this build", 198);
  }
  const std::string& yVar = bound[0];
  const std::string& xVar = bound[1];

  if (smoothOpt(parsed, "by") != nullptr)
    return aerErrorReason("lowess: option by() is not implemented in this build", 198);
  const bool drawn = smoothFirstOpt(parsed, "nograph", 3) == nullptr;
  if (drawn) {
    for (const char* graphOnly : {"plot", "addplot"})
      if (smoothOpt(parsed, graphOnly) != nullptr)
        return aerErrorReason(std::string("lowess: option ") + graphOnly + "() is not implemented in this build", 198);
    if (auto bad = smoothDrawnNet(parsed, "lowess")) return *bad;
  }
  const std::vector<std::string> outWords = smoothBlankWords(smoothOptText(smoothFirstOpt(parsed, "generate", 3)));
  if (auto bad = lowessGenerateRefusal(session, outWords)) return *bad;

  const bool wantMean = smoothFirstOpt(parsed, "mean", 1) != nullptr;
  const bool wantLogit = smoothFirstOpt(parsed, "logit", 2) != nullptr;
  const bool wantAdjust = smoothFirstOpt(parsed, "adjust", 1) != nullptr;

  std::vector<std::string> args = {"lowess", "--data", *session.currentHbin(), "--y", yVar, "--x", xVar, "--emit-touse"};
  // Anything but a plain number means the default; the engine itself replaces a
  // number outside (0,1) with 0.8.
  const std::string widthText = smoothOptText(smoothFirstOpt(parsed, "bwidth", 2));
  double ignored = 0.0;
  if (smoothPlainNumber(widthText, &ignored)) {
    args.push_back("--bw");
    args.push_back(widthText);
  }
  if (wantMean) args.push_back("--mean");
  if (smoothFirstOpt(parsed, "noweight", 3) != nullptr) args.push_back("--noweight");
  if (auto bad = smoothSampleArgs(parsed, session, "lowess", &args)) return *bad;

  nlohmann::json reply;
  try {
    reply = runCobraJson(args);
  } catch (const CobraError& ce) {
    return smoothEngineError("lowess", ce);
  }
  const double sampleRows = static_cast<double>(reply.value("n_obs", 0));
  const double bandwidth = reply.value("bwidth", 0.8);
  const std::vector<int> mask = smoothSampleMask(reply);
  std::vector<double> smooth = jsonDoubles(reply["smooth"]);
  lowessBlankThinWindow(sampleRows, bandwidth, &smooth);

  std::vector<double> xColumn, yColumn;
  {
    Datastore* ds = session.datastore();
    std::string why;
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (!numericColumn(*ds, xVar, &xColumn, &why) || !numericColumn(*ds, yVar, &yColumn, &why)) return aerError(why);
  }
  if (sampleRows > 0.0) lowessRescale(yColumn, sampleRows, wantAdjust, wantLogit, &smooth);

  // lowess is no r-class command, but what survives is the summary its last internal
  // step leaves: nothing when drawn; else the sample size alone after logit, y's
  // summary after adjust, x's over the sample otherwise.
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (!drawn) {
    if (sampleRows > 0.0 && wantLogit) {
      session.eval.rValues.set("N", sampleRows);
      posted = {"N"};
    } else if (sampleRows > 0.0 && wantAdjust) {
      posted = smoothPostSummary(session, yColumn, {});
    } else {
      posted = smoothPostSummary(session, xColumn, mask);
    }
  }
  eval::declareRPosted(session.eval, "lowess", posted);

  const std::string outName = outWords.empty() ? std::string() : outWords[0];
  if (!outName.empty())
    if (const auto reserved = strutil::newVarNameRefusal(outName)) return aerErrorReason(reserved->message, reserved->rc);

  ojson blocks = ojson::array();
  if (drawn) {
    std::vector<std::pair<double, double>> scatter;
    std::vector<std::pair<double, double>> line;
    for (size_t r = 0; r < smooth.size() && r < xColumn.size(); ++r)
      if (!smoothCellMissing(smooth[r])) line.emplace_back(xColumn[r], smooth[r]);
    for (size_t r = 0; !wantLogit && r < mask.size() && r < xColumn.size(); ++r)
      if (mask[r] == 1) scatter.emplace_back(xColumn[r], yColumn[r]);
    ojson graph = smoothGraphBlock(parsed, session, wantMean ? "Running mean smoother" : "Lowess smoother",
                                   "bandwidth = " + strutil::trim(eval::applyStataFmt(bandwidth, "%9.0g")), yVar, xVar,
                                   scatter, {}, smoothPairsSortedByX(std::move(line)));
    std::string subtitle = wantLogit ? "Logit transformed smooth" : "";
    if (wantAdjust) subtitle += (subtitle.empty() ? "" : " ") + std::string("Mean adjusted smooth");
    graph["subtitle"] = subtitle;
    graph["n_obs"] = static_cast<int64_t>(sampleRows);
    blocks.push_back(std::move(graph));
  }
  if (!outName.empty()) {
    ojson failure;
    const std::string label = drawn ? "lowess: " + smoothVarTitle(session, yVar) : std::string();
    if (!smoothWriteColumn(session, outName, smooth, label, &failure)) return failure;
    blocks.push_back(mutationResult(session, "lowess", {outName}, {}));
  }
  return blocks;
}

// `lpoly yvar xvar [if] [in] [aw] [, options]`. Same arrangement as lowess: the command
// layer was measured on StataBE 19.5, the local fits are the engine's.
ojson runLpoly(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lpoly")) return engineTooOld("lpoly", "lpoly");
  // lpoly is r-class: a refusal before the fit leaves r() known to be empty, so
  // reading a result then gives missing. A fit declares its names below.
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "lpoly", {});

  const std::string weightKind = parsed.weightType.value_or(std::string()).substr(0, 1);
  if (weightKind == "p") return aerErrorReason("pweights not allowed", 101);
  if (weightKind == "i") return aerErrorReason("iweights not allowed", 101);
  if (weightKind == "f")
    return aerErrorReason(
        "lpoly: fweights are not implemented in this build (how Stata counts r(N) and the rule-of-thumb n under "
        "fweights is unmeasured); [aw=] is",
        198);
  if (parsed.varlist.size() < 2) return aerErrorReason("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return aerErrorReason("too many variables specified", 103);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (smoothHasOperator(parsed.varlist[0]) || smoothHasOperator(parsed.varlist[1]))
    return aerErrorReason("factor-variable and time-series operators not allowed", 101);
  std::string yVar, xVar;
  if (auto bad = smoothBindYorX(session, parsed.varlist[0], &yVar)) return *bad;
  if (auto bad = smoothBindYorX(session, parsed.varlist[1], &xVar)) return *bad;

  const std::string atText = smoothOptText(smoothFirstOpt(parsed, "at", 2));
  std::string atVar;
  if (!atText.empty()) {
    const std::optional<std::string> found = resolveVarName(atText, session);
    if (!found) return aerErrorReason("variable " + atText + " not found\n(error in option at())", 111);
    atVar = *found;
  }

  double confidence = 95.0;
  const std::string levelText = smoothOptText(smoothFirstOpt(parsed, "level", 1));
  if (!levelText.empty()) {
    if (!smoothPlainNumber(levelText, &confidence)) return aerErrorReason(levelText + " found where number expected", 198);
    if (confidence < 10.0 || confidence > 99.99)
      return aerErrorReason("level() must be between 10 and 99.99 inclusive", 198);
  }

  if (smoothOpt(parsed, "by") != nullptr) return aerErrorReason("option by() not allowed", 191);
  const std::vector<std::pair<std::string, size_t>> namedGaps = {{"var", 1}, {"generatevar", 9}, {"generateci", 9}};
  for (const auto& [gap, atLeast] : namedGaps)
    if (smoothFirstOpt(parsed, gap, atLeast) != nullptr)
      return aerErrorReason("lpoly: option " + gap + "() is not implemented in this build", 198);

  const std::string pilotText = smoothOptText(smoothFirstOpt(parsed, "pwidth", 2));
  const bool pilotGiven = smoothFirstOpt(parsed, "pwidth", 2) != nullptr;
  double numeric = 0.0;
  if (pilotGiven && (!smoothPlainNumber(pilotText, &numeric) || numeric <= 0.0))
    return aerErrorReason("pwidth() must be a positive number", 198);

  // degree() and n() read loosely: a non-integer or non-number means the default
  // (degree 0, the default grid), and n(1) too. Only a nonpositive number refuses.
  std::string degreeArg;
  const std::string degreeText = smoothOptText(smoothFirstOpt(parsed, "degree", 3));
  if (!degreeText.empty()) {
    degreeArg = "0";
    if (smoothPlainNumber(degreeText, &numeric)) {
      if (numeric < 0.0) return aerErrorReason("degree() must be a nonnegative number", 198);
      if (numeric == std::floor(numeric)) degreeArg = std::to_string(static_cast<long long>(numeric));
    }
  }
  std::string gridArg;
  const std::string gridText = smoothOptText(smoothFirstOpt(parsed, "n", 1));
  if (smoothPlainNumber(gridText, &numeric)) {
    if (numeric <= 0.0) return aerErrorReason("n() must be a positive number", 198);
    if (numeric == std::floor(numeric) && numeric >= 2.0) gridArg = std::to_string(static_cast<long long>(numeric));
  }

  std::string kernel;
  if (auto bad = lpolyChooseKernel(parsed, &kernel)) return *bad;

  if (!atVar.empty() && !smoothIsNumeric(session, atVar))
    return aerErrorReason("at(): " + atVar + " is not a numeric variable", 198);

  const std::vector<std::string> outWords = smoothBlankWords(smoothOptText(smoothFirstOpt(parsed, "generate", 3)));
  if (outWords.size() == 1 && atVar.empty())
    return aerErrorReason("at() must be specified for generate() to work with one variable", 198);
  if (outWords.size() > 2) return aerErrorReason("generate(): too many variables specified", 198);
  for (const std::string& word : outWords)
    if (auto bad = lpolyNewNameRefusal(session, word)) return *bad;

  const std::string seName = smoothOptText(smoothFirstOpt(parsed, "se", 2));
  if (!seName.empty()) {
    if (atVar.empty() && outWords.empty())
      return aerErrorReason("se() requires the specification of at() or generate()", 198);
    if (auto bad = lpolyNewNameRefusal(session, seName)) return *bad;
  }

  const std::string widthText = smoothOptText(smoothFirstOpt(parsed, "bwidth", 2));
  if (auto bad = lpolyBandwidthRefusal(session, widthText, !atVar.empty())) return *bad;

  const bool drawn = smoothFirstOpt(parsed, "nograph", 4) == nullptr;
  if (drawn)
    if (auto bad = smoothDrawnNet(parsed, "lpoly")) return *bad;

  const bool bandsInForce = smoothFirstOpt(parsed, "ci", 2) != nullptr || pilotGiven;
  const bool wantScatter = drawn && smoothFirstOpt(parsed, "noscatter", 4) == nullptr;
  std::vector<std::string> args = {"lpoly", "--data", *session.currentHbin(), "--y", yVar, "--x", xVar, "--kernel", kernel};
  auto addFlag = [&args](const char* flag, const std::string& value) {
    if (value.empty()) return;
    args.push_back(flag);
    args.push_back(value);
  };
  addFlag("--bw", widthText);
  addFlag("--degree", degreeArg);
  addFlag("--pwidth", pilotText);
  addFlag("--at", atVar);
  if (atVar.empty()) addFlag("--n", gridArg);
  if (bandsInForce || !seName.empty()) args.push_back("--se");
  if (wantScatter) args.push_back("--emit-touse");
  if (weightKind == "a") {
    addFlag("--weight", parsed.weightVar.value_or(std::string()));
    addFlag("--wtype", "aweight");
  }
  if (auto bad = smoothSampleArgs(parsed, session, "lpoly", &args)) return *bad;

  nlohmann::json reply;
  try {
    reply = runCobraJson(args);
  } catch (const CobraError& ce) {
    return smoothEngineError("lpoly", ce);
  }

  const double absent = std::numeric_limits<double>::quiet_NaN();
  const double chosenWidth = reply.value("bwidth", absent);
  const double chosenPilot = reply.value("pwidth", absent);
  const int chosenDegree = reply.value("degree", 0);
  const std::string chosenKernel = reply.value("kernel", kernel);
  session.eval.rValues.set("N", static_cast<double>(reply.value("n_obs", 0)));
  session.eval.rValues.set("bwidth", chosenWidth);
  session.eval.rValues.set("pwidth", (bandsInForce || !seName.empty()) ? chosenPilot : absent);
  session.eval.rValues.set("ngrid", static_cast<double>(reply.value("ngrid", 0)));
  session.eval.rValues.set("degree", static_cast<double>(chosenDegree));
  session.eval.rStrings.set("kernel", chosenKernel);
  eval::declareRPosted(session.eval, "lpoly", {"N", "bwidth", "pwidth", "ngrid", "degree", "kernel"});

  const std::vector<double> grid = jsonDoubles(reply["grid"]);
  const std::vector<double> fitted = jsonDoubles(reply["smooth"]);
  const std::vector<double> fittedSe = reply.contains("se") ? jsonDoubles(reply["se"]) : std::vector<double>();

  ojson blocks = ojson::array();
  const bool capped = reply.contains("n_capped") && reply["n_capped"].is_boolean() ? reply["n_capped"].get<bool>()
                      : reply.value("n_capped", 0) != 0;
  if (capped && atVar.empty()) blocks.push_back(displayBlock("(n() set to " + std::to_string(grid.size()) + ")\n"));

  // Stata makes the columns smooth, grid, se after r() is in place and stops at the
  // first that cannot be made, keeping the earlier ones. What survives is added in the
  // order grid, smooth, se (`varlist_after` in the freeze row pins this).
  const size_t rows = static_cast<size_t>(session.datastore()->nRows());
  std::vector<LpolyOutput> outputs;
  if (!outWords.empty())
    outputs.push_back({outWords.back(), "lpoly smooth: " + smoothVarTitle(session, yVar), lpolyRowLength(fitted, rows), 1});
  if (outWords.size() == 2) outputs.push_back({outWords[0], "lpoly smoothing grid", lpolyRowLength(grid, rows), 0});
  if (!seName.empty()) outputs.push_back({seName, "lpoly SEs", lpolyRowLength(fittedSe, rows), 2});
  std::optional<ojson> lateRefusal;
  size_t creatable = 0;
  for (; creatable < outputs.size(); ++creatable) {
    const std::string& name = outputs[creatable].name;
    if (const auto reserved = strutil::newVarNameRefusal(name)) {
      lateRefusal = aerErrorReason(reserved->message, reserved->rc);
      break;
    }
    bool repeated = false;
    for (size_t earlier = 0; earlier < creatable; ++earlier) repeated = repeated || outputs[earlier].name == name;
    if (repeated) {
      lateRefusal = aerErrorReason("variable " + name + " already defined", 110);
      break;
    }
  }
  outputs.resize(creatable);

  if (drawn && !lateRefusal) {
    std::vector<std::pair<double, double>> scatter;
    if (wantScatter) {
      ojson failure;
      if (!smoothSamplePoints(session, yVar, xVar, smoothSampleMask(reply), &scatter, &failure)) return failure;
    }
    std::vector<std::array<double, 3>> band;
    std::vector<std::pair<double, double>> line;
    const double zScore = cecon::z_critical(1.0 - confidence / 100.0);
    for (size_t g = 0; g < grid.size() && g < fitted.size(); ++g) {
      if (smoothCellMissing(grid[g]) || smoothCellMissing(fitted[g])) continue;
      line.emplace_back(grid[g], fitted[g]);
      if (bandsInForce && g < fittedSe.size() && !smoothCellMissing(fittedSe[g]))
        band.push_back({grid[g], fitted[g] - zScore * fittedSe[g], fitted[g] + zScore * fittedSe[g]});
    }
    std::stable_sort(band.begin(), band.end(), [](const auto& l, const auto& r) { return l[0] < r[0]; });
    std::string note = "kernel = " + chosenKernel + ", degree = " + std::to_string(chosenDegree) +
                       ", bandwidth = " + smoothRound2(chosenWidth);
    if (bandsInForce) note += ", pwidth = " + smoothRound2(chosenPilot);
    blocks.push_back(smoothGraphBlock(parsed, session, "Local polynomial smooth", note, yVar, xVar, scatter, band,
                                      smoothPairsSortedByX(std::move(line))));
  }

  std::stable_sort(outputs.begin(), outputs.end(),
                   [](const LpolyOutput& l, const LpolyOutput& r) { return l.addOrder < r.addOrder; });
  std::vector<std::string> added;
  for (const LpolyOutput& output : outputs) {
    ojson failure;
    if (!smoothWriteColumn(session, output.name, output.values, output.label, &failure)) return failure;
    added.push_back(output.name);
  }
  if (!added.empty()) blocks.push_back(mutationResult(session, "lpoly", added, {}));
  if (lateRefusal) blocks.push_back(*lateRefusal);
  return blocks;
}

}  // namespace piet::handlers
