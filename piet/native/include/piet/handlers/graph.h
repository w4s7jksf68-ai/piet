// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: graph commands, distribution math, save/combine/export, SVG.
// Implemented under piet/native/src/graph/.
#ifndef PIET_NATIVE_HANDLERS_GRAPH_H
#define PIET_NATIVE_HANDLERS_GRAPH_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "piet/twoway_parse.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// ---- AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) -----------------------------
// Community graph and geo verbs as thin shims over the graph/column paths that
// already exist. Nothing here adds a plot type: `cdfplot` emits the twoway line
// block the renderer already draws, `event_plot` forwards to coefplot, and
// `grc1leg2` is `graph combine` with one panel's legend kept.
//
// SIX commands, not the plan's fourteen. `grc1leg` itself is absent: its
// package makes no authorship statement -- no author line in the ado and none
// in the help file -- and a distribution URL is not the package's own claim, so
// the attribution rule gives it no row. The eight refusals (cibar, binsreg,
// spmap, maptile, shp2dta, geoinpoly, heatplot, distplot) are community names
// too, so they are unregistered for the same reason and answer a generic
// r(199), which is what a named refusal would have said anyway.

// `grc1leg2 name1 name2 ..., [legendfrom() rows() cols() title() ...]` --
// `runGraphCombine` with every panel but one having `legend.off` set, which the
// frontend already honours.
nlohmann::ordered_json runGrc1leg(const eval::ParsedCommand& parsed, SessionState& session);

// `cdfplot var [, by()]` -- the ECDF as a twoway line block.
nlohmann::ordered_json runCdfplot(const eval::ParsedCommand& parsed, SessionState& session);
// `event_plot [name], stub_lag() stub_lead() ...` -- forwards to runCoefplot.
nlohmann::ordered_json runEventPlot(const eval::ParsedCommand& parsed, SessionState& session);

// `coefplot [NAME...] [, drop() keep() xline() level() title() xtitle()
// ytitle()]` — coefficient-plot graph block from the stored estimates (or
// session.lastModel() as a single-model "last" fallback when the store is
// empty). C++ port of coefplot.py's run_coefplot.
// coefplot-family AN-1..AN-4 (graph/coefplot.cpp, graph/coefplot_syntax.cpp): coefplot 1.8.8's data
// pipeline -- the subgraph/plot/model grammar, keep/drop/order/rename/levels/
// ci()/matrix(), generate(), r(). Returns the blocks to emit before the graph
// (notes, the generate() mutation) or a refusal; `drawRows` receives the
// plotted rows (empty when there is nothing to plot).
nlohmann::ordered_json runCoefplotJann(SessionState& session, const eval::ParsedCommand& parsed,
                                       nlohmann::ordered_json* drawRows);

nlohmann::ordered_json runCoefplot(const std::vector<std::string>& names,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    SessionState& session);

// -- M4a item 4: graph JSON surface (ROADMAP M4a) -----------------------------
// Block STRUCTURE/JSON only (matches the frozen corpus's schema) — SVG/PNG
// rendering is frontend M4b scope. C++ ports of piet/backend/piet/graph.py's
// run_scatter/run_histogram/run_kdensity/run_line/run_xtline/run_tsline.
//
// `ds` is the (possibly `if`-filtered, temp-hbin-backed) Datastore the
// dispatcher resolves BEFORE calling these — mirrors dispatcher.py's own
// if_expr -> temp-hbin-swap done once, upstream of the graph_cmd branch.

nlohmann::ordered_json runScatter(Datastore& ds, const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// gfx-histogram-kdensity-family (oracle aer_w2 w2aq_histogram_family): ONE
// binning rule ([R] histogram) and ONE density rule ([R] kdensity, Methods and
// formulas). histogram/hist, twoway__histogram_gen and kdensity read them; the
// twoway histogram/kdensity layers read the same.
struct HistGenSpec {
  std::string varname;            // for start()'s message
  bool discrete = false;
  std::optional<int64_t> bins;    // bins()/bin(), integer > 0
  std::optional<double> width;    // width(), > 0
  std::optional<double> start;    // start()
  std::string type = "density";   // density | fraction | frequency | percent
  bool genFloat = true;           // generated values are stored as c(type) (float by default)
};
struct HistGenResult {
  int rc = 0;                     // 198: start() above the minimum
  std::string message;
  bool empty = false;             // r(N) == 0: Stata's empty-sample results
  double area = 0, max = 0, min = 0, start = 0, width = 0, N = 0;
  int64_t bin = 1;
  int64_t nx = 0;                 // r(n_x): rows of the generated centre
  bool userStart = false;
  bool closingRow = false;        // the extra (x = start, y = .) row
  std::string type;
  // generate(): one entry per OCCUPIED bin, ascending; values as stored
  // (float-rounded under c(type) float). `centre` may carry one more entry,
  // the closing row.
  std::vector<double> binIndex, count, centre, height;
};
HistGenResult histogramGen(const std::vector<double>& v, const std::vector<double>* w,
                           const HistGenSpec& spec, int64_t nDataRows, bool wantGenerate);

struct KdensitySpec {
  std::string kernel = "epanechnikov";
  double width = 0.0;   // 0 = the default bandwidth
  int64_t n = 50;
  std::string wtype;    // "" | fweight | aweight | iweight
};
struct KdensityResult {
  std::string kernel;
  double bwidth = 0, scale = 0, mean = 0, sd = 0;
  int64_t n = 0;
  bool nCapped = false;            // n() > _N: "(n() set to #)"
  std::vector<double> grid, density;  // grid mode: n points; at(): one per row
};
// The kernel K(z) by its full name; NaN for an unknown name.
double kdeKernel(const std::string& kernel, double z);
// The kernel a typed kernel() word names (Stata's abbreviations), nullopt when invalid.
std::optional<std::string> kdensityKernelName(const std::string& typed);
// The kernel density estimate at each point of `at` (NaN points stay NaN).
std::vector<double> kdensityAt(const std::vector<double>& x, const std::vector<double>& w,
                               const std::string& wtype, const std::string& kernel, double h,
                               const std::vector<double>& at);
// kdensity's default bandwidth ([R] kdensity), with the summarize min/max/mean/sd it reads.
double kdensityDefaultWidth(const std::vector<double>& x, const std::vector<double>& w,
                            const std::string& wtype, double* outMin, double* outMax,
                            double* outMean, double* outSd);
KdensityResult kdensityEstimate(const std::vector<double>& x, const std::vector<double>& w,
                                const KdensitySpec& spec, int64_t nDataRows,
                                const std::vector<double>* at);

// `histogram`/`hist`: `ds` is the (if-filtered) frame. Returns the graph block
// or an error block; `pre` receives the blocks Stata prints before the graph
// (the `(bin=#, start=#, width=#)` note).
// serset-twoway HIST-1: `sersets`, when given, receives the graph's sersets in
// plot order (histogram or by-panels, kdensity overlay, normal overlay).
nlohmann::ordered_json runHistogram(SessionState& session, Datastore& ds,
                                     const std::vector<std::string>& varlist,
                                     const eval::ParsedCommand& parsed,
                                     std::vector<nlohmann::ordered_json>* pre,
                                     std::vector<serset::Serset>* sersets = nullptr);

// `kdensity`: the graph block (empty object under nograph; *graphDrawn says
// which) or an error block; posts r() and writes generate(). `pre` receives
// the `(n() set to #)` note and the generate() mutation block.
// serset-twoway HIST-1: `sersets` receives the drawn graph's sersets (the
// density line, then the normal overlay); none under nograph.
nlohmann::ordered_json runKdensity(SessionState& session, Datastore& ds,
                                    const std::vector<std::string>& varlist,
                                    const eval::ParsedCommand& parsed,
                                    std::vector<nlohmann::ordered_json>* pre, bool* graphDrawn,
                                    std::vector<serset::Serset>* sersets = nullptr);

// A graph command's saving(filename[, replace]): writes the block as `graph
// save` does, without registering a name. Null on success, else an error block.
// serset-twoway GST: `sersets`, when given, are written beside the block
// (doc["sersets"]) so `graph use` can restore them. BUG-1529: on success
// `savedNote` (when given) receives the `file ... saved` note to print after the
// graph.
nlohmann::ordered_json saveGraphOption(const nlohmann::ordered_json& block, const std::string& savingText,
                                       SessionState& session,
                                       const std::vector<serset::Serset>* sersets = nullptr,
                                       nlohmann::ordered_json* savedNote = nullptr);

// graph-family AI-1/AI-2 (graph_bar.cpp): `graph bar|hbar|box|hbox` over `ds`
// (the if/in-filtered frame), `args` = the words after the subcommand (the
// statlist). `sets` holds the graph's one serset (`serset use`), `block` the
// graph block or the refusal.
struct BarGraphResult {
  nlohmann::ordered_json block;
  std::vector<serset::Serset> sets;
  bool ok = false;
};
BarGraphResult runGraphBarBox(SessionState& session, Datastore& ds, const std::string& kind,
                              const std::vector<std::string>& args, const eval::ParsedCommand& parsed);

// graph-family AI-4/AI-6/AI-7 (named_graphs.cpp): `graph describe [name |
// "file.gph"]` (argText = the text after the subcommand, before the comma),
// `graph copy|rename [old] new [, replace]`, `graph display|draw [name] [,
// xsize() ysize() ...]`. Each returns the blocks to emit.
nlohmann::ordered_json runGraphDescribe(SessionState& session, const std::string& argText);
nlohmann::ordered_json runGraphCopyRename(SessionState& session, bool rename,
                                          const std::vector<std::string>& names, bool replace);
nlohmann::ordered_json runGraphDisplay(SessionState& session, const std::string& nameArg,
                                       const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// serset-twoway GST: `graph use filename [, name(nm[, replace]) nodraw ...]`
// (gr_use.ado): opens filename (.gph appended to a name without a suffix;
// r(601) `file <path> not found`), registers the graph under name() or the
// file's base name and restores its sersets (w2al tf_st_use_sc `2|1|0 1|`,
// log:5100-5118). Returns the blocks to emit.
nlohmann::ordered_json runGraphUse(const std::string& fileArg,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `twoway__histogram_gen` (Stata's internal program histogram calls): an ARRAY
// of blocks (the display note, the generate() mutation) or one error object.
nlohmann::ordered_json runTwowayHistogramGen(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runGraphLine(Datastore& ds, const std::vector<std::string>& varlist,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts);

nlohmann::ordered_json runXtline(Datastore& ds, const std::vector<std::string>& varlist,
                                  const std::optional<std::string>& panelvar,
                                  const std::optional<std::string>& timevar,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts);

nlohmann::ordered_json runTsline(Datastore& ds, const std::vector<std::string>& varlist,
                                  const std::optional<std::string>& timevar,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// `twoway (plottype ...) (plottype ...) ...` — the parenthesised multi-layer
// form (M4a-remaining, closed by the analysis.do acceptance campaign). C++
// port of graph.py's run_twoway_layers(): renders each parsed LayerSpec
// (piet::twoway::parseTwoway output) against `dsPtr` (the caller resolves
// per-command `if`-filtering exactly as the bare-twoway path does; per-LAYER
// `if` composition happens INSIDE this function, mirroring the oracle).
// `dsPtr == nullptr` -> no_data_loaded. Returns a `kind:"twoway"` graph block
// (or an error block) — same shape buildSvgTwoway() (graph/svg.cpp's SVG
// structural builder) already renders.
// serset-twoway SS: `sersetPlots`, when given, receives one serset::PlotSpec per
// drawn layer, in layer order (the input of serset::buildPlotSersets).
// serset-twoway IN: `globalIn` is the `in` range after the last group; it
// intersects each layer's own `in` and `if`.
nlohmann::ordered_json runTwowayLayers(
    Datastore* dsPtr, SessionState& session, const std::vector<twoway::LayerSpec>& layers,
    const std::vector<std::pair<std::string, eval::OptValue>>& globalOpts,
    const std::optional<std::string>& globalIf,
    std::vector<serset::PlotSpec>* sersetPlots = nullptr,
    const std::optional<std::string>& globalIn = std::nullopt);

// `marginsplot` — reads session.lastMargins (cached by runMargins), plots one
// dot + 95% CI whisker per AME. C++ port of dispatcher.py's _run_marginsplot.
nlohmann::ordered_json runMarginsplot(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                       SessionState& session);

// -- M4b: graph save/combine/export (ROADMAP M4a-remaining + M4b item 4) ----

// `graph save [NAME] ["file.gph"] [, replace]` — register session.lastGraph
// (or an already-named graph) under NAME in session.namedGraphs, optionally
// persisting a JSON `.gph` sidecar ({"_magic":"PIETGPH1","block":<block>}).
// C++ port of dispatcher.py's `graph_save` handler branch.
nlohmann::ordered_json runGraphSave(const std::optional<std::string>& name,
                                     const std::optional<std::string>& filepath,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// `graph combine NAME1 NAME2 ...` — tile N panels into a grid
// (`kind:"graph_combine"`), sets session.lastGraph. Each token resolves as
// EITHER an in-memory named graph (session.namedGraphs) OR, if no such name
// exists, an on-disk `.gph` spec-JSON path (loadGphFile, graph/files.cpp) —
// BS-007/D3: analysis.do's loop-then-`graph save`-then-combine idiom passes
// FILE PATHS here (a Stata tempfile local reused every loop iteration means
// the in-memory name only ever holds the LAST iteration's graph). The .gph
// loader is a plain JSON deserialize — NEVER a raster/frozen-pixel format
// (D3 hard-stop: no more than serialize+load). C++ port of
// graph_combine.py's run_graph_combine, extended per BS-007.
nlohmann::ordered_json runGraphCombine(const std::vector<std::string>& names,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        SessionState& session);

// `graph export FILE[.svg|.png|.pdf] [, as(svg|png|pdf) replace width() height()]`
// — reads session.lastGraph. Format resolves from `as()` if given (BS-012),
// else the filename's extension. `svg`: pure-C++ structural SVG matching
// graph_export.py's `_build_svg*` geometry exactly (W=520,H=360, margins
// {34,18,48,62}) — scatter/histogram/line/twoway/graph_combine/coefplot/
// marginsplot kinds. `png`/`pdf`: real webview offscreen rasterization is
// deferred post-beta (D8) — PRE-AUTHORIZED FALLBACK writes an honest `.svg`
// file (basename-substituted, never SVG bytes under the requested .png/.pdf
// name) plus an explicit "rasterizer pending" warning block. C++ port of
// graph_export.py's run_graph_export (SVG half), extended per BS-012.
nlohmann::ordered_json runGraphExport(const std::string& filepathArg,
                                       const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                       SessionState& session);

// graph-family AI-13 (graph_export_formats.cpp): the structural SVG of a graph
// replayed as `fmt` ("png" | "pdf" | "eps"/"ps"); width/height in pixels for a
// png (0 = derived). False for any other format.
bool graphSvgToFormat(const std::string& svg, const std::string& fmt, int widthPx, int heightPx,
                      std::string* out);

// Builds the structural SVG string for one graph block (dispatches on
// block["kind"]). Exposed for runGraphExport and (if ever needed) direct
// testing; matches graph_export.py's `_build_svg` dispatcher exactly.
std::string buildGraphSvg(const nlohmann::ordered_json& block);

// L2-FIX-B (BUG-066 / C3-F5): thin a graph block's plotted point arrays for the
// LIVE VIEW ONLY (order-preserving proportional stride), tagging
// n_shown_points / n_total_points when anything was thinned so the frontend can
// draw the honest "(showing N of M points)" caption. MUST be called only AFTER
// the full block is cached into session.lastGraph / namedGraphs -- `graph
// export` and the SVG goldens read those and keep every point. No-op on
// non-graph blocks and on blocks already under the view budget.
void decimateGraphBlockForView(nlohmann::ordered_json& block);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_GRAPH_H
