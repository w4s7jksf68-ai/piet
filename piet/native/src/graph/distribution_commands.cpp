// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "piet/handlers.h"
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "piet/translate.h"
#include "piet/twoway_parse.h"
#include "io/win_compat.h"
#include "support.h"
#include "distribution.h"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


// The weight word as the tokenizer keeps it (`fw`, `fweight`, ...) -> the full
// type name, or "" when there is none.
static std::string weightTypeName(const eval::ParsedCommand& parsed) {
  if (!parsed.weightType.has_value()) return std::string();
  std::string w = strutil::lower(*parsed.weightType);
  if (w == "fw" || w == "fweight") return "fweight";
  if (w == "aw" || w == "aweight") return "aweight";
  if (w == "pw" || w == "pweight") return "pweight";
  if (w == "iw" || w == "iweight") return "iweight";
  return w;
}


// The estimation sample of one of these commands over `ds`: rows where the
// variable is nonmissing and, under a weight, the weight is nonmissing and
// nonzero (marksample: zero weights are out unless zeroweight). `rows` holds
// each kept row's index in `ds`.
struct GraphSample {
  std::vector<double> v;
  std::vector<double> w;  // empty when unweighted
  std::vector<int64_t> rows;
  std::string wtype;      // "" | fweight | aweight | iweight
  std::optional<ojson> err;
};


static GraphSample graphSample(Datastore& ds, const std::string& var, const eval::ParsedCommand& parsed,
                        const std::string& wtype) {
  GraphSample s;
  s.wtype = wtype;
  const std::vector<double> col = numericColumn(ds, var);
  std::vector<double> wcol;
  if (!wtype.empty()) {
    const std::string wvar = trimG(parsed.weightVar.value_or(std::string()));
    if (!ds.has(wvar)) {
      s.err = stataRefusal("variable " + wvar + " not found", 111);
      return s;
    }
    if (ds.dtype(wvar) == "factor") {
      s.err = stataRefusal("type mismatch", 109);
      return s;
    }
    wcol = numericColumn(ds, wvar);
  }
  for (size_t i = 0; i < col.size(); ++i) {
    if (std::isnan(col[i])) continue;
    double wi = 1.0;
    if (!wtype.empty()) {
      wi = wcol[i];
      if (std::isnan(wi) || wi == 0.0) continue;
      if (wtype == "fweight" && wi != std::floor(wi)) {
        s.err = stataRefusal("may not use noninteger frequency weights", 401);
        return s;
      }
      if (wtype != "iweight" && wi < 0) {
        s.err = stataRefusal("negative weights encountered", 402);
        return s;
      }
    }
    s.v.push_back(col[i]);
    if (!wtype.empty()) s.w.push_back(wi);
    s.rows.push_back(static_cast<int64_t>(i));
  }
  return s;
}


// Writes one new numeric variable at the end of the session frame: the values
// as stored (float-rounded already when `stataType` is float), its storage
// type claim, display format and label.
static std::optional<ojson> writeGenVariable(SessionState& session, const std::string& name,
                                      const std::vector<double>& vals, const std::string& stataType,
                                      const std::string& format, const std::string& label) {
  const std::string hbin = mintScratchPath(session.workdir(), "histgen");
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return stataRefusal("could not write the values payload", 693);
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double d : vals) {
      if (!std::isnan(d)) f << d;
      f << "\n";
    }
    if (!f) return stataRefusal("could not write the values payload", 693);
  }
  const std::string out = mintScratchPath(session.workdir(), "histgen");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  cecon::native_unlink(csv);
  if (!r.ok || !cecon::native_exists(out)) {
    return ojson(eval::engineError("could not create " + name + " (disk full or I/O error)", "data_io"));
  }
  session.load(out);
  session.implicitFloatVars.erase(name);
  session.varMeta[name].stataType = stataType;
  session.varMeta[name].format = format;
  session.setVarLabel(name, label);
  return std::nullopt;
}


// `confirm new var` for each name: an existing variable is r(110).
// BUG-1120 review: a reserved name is refused by the new-variable rule --
// `kdensity x, gen(_cons d)' "_cons invalid varname" r(198) -- or, where
// generate() is read as a newvarlist (`optionList`, twoway__histogram_gen), with
// if/in/using/with taken for qualifiers ("if not allowed" r(101); StataBE 19.5).
static std::optional<ojson> confirmNewVars(SessionState& session, const std::vector<std::string>& names,
                                    bool optionList = false) {
  Datastore* ds = session.datastore();
  for (const auto& n : names) {
    if (!strutil::isValidStataIdentifier(n)) return stataRefusal(n + " invalid name", 198);
    if (auto r = optionList ? strutil::newVarOptionRefusal(n) : strutil::newVarNameRefusal(n))
      return stataRefusal(r->message, r->rc);
    if (ds != nullptr && ds->has(n)) return stataRefusal("variable " + n + " already defined", 110);
  }
  return std::nullopt;
}


// A number as twoway__histogram_gen's display note prints it: the most
// significant digits (at most 8) that fit nine characters once a fraction's
// leading 0 is dropped -- (bin=13, start=.125, width=.91346154) and (start=0,
// width=.33333325) (w2aq_histogram_family.log:556, :1699). piet::fmtStataDefaultG
// counts that 0 and prints .9134615 / .3333333 (a follow-up of this goal), so it
// is not used here; whole numbers and 9-character values agree with it.
static std::string displayNum(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  for (int prec = 8; prec >= 1; --prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t signLen = (!t.empty() && t[0] == '-') ? 1 : 0;
    if (t.compare(signLen, 2, "0.") == 0) t.erase(signLen, 1);
    if (t.size() - signLen <= 9) return t;
  }
  return piet::fmtStataDefaultG(v);
}




// The histogram twin's note, as twoway__histogram_gen ..., display prints it.
static std::string histogramNoteText(const HistGenResult& r, bool discrete) {
  if (discrete) return "(start=" + displayNum(r.start) + ", width=" + displayNum(r.width) + ")";
  return "(bin=" + std::to_string(r.bin) + ", start=" + displayNum(r.start) +
         ", width=" + displayNum(r.width) + ")";
}


static ojson displayNoteBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}


// The text of option `key`: its first occurrence when `first`, else its last
// (Stata's syntax keeps the last of a repeated option, hk_h_dupw2).
static std::optional<std::string> optionText(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      const std::string& key, bool first) {
  std::optional<std::string> found;
  for (const auto& [k, v] : opts) {
    if (k != key) continue;
    found = v.isBool ? std::string() : v.strValue;
    if (first) break;
  }
  return found;
}


// bins()/width() take one positive number, and bins() also an integer; the
// refusal names the option the way Stata's number-list check does.
static std::optional<ojson> readPositiveScalar(const std::string& optName, const std::string& text,
                                        bool integerOnly, double* value) {
  const std::string prefix = optName + "() invalid -- ";
  const std::vector<std::string> words = splitWsG(text);
  if (words.size() != 1) return stataRefusal(prefix + "invalid numlist has too many elements", 123);
  const std::optional<double> d = parseNumberG(words.front());
  if (!d.has_value()) return stataRefusal(prefix + "invalid number", 121);
  if (integerOnly && std::floor(*d) != *d) return stataRefusal(prefix + "invalid numlist has noninteger elements", 126);
  if (!(*d > 0.0)) return stataRefusal(prefix + "invalid number, outside of allowed range", 125);
  *value = *d;
  return std::nullopt;
}


// The height types switched on, in the order typed, `freq` read as frequency.
static std::vector<std::string> requestedHeightTypes(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  static const std::set<std::string> kTypes = {"density", "fraction", "frequency", "percent"};
  std::vector<std::string> seen;
  for (const auto& [k, v] : opts) {
    if (v.isBool && !v.boolValue) continue;
    const std::string t = (k == "freq") ? std::string("frequency") : k;
    if (kTypes.count(t) != 0 && std::find(seen.begin(), seen.end(), t) == seen.end()) seen.push_back(t);
  }
  return seen;
}


HistOpts parseHistOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       const std::string& varname, bool genFloat, bool firstWins ) {
  HistOpts out;
  HistGenSpec& sp = out.spec;
  sp.varname = varname;
  sp.genFloat = genFloat;
  sp.discrete = hasOpt(opts, "discrete");
  const std::optional<std::string> binText = optionText(opts, "bin", firstWins);
  const std::optional<std::string> widthText = optionText(opts, "width", firstWins);
  const std::optional<std::string> startText = optionText(opts, "start", firstWins);
  auto refuse = [&out](ojson block) {
    out.err = std::move(block);
    return out;
  };

  if (binText.has_value() && sp.discrete) {
    return refuse(stataRefusal("options discrete and bins() may not be combined", 198));
  }
  if (binText.has_value() && widthText.has_value()) {
    return refuse(stataRefusal("options bins() and width() may not be combined", 198));
  }
  const std::vector<std::string> types = requestedHeightTypes(opts);
  if (types.size() > 1) {
    std::string named;
    for (const std::string& t : types) named += (named.empty() ? "" : " ") + t;
    return refuse(stataRefusal("options " + named + " may not be combined", 198));
  }
  sp.type = types.empty() ? std::string("density") : types.front();

  if (binText.has_value()) {
    double b = 0.0;
    if (auto bad = readPositiveScalar("bins", *binText, true, &b)) return refuse(*bad);
    sp.bins = static_cast<int64_t>(b);
  }
  if (widthText.has_value()) {
    double wd = 0.0;
    if (auto bad = readPositiveScalar("width", *widthText, false, &wd)) return refuse(*bad);
    sp.width = wd;
  }
  if (startText.has_value()) {
    const std::vector<std::string> words = splitWsG(*startText);
    const std::optional<double> s = words.size() == 1 ? parseNumberG(words.front()) : std::nullopt;
    if (!s.has_value()) return refuse(stataRefusal("start() invalid -- invalid numlist", 121));
    sp.start = *s;
  }
  return out;
}


// serset-twoway HIST-1: a twoway function's (y, x) serset, `double double`
// (the normal overlays: w2aq hk_h_064normal_s2 log:2145, hk_kd_106norm_s2
// log:4414).
static serset::Serset functionSerset(const ojson& points) {
  std::vector<double> y, x;
  for (const auto& p : points) {
    x.push_back(p[0].is_number() ? p[0].get<double>() : kNaN);
    y.push_back(p[1].is_number() ? p[1].get<double>() : kNaN);
  }
  serset::Serset s;
  s.n = static_cast<int64_t>(y.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(y)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(x)));
  return s;
}


// A height type as its column label: `Density`, `Fraction`, ...
static std::string capitalised(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}


// `length` values: `head` in the first rows, missing after.
static std::vector<double> paddedColumn(const std::vector<double>& head, int64_t length) {
  std::vector<double> col(static_cast<size_t>(std::max<int64_t>(0, length)), kNaN);
  std::copy_n(head.begin(), std::min(head.size(), col.size()), col.begin());
  return col;
}


// 300 points of a normal density with the given mean and sd over [lo, hi]
// (point i at lo + (hi - lo) i/299), each times `area`.
static ojson normalCurve(double lo, double hi, double mean, double sd, double area) {
  static const double root2pi = std::sqrt(2.0 * M_PI);
  constexpr int kPoints = 300;
  ojson pts = ojson::array();
  for (int i = 0; i < kPoints; ++i) {
    const double px = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(kPoints - 1);
    const double z = (px - mean) / sd;
    pts.push_back(ojson::array({px, std::exp(-0.5 * z * z) / (root2pi * sd) * area}));
  }
  return pts;
}


// A value written with 13 significant digits and read back. The drawn bars of
// `histogram` start at the note's r(min) and use its r(width) at this
// precision: it is what puts hk_h_discnonint's first serset centre at 5.0e-14
// (+1.c240000000000X-02d, w2aq_histogram_family.log:1712) instead of 0.
static double thirteenDigits(double d) {
  if (std::isnan(d)) return d;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.13g", d);
  return std::strtod(buf, nullptr);
}


// The overlays' scale factor r(area) at the precision StataBE scales them by:
// seven significant digits for the non-integer values measured (fraction
// 1.571429, percent 157.1429, frequency 73.33333 / 94.28571; measured on StataBE:
// nfr, npc, nb9, hnfq), whole numbers as they are (132, 78).
static double overlayScale(double area) {
  if (std::isnan(area) || (area == std::floor(area) && std::fabs(area) < 1e8)) return area;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.7g", area);
  return std::strtod(buf, nullptr);
}


// The occupied bins of a result as the renderers read them (graph.js,
// buildSvgHistogram): edges x0/x1 from r(min) and r(width), the centre, the raw
// count and the height. The closing row is not a bar.
ojson barList(const HistGenResult& g) {
  ojson bars = ojson::array();
  for (size_t i = 0; i < g.height.size(); ++i) {
    const double q = g.binIndex[i];
    bars.push_back(ojson{{"x0", g.min + g.width * q},
                         {"x1", g.min + g.width * (q + 1.0)},
                         {"x", g.centre[i]},
                         {"count", g.count[i]},
                         {"y", g.height[i]}});
  }
  return bars;
}


// The serset of a histogram plot: height, a byte 0, the bin centre, one row per
// occupied bin, plus (., 0, start) when the result has the closing row. Heights
// and centres are float under c(type) float (hk_histogram_bare `float byte
// float`, w2al tf_tw_073hist_s1).
serset::Serset barSerset(const HistGenResult& g, bool genFloat) {
  const size_t occupied = g.height.size();
  std::vector<double> heights(g.height);
  std::vector<double> centres(g.centre.begin(), g.centre.begin() + static_cast<std::ptrdiff_t>(
                                                                       std::min(occupied, g.centre.size())));
  if (g.closingRow && g.centre.size() > occupied) {
    heights.push_back(kNaN);
    centres.push_back(g.centre[occupied]);
  }
  std::vector<double> zeros(heights.size(), 0.0);
  const std::string type = genFloat ? "float" : "double";
  serset::Serset s;
  s.n = static_cast<int64_t>(heights.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), type, std::move(heights)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "byte", std::move(zeros)));
  s.cols.push_back(serset::numericColumn(serset::tempName(2), type, std::move(centres)));
  return s;
}


// A computed curve over the whole frame: `curve` in its first rows, the grid
// beside it, and then the plotted variable of the full dataset in data order;
// max(curve length, _N) rows (hk_h_044disc_s2 `double double <type>`, w2al
// tf_kd_n).
serset::Serset curveWithVariableSerset(SessionState& session, Datastore& full, const std::string& var,
                                       const std::vector<double>& curve, const std::vector<double>& grid) {
  const int64_t nData = full.nRows();
  const int64_t rows = std::max<int64_t>(static_cast<int64_t>(curve.size()), nData);
  std::vector<int64_t> dataRow(static_cast<size_t>(rows));
  for (int64_t i = 0; i < rows; ++i) dataRow[static_cast<size_t>(i)] = i < nData ? i : -1;
  serset::Serset s;
  s.n = rows;
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", paddedColumn(curve, rows)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", paddedColumn(grid, rows)));
  s.cols.push_back(serset::variableColumn(session, full, var, dataRow));
  return s;
}


// `count` points from `lo`, each the previous plus (hi - lo)/(count - 1): the
// accumulated grid of the kdensity curves (hnk_s2 point 7 -1.63beee05231a6X+001,
// tf_kd_n row 11 one ulp under 6.25).
std::vector<double> accumulatedGrid(double lo, double hi, int64_t count) {
  std::vector<double> grid(static_cast<size_t>(std::max<int64_t>(count, 0)), kNaN);
  if (grid.empty()) return grid;
  grid[0] = lo;
  const double step = count > 1 ? (hi - lo) / static_cast<double>(count - 1) : kNaN;
  for (size_t i = 1; i < grid.size(); ++i) grid[i] = grid[i - 1] + step;
  return grid;
}


// histogram's by(): repeated by() options merge; the variable is the first word
// of the last one that names any.
struct ByRequest {
  bool given = false;
  std::string var;
};


static ByRequest readByRequest(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  ByRequest by;
  for (const auto& [k, v] : opts) {
    if (k != "by" || v.isBool) continue;
    by.given = true;
    const std::string& text = v.strValue;
    const std::string head = trimG(text.substr(0, text.find(',')));
    if (!head.empty()) by.var = splitWsG(head).front();
  }
  return by;
}


// One by() panel of histogram: its key and its share of the sample.
struct HistPanel {
  ojson key;
  std::vector<double> v, w;
};


// The sample split by the by variable, in Stata's sort order (numeric
// ascending; a string variable by its text). Missing and "" groups are left
// out.
static std::vector<HistPanel> splitByPanels(Datastore& ds, const std::string& byVar, const GraphSample& smp) {
  const std::vector<double> keys = numericColumn(ds, byVar);
  const bool weighted = !smp.w.empty();
  std::vector<std::string> levelText;
  const bool textual = ds.dtype(byVar) == "factor";
  if (textual) {
    const VarInfo vi = ds.varinfo(byVar);
    levelText.assign(vi.levels.begin(), vi.levels.end());
  }
  std::map<double, HistPanel> byNumber;
  std::map<std::string, HistPanel> byText;
  for (size_t i = 0; i < smp.v.size(); ++i) {
    const double key = keys[static_cast<size_t>(smp.rows[i])];
    if (std::isnan(key)) continue;
    HistPanel* panel = nullptr;
    if (textual) {
      if (key < 0 || static_cast<size_t>(key) >= levelText.size()) continue;
      const std::string& label = levelText[static_cast<size_t>(key)];
      if (label.empty()) continue;
      panel = &byText[label];
      panel->key = label;
    } else {
      panel = &byNumber[key];
      panel->key = key;
    }
    panel->v.push_back(smp.v[i]);
    if (weighted) panel->w.push_back(smp.w[i]);
  }
  std::vector<HistPanel> out;
  for (auto& entry : byNumber) out.push_back(std::move(entry.second));
  for (auto& entry : byText) out.push_back(std::move(entry.second));
  return out;
}


// The spec of the drawn bars: the continuous rule from the note's r(min) (as
// 13-digit text) with r(bin), or with r(width) under discrete, width() or by();
// a bin option repeated with a different value takes the last one (a repeated
// option is a graph option, hk_h_dupw2).
static HistGenSpec drawnBarSpec(const HistGenResult& note, const HistGenSpec& firstSpec, const HistGenSpec& lastSpec,
                         bool widthOnly) {
  HistGenSpec bars;
  bars.varname = firstSpec.varname;
  bars.type = firstSpec.type;
  bars.genFloat = firstSpec.genFloat;
  bars.start = thirteenDigits(note.min);
  if (widthOnly || firstSpec.discrete || firstSpec.width.has_value()) {
    bars.width = thirteenDigits(note.width);
  } else {
    bars.bins = note.bin;
  }
  if (lastSpec.start != firstSpec.start) bars.start = lastSpec.start;
  if (lastSpec.width != firstSpec.width) {
    bars.width = lastSpec.width;
    bars.bins.reset();
  }
  if (lastSpec.bins != firstSpec.bins) {
    bars.bins = lastSpec.bins;
    bars.width.reset();
  }
  return bars;
}


static double tallestBar(const HistGenResult& g, double floor) {
  for (double y : g.height) floor = std::max(floor, y);
  return floor;
}


// histogram's addplot(): the added plots are twoway plots of their own over the
// full frame; their layers and sersets join the histogram's. An error block
// from them is returned.
static std::optional<ojson> addAddplot(SessionState& session, const std::string& text, ojson& graph,
                                std::vector<serset::Serset>* sersets) {
  std::string body = trimG(text);
  while (body.size() >= 2 && body.compare(body.size() - 2, 2, "||") == 0) body = trimG(body.substr(0, body.size() - 2));
  if (body.empty()) return std::nullopt;
  twoway::ParseResult parsedPlots =
      twoway::parseTwoway(twoway::hasToplevelParens(body) ? body : twoway::expandDoublePipes(body));
  if (parsedPlots.layers.empty()) return std::nullopt;
  for (auto& layer : parsedPlots.layers) eval::canonicalizeForCommand(layer.opts, "twoway_layer");
  eval::canonicalizeForCommand(parsedPlots.globalOpts, "twoway_layer");
  std::vector<serset::PlotSpec> plots;
  ojson drawn = runTwowayLayers(session.datastore(), session, parsedPlots.layers, parsedPlots.globalOpts,
                                parsedPlots.ifExpr, &plots, parsedPlots.inRange);
  if (drawn.is_object() && drawn.value("type", std::string()) == "error") return drawn;
  if (drawn.is_object() && drawn.contains("layers")) graph["addplot_layers"] = drawn["layers"];
  if (sersets != nullptr) {
    for (auto& s : serset::buildPlotSersets(std::move(plots))) sersets->push_back(std::move(s));
  }
  return std::nullopt;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


ojson runHistogram(SessionState& session, Datastore& ds, const std::vector<std::string>& varlist,
                   const eval::ParsedCommand& parsed, std::vector<ojson>* pre,
                   std::vector<serset::Serset>* sersets) {
  const auto& opts = parsed.opts;
  if (varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = varlist.front();
  if (!ds.has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (ds.dtype(var) == "factor") return stataRefusal(var + " is a string variable", 109);
  const std::string wtype = weightTypeName(parsed);
  if (!wtype.empty() && wtype != "fweight") return stataRefusal(wtype + "s not allowed", 101);
  Datastore* full = session.datastore();
  const int64_t nRows = full != nullptr ? full->nRows() : ds.nRows();
  if (nRows == 0) return stataRefusal("no observations", 2000);

  // The printed note reads the first bins()/width()/start(); the drawn bars
  // the last. Both must be valid.
  const bool genFloat = session.defaultStorageType != "double";
  const HistOpts firstOpts = parseHistOpts(opts, var, genFloat, /*firstWins=*/true);
  if (firstOpts.err) return *firstOpts.err;
  const HistOpts lastOpts = parseHistOpts(opts, var, genFloat, /*firstWins=*/false);
  if (lastOpts.err) return *lastOpts.err;
  const HistGenSpec& spec = firstOpts.spec;

  const GraphSample smp = graphSample(ds, var, parsed, wtype);
  if (smp.err) return *smp.err;
  if (smp.v.empty()) return stataRefusal("no observations", 2000);

  const ByRequest by = readByRequest(opts);
  if (by.given && by.var.empty()) return stataRefusal("varlist required in by()", 100);
  const bool wantKdensity = hasOpt(opts, "kdensity") || hasOpt(opts, "kdenopts");
  const bool wantNormal = hasOpt(opts, "normal") || hasOpt(opts, "normopts");
  if (by.given && spec.type == "frequency" && (wantKdensity || wantNormal)) {
    const std::string which = wantNormal && wantKdensity ? "normal kdensity" : (wantNormal ? "normal" : "kdensity");
    const std::string tail = wantNormal && wantKdensity ? " and by() options" : " and by() option";
    return stataRefusal("option frequency may not be combined with the " + which + tail, 191);
  }
  if (by.given && !ds.has(by.var)) return stataRefusal("variable " + by.var + " not found", 111);

  const std::vector<double>* weights = smp.w.empty() ? nullptr : &smp.w;
  const HistGenResult note = histogramGen(smp.v, weights, spec, nRows, false);
  if (note.rc != 0) return stataRefusal(note.message, note.rc);
  if (pre != nullptr && !by.given) pre->push_back(displayNoteBlock(histogramNoteText(note, spec.discrete)));
  // histogram leaves no numeric r() behind (measured on StataBE, probe hret: r(N) missing).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "histogram", {});

  const HistGenSpec barSpec = drawnBarSpec(note, spec, lastOpts.spec, /*widthOnly=*/by.given);

  ojson graph;
  graph["type"] = "graph";
  graph["kind"] = "histogram";
  graph["title"] = optTitleStr(opts, "title").value_or(std::string());
  graph["x_label"] = optTitleStr(opts, "xtitle").value_or(varTitleG(session, var));
  graph["y_label"] = optTitleStr(opts, "ytitle").value_or(capitalised(spec.type));
  graph["note"] = optTitleStr(opts, "note").value_or(std::string());
  graph["x_var"] = var;
  graph["y_kind"] = spec.type;
  graph["n"] = note.N;
  graph["bin"] = note.bin;
  graph["start"] = note.start;
  graph["width"] = note.width;
  graph["discrete"] = spec.discrete;
  if (!wtype.empty()) graph["weight"] = wtype + "=" + trimG(parsed.weightVar.value_or(std::string()));

  double yTop = 0.0;
  if (!by.given) {
    const HistGenResult bars = histogramGen(smp.v, weights, barSpec, nRows, true);
    graph["bins"] = barList(bars);
    graph["layer_start"] = bars.start;
    yTop = tallestBar(bars, yTop);
    if (sersets != nullptr) sersets->push_back(barSerset(bars, genFloat));
  } else {
    ojson panels = ojson::array();
    for (const HistPanel& panel : splitByPanels(ds, by.var, smp)) {
      const HistGenResult bars =
          histogramGen(panel.v, weights != nullptr ? &panel.w : nullptr, barSpec, nRows, true);
      if (sersets != nullptr) sersets->push_back(barSerset(bars, genFloat));
      panels.push_back(ojson{{"by", panel.key}, {"n", bars.N}, {"bins", barList(bars)}});
      yTop = tallestBar(bars, yTop);
    }
    graph["by"] = by.var;
    graph["panels"] = panels;
    graph["bins"] = panels.empty() ? ojson::array() : panels[0]["bins"];
  }
  graph["x_min"] = note.min;
  graph["x_max"] = note.max;
  graph["y_min"] = 0.0;
  graph["y_max"] = yTop;
  graph["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)

  // The overlays are scaled by r(area), so they share the bars' units.
  const double scale = overlayScale(note.area);
  double mean = kNaN, sd = kNaN;
  double ignoredLo = kNaN, ignoredHi = kNaN;
  const double defaultH = kdensityDefaultWidth(smp.v, smp.w, wtype, &ignoredLo, &ignoredHi, &mean, &sd);

  if (wantKdensity && !by.given) {
    // The epanechnikov density at the default bandwidth over 300 accumulated
    // points from r(min) to r(max).
    constexpr int64_t kCurvePoints = 300;
    const std::vector<double> grid =
        accumulatedGrid(thirteenDigits(note.min), thirteenDigits(note.max), kCurvePoints);
    std::vector<double> curve = kdensityAt(smp.v, smp.w, wtype, "epanechnikov", defaultH, grid);
    ojson pts = ojson::array();
    for (size_t i = 0; i < curve.size(); ++i) {
      curve[i] *= scale;
      pts.push_back(ojson::array({grid[i], curve[i]}));
      if (!std::isnan(curve[i])) yTop = std::max(yTop, curve[i]);
    }
    graph["y_max"] = yTop;
    graph["kdensity_points"] = pts;
    graph["kdensity_bwidth"] = defaultH;
    if (sersets != nullptr && full != nullptr) {
      sersets->push_back(curveWithVariableSerset(session, *full, var, curve, grid));
    }
  }

  if (wantNormal && !by.given && sd > 0) {
    // The normal density with the sample's (fweighted) mean and sd.
    ojson pts = normalCurve(note.min, note.max, mean, sd, scale);
    for (const auto& p : pts) yTop = std::max(yTop, p[1].get<double>());
    graph["y_max"] = yTop;
    graph["normal_points"] = pts;
    graph["normal_mean"] = mean;
    graph["normal_sd"] = sd;
    if (sersets != nullptr) {
      serset::Serset curve = functionSerset(pts);
      const std::string wvar = trimG(parsed.weightVar.value_or(std::string()));
      if (!wtype.empty() && full != nullptr && full->has(wvar)) {
        // Under fweights the curve's serset also keeps the weight variable, in
        // data order (measured on StataBE, probes hnfw, nfwf: `double double byte`).
        curve = curveWithVariableSerset(session, *full, wvar, curve.cols[0].num, curve.cols[1].num);
      }
      sersets->push_back(std::move(curve));
    }
  }

  if (const std::optional<std::string> added = optLast(opts, "addplot")) {
    graph["addplot"] = *added;
    if (auto bad = addAddplot(session, *added, graph, sersets)) return *bad;
  }
  return graph;
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;




// twoway__histogram_gen's generate(h x [, replace]): the two new names (height
// first, centre second), or none.
struct HistGenTargets {
  std::vector<std::string> names;
  bool replace = false;
  std::optional<ojson> err;
};


static HistGenTargets readHistGenTargets(SessionState& session,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  HistGenTargets t;
  const std::optional<std::string> text = optLast(opts, "generate");
  if (!text.has_value()) return t;
  std::string list = *text;
  const size_t comma = list.find(',');
  if (comma != std::string::npos) {
    const std::string sub = trimG(list.substr(comma + 1));
    if (!sub.empty() && sub != "replace") {
      t.err = stataRefusal("option " + sub + " not allowed", 198);
      return t;
    }
    t.replace = (sub == "replace");
    list.erase(comma);
  }
  t.names = splitWsG(list);
  if ((t.replace || !t.names.empty()) && t.names.size() != 2) {
    t.err = stataRefusal("option generate() incorrectly specified", 198);
    return t;
  }
  if (!t.names.empty() && !t.replace) t.err = confirmNewVars(session, t.names, /*optionList=*/true);
  return t;
}


// `drop` whichever of `names` exist, through the dispatcher; its error block
// when it fails.
static std::optional<ojson> dropIfPresent(SessionState& session, const std::vector<std::string>& names) {
  std::string cmd;
  for (const std::string& n : names) {
    Datastore* cur = session.datastore();
    if (cur != nullptr && cur->has(n)) cmd += " " + n;
  }
  if (cmd.empty()) return std::nullopt;
  for (const auto& block : piet::dispatch("drop" + cmd, session)) {
    if (block.value("type", std::string()) == "error") return block;
  }
  return std::nullopt;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


// `twoway__histogram_gen`: Stata's internal histogram subroutine, served (registry
// row, floors row, pangram S24, freeze fixture).
ojson runTwowayHistogramGen(const eval::ParsedCommand& parsed, SessionState& session) {
  const auto& opts = parsed.opts;
  if (parsed.varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = parsed.varlist.front();
  Datastore* full = session.hasData() ? session.datastore() : nullptr;
  if (full == nullptr || !full->has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (parsed.varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (full->dtype(var) == "factor") return stataRefusal("type mismatch", 109);
  const std::string wtype = weightTypeName(parsed);
  if (!wtype.empty() && wtype != "fweight") return stataRefusal(wtype + "s not allowed", 101);

  static const std::set<std::string> kAllowed = {"discrete", "bin",     "width",   "start",  "density",
                                                 "fraction", "frequency", "freq",  "percent", "return",
                                                 "generate", "display"};
  for (const auto& [k, v] : opts) {
    if (kAllowed.count(k) == 0) return stataRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198);
  }
  const HistGenTargets targets = readHistGenTargets(session, opts);
  if (targets.err) return *targets.err;
  const bool genFloat = session.defaultStorageType != "double";
  const HistOpts ho = parseHistOpts(opts, var, genFloat);
  if (ho.err) return *ho.err;

  const bool wantDisplay = hasOpt(opts, "display");
  const bool wantGenerate = !targets.names.empty();
  // From here the program counts as having run: r() is emptied, and a call
  // with nothing to return or generate leaves it empty (hk_thg_syntaxonly_r).
  eval::replaceRClass(session.eval);
  if (!wantGenerate && !wantDisplay && !hasOpt(opts, "return")) {
    eval::declareRPosted(session.eval, "twoway__histogram_gen", {});
    return ojson::array();
  }

  // The sample over the whole frame, `if` applied through a scratch copy.
  std::string ifCopy;
  std::unique_ptr<Datastore> restricted;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    ifCopy = mintScratchPath(session.workdir(), "histgenif");
    const std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", full->path(), "--keepif", *parsed.ifExpr, "--out", ifCopy}, *parsed.ifExpr, session);
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      cecon::native_unlink(ifCopy);
      return ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
    }
    restricted = std::make_unique<Datastore>(ifCopy);
  }
  const GraphSample smp = graphSample(restricted ? *restricted : *full, var, parsed, wtype);
  const int64_t nRows = full->nRows();
  const std::string centreFormat = displayFormatOf(session, *full, var);
  const std::string centreLabel = varTitleG(session, var);
  restricted.reset();
  if (!ifCopy.empty()) cecon::native_unlink(ifCopy);
  if (smp.err) return *smp.err;

  const HistGenResult g = histogramGen(smp.v, smp.w.empty() ? nullptr : &smp.w, ho.spec, nRows, wantGenerate);
  if (g.rc != 0) return stataRefusal(g.message, g.rc);

  ojson blocks = ojson::array();
  if (wantDisplay && !g.empty) blocks.push_back(displayNoteBlock(histogramNoteText(g, ho.spec.discrete)));

  if (wantGenerate) {
    const std::string& heightVar = targets.names[0];
    const std::string& centreVar = targets.names[1];
    if (targets.replace) {
      if (auto bad = dropIfPresent(session, {heightVar, centreVar})) return *bad;
    }
    const std::string storage = genFloat ? "float" : "double";
    const std::string heightFormat = genFloat ? "%9.0g" : "%10.0g";
    const std::string heightLabel = capitalised(ho.spec.type);
    const std::vector<double> heights = paddedColumn(g.height, nRows);
    const std::vector<double> centres = paddedColumn(g.centre, nRows);
    // Creation order is observable (variable positions): the centre comes
    // first with the source variable's format and label; an empty sample makes
    // two all-missing columns, the height first and the centre unlabelled
    // (hk_thg_empty_g).
    std::vector<std::string> made;
    std::optional<ojson> bad;
    if (g.empty) {
      bad = writeGenVariable(session, heightVar, heights, storage, heightFormat, heightLabel);
      if (!bad) bad = writeGenVariable(session, centreVar, centres, storage, heightFormat, std::string());
      made = {heightVar, centreVar};
    } else {
      bad = writeGenVariable(session, centreVar, centres, storage, centreFormat, centreLabel);
      if (!bad) bad = writeGenVariable(session, heightVar, heights, storage, heightFormat, heightLabel);
      made = {centreVar, heightVar};
    }
    if (bad) return *bad;
    blocks.push_back(mutationResult(session, "twoway__histogram_gen", made, {}));
  }

  const std::vector<std::pair<std::string, double>> scalars = {
      {"area", g.area}, {"max", g.max},   {"min", g.min},
      {"start", g.start}, {"width", g.width}, {"bin", static_cast<double>(g.bin)}, {"N", g.N}};
  std::vector<std::string> posted;
  for (const auto& [name, value] : scalars) {
    session.eval.rValues.set(name, value);
    posted.push_back(name);
  }
  session.eval.rStrings.set("type", ho.spec.type);
  posted.push_back("type");
  if (wantGenerate || g.empty) {
    session.eval.rValues.set("n_x", static_cast<double>(g.nx));
    posted.push_back("n_x");
  }
  eval::declareRPosted(session.eval, "twoway__histogram_gen", posted);
  return blocks;
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


KernelChoice chooseKernel(const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool strictWords) {
  KernelChoice c;
  std::vector<std::string> bareNames;
  for (const auto& [k, v] : opts) {
    if (!(v.isBool && !v.boolValue) && isKernelName(k)) bareNames.push_back(k);
  }
  const std::optional<std::string> typed = optLast(opts, "kernel");
  const std::vector<std::string> words = typed.has_value() ? splitWsG(*typed) : std::vector<std::string>{};
  if (words.empty()) {
    if (bareNames.size() > 1) {
      c.err = stataRefusal("only one kernel may be specified", 198);
    } else if (bareNames.size() == 1) {
      c.name = bareNames.front();
    }
    return c;
  }
  if (!bareNames.empty()) {
    c.err = stataRefusal("option kernel(): old syntax may not be combined with new syntax", 198);
    return c;
  }
  if (strictWords && words.size() > 1) {
    c.err = stataRefusal("only one kernel may be specified", 198);
    return c;
  }
  const std::optional<std::string> resolved = kdensityKernelName(words.front());
  if (!resolved.has_value()) {
    c.err = stataRefusal("invalid kernel function", 198);
    return c;
  }
  c.name = *resolved;
  return c;
}


BandwidthChoice chooseBandwidth(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                const std::string& conflictMessage) {
  BandwidthChoice b;
  // The number in option `key`, 0 when absent; false when it is not a number.
  auto numberIn = [&](const std::string& key, double* out) {
    const std::optional<std::string> text = optLast(opts, key);
    if (!text.has_value()) return true;
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value()) {
      b.err = stataRefusal("option " + key + "() incorrectly specified", 198);
      return false;
    }
    *out = *d;
    return true;
  };
  double byWidth = 0.0, byBwidth = 0.0;
  if (!numberIn("width", &byWidth) || !numberIn("bwidth", &byBwidth)) return b;
  if (byBwidth != 0.0 && byWidth != 0.0) {
    b.err = stataRefusal(conflictMessage, 198);
    return b;
  }
  b.h = byBwidth != 0.0 ? byBwidth : byWidth;
  return b;
}


// The bandwidth as kdensity's default note prints it: rounded to .0001 and
// shown %6.4f, or %10.3e when that rounding leaves 0; `0` for a zero bandwidth.
static std::string bandwidthNoteText(double h) {
  if (std::isnan(h)) return ".";
  if (h == 0.0) return "0";
  const bool tiny = std::round(h / 0.0001) * 0.0001 == 0.0;
  char buf[64];
  std::snprintf(buf, sizeof buf, tiny ? "%10.3e" : "%6.4f", h);
  return trimG(buf);
}


// kdensity's density line serset (two `double` columns, density then grid):
// grid mode keeps the grid rows and adds one all-missing row when the grid is
// shorter than the data (hk_kd_bare); at() mode keeps every row, sorted by the
// evaluation point with the missing ones last (hk_kd_039aw).
static serset::Serset kdensityLineSerset(const KdensityResult& k, bool atMode, int64_t nRows) {
  std::vector<std::pair<double, double>> rows;  // (point, density)
  rows.reserve(k.grid.size() + 1);
  for (size_t i = 0; i < k.grid.size(); ++i) rows.emplace_back(k.grid[i], i < k.density.size() ? k.density[i] : kNaN);
  if (atMode) {
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      if (std::isnan(b.first)) return !std::isnan(a.first);
      return !std::isnan(a.first) && a.first < b.first;
    });
  } else if (static_cast<int64_t>(rows.size()) < nRows) {
    rows.emplace_back(kNaN, kNaN);
  }
  std::vector<double> dens, pts;
  for (const auto& [p, d] : rows) {
    pts.push_back(p);
    dens.push_back(d);
  }
  serset::Serset s;
  s.n = static_cast<int64_t>(dens.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(dens)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(pts)));
  return s;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


ojson runKdensity(SessionState& session, Datastore& ds, const std::vector<std::string>& varlist,
                  const eval::ParsedCommand& parsed, std::vector<ojson>* pre, bool* graphDrawn,
                  std::vector<serset::Serset>* sersets) {
  *graphDrawn = false;
  const auto& opts = parsed.opts;
  if (varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = varlist.front();
  if (!ds.has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (ds.dtype(var) == "factor") return stataRefusal(var + " is a string variable", 109);
  const std::string wtype = weightTypeName(parsed);
  if (wtype == "pweight") return stataRefusal("pweights not allowed", 101);

  // Under nograph only kdensity's own options are accepted (hk_r_kd_ngopt).
  const bool nograph = hasOpt(opts, "nograph");
  if (nograph) {
    static const std::set<std::string> kOwn = {"generate", "gen", "at", "n", "width", "bwidth", "nograph", "kernel"};
    for (const auto& [k, v] : opts) {
      if (kOwn.count(k) == 0 && !isKernelName(k)) {
        return stataRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198);
      }
    }
  }
  Datastore* full = session.datastore();
  const int64_t nRows = full != nullptr ? full->nRows() : ds.nRows();

  constexpr int64_t kDefaultPoints = 50;
  int64_t points = kDefaultPoints;
  if (const auto text = optLast(opts, "n")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value() || std::floor(*d) != *d) return stataRefusal("option n() incorrectly specified", 198);
    points = static_cast<int64_t>(*d);
  }
  const std::optional<std::string> atText = optLast(opts, "at");
  if (atText.has_value() && points != kDefaultPoints) {
    return stataRefusal("options at() and n() may not be specified together", 198);
  }
  const KernelChoice kc = chooseKernel(opts, /*strictWords=*/true);
  if (kc.err) return *kc.err;
  const BandwidthChoice bw = chooseBandwidth(opts, "options width() and bwidth() may not be specified together");
  if (bw.err) return *bw.err;
  if (bw.h < 0.0) return stataRefusal("bandwidth must be positive", 198);

  std::vector<std::string> newNames;
  for (const auto& [k, v] : opts) {
    if ((k == "generate" || k == "gen") && !v.isBool) newNames = splitWsG(v.strValue);
  }
  if (newNames.size() > 2 || (newNames.size() == 1 && !atText.has_value())) {
    return stataRefusal("invalid syntax", 198);
  }
  if (auto bad = confirmNewVars(session, newNames)) return *bad;

  std::vector<double> atPoints;
  if (atText.has_value()) {
    const std::string atVar = trimG(*atText);
    if (full == nullptr || !full->has(atVar)) return stataRefusal("variable " + atVar + " not found", 111);
    if (full->dtype(atVar) == "factor") return stataRefusal("type mismatch", 109);
    atPoints = numericColumn(*full, atVar);
  }

  const GraphSample smp = graphSample(ds, var, parsed, wtype);
  if (smp.err) return *smp.err;
  KdensitySpec ks;
  ks.kernel = kc.name;
  ks.width = bw.h;
  ks.n = points;
  ks.wtype = wtype;
  const KdensityResult k = kdensityEstimate(smp.v, smp.w, ks, nRows, atText.has_value() ? &atPoints : nullptr);
  if (k.nCapped && pre != nullptr) pre->push_back(displayNoteBlock("(n() set to " + std::to_string(k.n) + ")"));

  const std::string varTitle = varTitleG(session, var);
  ojson graph = ojson::object();
  if (!nograph) {
    *graphDrawn = true;
    std::vector<std::pair<double, double>> drawn;
    for (size_t i = 0; i < k.grid.size(); ++i) {
      if (!std::isnan(k.grid[i]) && !std::isnan(k.density[i])) drawn.emplace_back(k.grid[i], k.density[i]);
    }
    if (atText.has_value()) {
      std::stable_sort(drawn.begin(), drawn.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    ojson pts = ojson::array();
    double yTop = 0.0;
    double xLow = 0.0, xHigh = 0.0;
    for (size_t i = 0; i < drawn.size(); ++i) {
      const auto& [px, py] = drawn[i];
      pts.push_back(ojson::array({px, py}));
      yTop = std::max(yTop, py);
      xLow = (i == 0) ? px : std::min(xLow, px);
      xHigh = (i == 0) ? px : std::max(xHigh, px);
    }
    graph["type"] = "graph";
    graph["kind"] = "line";
    graph["title"] = optTitleStr(opts, "title").value_or("Kernel density estimate");
    graph["x_label"] = optTitleStr(opts, "xtitle").value_or(varTitle);
    graph["y_label"] = optTitleStr(opts, "ytitle").value_or(std::string("Density"));
    graph["note"] = optTitleStr(opts, "note")
                        .value_or("kernel = " + kc.name + ", bandwidth = " + bandwidthNoteText(k.bwidth));
    graph["x_var"] = var;
    graph["n"] = static_cast<int64_t>(smp.v.size());
    graph["bwidth"] = k.bwidth;
    graph["n_grid"] = k.n;
    graph["kernel"] = kc.name;
    if (!wtype.empty()) graph["weight"] = wtype + "=" + trimG(parsed.weightVar.value_or(std::string()));
    graph["points"] = pts;
    graph["x_min"] = xLow;
    graph["x_max"] = xHigh;
    graph["y_min"] = 0.0;
    graph["y_max"] = yTop;
    graph["subkind"] = "kdensity";
    graph["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
    if (sersets != nullptr) sersets->push_back(kdensityLineSerset(k, atText.has_value(), nRows));
    // normal / normopts(): the normal density with the summarize mean and sd
    // over the drawn range.
    const bool wantNormal = hasOpt(opts, "normal") || hasOpt(opts, "normopts");
    if (wantNormal && k.sd > 0 && !drawn.empty()) {
      graph["normal_points"] = normalCurve(xLow, xHigh, k.mean, k.sd, 1.0);
      graph["normal_mean"] = k.mean;
      graph["normal_sd"] = k.sd;
      if (sersets != nullptr) sersets->push_back(functionSerset(graph["normal_points"]));
    }
  }

  // kdensity is r-class: r(kernel bwidth n scale), also under nograph.
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("kernel", kc.name);
  session.eval.rValues.set("bwidth", k.bwidth);
  session.eval.rValues.set("n", static_cast<double>(k.n));
  session.eval.rValues.set("scale", k.scale);
  eval::declareRPosted(session.eval, "kdensity", {"kernel", "bwidth", "n", "scale"});

  if (!newNames.empty()) {
    // The density column is created before the point column (hk_kd_*_g
    // positions 16 15); one name with at() keeps the density only.
    const std::string densVar = newNames.back();
    std::vector<std::string> made = {densVar};
    std::optional<ojson> bad =
        writeGenVariable(session, densVar, paddedColumn(k.density, nRows), "double", "%10.0g", "density: " + varTitle);
    if (!bad && newNames.size() == 2) {
      bad = writeGenVariable(session, newNames.front(), paddedColumn(k.grid, nRows), "double", "%10.0g", varTitle);
      made.push_back(newNames.front());
    }
    if (bad) return *bad;
    if (pre != nullptr) pre->push_back(mutationResult(session, "kdensity", made, {}));
  }
  return graph;
}

}  // namespace piet::handlers
