// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/display_hardening.h"
#include "piet/handlers/data_values.h"
#include "piet/handlers/observations.h"
#include "piet/user_paths.h"

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
#include "graph/graph_extension_internal.h"

namespace {

// geodist. Every one of these is read: gen()/generate() names the column,
// `sphere` selects haversine, `radius()` replaces the sphere's radius, `miles`
// converts the unit.
const piet::eval::OptVocab& kVocabGeodist = piet::eval::registerOptVocab(
    {"geodist"}, {{"generate", "gen", "miles", "sphere", "radius"}, {}});

// geonear. `within()` (a distance cutoff), `long` (a reshaped long result) and
// `report` (a diagnostic table) are real geonear options that would each need a
// different result SHAPE, not a different number, so they are refused by name.
const piet::eval::OptVocab& kVocabGeonear = piet::eval::registerOptVocab(
    {"geonear"}, {{"neighbors", "nearcount", "miles", "ellipsoid", "genstub"},
                  {"within", "long", "report"},
                  // BUG-430: the minimum abbreviations geonear.ado DECLARES
                  // (`Neighbors`, `NEArcount`, `MIles`, `Ellipsoid`, `Genstub`,
                  // `RAdius`, `Ignoreself`, `REport`, `WIDe` -- the capitalised
                  // stem is the minimum). Only the ones the prefix rule cannot
                  // produce are listed: `mi`/`nea`/`gen` already resolve as
                  // unique prefixes, while `n` prefixes BOTH neighbors and
                  // nearcount and `e`/`g` are below the two-character floor.
                  {{"n", "neighbors"},
                   {"e", "ellipsoid"},
                   {"g", "genstub"},
                   {"i", "ignoreself"},
                   {"ra", "radius"},
                   {"re", "report"},
                   {"wid", "within"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace graph_extension_detail;

namespace {

ojson noteBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["class"] = "note";
  b["text"] = text;
  return b;
}

// ===========================================================================
// cobra plumbing
// ===========================================================================

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}

// Run ONE cobra verb. nullopt = success; otherwise the error BLOCK to return.
// Never a bare parse of stdout_text and never a swallowed rc (IDIOMS).
std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return gxError(cobraErrorMessage(ce), ce.code);
}

std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (auto v = optStr(opts, k)) return v;
  }
  return std::nullopt;
}

// The one-column HEADERLESS payload `cobra setcol` reads: one raw value per
// row, row count == n_rows exactly. String cells are quoted even when they do
// not need it, so a value's own padding survives the trip (BUG-292); an EMPTY
// cell is a bare blank line, the only spelling read_csv_column accepts for a
// missing factor cell.
bool writePayloadCsv(const std::string& path, const std::vector<std::string>& cells, bool quoted) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  for (const auto& c : cells) {
    if (c.empty()) {
      f << "\n";
      continue;
    }
    if (quoted) {
      f << '"';
      for (char ch : c) {
        if (ch == '"') f << '"';
        f << ch;
      }
      f << '"';
    } else {
      f << c;
    }
    f << "\n";
  }
  f.flush();
  return static_cast<bool>(f);
}

// Append ONE new column to `basePath` from a payload written here. `cells` are
// the rendered values ("" == missing); `dtype` is "f64" or "factor".
std::optional<ojson> appendColumn(SessionState& session, const std::string& basePath,
                                  const std::string& var, const std::vector<std::string>& cells,
                                  const std::string& dtype, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), "gxvals") + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  if (!writePayloadCsv(csvPath, cells, dtype == "factor")) {
    cecon::native_unlink(csvPath);
    return gxError("could not write the column payload (disk full or I/O error)", "data_io");
  }
  const std::string out = nextPath(session.workdir(), "gxsetcol");
  if (auto err = runVerb({"setcol", "--data", basePath, "--var", var, "--values", csvPath,
                          "--dtype", dtype, "--new", "--out", out})) {
    cecon::native_unlink(out);
    return err;
  }
  *outPath = out;
  return std::nullopt;
}

// A new variable name has to be legal AND free. Both checks, once, so every
// handler in this file refuses the same things for the same reasons.
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return gxSyntax(verb + ": invalid variable name `" + newvar + "' — " +
                    strutil::stataIdentifierRuleText());
  // BUG-1120: nor is a reserved name one (`geodist ..., gen(_cons)' is "_cons
  // invalid varname" r(198), StataBE 19.5).
  if (strutil::isReservedVarName(newvar))
    return ojson(eval::errorBlock("syntax_error", newvar + " invalid varname", std::nullopt,
                                  std::nullopt, std::nullopt, "r(198)"));
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(newvar))
    return gxError(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // null => no objection
}

// ===========================================================================
// `if`/`in` as a ROW MASK (geodist only)
// ===========================================================================
//
// The registry gives `geodist` allowIf/allowInRange, and it must: Stata's
// geodist takes a qualifier, and the honest reading of `geodist ... if x>0,
// gen(d)` is "compute d where the condition holds, leave it MISSING elsewhere"
// -- the same rule `generate ... if` follows. That needs a ROW-ALIGNED MASK
// over the full frame, not a filtered frame: the column written by
// `setcol --new` has to be exactly as long as the dataset.
//
// `applyIfAndInFilter` (inrange_filter.h) is the shared helper for this
// composition and `cdfplot` below uses it exactly as designed -- but it DROPS
// its materialized row-index column before returning, which is the one thing a
// mask needs. So this is a deliberate small private copy of the same 2-call
// shape (`--bygen IDX=_n` then `--keepif`), with its OWN index-column name so a
// leaked column can be attributed to the command that leaked it, reusing the
// shared `parseInRangeBounds` grammar and the shared `inRangeRejectError`
// wording so a user sees the same two sentences whichever command's `in`
// failed. It runs ONLY when a qualifier is present: no qualifier costs zero
// cobra calls.
constexpr const char* kRowIdx = "__piet_gx_row";

struct MaskResult {
  bool ok = true;
  bool present = false;    // a qualifier was given
  std::vector<char> keep;  // size == nRows, valid iff present && ok
  ojson err;               // valid iff !ok
};

MaskResult qualifierMask(SessionState& session, const std::string& verb,
                         const std::string& basePath, const eval::ParsedCommand& parsed,
                         int64_t nRows) {
  MaskResult r;
  const bool hasIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
  const bool hasIn = parsed.inRange.has_value() && !parsed.inRange->empty();
  if (!hasIf && !hasIn) return r;
  r.present = true;

  std::string pred;
  if (hasIn) {
    InRangeBounds b = parseInRangeBounds(*parsed.inRange, nRows);
    if (!b.ok) {
      InRangeFilterResult f;
      f.ok = false;
      f.negative = b.negative;
      r.ok = false;
      r.err = inRangeRejectError(f, *parsed.inRange, verb);
      return r;
    }
    pred = std::string(kRowIdx) + ">=" + std::to_string(b.a) + " & " + std::string(kRowIdx) +
           "<=" + std::to_string(b.b);
  }
  if (hasIf) pred = pred.empty() ? *parsed.ifExpr : ("(" + pred + ") & (" + *parsed.ifExpr + ")");

  const std::string idxPath = nextPath(session.workdir(), "gxidx");
  if (auto e = runVerb({"eval", "--data", basePath, "--bygen", std::string(kRowIdx) + "=_n",
                        "--out", idxPath})) {
    r.ok = false;
    r.err = *e;
    return r;
  }
  const std::string keptPath = nextPath(session.workdir(), "gxkeep");
  if (auto e = runVerb({"eval", "--data", idxPath, "--keepif", pred, "--out", keptPath})) {
    r.ok = false;
    r.err = *e;
    return r;
  }
  r.keep.assign(static_cast<size_t>(nRows), 0);
  try {
    Datastore kept(keptPath);
    if (!kept.has(kRowIdx)) {
      r.ok = false;
      r.err = gxError(verb + ": internal — the filtered frame lost its row index.", "internal");
      return r;
    }
    const std::vector<double> idx = columnAsDoubles(kept, kRowIdx);
    for (double v : idx) {
      if (std::isnan(v)) continue;
      const long long i = static_cast<long long>(v);
      if (i >= 1 && i <= nRows) r.keep[static_cast<size_t>(i - 1)] = 1;
    }
  } catch (const std::exception& e) {
    r.ok = false;
    r.err = gxError(verb + ": could not read the filtered rows — " + e.what(), "data_io");
    return r;
  }
  return r;
}

// ===========================================================================
// GEODESY
// ===========================================================================
//
// Design decision 2, verbatim: the DEFAULT is Vincenty's inverse solution on
// the WGS84 ellipsoid (a = 6378137 m, f = 1/298.257223563, convergence 1e-12
// on lambda, at most 200 iterations); `sphere` is the haversine great circle
// with R = 6371 km (replaceable by `radius(#)`); `miles` DIVIDES by 1.609344,
// which is what the package writes (see kKmPerMileDivisor).
//
// A NON-CONVERGING PAIR IS MISSING, NEVER A NUMBER. Vincenty's iteration does
// not converge for (near-)antipodal pairs; the classic failure mode of a naive
// port is to fall out of the loop and return whatever the last iterate implied,
// which is a plausible-looking wrong distance. Here the loop reports its own
// failure, the row gets `.`, and the command says how many rows that happened
// to.
//
// MEASURED, not assumed (this file's author ran the arithmetic before writing
// the handler): Seoul (37.5, 127.0) to Busan (35.1, 129.0) is
// 321.218659806 km on this implementation. Haversine with R = 6371 gives
// 321.447921772 km (0.0714% apart), and a plain equirectangular approximation
// gives 321.3 km, so 321.2 is the right neighbourhood and the plan's "≈325.0
// km" Done-When figure is wrong. See this bundle's report; the pin belongs at
// 321.2187, not 325.0.
constexpr double kPi = 3.14159265358979323846;
// 6371 EXACTLY, not the IUGG mean 6371.0088. Read out of the packages
// themselves on 2026-09-04: `geodist` and `geonear` both write
// `local radius 6371` / `scalar R = 6371`. The plan drafted the IUGG value, and
// on the Seoul-Busan pair the two radii differ by 0.000444 km -- far too small
// to see in any rendered output and exactly the kind of constant that gets
// frozen into a golden and never questioned again. The package's number is the
// contract; the more "correct" one would be a silent parity break.
constexpr double kSphereRadiusKm = 6371.0;
// BUG-463 (2026-09-09): this was `0.621371192`, a TRUNCATED reciprocal, and
// geodist writes `local km_to_miles / 1.609344` -- a DIVISION by the
// exact international-mile definition, used at :160, :169 and :294. The two
// differ at relative 3.8e-11: on the Seoul-Busan pair StataBE answers
// 199.59602161268270 and the truncated constant gave 199.59602153644656.
// Inside TOLERANCE.md's rtol 1e-9, so no gate saw it -- and invisible in any
// %9.0g rendering, which is how a wrong low digit survives. The divisor is the
// package's own contract, so divide, and keep the digits it keeps.
constexpr double kKmPerMileDivisor = 1.609344;

double degToRad(double d) { return d * kPi / 180.0; }

// nullopt == did not converge.
std::optional<double> vincentyKm(double lat1, double lon1, double lat2, double lon2) {
  const double a = 6378137.0;              // WGS84 semi-major axis, metres
  const double f = 1.0 / 298.257223563;    // WGS84 flattening
  const double b = (1.0 - f) * a;          // semi-minor axis
  const double L = degToRad(lon2 - lon1);
  const double U1 = std::atan((1.0 - f) * std::tan(degToRad(lat1)));
  const double U2 = std::atan((1.0 - f) * std::tan(degToRad(lat2)));
  const double sinU1 = std::sin(U1), cosU1 = std::cos(U1);
  const double sinU2 = std::sin(U2), cosU2 = std::cos(U2);

  double lambda = L;
  double sinSigma = 0.0, cosSigma = 0.0, sigma = 0.0, cos2Alpha = 0.0, cos2SigmaM = 0.0;
  int it = 0;
  for (; it < 200; ++it) {
    const double sinL = std::sin(lambda), cosL = std::cos(lambda);
    const double t1 = cosU2 * sinL;
    const double t2 = cosU1 * sinU2 - sinU1 * cosU2 * cosL;
    sinSigma = std::sqrt(t1 * t1 + t2 * t2);
    if (sinSigma == 0.0) return 0.0;  // coincident points
    cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosL;
    sigma = std::atan2(sinSigma, cosSigma);
    const double sinAlpha = cosU1 * cosU2 * sinL / sinSigma;
    cos2Alpha = 1.0 - sinAlpha * sinAlpha;
    // Equatorial line: cos^2(alpha) == 0 leaves cos(2 sigma_m) undefined;
    // Vincenty's own note sets it to 0 there.
    cos2SigmaM = (cos2Alpha == 0.0) ? 0.0 : cosSigma - 2.0 * sinU1 * sinU2 / cos2Alpha;
    const double C = f / 16.0 * cos2Alpha * (4.0 + f * (4.0 - 3.0 * cos2Alpha));
    const double lambdaPrev = lambda;
    lambda = L + (1.0 - C) * f * sinAlpha *
                     (sigma + C * sinSigma *
                                  (cos2SigmaM +
                                   C * cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
    if (std::fabs(lambda - lambdaPrev) < 1e-12) break;
  }
  if (it >= 200) return std::nullopt;  // (near-)antipodal: no answer, not a guess

  const double u2 = cos2Alpha * (a * a - b * b) / (b * b);
  const double A =
      1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0 + u2 * (320.0 - 175.0 * u2)));
  const double B = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - 47.0 * u2)));
  const double dSigma =
      B * sinSigma *
      (cos2SigmaM +
       B / 4.0 *
           (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM) -
            B / 6.0 * cos2SigmaM * (-3.0 + 4.0 * sinSigma * sinSigma) *
                (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));
  return b * A * (sigma - dSigma) / 1000.0;  // metres -> km
}

double haversineKm(double lat1, double lon1, double lat2, double lon2, double radiusKm) {
  const double dLat = degToRad(lat2 - lat1);
  const double dLon = degToRad(lon2 - lon1);
  const double s1 = std::sin(dLat / 2.0), s2 = std::sin(dLon / 2.0);
  double h = s1 * s1 + std::cos(degToRad(lat1)) * std::cos(degToRad(lat2)) * s2 * s2;
  if (h > 1.0) h = 1.0;  // rounding guard: asin's domain
  if (h < 0.0) h = 0.0;
  return 2.0 * radiusKm * std::asin(std::sqrt(h));
}

// How a pair of coordinates becomes ONE number, for BOTH geo verbs -- one
// function, so `geodist` and `geonear` cannot answer the same question with
// different arithmetic.
struct GeoMode {
  bool sphere = false;           // haversine instead of Vincenty
  double radiusKm = kSphereRadiusKm;
  bool miles = false;
};

struct GeoOutcome {
  double value = std::numeric_limits<double>::quiet_NaN();
  bool badLatitude = false;   // |lat| > 90
  bool noConverge = false;    // Vincenty gave up
};

GeoOutcome geoDistance(double lat1, double lon1, double lat2, double lon2, const GeoMode& mode) {
  GeoOutcome r;
  if (std::isnan(lat1) || std::isnan(lon1) || std::isnan(lat2) || std::isnan(lon2)) return r;
  // Edge case from the plan: a latitude outside [-90, 90] is not a place. It
  // becomes missing plus a note -- never a number the formula would happily
  // produce from a transposed lat/lon pair.
  if (std::fabs(lat1) > 90.0 || std::fabs(lat2) > 90.0) {
    r.badLatitude = true;
    return r;
  }
  double km;
  if (mode.sphere) {
    km = haversineKm(lat1, lon1, lat2, lon2, mode.radiusKm);
  } else {
    auto v = vincentyKm(lat1, lon1, lat2, lon2);
    if (!v.has_value()) {
      r.noConverge = true;
      return r;
    }
    km = *v;
  }
  r.value = mode.miles ? km / kKmPerMileDivisor : km;
  return r;
}

// `sphere` / `radius()` / `miles`, read once for both verbs. Returns an error
// block through `err` when radius() is not a positive number.
bool readGeoMode(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 const std::string& verb, bool allowSphere, GeoMode* mode, ojson* err) {
  if (allowSphere) mode->sphere = hasFlag(opts, "sphere");
  mode->miles = hasFlag(opts, "miles");
  if (auto rad = optStr(opts, "radius")) {
    double v = 0.0;
    if (!parseStrictDouble(*rad, &v) || !(v > 0.0)) {
      *err = gxSyntax(verb + ": radius() must be a positive number of kilometres, got `" + *rad +
                      "'.");
      return false;
    }
    mode->radiusKm = v;
    mode->sphere = true;  // a radius is a sphere's radius; the ellipsoid has none
  }
  return true;
}

}  // namespace

// ===========================================================================
// geodist
// ===========================================================================
//
// `geodist lat1 lon1 lat2 lon2, gen(newvar) [miles sphere radius(#)]`. Each of
// the four arguments is EITHER a variable name OR a numeric literal, so
// `geodist 37.5 127.0 lat lon, gen(d)` (a fixed origin against a column of
// destinations) works the same way as four columns. Missing propagates: any
// missing coordinate gives a missing distance, and missing is `.`, never 0.
ojson runGeodist(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "geodist";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGeodist.known,
                                            kVocabGeodist.unimplemented))
    return ojson(*err);
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`geodist` computes a distance per observation; weights do not enter it."));
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 4) {
    return gxSyntax(kCmd +
                    ": syntax is `geodist lat1 lon1 lat2 lon2, generate(newvar) [miles sphere "
                    "radius(#)]` — four coordinate arguments, each a variable or a number.");
  }
  const std::optional<std::string> genOpt = optStrAny(parsed.opts, {"generate", "gen"});

  GeoMode mode;
  ojson modeErr;
  if (!readGeoMode(parsed.opts, kCmd, /*allowSphere=*/true, &mode, &modeErr)) return modeErr;

  // Each argument: a literal, or a column name to fetch.
  struct Arg {
    bool constant = false;
    double value = 0.0;
    std::string var;
  };
  std::vector<Arg> args(4);
  std::vector<std::string> wanted;
  for (size_t i = 0; i < 4; ++i) {
    const std::string tok = trimOf(dequote(trimOf(parsed.varlist[i])));
    double v = 0.0;
    if (parseStrictDouble(tok, &v)) {
      args[i].constant = true;
      args[i].value = v;
    } else {
      args[i].var = tok;
      wanted.push_back(tok);
    }
  }

  // BUG-426: geodist has TWO shapes and this handler had one. With four LITERAL
  // coordinates it is the IMMEDIATE form — a calculator: it prints the distance,
  // returns r(distance), and writes no column (geodist.ado's own `if
  // "`generate'" == "" ... display` arm). generate() is then an ERROR (Stata
  // r(198)), not an alternative. With variables it writes a column and
  // generate() is required. PIET had it exactly inverted: literals+generate()
  // was accepted (writing a constant column) and the calculator form refused.
  const bool immediate = args[0].constant && args[1].constant && args[2].constant &&
                         args[3].constant;
  if (immediate) {
    if (genOpt.has_value() && !genOpt->empty()) {
      return gxSyntax(kCmd +
                      ": generate() is not allowed with four literal coordinates — that is the "
                      "immediate form, which returns the distance in r(distance).");
    }
    const GeoOutcome o = geoDistance(args[0].value, args[1].value, args[2].value, args[3].value,
                                     mode);
    if (o.badLatitude) {
      return gxError(kCmd + ": a latitude outside [-90, 90] has no distance.");
    }
    session.eval.rValues.set("distance", o.value);
    // Stata's own %12.0g rendering of the immediate form's one number.
    ojson blk;
    blk["type"] = "display";
    blk["text"] = piet::fmtStataDefaultG(o.value);
    return blk;
  }
  if (!genOpt.has_value() || genOpt->empty())
    return gxSyntax(kCmd + ": generate() is required — geodist writes its distance to a new "
                           "variable.");
  const std::string newvar = *genOpt;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  ojson guard = newVarGuard(session, kCmd, newvar);
  if (!guard.is_null()) return guard;

  ReadFrame rf(session, wanted);
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return gxError(kCmd + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  const int64_t nRows = ds->nRows();

  std::vector<std::vector<double>> cols(4);
  for (size_t i = 0; i < 4; ++i) {
    if (args[i].constant) continue;
    if (!ds->has(args[i].var)) return gxError("variable " + args[i].var + " not found");
    if (ds->dtype(args[i].var) == "factor") {
      return gxError(kCmd + ": `" + args[i].var +
                     "' is a string variable; a coordinate has to be numeric.");
    }
    cols[i] = columnAsDoubles(*ds, args[i].var);
  }

  // The mask is evaluated over the WHOLE frame (an `if` may name variables the
  // §38 projection above never fetched), and it is handed to `cobra eval`, so
  // the base has to be a path that EXISTS -- `session.datastore()` is what
  // guarantees a virtual head has been materialized. Only paid for when a
  // qualifier is actually present.
  std::string maskBase;
  if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty())) {
    Datastore* full = nullptr;
    try {
      full = session.datastore();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the data — " + e.what(), "data_io");
    }
    if (full == nullptr) return ojson(eval::noDataLoaded());
    maskBase = full->path();
  }
  MaskResult mask = qualifierMask(session, kCmd, maskBase, parsed, nRows);
  if (!mask.ok) return mask.err;

  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(nRows));
  int64_t nMissing = 0, nBadLat = 0, nNoConverge = 0;
  for (int64_t r = 0; r < nRows; ++r) {
    if (mask.present && mask.keep[static_cast<size_t>(r)] == 0) {
      // Outside the qualifier: missing, exactly like `gen ... if`. Counted as a
      // missing value generated for the same reason Stata counts it -- the
      // column really does have a missing value in that row.
      cells.emplace_back();
      ++nMissing;
      continue;
    }
    double v[4];
    for (size_t i = 0; i < 4; ++i)
      v[i] = args[i].constant ? args[i].value : cols[i][static_cast<size_t>(r)];
    const GeoOutcome o = geoDistance(v[0], v[1], v[2], v[3], mode);
    if (o.badLatitude) ++nBadLat;
    if (o.noConverge) ++nNoConverge;
    if (std::isnan(o.value)) ++nMissing;
    cells.push_back(numCell(o.value));
  }

  std::string out;
  if (auto err = appendColumn(session, *session.currentHbin(), newvar, cells, "f64", &out))
    return *err;
  session.load(out);

  ojson blocks = ojson::array();
  blocks.push_back(mutationResult(session, "gen", {newvar}));
  if (nMissing > 0) {
    blocks.push_back(noteBlock("(" + std::to_string(nMissing) + " missing value" +
                               (nMissing == 1 ? "" : "s") + " generated)"));
  }
  if (nBadLat > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nBadLat) + " observation" +
                               (nBadLat == 1 ? " has a latitude" : "s have a latitude") +
                               " outside [-90, 90] — those distances are missing)"));
  }
  if (nNoConverge > 0) {
    blocks.push_back(noteBlock(
        "(" + kCmd + ": Vincenty's iteration did not converge for " + std::to_string(nNoConverge) +
        " (near-)antipodal pair" + (nNoConverge == 1 ? "" : "s") +
        " — those distances are missing. `sphere` gives an approximate answer for them.)"));
  }
  return blocks;
}

// ===========================================================================
// geonear
// ===========================================================================
//
// `geonear id lat lon using FILE, neighbors(nid nlat nlon) [nearcount(#)
// miles ellipsoid genstub(stub)]` -- for every master observation, the k
// nearest observations in the using file, as 2k new columns.
//
// BRUTE FORCE, WITH A HARD PAIR CAP. n*m distances, single-threaded, no spatial
// index. The plan's cap of 5e7 pairs is what makes that a bounded wall-clock
// promise instead of a hope; over the cap the command REFUSES and says so,
// rather than starting work it cannot finish. The cap is checked before a
// single column is written.
//
// COLUMN NAMES -- a decision the plan left open (its own text gives two
// different spellings) and this is the one taken, reported as such:
//   stem      = genstub() when given, else the neighbours' ID VARIABLE NAME
//   nearcount(1):   <stem>            and  km_to_<stem>
//   nearcount(k>1): <stem>1..<stem>k  and  km_to_<stem>1..km_to_<stem>k
//   under `miles` the distance prefix is `mi_to_` instead of `km_to_`, so the
//   column name always states its own unit.
// Every name is guarded (legal AND free) BEFORE the first write, so a collision
// on column 3 cannot leave columns 1 and 2 behind.
ojson runGeonear(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "geonear";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGeonear.known,
                                            kVocabGeonear.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // `id lat lon using FILE` -- `using` arrives as an ordinary varlist token.
  std::vector<std::string> head;
  std::string usingTyped;  // THE SPELLING THE USER TYPED (defect class 5)
  {
    bool sawUsing = false;
    for (const auto& w : parsed.varlist) {
      if (!sawUsing && lowerOf(w) == "using") {
        sawUsing = true;
        continue;
      }
      if (sawUsing) {
        if (usingTyped.empty()) usingTyped = w;
      } else {
        head.push_back(w);
      }
    }
    if (!sawUsing || usingTyped.empty() || head.size() != 3) {
      return gxSyntax(kCmd +
                      ": syntax is `geonear id lat lon using \"FILE\", neighbors(nid nlat nlon) "
                      "[nearcount(#) miles genstub(stub)]`");
    }
  }
  const std::string mId = trimOf(dequote(trimOf(head[0])));
  const std::string mLat = trimOf(dequote(trimOf(head[1])));
  const std::string mLon = trimOf(dequote(trimOf(head[2])));

  const std::optional<std::string> nbOpt = optStr(parsed.opts, "neighbors");
  if (!nbOpt.has_value() || nbOpt->empty())
    return gxSyntax(kCmd + ": neighbors(nid nlat nlon) is required — it names the id, latitude "
                           "and longitude variables in the using file.");
  const std::vector<std::string> nb = splitWs(*nbOpt);
  if (nb.size() != 3)
    return gxSyntax(kCmd + ": neighbors() takes exactly three variable names (id lat lon), got `" +
                    *nbOpt + "'.");

  long long nearcount = 1;
  if (auto nc = optStr(parsed.opts, "nearcount")) {
    if (!parseStrictInt(*nc, &nearcount) || nearcount < 1)
      return gxSyntax(kCmd + ": nearcount() must be a positive whole number, got `" + *nc + "'.");
  }

  GeoMode mode;
  ojson modeErr;
  if (!readGeoMode(parsed.opts, kCmd, /*allowSphere=*/false, &mode, &modeErr)) return modeErr;
  // geonear's DEFAULT IS THE SPHERE, and `ellipsoid` opts IN -- the opposite of
  // `geodist`, whose default is the ellipsoid. Read out of the package on
  // 2026-09-04 rather than assumed: `geonear` takes the ellipsoid
  // branch only when an ellipsoid option is present and otherwise sets
  // `local sphere 1` with `scalar R = 6371`, while `geodist` takes
  // haversine only under `sphere`/`radius()` and is WGS84 Vincenty otherwise.
  //
  // Two sibling commands from the same author defaulting to different earth
  // models looks like a mistake and is not; shipping ONE default for both would
  // have been a silently wrong number in whichever verb got the other's rule --
  // ~0.07% here, invisible in any rendered output, and permanent once a golden
  // froze it. That is why this is set explicitly instead of inherited.
  mode.sphere = !hasFlag(parsed.opts, "ellipsoid");

  // ---- master frame ------------------------------------------------------
  ReadFrame rf(session, {mId, mLat, mLon});
  Datastore* master = rf.get();
  if (master == nullptr)
    return gxError(kCmd + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!master->has(mId)) return gxError("variable " + mId + " not found");
  for (const auto& v : {mLat, mLon}) {
    if (!master->has(v)) return gxError("variable " + v + " not found");
    if (master->dtype(v) == "factor")
      return gxError(kCmd + ": `" + v + "' is a string variable; a coordinate has to be numeric.");
  }
  const std::vector<double> mLatV = columnAsDoubles(*master, mLat);
  const std::vector<double> mLonV = columnAsDoubles(*master, mLon);
  const int64_t n = master->nRows();

  // ---- using frame -------------------------------------------------------
  const std::string usingResolved = resolveUserPath(session, trimOf(dequote(trimOf(usingTyped))));
  session.materializeVirtualSave(usingResolved);  // BUG-125: a save_virtual'd tempfile
  {
    std::error_code ec;
    if (!std::filesystem::exists(cecon::to_native_path(usingResolved), ec)) {
      return gxError(kCmd + " using: file not found: '" + usingTyped + "'");
    }
  }
  std::string nbHbin = usingResolved;
  {
    std::string ext;
    const size_t slash = usingResolved.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? usingResolved : usingResolved.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) ext = lowerOf(base.substr(dot));
    if (ext == ".dta") {
      const std::string conv = nextPath(session.workdir(), "gxnb_dta");
      if (auto e = runVerb({"import-dta", usingResolved, conv})) {
        return gxError(kCmd + " using: cannot read '" + usingTyped + "': " +
                       e->value("message", std::string("conversion failed")));
      }
      nbHbin = conv;
    } else if (ext == ".csv" || ext == ".tsv") {
      const std::string conv = nextPath(session.workdir(), "gxnb_csv");
      const std::string delim = (ext == ".tsv") ? "\t" : ",";
      if (auto e = runVerb({"convert", "--data", usingResolved, "--out", conv, "--delim", delim})) {
        return gxError(kCmd + " using: cannot read '" + usingTyped + "': " +
                       e->value("message", std::string("conversion failed")));
      }
      nbHbin = conv;
    }
  }

  std::unique_ptr<Datastore> nbDs;
  try {
    nbDs = std::make_unique<Datastore>(nbHbin);
  } catch (const std::exception& e) {
    return gxError(kCmd + " using: could not open '" + usingTyped + "' — " + e.what(), "data_io");
  }
  for (const auto& v : nb) {
    if (!nbDs->has(v))
      return gxError(kCmd + " using: variable " + v + " not found in '" + usingTyped + "'");
  }
  for (size_t i = 1; i < 3; ++i) {
    if (nbDs->dtype(nb[i]) == "factor") {
      return gxError(kCmd + " using: `" + nb[i] + "' is a string variable in '" + usingTyped +
                     "'; a coordinate has to be numeric.");
    }
  }
  const std::vector<double> nLatV = columnAsDoubles(*nbDs, nb[1]);
  const std::vector<double> nLonV = columnAsDoubles(*nbDs, nb[2]);
  const bool nbIdIsFactor = nbDs->dtype(nb[0]) == "factor";
  const std::vector<std::string> nIdText = columnAsStrings(*nbDs, nb[0]);
  const std::vector<double> nIdNum =
      nbIdIsFactor ? std::vector<double>() : columnAsDoubles(*nbDs, nb[0]);
  const int64_t m = nbDs->nRows();

  // ---- the pair cap, BEFORE any column is written ------------------------
  {
    constexpr long double kPairCap = 5e7L;
    const long double pairs = static_cast<long double>(n) * static_cast<long double>(m);
    if (pairs > kPairCap) {
      std::ostringstream oss;
      oss << kCmd << ": n×m = " << static_cast<long long>(pairs)
          << " exceeds this build's 5e7 pair cap; restrict the sample.";
      return gxError(oss.str());
    }
  }

  // ---- the generated column names, all guarded before the first write ----
  // BUG-418: the default stem is the LITERAL `nid`, not the neighbours' id
  // variable. geonear is `if "`genstub'" == "" local genstub nid`, and the
  // package's help documents `nid`/`km_to_nid` "unless genstub()". Taking the
  // variable name was a deliberate open decision here (the note below used to say
  // so); the 2026-09-06 oracle settled it, and taking the variable name is wrong
  // twice: `gen near = km_to_nid` written against Stata fails with "variable not
  // found", and the help's own same-name idiom `neighbors(id lat lon)` was refused
  // rc 110 because the stem collided with the master's key.
  const std::string stem = optStr(parsed.opts, "genstub").value_or(std::string("nid"));
  const std::string distPrefix = mode.miles ? "mi_to_" : "km_to_";
  const size_t k = static_cast<size_t>(nearcount);
  std::vector<std::string> idNames, distNames;
  for (size_t j = 1; j <= k; ++j) {
    const std::string suffix = (k == 1) ? std::string() : std::to_string(j);
    idNames.push_back(stem + suffix);
    distNames.push_back(distPrefix + stem + suffix);
  }
  for (const auto& nm : idNames) {
    ojson g = newVarGuard(session, kCmd, nm);
    if (!g.is_null()) return g;
  }
  for (const auto& nm : distNames) {
    ojson g = newVarGuard(session, kCmd, nm);
    if (!g.is_null()) return g;
  }

  // ---- brute force -------------------------------------------------------
  // Neighbour rows with a missing coordinate are EXCLUDED (plan edge case), not
  // treated as distance-zero and not silently kept.
  std::vector<size_t> nbRows;
  nbRows.reserve(static_cast<size_t>(m));
  for (int64_t j = 0; j < m; ++j) {
    if (std::isnan(nLatV[static_cast<size_t>(j)]) || std::isnan(nLonV[static_cast<size_t>(j)]))
      continue;
    nbRows.push_back(static_cast<size_t>(j));
  }
  const int64_t nbDropped = m - static_cast<int64_t>(nbRows.size());

  std::vector<std::vector<std::string>> idCells(k), distCells(k);
  for (size_t j = 0; j < k; ++j) {
    idCells[j].reserve(static_cast<size_t>(n));
    distCells[j].reserve(static_cast<size_t>(n));
  }
  int64_t nBadLat = 0, nNoConverge = 0;
  std::vector<std::pair<double, size_t>> best;  // (distance, neighbour row)
  for (int64_t i = 0; i < n; ++i) {
    best.clear();
    const double la = mLatV[static_cast<size_t>(i)];
    const double lo = mLonV[static_cast<size_t>(i)];
    if (!std::isnan(la) && !std::isnan(lo)) {
      for (size_t jj : nbRows) {
        const GeoOutcome o = geoDistance(la, lo, nLatV[jj], nLonV[jj], mode);
        if (o.badLatitude) ++nBadLat;
        if (o.noConverge) ++nNoConverge;
        if (std::isnan(o.value)) continue;
        best.emplace_back(o.value, jj);
      }
      // BUG-419: a tie goes to the LOWEST NEIGHBOUR ID, not to the neighbour
      // file's row order. geonear.hlp:138-139 says so in as many words ("If
      // there is a tie for the #th nborid position, the neighbor is chosen by
      // ascending order of nborid") and geonear.ado implements it by sorting
      // the neighbour file by id before the scan. Row order was this handler's
      // own choice, and it is observable: symmetric grids and duplicated
      // coordinates produce exact ties routinely (the oracle's base at (0,0)
      // with neighbours at (0,-1) and (0,1) is bit-identical on both arms).
      // The row index stays as the last key so the sort is still total.
      std::stable_sort(best.begin(), best.end(),
                       [&](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) {
                         if (a.first != b.first) return a.first < b.first;
                         if (nbIdIsFactor) {
                           if (nIdText[a.second] != nIdText[b.second])
                             return nIdText[a.second] < nIdText[b.second];
                         } else if (nIdNum[a.second] != nIdNum[b.second]) {
                           return nIdNum[a.second] < nIdNum[b.second];
                         }
                         return a.second < b.second;
                       });
    }
    for (size_t j = 0; j < k; ++j) {
      if (j < best.size()) {
        const size_t src = best[j].second;
        idCells[j].push_back(nbIdIsFactor ? nIdText[src] : numCell(nIdNum[src]));
        distCells[j].push_back(numCell(best[j].first));
      } else {
        idCells[j].emplace_back();   // fewer than k usable neighbours: missing
        distCells[j].emplace_back();
      }
    }
  }

  // ---- write -------------------------------------------------------------
  std::string cur = *session.currentHbin();
  std::string out;
  std::vector<std::string> written;
  for (size_t j = 0; j < k; ++j) {
    if (auto err = appendColumn(session, cur, idNames[j], idCells[j],
                                nbIdIsFactor ? "factor" : "f64", &out))
      return *err;
    cur = out;
    written.push_back(idNames[j]);
    if (auto err = appendColumn(session, cur, distNames[j], distCells[j], "f64", &out)) return *err;
    cur = out;
    written.push_back(distNames[j]);
  }
  session.load(cur);

  // BUG-420: Stata returns the master SORTED BY BASEID. geonear
  // restores the master, sorts it by baseid and merges the results onto it, so
  // observation 1 afterwards is the lowest base id — not whatever row order the
  // caller had. This handler appended in place, which is a silent divergence
  // for any `_n`-based code after the command. runSort is the native verb's own
  // entry point, so the sort is one implementation rather than a second one.
  {
    ojson sorted = handlers::runSort({mId}, session);
    if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
  }

  ojson blocks = ojson::array();
  blocks.push_back(mutationResult(session, "gen", written));
  if (nbDropped > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nbDropped) + " observation" +
                               (nbDropped == 1 ? "" : "s") + " in '" + usingTyped +
                               "' had a missing coordinate and " +
                               (nbDropped == 1 ? "was" : "were") + " excluded)"));
  }
  if (nBadLat > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nBadLat) +
                               " pair(s) had a latitude outside [-90, 90] and were skipped)"));
  }
  if (nNoConverge > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": Vincenty's iteration did not converge for " +
                               std::to_string(nNoConverge) +
                               " (near-)antipodal pair(s), which were skipped)"));
  }
  return blocks;
}

}  // namespace piet::handlers
