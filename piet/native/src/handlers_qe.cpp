// Arc F — the seven QE / RD commands' PIET half: parse the Stata line, call the
// already-merged cobra verb, and print the table exactly as Stata prints it.
//
// NO STATISTICS LIVE HERE. Every number in every table below is read straight
// out of the engine's JSON; this file's whole job is the three things a human
// actually sees — the Stata syntax it accepts, the column geometry it prints,
// and the e()/rc it leaves behind.
//
//   rdrobust y running, c(0)          -> cobra rdrobust
//   rddensity running, c(0)           -> cobra rddensity
//   rdplot y running, c(0)            -> cobra rdplot          (table only; the
//                                        plot coordinates in the JSON are a
//                                        sibling arc's GRAPH half)
//   binscatter y x1, nquantiles(20)   -> cobra binscatter      (prints NOTHING —
//                                        real Stata prints zero lines here; the
//                                        deliverable is the graph)
//   eventstudyinteract y F2 .. L4, absorb() cohort() control_cohort() vce()
//   did_imputation y id t g, horizons() pretrends() autosample
//   sdid y id t treated, vce() reps() seed()
//
// RENDERING CONTRACT. Handlers here return an ARRAY of blocks: one `display`
// block per printed line, then ONE structured block last. That is the
// handlers_resample.cpp precedent (P6 wave) and it is what makes the log tee
// and the console show the Stata table rather than a JSON key dump — the tee
// (session_state.cpp::logTee) renders `display` blocks as their text and
// everything else as `[type]` + an indented dump. It also composes with
// `quietly` for free: the quietly filter keeps only error/echo blocks, so
// `quietly rdrobust ...` prints nothing without this file knowing about it.
//
// LAYOUT AUTHORITY. tests/kitchensink/pangram/waves/_oracle/p3_stata.log lines
// 152-378 (live Stata 19.5). Every column width below was measured off that
// file, not guessed. The two RD header blocks and rddensity's binomial ladder
// are genuinely unique shapes, so they are laid out per-command here rather
// than by bending the shared coefficient-table renderer; the ordinary
// coefficient tables (eventstudyinteract / did_imputation) reuse the EXACT
// widths handlers_resample.cpp already established for Stata's 78-column
// regression table, and every number goes through fmtStataDefaultG — the one
// %9.0g renderer the whole product shares.
//
// P-VALUES. Not computed here. The engine returns a p for every row of every
// one of these commands, so there is no normal-CDF in this file at all.
#include "piet/handlers.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabRdrobust = piet::eval::registerOptVocab(
    {"rdrobust"}, {{"c", "p", "q", "h", "b", "kernel", "nnmatch", "level"},
     {"covs", "fuzzy", "bwselect", "vce", "scalepar", "scaleregul", "masspoints",
      "bwcheck", "bwrestrict", "stdvars", "all", "weights", "deriv", "rho", "sharpbw"}});
const piet::eval::OptVocab& kVocabRddensity = piet::eval::registerOptVocab(
    {"rddensity"}, {{"c", "p", "q", "h", "kernel"},
     {"vce", "fitselect", "bwselect", "all", "plot", "masspoints", "bino", "nobino",
      "binomw", "binomn", "binomv", "level"}});
const piet::eval::OptVocab& kVocabRdplot = piet::eval::registerOptVocab(
    {"rdplot"}, {{"c", "p", "h", "nbins", "kernel"},
     {"covs", "binselect", "scale", "weights", "ci", "shade", "hide", "genvars",
      "graph_options", "support", "masspoints"}});
const piet::eval::OptVocab& kVocabBinscatter = piet::eval::registerOptVocab(
    {"binscatter"}, {{"nquantiles"},
     {"controls", "absorb", "by", "line", "discrete", "medians", "reportreg", "linetype",
      "xq", "genxq", "savedata", "savegraph", "replace", "colors", "mcolors", "lcolors",
      "rd", "nbins"}});
const piet::eval::OptVocab& kVocabEventstudyinteract = piet::eval::registerOptVocab(
    {"eventstudyinteract"}, {{"absorb", "cohort", "control_cohort",
                                                          "vce", "level", "cluster"},
     {"covariates", "weights",
                                                                  "residualize"}});
const piet::eval::OptVocab& kVocabDidImputation = piet::eval::registerOptVocab(
    {"did_imputation"}, {{"horizons", "pretrends", "autosample",
                                                          "level"},
     {"fe", "controls", "unitcontrols", "timecontrols", "cluster", "wtr", "delta",
      "allhorizons", "avgeffectsby", "saveweights", "minn", "shift", "maxit", "tol",
      "loadweights", "autosample_off", "leaveout", "nose"}});
const piet::eval::OptVocab& kVocabSdid = piet::eval::registerOptVocab(
    {"sdid"}, {{"vce", "reps", "seed", "level"},
     {"covariates", "method", "graph", "g1on", "g1_opt", "g2_opt", "graph_export",
      "msize", "unstandardized", "mattitles"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

// --- error / engine plumbing -------------------------------------------------

ojson qeError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Handlers here return an ARRAY, so a rejection has to be an array too.
ojson qeErrorBlocks(const std::string& message, const std::string& code = "engine_error") {
  return ojson::array({qeError(message, code)});
}

// --- formatting helpers ------------------------------------------------------
// Deliberately local and tiny: the SHARED piece is fmtStataDefaultG (the %9.0g
// numeric renderer), which is what "do not write a second formatter" is about.
// padLeft/padRight/fixedN are three-line column primitives, identical in shape
// to handlers_resample.cpp's own file-local set.

std::string padLeft(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}
std::string padRight(const std::string& s, size_t w) {
  return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}
std::string g9(double v) { return fmtStataDefaultG(v); }

// Stata's coefficient-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer: _coef_table reserves the sign column, so the
// digits get 8 characters where `display` gets 9. fmtStataDefaultG is
// calibrated for the display context — its own doc comment records
// `2.157789219986937 -> "2.1577892"`, a NINE-character result — and that is
// correct there; it is only the table cell that is narrower. Measured on the
// oracle twice over: did_imputation's tau0 (1.2081958740803342) prints
// `1.208196`, not `1.2081959` (p3_stata.log:326), and P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64). Below |1| the two rules COINCIDE —
// ".2071479" is eight characters either way — which is exactly why the shared
// renderer already reproduces every eventstudyinteract row byte for byte and
// only the >=1 magnitudes need this pass. The shared renderer still decides
// the digits whenever its answer fits; this only re-runs the same %g ladder
// one notch tighter when it does not.
std::string g9cell(double v) {
  std::string s = fmtStataDefaultG(v);
  const size_t sign = (!s.empty() && s[0] == '-') ? 1u : 0u;
  if (s.size() - sign <= 8) return s;
  for (int prec = 7; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t sg = (!t.empty() && t[0] == '-') ? 1u : 0u;
    if (t.size() > sg + 1 && t.compare(sg, 2, "0.") == 0) t.erase(sg, 1);
    if (t.size() - sg <= 8) return t;
  }
  return s;
}

std::string fixedN(double v, int n) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", n, v);
  return std::string(buf);
}
std::string f2(double v) { return fixedN(v, 2); }
std::string f3(double v) { return fixedN(v, 3); }
std::string f4(double v) { return fixedN(v, 4); }
std::string f5(double v) { return fixedN(v, 5); }

std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

// JSON readers that never throw: a field the engine did not send prints as
// Stata's own missing marker rather than crashing or inventing a zero.
double num(const ojson& j, const char* key, double fallback = 0.0) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return j.at(key).get<double>();
}
int64_t inum(const ojson& j, const char* key, int64_t fallback = 0) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return static_cast<int64_t>(j.at(key).get<double>());
}
std::string str(const ojson& j, const char* key, const std::string& fallback = "") {
  if (!j.contains(key) || !j.at(key).is_string()) return fallback;
  return j.at(key).get<std::string>();
}
// Stata's standard 78-column coefficient-table column header, e.g.
//   " Coefficient  std. err.      t    P>|t|     [95% conf. interval]"
// `errWord` is "std. err." (eventstudyinteract, under a Robust banner) or
// "Std. err." (did_imputation); `stat` is "t" or "z".
std::string coefHeader(const std::string& errWord, const std::string& stat,
                       const std::string& lvl) {
  return " Coefficient  " + errWord + "      " + stat + "    P>|" + stat + "|     [" + lvl +
         "% conf. interval]";
}

// A nested engine map (`coef`, `se`, ...) keyed by coefficient name.
std::optional<double> mapNum(const ojson& j, const char* obj, const std::string& key) {
  if (!j.contains(obj) || !j.at(obj).is_object()) return std::nullopt;
  const auto& m = j.at(obj);
  if (!m.contains(key) || !m.at(key).is_number()) return std::nullopt;
  return m.at(key).get<double>();
}

// The confidence-level word Stata stamps into a CI header: 95 -> "95".
std::string levelWord(double level) { return g9(level); }

// --- the two RD header-block geometries --------------------------------------
// rdrobust / rddensity share ONE geometry (label right-aligned in 18, " |",
// then a 10- and a 12-wide numeric column = 42 columns); rdplot's label field
// is three wider (21 + " |", still 10/12 after it = 45). The right-hand
// "Number of obs = ..." panel differs per command in both its start column and
// its value width, so it is a parameter rather than a constant.

struct TwoColGeom {
  size_t label;  // width of the right-aligned row label
  size_t c1;     // width of the "Left of c" column
  size_t c2;     // width of the "Right of c" column
};
constexpr TwoColGeom kRdGeom{18, 10, 12};      // rdrobust, rddensity
constexpr TwoColGeom kRdplotGeom{21, 10, 12};  // rdplot

std::string twoColRow(const TwoColGeom& g, const std::string& label, const std::string& a,
                      const std::string& b) {
  return padLeft(label, g.label) + " |" + padLeft(a, g.c1) + padLeft(b, g.c2);
}
std::string twoColRule(const TwoColGeom& g) {
  return std::string(g.label + 1, '-') + "+" + std::string(g.c1 + g.c2, '-');
}

// The right-hand info panel: `pad` blanks, then key padded to keyW, "=", then
// the value right-aligned in valW.
std::string infoCell(size_t pad, const std::string& key, size_t keyW, const std::string& value,
                     size_t valW) {
  return std::string(pad, ' ') + padRight(key, keyW) + "=" + padLeft(value, valW);
}

// --- block emission ----------------------------------------------------------

struct Printer {
  ojson lines = ojson::array();
  void operator()(const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  }
};

// --- e-class posting ---------------------------------------------------------
//
// BUG-163's rule, applied: a command that posts e() in Stata must post OUR
// numbers, never leave the previous model's e() reachable under its name. The
// three estimation commands (eventstudyinteract / did_imputation / sdid) build
// a real coeftable so `_b[F2]` / `_se[tau0]` / `e(N)` answer from the fit that
// just ran. rdrobust and rddensity are e-class in Stata too, but they
// `ereturn clear` + post SCALARS ONLY (there is no e(b) after rdrobust), so
// they post e(N) with NO coeftable: `_b[...]` then answers "." — visibly
// absent, which is the honest report, rather than silently answering from
// whatever regression ran three lines earlier.
//
// rdplot and binscatter are NOT e-class in Stata (rdplot is r-class; binscatter
// posts nothing), so they deliberately leave lastModel untouched — clearing it
// would be its own wrong answer.
//
// `result` carries the engine's key names where they already match what
// macros.cpp's resolver wants (n_obs -> e(N), n_clusters -> e(N_clust), F, r2,
// adj_r2, rmse, p_F -> e(p)); anything we do not compute is simply absent, so
// e(ll) and friends answer "." instead of a fabricated number.
void postEClass(SessionState& session, const std::string& cobraCmd, const std::string& depvar,
                const std::string& dataPath, ojson result) {
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.y = depvar;
  lm.depvar = depvar;
  lm.dataPath = dataPath;
  lm.result = std::move(result);
  session.setLastModel(lm);
}

ojson coefRow(const std::string& name, std::optional<double> b, std::optional<double> se,
              std::optional<double> t, std::optional<double> p, std::optional<double> lo,
              std::optional<double> hi) {
  ojson r;
  r["name"] = name;
  auto put = [&](const char* k, const std::optional<double>& v) {
    if (v.has_value()) r[k] = *v; else r[k] = nullptr;
  };
  put("b", b);
  put("se", se);
  put("t", t);
  put("p", p);
  put("ci_lo", lo);
  put("ci_hi", hi);
  return r;
}

// --- shared option plumbing --------------------------------------------------

std::optional<std::string> optStr(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}
bool optFlag(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name) return true;
  }
  return false;
}
std::vector<std::string> splitWsOrComma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
// ROW ORDER IS NOT THE ENGINE'S KEY ORDER. runCobraJson returns
// nlohmann::json (NOT ordered_json), whose object is a std::map — so the
// engine's `coef` object arrives SORTED BY NAME, not in the order the model
// was specified. did_imputation showed this immediately: `pre1 pre2 tau0..`
// instead of Stata's `tau0..tau3 pre1 pre2`. Every table below therefore
// builds its own row order from the REQUEST (the varlist / horizons /
// pretrends the user actually asked for), then appends any leftover key the
// order missed so a coefficient can never be silently dropped.
std::vector<std::string> orderedCoefNames(const ojson& r, const std::vector<std::string>& wanted) {
  std::vector<std::string> out;
  auto has = [&](const std::string& n) {
    return r.contains("coef") && r.at("coef").is_object() && r.at("coef").contains(n);
  };
  for (const auto& n : wanted) {
    if (has(n)) out.push_back(n);
  }
  if (r.contains("coef") && r.at("coef").is_object()) {
    for (auto it = r.at("coef").begin(); it != r.at("coef").end(); ++it) {
      bool seen = false;
      for (const auto& n : out) {
        if (n == it.key()) { seen = true; break; }
      }
      if (!seen) out.push_back(it.key());
    }
  }
  return out;
}

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// Forward a scalar-valued option straight through under an engine flag.
void pushOpt(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
             const std::string& stataOpt, const std::string& flag) {
  auto v = optStr(parsed, stataOpt);
  if (v.has_value() && !v->empty()) {
    argv.push_back(flag);
    argv.push_back(*v);
  }
}

// `level(#)` -> --level, honouring `set level` when the command carries none
// (the same fallback every other estimator in this codebase uses).
void pushLevel(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
               const SessionState& session) {
  auto v = optStr(parsed, "level");
  if (v.has_value() && !v->empty()) {
    argv.push_back("--level");
    argv.push_back(*v);
  } else if (session.level != 95) {
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }
}

void pushKeepif(std::vector<std::string>& argv, const eval::ParsedCommand& parsed) {
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
}

// One call site for "run the engine, or turn its refusal into an honest PIET
// error". The engine already refuses unsupported options BY NAME
// ("rdrobust: --kernel epanechnikov is not supported ..."); that message is
// surfaced verbatim rather than being replaced with a generic one.
struct EngineCall {
  bool ok = false;
  ojson raw;
  ojson error;
};
EngineCall callEngine(std::vector<std::string> argv) {
  EngineCall r;
  if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");
  try {
    r.raw = runCobraJson(argv);
    r.ok = true;
  } catch (const CobraError& ce) {
    r.error = qeError(cobraErrorMessage(ce), ce.code);
  }
  return r;
}

// --- Arc G: the graph half of rdplot / binscatter ---------------------------
//
// COMPOSE, DO NOT INVENT. rdplot and binscatter are both, structurally,
// "some binned/pre-averaged points plus one overlay curve" — exactly the
// shape `twoway (scatter y x) (line y x)` already draws. So this section
// builds the SAME block shape runTwowayLayers (graph_m4.cpp) emits —
// `{"type":"graph","kind":"twoway","layers":[{"kind":"scatter",...},
// {"kind":"line",...}]}` — and hands it to buildSvgTwoway UNCHANGED; no new
// SVG-drawing code exists anywhere in this file or in graph_m4.cpp. The one
// shared piece pulled in is twoway::layerStyle (piet/twoway_opts.h), the
// exact per-layer color/marker resolver every twoway layer already uses.
//
// SHAPE MISMATCH. The engine's own plot arrays are `[{"x":.., "y":..}, ...]`
// (cmds_rd.cpp's plot_pairs / cmds_binscatter.cpp's per-bin objects) — the
// graph layer's own `points` field is positional `[[x,y], ...]`
// (buildSvgTwoway indexes `pts[i][0]`/`pts[i][1]`). The two converters below
// are the entire translation.

// `[{"x":.., "y":..}, ...]` -> `[[x,y], ...]`. A non-numeric or malformed
// entry is dropped rather than crashing or inventing a zero — the same
// never-throw discipline num()/str() above use for the table half.
ojson xyObjArrayToPoints(const ojson& arr) {
  ojson out = ojson::array();
  if (!arr.is_array()) return out;
  for (const auto& p : arr) {
    if (p.is_object() && p.contains("x") && p.contains("y") && p.at("x").is_number() &&
        p.at("y").is_number()) {
      out.push_back(ojson::array({p.at("x").get<double>(), p.at("y").get<double>()}));
    }
  }
  return out;
}

// rdplot's `bins`/`fit` keys are each `{"left":[...], "right":[...]}` —
// reach one side of one of those two groups and convert it in one call.
ojson qeSidePoints(const ojson& r, const char* group, const char* side) {
  if (!r.contains(group) || !r.at(group).is_object()) return ojson::array();
  const ojson& g = r.at(group);
  if (!g.contains(side) || !g.at(side).is_array()) return ojson::array();
  return xyObjArrayToPoints(g.at(side));
}

// One rendered twoway layer, built exactly the way layerScatter/
// layerLineOrConnected (graph_m4.cpp) build theirs — `style` comes from
// twoway::layerStyle with an EMPTY opts list (these two commands have no
// per-layer color/symbol options of their own), so a scatter layer gets a
// filled circle and a line layer gets a plain stroked path, same as any
// other twoway plot's default styling.
ojson qeGraphLayer(const std::string& kind, ojson points, int layerIdx) {
  ojson lyr;
  lyr["kind"] = kind;
  const int64_t n = static_cast<int64_t>(points.size());
  lyr["points"] = std::move(points);
  lyr["style"] = twoway::layerStyle({}, layerIdx, kind);
  lyr["n"] = n;
  return lyr;
}

// Assembles the full `{"type":"graph","kind":"twoway",...}` block, field for
// field the same as runTwowayLayers' own tail (graph_m4.cpp) — x_min/x_max/
// y_min/y_max computed from the layers actually drawn, an empty-data
// fallback to a unit range rather than an error (mirroring runTwowayLayers'
// own "Stata still draws the framed plot ... Never error" rule), and the
// same axes/legend/name/n_obs/suppressed fields every other graph command's
// block carries so `graph export`/`graph save`/`graph combine` (which read
// those fields generically) see nothing unusual about a QE graph.
ojson qeGraphBlock(const std::vector<ojson>& layers, const std::string& title,
                   const std::string& xlabel, const std::string& ylabel,
                   SessionState& session, std::optional<double> xlineAt = std::nullopt) {
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nObs = 0;
  ojson layersJ = ojson::array();
  for (const auto& lyr : layers) {
    for (const auto& p : lyr.value("points", ojson::array())) {
      double x = p[0].get<double>(), y = p[1].get<double>();
      if (first) {
        xMin = xMax = x;
        yMin = yMax = y;
        first = false;
      } else {
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
        yMin = std::min(yMin, y);
        yMax = std::max(yMax, y);
      }
    }
    nObs += lyr.value("n", 0);
    layersJ.push_back(lyr);
  }
  if (first) {  // no plottable point in any layer
    xMin = 0.0; xMax = 1.0; yMin = 0.0; yMax = 1.0;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "twoway";
  out["title"] = title;
  out["subtitle"] = std::string();
  out["x_label"] = xlabel;
  out["y_label"] = ylabel;
  out["note"] = std::string();
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["layers"] = layersJ;

  ojson axes;
  axes["xline"] = ojson::array();
  if (xlineAt.has_value()) axes["xline"].push_back(*xlineAt);
  axes["yline"] = ojson::array();
  axes["xscale_noline"] = false;
  axes["yscale_noline"] = false;
  axes["xlabel_none"] = false;
  axes["ylabel_none"] = false;
  out["axes"] = axes;

  ojson legend;
  legend["order"] = ojson::array();
  legend["cols"] = nullptr;
  legend["size"] = nullptr;
  legend["off"] = true;  // neither command has a legend() option to honour
  out["legend"] = legend;

  out["name"] = nullptr;  // rdplot/binscatter accept no name() option (yet)
  out["n_obs"] = nObs;
  out["suppressed"] = session.graphicsOff;
  return out;
}

// The dual-flow cache seam (graph_m4.cpp's L2-FIX-B / BUG-066): the FULL
// block goes into session.lastGraph verbatim (so `graph export`/`graph save`
// see every point, byte-identical to what was computed); a SEPARATE copy is
// thinned by decimateGraphBlockForView for the live console/bridge view. The
// two must never alias the same ojson, or the mutation done for the view
// would corrupt the one graph export reads back later.
ojson registerQeGraph(ojson block, SessionState& session) {
  session.lastGraph = block;      // full copy, this is what export/save read
  ojson view = block;             // independent deep copy for the live view
  decimateGraphBlockForView(view);
  return view;
}

}  // namespace

// ===========================================================================
// rdrobust y running [if], c(#) p(#) q(#) h(#) b(#) kernel() nnmatch(#) level(#)
//
// Oracle: p3_stata.log:153-175.
ojson runRdrobust(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdrobust requires exactly two variables — `rdrobust depvar runvar, c(#)`.",
        "syntax_error");
  }
  // Stata's rdrobust has a much wider option surface than cobra's CLI. The
  // ones cobra has no flag for are named explicitly as "not yet implemented"
  // rather than being silently dropped (a dropped bwselect() would answer a
  // DIFFERENT specification with rc 0).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdrobust.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdrobust.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdrobust", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"b", "--b"},
                                   {"kernel", "--kernel"},
                                   {"nnmatch", "--nnmatch"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const bool mass = (str(r, "masspoints") == "adjust");
  if (mass) {
    say("Mass points detected in the running variable.");
    say("");
  }
  const std::string design = str(r, "design", "sharp");
  say((design == "fuzzy" ? std::string("Fuzzy") : std::string("Sharp")) +
      " RD estimates using local polynomial regression.");
  say("");

  const auto& G = kRdGeom;
  // Header row + the right-hand info panel (10 blanks are folded into the
  // 12-space gap the oracle shows between the two halves: 12 blanks, key
  // padded to 14, "=", value right-aligned in 11 => column 80).
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(12, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(12, "BW type", 14, str(r, "bw_type"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(12, "Kernel", 14, str(r, "kernel"), 11));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(12, "VCE method", 14, str(r, "vce"), 11));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "BW bias (b)", f3(num(r, "b_left")), f3(num(r, "b_right"))));
  say(twoColRow(G, "rho (h/b)", f3(num(r, "rho")), f3(num(r, "rho"))));
  say(twoColRow(G, "Unique obs", std::to_string(inum(r, "n_unique_left")),
                std::to_string(inum(r, "n_unique_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  // The estimate table. 80 columns: label 18 + " |", an 8-wide estimate cell
  // padded out to 15, "|", then z (7) / p (13) / CI (12 + 12).
  const std::string rule80(80, '-');
  const std::string sep = std::string(19, '-') + "+" + std::string(60, '-');
  const std::string lvl = levelWord(num(r, "level", 95.0));
  say(rule80);
  say(padLeft("", 18) + " |" + padRight(" Point", 15) + "| Robust Inference");
  say(padLeft("", 18) + " |" + padRight(" Estimate", 15) + "|" + padLeft("z-stat", 7) +
      padLeft("P>|z|", 13) + padLeft("[" + lvl + "% Conf. Interval]", 24));
  say(sep);
  say(padLeft("RD Effect", 18) + " |" + padRight(padLeft(g9(num(r, "effect")), 8), 15) + "|" +
      padLeft(f4(num(r, "z")), 7) + padLeft(f3(num(r, "pvalue")), 13) +
      padLeft(g9(num(r, "ci_lo")), 12) + padLeft(g9(num(r, "ci_hi")), 12));
  say(rule80);
  if (mass) say("Estimates adjusted for mass points in the running variable.");

  // e-class: scalars only, no coeftable (see postEClass's doc comment).
  ojson e;
  e["n_obs"] = inum(r, "n_obs");
  e["tau_cl"] = num(r, "effect");
  e["se_tau_rb"] = num(r, "se");
  e["pv_rb"] = num(r, "pvalue");
  e["ci_l_rb"] = num(r, "ci_lo");
  e["ci_r_rb"] = num(r, "ci_hi");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  e["b_l"] = num(r, "b_left");
  e["b_r"] = num(r, "b_right");
  e["N_h_l"] = inum(r, "n_eff_left");
  e["N_h_r"] = inum(r, "n_eff_right");
  postEClass(session, "rdrobust", str(r, "y"), *session.currentHbin(), e);

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rddensity runvar [if], c(#) p(#) q(#) h(#) kernel()
//
// Oracle: p3_stata.log:186-222. Note the manipulation test takes ONLY the
// running variable — there is no depvar.
ojson runRddensity(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 1) {
    return qeErrorBlocks(
        "syntax_error: rddensity takes exactly one variable (the running variable) — "
        "`rddensity runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRddensity.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRddensity.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rddensity", "--x", parsed.varlist[0]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  say("Computing data-driven bandwidth selectors.");
  say("");
  if (str(r, "masspoints") == "adjust") {
    say("Point estimates and standard errors have been adjusted for repeated observations.");
    say("(Use option nomasspoints to suppress this adjustment.)");
    say("");
  }
  say("RD Manipulation test using local polynomial density estimation.");
  say("");

  const auto& G = kRdGeom;
  // rddensity's cutoff label is `c = ` + a %.3f value in its own 10-wide cell,
  // not the trimmed spelling rdrobust uses.
  const std::string cutoffLabel = padLeft("c =", 8) + padLeft(f3(num(r, "cutoff")), 10);
  say(cutoffLabel + " |" + padLeft("Left of c", G.c1) + padLeft("Right of c", G.c2) +
      infoCell(10, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 13));
  say(twoColRule(G) + infoCell(10, "Model", 14, str(r, "model"), 13));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(10, "BW method", 14, str(r, "bw_method"), 13));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(10, "Kernel", 14, str(r, "kernel"), 13));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))) +
      infoCell(10, "VCE method", 14, str(r, "vce"), 13));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say("");
  say("Running variable: " + str(r, "x") + ".");

  const std::string rule42(42, '-');
  say(rule42);
  // Stata's own header spacing here is not column-aligned with the values
  // below it (the "T" sits three columns left of where the value ends); the
  // oracle is the spec, so the header is reproduced literally.
  say(padLeft("Method", 18) + " |" + "      T          P>|T|");
  say(twoColRule(G));
  say(twoColRow(G, "Robust", f4(num(r, "T")), f4(num(r, "pvalue"))));
  say(rule42);
  say("");

  say("P-values of binomial tests. (H0: prob = .5)");
  const std::string rule53(53, '-');
  say(rule53);
  say(padLeft("Window Length / 2", 18) + " |" + padLeft("<c", 9) + padLeft(">=c", 12) + " |" +
      padLeft("P>|T|", 10));
  say(std::string(19, '-') + "+" + std::string(22, '-') + "+" + std::string(10, '-'));
  if (r.contains("binomial") && r.at("binomial").is_array()) {
    for (const auto& row : r.at("binomial")) {
      say(padLeft(f3(num(row, "window")), 18) + " |" +
          padLeft(std::to_string(inum(row, "n_left")), 9) +
          padLeft(std::to_string(inum(row, "n_right")), 12) + " |" +
          padLeft(f4(num(row, "p")), 10));
    }
  }
  say(rule53);

  ojson e;
  e["n_obs"] = inum(r, "n_obs");
  e["N_l"] = inum(r, "n_left");
  e["N_r"] = inum(r, "n_right");
  e["c"] = num(r, "cutoff");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  postEClass(session, "rddensity", str(r, "x"), *session.currentHbin(), e);

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rdplot y running [if], c(#) p(#) h(#) nbins(# #) kernel()
//
// Oracle: p3_stata.log:233-261 (note the ONE trailing blank line rdplot prints
// after its rule — every other command here prints none).
//
// SCOPE: table only. The JSON's `bins`/`fit`/`fit_coef` arrays are the plot,
// and drawing them is a sibling arc's GRAPH half; they ride out on the
// structured block untouched so that arc has them.
ojson runRdplot(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdplot requires exactly two variables — `rdplot depvar runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdplot.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdplot.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdplot", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  // Stata writes `nbins(# #)` space-separated; the engine wants `L,R`.
  if (auto nb = optStr(parsed, "nbins"); nb.has_value() && !nb->empty()) {
    argv.push_back("--nbins");
    argv.push_back(joinComma(splitWsOrComma(*nb)));
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  if (str(r, "masspoints") == "adjust") {
    say("Mass points detected in the running variable.");
    say("");
  }
  // Only the `esmv` phrasing is oracle-verified (p3_stata.log:235); the manual
  // form is the plain evenly-spaced wording rdplot uses when the bin count is
  // given rather than selected.
  const std::string binselect = str(r, "binselect", "esmv");
  std::string what = "evenly spaced mimicking variance number of bins";
  if (binselect == "manual" || binselect == "es") what = "evenly spaced number of bins";
  say("RD Plot with " + what + " using polynomial regression.");
  say("");

  const auto& G = kRdplotGeom;
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(8, "Number of obs", 15, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(8, "Kernel", 15, str(r, "kernel"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))));
  say(twoColRow(G, "Order poly. fit (p)", std::to_string(inum(r, "p_left")),
                std::to_string(inum(r, "p_right"))));
  say(twoColRow(G, "BW poly. fit (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "Number of bins scale", f3(num(r, "scale_left")), f3(num(r, "scale_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  const std::string rule45(45, '-');
  say(rule45);
  say(twoColRow(G, "", "Left of c", "Right of c"));
  say(twoColRule(G));
  say(twoColRow(G, "Bins selected", std::to_string(inum(r, "bins_left")),
                std::to_string(inum(r, "bins_right"))));
  say(twoColRow(G, "Average bin length", f3(num(r, "avg_bin_length_left")),
                f3(num(r, "avg_bin_length_right"))));
  say(twoColRow(G, "Median bin length", f3(num(r, "med_bin_length_left")),
                f3(num(r, "med_bin_length_right"))));
  say(twoColRule(G));
  say(twoColRow(G, "IMSE-optimal bins", std::to_string(inum(r, "bins_imse_left")),
                std::to_string(inum(r, "bins_imse_right"))));
  say(twoColRow(G, "Mimicking Var. bins", std::to_string(inum(r, "bins_mv_left")),
                std::to_string(inum(r, "bins_mv_right"))));
  say(twoColRule(G));
  // Stata prints this section label with a trailing space after the pipe.
  say(padLeft("Rel. to IMSE-optimal:", G.label) + " | ");
  say(twoColRow(G, "Implied scale", f3(num(r, "implied_scale_left")),
                f3(num(r, "implied_scale_right"))));
  say(twoColRow(G, "WIMSE var. weight", f3(num(r, "wimse_var_weight")),
                f3(num(r, "wimse_var_weight"))));
  say(twoColRow(G, "WIMSE bias weight", f3(num(r, "wimse_bias_weight")),
                f3(num(r, "wimse_bias_weight"))));
  say(rule45);
  say("");  // rdplot's own trailing blank line (oracle line 261)

  // rdplot is r-class in Stata, not e-class: lastModel is deliberately left
  // alone so a preceding regression's e() keeps answering, exactly as it would
  // in Stata.

  // Arc G: the picture. Four layers — one binned-scatter + one fitted-curve
  // per side of the cutoff — built from the SAME bins/fit arrays the table
  // above already read (no second engine call, no recomputation). The two
  // sides never share a layer, so there is no line connecting the last left
  // bin to the first right bin — that IS the "visually separated at the
  // cutoff" requirement, for free, just from drawing two independent
  // layers rather than one continuous one. layerIdx 0/1 also gives each
  // side its own palette color (scatter and its own fit curve share a
  // color), so the two sides read as two series even before the cutoff
  // line is looked at.
  std::vector<ojson> rdplotLayers = {
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "left"), 0),
      qeGraphLayer("line", qeSidePoints(r, "fit", "left"), 0),
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "right"), 1),
      qeGraphLayer("line", qeSidePoints(r, "fit", "right"), 1),
  };
  ojson rdplotGraph = qeGraphBlock(rdplotLayers, std::string(), str(r, "x"), str(r, "y"), session,
                                   num(r, "cutoff"));
  say.lines.push_back(registerQeGraph(std::move(rdplotGraph), session));

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// binscatter y x [if], nquantiles(#)
//
// Oracle: p3_stata.log:271-272 — real Stata prints ZERO lines. The deliverable
// is the graph, and drawing it is a sibling arc's job. Adding a "done" line
// here would be a parity violation, so this handler emits the structured block
// ONLY (which the log tee dumps under [qe_result] the same way [resample] is
// dumped, and which the graph arc reads for its coordinates).
ojson runBinscatter(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: binscatter requires exactly two variables — `binscatter yvar xvar, "
        "nquantiles(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabBinscatter.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabBinscatter.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }
  // cobra binscatter has no --keepif: refusing here (naming the qualifier) is
  // the honest answer, rather than dropping the restriction and binning the
  // WHOLE dataset under an `if` the user wrote.
  if (parsed.ifExpr.has_value()) {
    return qeErrorBlocks(
        "binscatter: an `if` qualifier is not implemented (the engine's binscatter has no "
        "row-restriction flag). Restrict the data first, e.g. with `keep if`.",
        "unsupported_option");
  }

  std::vector<std::string> argv = {"binscatter", "--y", parsed.varlist[0], "--x",
                                   parsed.varlist[1]};
  pushOpt(argv, parsed, "nquantiles", "--nquantiles");
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  // binscatter is not e-class in Stata — lastModel untouched (see rdplot).

  // Arc G: the picture, and here it is the ENTIRE deliverable (see the
  // comment atop this function — Stata prints nothing). Two layers: the
  // nquantiles() bin means (cmds_binscatter.cpp's `bins`, keyed x_mean/
  // y_mean rather than rdplot's x/y — hence the bespoke conversion below
  // instead of xyObjArrayToPoints) plus ONE straight overlay line built from
  // `fit.intercept`/`fit.slope`, evaluated at the two ends of the bin-mean
  // x-range — the same "2-point line over the layer's own x-range" shape
  // layerLfit (graph_m4.cpp) already draws, just with the already-fitted
  // coefficients handed over rather than refit here.
  ojson binPoints = ojson::array();
  double xMin = 0.0, xMax = 0.0;
  bool firstBin = true;
  if (r.contains("bins") && r.at("bins").is_array()) {
    for (const auto& b : r.at("bins")) {
      if (!b.is_object() || !b.contains("x_mean") || !b.contains("y_mean") ||
          !b.at("x_mean").is_number() || !b.at("y_mean").is_number()) {
        continue;
      }
      double x = b.at("x_mean").get<double>(), y = b.at("y_mean").get<double>();
      binPoints.push_back(ojson::array({x, y}));
      if (firstBin) {
        xMin = xMax = x;
        firstBin = false;
      } else {
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
      }
    }
  }
  ojson fitPoints = ojson::array();
  if (!firstBin && r.contains("fit") && r.at("fit").is_object() && r.at("fit").contains("intercept") &&
      r.at("fit").contains("slope") && r.at("fit").at("intercept").is_number() &&
      r.at("fit").at("slope").is_number()) {
    const double a = r.at("fit").at("intercept").get<double>();
    const double b = r.at("fit").at("slope").get<double>();
    fitPoints.push_back(ojson::array({xMin, a + b * xMin}));
    fitPoints.push_back(ojson::array({xMax, a + b * xMax}));
  }
  std::vector<ojson> binscatterLayers = {
      qeGraphLayer("scatter", std::move(binPoints), 0),
      qeGraphLayer("line", std::move(fitPoints), 0),
  };
  ojson binscatterGraph =
      qeGraphBlock(binscatterLayers, std::string(), str(r, "xvar"), str(r, "depvar"), session);
  ojson graphView = registerQeGraph(std::move(binscatterGraph), session);

  ojson out = r;
  out["type"] = "qe_result";
  return ojson::array({graphView, out});
}

// ===========================================================================
// eventstudyinteract y rel1 rel2 ... [if], absorb(F1 F2) cohort(G)
//                                          control_cohort(NEVER) vce(cluster ID) level(#)
//
// Oracle: p3_stata.log:290-309. Stata's `absorb()` is space-separated and the
// engine wants a comma list; `vce(cluster id)` carries the cluster variable
// INSIDE the option and has to be split into `--vce cluster --cluster id`.
ojson runEventstudyinteract(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() < 2) {
    return qeErrorBlocks(
        "syntax_error: eventstudyinteract requires a depvar followed by at least one "
        "relative-period indicator — `eventstudyinteract y F2 L2, absorb(id t) cohort(g) "
        "control_cohort(never)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabEventstudyinteract.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabEventstudyinteract.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  auto absorb = optStr(parsed, "absorb");
  auto cohort = optStr(parsed, "cohort");
  auto controlCohort = optStr(parsed, "control_cohort");
  if (!absorb.has_value() || absorb->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `absorb(varlist)`.", "unsupported_option");
  }
  if (!cohort.has_value() || cohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `cohort(varname)`.", "unsupported_option");
  }
  if (!controlCohort.has_value() || controlCohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `control_cohort(varname)`.",
                          "unsupported_option");
  }

  std::vector<std::string> rel(parsed.varlist.begin() + 1, parsed.varlist.end());
  std::vector<std::string> argv = {"eventstudyinteract",
                                   "--y",
                                   parsed.varlist[0],
                                   "--rel",
                                   joinComma(rel),
                                   "--absorb",
                                   joinComma(splitWsOrComma(*absorb)),
                                   "--cohort",
                                   *cohort,
                                   "--control_cohort",
                                   *controlCohort};

  // vce(cluster id) -> --vce cluster --cluster id. A bare cluster(id) is
  // accepted too (some call sites spell it that way).
  std::string vceKind, clusterVar;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) {
      vceKind = toks[0];
      if (toks.size() > 1) clusterVar = toks[1];
    }
  }
  if (auto cl = optStr(parsed, "cluster"); cl.has_value() && !cl->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*cl);
    if (!toks.empty()) {
      clusterVar = toks[0];
      if (vceKind.empty()) vceKind = "cluster";
    }
  }
  if (vceKind == "cluster" && clusterVar.empty()) {
    return qeErrorBlocks("eventstudyinteract: vce(cluster ...) requires a cluster variable — "
                          "e.g. `vce(cluster id)`.",
                          "syntax_error");
  }
  if (!vceKind.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  if (!clusterVar.empty()) {
    argv.push_back("--cluster");
    argv.push_back(clusterVar);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  say("(obs=" + commaGroup(inum(r, "n_obs_weights")) + ")");
  say("");
  const int64_t nAbsorb =
      (r.contains("absorb") && r.at("absorb").is_array()) ? static_cast<int64_t>(r.at("absorb").size()) : 0;
  // Stata's standard 78-column estimation header: title padded to 56, then a
  // 14-wide key, "=", and a 7-wide right-aligned value.
  auto hdr = [&](const std::string& title, const std::string& key, const std::string& val) {
    return padRight(title, 56) + padRight(key, 14) + "=" + padLeft(val, 7);
  };
  say(hdr("IW estimates for dynamic effects", "Number of obs", commaGroup(inum(r, "n_obs"))));
  say(hdr("Absorbing " + std::to_string(nAbsorb) + " HDFE groups",
          "F(" + std::to_string(inum(r, "df1")) + ", " + std::to_string(inum(r, "df2")) + ")",
          f2(num(r, "F"))));
  say(hdr("", "Prob > F", f4(num(r, "prob_F"))));
  say(hdr("", "R-squared", f4(num(r, "r2"))));
  say(hdr("", "Adj R-squared", f4(num(r, "r2_a"))));
  say(hdr("", "Root MSE", f4(num(r, "rmse"))));
  const int64_t nClust = inum(r, "n_clusters");
  if (nClust > 0) {
    say(padLeft("(Std. err. adjusted for " + commaGroup(nClust) + " clusters in " +
                    str(r, "cluster") + ")",
                78));
  }

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft("", 12) + " |" + padLeft("Robust", 21));
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("std. err.", "t", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, rel)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto t = mapNum(r, "tstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(t.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, t, p, lo, hi));
  }
  say(rule78);

  // ONE object serves both consumers: `e()`/`_b[]` (macros.cpp reads it as
  // LastModel::result) and eststo/esttab (which snapshots lastEstBlock and
  // wants an estimation-shaped block with coeftable/n_obs/r2/depvar).
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "eventstudyinteract";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = nClust;
  e["F"] = num(r, "F");
  e["p_F"] = num(r, "prob_F");
  e["r2"] = num(r, "r2");
  e["adj_r2"] = num(r, "r2_a");
  e["rmse"] = num(r, "rmse");
  e["df_m"] = inum(r, "df1");
  e["df_r"] = inum(r, "df2");
  postEClass(session, "eventstudyinteract", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// did_imputation y ivar tvar gvar [if], horizons(numlist) pretrends(#) autosample level(#)
//
// Oracle: p3_stata.log:320-332. Note the z column (not t) and the verbatim
// autosample warning line, which comes straight from the engine's own
// `autosample_message` rather than being reconstructed here.
ojson runDidImputation(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: did_imputation takes four variables positionally — "
        "`did_imputation Y ivar tvar gvar`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabDidImputation.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabDidImputation.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"did_imputation", "--y",    parsed.varlist[0],
                                   "--ivar",         parsed.varlist[1],
                                   "--time",         parsed.varlist[2],
                                   "--gvar",         parsed.varlist[3]};
  pushOpt(argv, parsed, "horizons", "--horizons");
  pushOpt(argv, parsed, "pretrends", "--pretrends");
  if (optFlag(parsed, "autosample")) argv.push_back("--autosample");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const std::string warn = str(r, "autosample_message");
  if (!warn.empty()) {
    say(warn);
    say("");
  }
  // Unlike the eventstudyinteract header, did_imputation has no left-hand
  // title, and Stata right-aligns the whole "Number of obs = N" phrase in the
  // 78-column band (one column further right than the titled form).
  say(padLeft("Number of obs = " + commaGroup(inum(r, "n_obs")), 78));

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("Std. err.", "z", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  // Stata's row order is every horizon's tau, THEN the pretrends — not the
  // alphabetical order the engine's key-sorted JSON hands over.
  std::vector<std::string> wanted;
  if (r.contains("horizons") && r.at("horizons").is_array()) {
    for (const auto& h : r.at("horizons")) {
      if (h.is_number()) {
        wanted.push_back("tau" + std::to_string(static_cast<long long>(h.get<double>())));
      }
    }
  }
  for (int64_t k = 1; k <= inum(r, "pretrends"); ++k) {
    wanted.push_back("pre" + std::to_string(k));
  }

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, wanted)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto z = mapNum(r, "zstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(z.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, z, p, lo, hi));
  }
  say(rule78);

  // e(): the coefficient vector and N. No F/R2 — did_imputation computes
  // neither, so e(F)/e(r2) stay absent rather than reporting a stale model's.
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "did_imputation";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = inum(r, "n_clusters");
  postEClass(session, "did_imputation", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// sdid y ivar tvar treated [if], vce(bootstrap|noinference) reps(#) seed(#) level(#)
//
// Oracle: p3_stata.log:366-378.
//
// OUR SE IS OURS. The bootstrap draws come from this project's own RNG stream,
// so the SE / t / CI cells legitimately differ from Stata's. The ATT and the
// table shape are the parity claim.
ojson runSdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: sdid takes four variables positionally — `sdid Y ivar tvar treated`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdid.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabSdid.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"sdid",      "--y",    parsed.varlist[0],
                                   "--ivar",    parsed.varlist[1],
                                   "--time",    parsed.varlist[2],
                                   "--treated", parsed.varlist[3]};
  std::string vceKind;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) vceKind = toks[0];
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  pushOpt(argv, parsed, "reps", "--reps");
  pushOpt(argv, parsed, "seed", "--seed");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  // Stata prints the bootstrap progress ruler BEFORE the replications run;
  // ours is emitted around the (synchronous) engine call, which is the same
  // place in the transcript.
  Printer say;
  const int64_t repsRequested =
      [&]() -> int64_t {
        auto v = optStr(parsed, "reps");
        if (!v.has_value()) return 0;
        try { return std::stoll(*v); } catch (...) { return 0; }
      }();

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  const std::string vceUsed = str(r, "vce", vceKind);
  if (vceUsed == "bootstrap" || vceUsed == "placebo" || vceUsed == "jackknife") {
    const int64_t done = inum(r, "reps_done", repsRequested);
    say("Bootstrap replications (" + std::to_string(done) + "). This may take some time.");
    // Stata's fixed 50-column ruler, one dot per completed replication,
    // wrapping at 50.
    const std::string ruler = "----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5";
    say(ruler);
    for (int64_t i = 0; i < done; i += 50) {
      const int64_t n = std::min<int64_t>(50, done - i);
      say(std::string(static_cast<size_t>(n), '.'));
    }
  }
  say("");
  say("Synthetic Difference-in-Differences Estimator");
  say("");

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule77(77, '-');
  say(rule77);
  say(padLeft(str(r, "y"), 12) + " |" + "     ATT     Std. Err.     t      P>|t|    [" + lvl +
      "% Conf. Interval]");
  say(std::string(13, '-') + "+" + std::string(63, '-'));
  say(padLeft(parsed.varlist[3], 12) + " |" + padLeft(f5(num(r, "att")), 10) +
      padLeft(f5(num(r, "se")), 11) + padLeft(f2(num(r, "t")), 9) + padLeft(f3(num(r, "p")), 9) +
      padLeft(f5(num(r, "ci_lo")), 12) + padLeft(f5(num(r, "ci_hi")), 12));
  say(rule77);
  say(lvl + "% CIs and p-values are based on large-sample approximations.");
  say("Refer to Arkhangelsky et al., (2021) for theoretical derivations.");

  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "sdid";
  e["depvar"] = str(r, "y");
  e["coeftable"] = ojson::array({coefRow(parsed.varlist[3], num(r, "att"), num(r, "se"),
                                          num(r, "t"), num(r, "p"), num(r, "ci_lo"),
                                          num(r, "ci_hi"))});
  e["n_obs"] = inum(r, "n_obs");
  e["n_units"] = inum(r, "n_units");
  postEClass(session, "sdid", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
