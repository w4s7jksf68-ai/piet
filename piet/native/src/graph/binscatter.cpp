// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/estimation.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

#include "binscatter_internal.h"
#include "graph_point_layers.h"
#include "estimation/engine_report_support.h"

namespace piet::eval {
// eval/macros.cpp: the text a `local v = <real>` stores (%18.0g). Declared
// here as loops.cpp declares it (macros.h does not export it).
std::string stataMacroRealText(double d);
}  // namespace piet::eval

namespace {
// binscatter (7.02). `known` = the options runBinscatter serves;
// `unimplemented` = the rest of the ado's syntax line, refused by name. The
// aliases are the syntax line's minimum abbreviations (the capitalised stems:
// Nquantiles, MEDians, CONTROLs, noAddmean, LINEtype, COLors, MColors,
// LColors, Msymbols, GENxq) plus the longer prefixes the graph keys unioned in
// by dispatcher.cpp's kVocabBinscatterGraph would otherwise make ambiguous.
// runBinscatter itself reads the options from the typed line by the same
// stems, so these only keep the reject net from refusing a spelling Stata
// takes.
const piet::eval::OptVocab& kVocabBinscatter = piet::eval::registerOptVocab(
    {"binscatter"},
    {{"by", "nquantiles", "genxq", "discrete", "medians", "controls", "absorb", "noaddmean",
      "linetype", "reportreg", "colors", "mcolors", "lcolors", "msymbols", "savedata", "replace"},
     {"xq", "rd", "savegraph", "nofastxtile", "randvar", "randcut", "randn", "nbins",
      "create_xq", "x_q", "symbols", "method", "unique"},
     {{"n", "nquantiles"}, {"nq", "nquantiles"}, {"med", "medians"}, {"median", "medians"},
      {"control", "controls"}, {"noa", "noaddmean"}, {"line", "linetype"}, {"col", "colors"},
      {"color", "colors"}, {"mc", "mcolors"}, {"mcolor", "mcolors"}, {"lc", "lcolors"},
      {"lcolor", "lcolors"}, {"m", "msymbols"}, {"ms", "msymbols"}, {"msymbol", "msymbols"},
      {"gen", "genxq"}}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;
using namespace engine_report_detail;

// ===========================================================================
// binscatter yvars xvar [if] [in] [aw|fw], by() nquantiles() discrete medians
//            controls() absorb() noaddmean linetype() reportreg colors()
//            mcolors() lcolors() msymbols() savedata() replace [twoway options]
//
// Michael Stepner's binscatter 7.02 as StataBE 19.5 runs it. The installed ado
// (~/Library/Application Support/Stata/ado/plus/b/binscatter.ado, "ado:N"
// below) is the text; the oracle is tests/eval/golden/aer_w2/w2k_binscatter.log
// (the BS_* arms). The engine (cobra binscatter, apps/cmds/cmds_binscatter.cpp)
// computes every number: the sample, the residuals, the bins, the bin points
// and the fit lines. This handler owns what Stata builds as TEXT from them:
//   * the option checks the ado makes before it computes anything (ado),
//     three of which print an error and `exit` with rc 0, posting nothing
//     (BS_lt_bad, BS_reportreg_none, BS_disc_nq);
//   * reportreg's regress tables (ado), named the way Stata names the
//     residualized tempvars;
//   * the graph command e(graphcmd) (ado): scatteri points and function
//     lines whose numbers are macro text (`local v = M[i,j]`, %18.0g), colors,
//     legend, and the options binscatter does not know appended as typed;
//   * savedata's f.csv and f.do (ado) and r(602) without replace, which
//     is checked before anything is computed (ado);
//   * e(): `ereturn post` replaces the previous estimation results wholesale,
//     then e(N), e(graphcmd), e(y#_coefs) and e(byvalues) (ado).

namespace {

// The regress family's name abbreviation: a name past `w` CHARACTERS keeps its
// first w-2, `~`, and its last character (never splits a UTF-8 sequence).
std::string abbrevName(const std::string& n, size_t w) {
  if (cecon::ident::codePointCount(n) <= w) return n;
  size_t i = n.size();
  do {
    --i;
  } while (i > 0 && (static_cast<unsigned char>(n[i]) & 0xC0) == 0x80);
  return cecon::ident::truncateCodePoints(n, w - 2) + "~" + n.substr(i);
}

// The options this build computes. Every other binscatter option is refused by
// name (rc 198) before anything runs: each changes which bins or which fit
// Stata draws (xq, rd, the fastxtile sampling options) or writes a file
// (savegraph), so accepting one and ignoring it would be a wrong answer.
const std::unordered_set<std::string> kBsServed = {
    "by",        "nquantiles", "genxq",  "discrete", "medians", "controls", "absorb",
    "noaddmean", "linetype",   "reportreg", "colors", "mcolors", "lcolors", "msymbols",
    "savedata",  "replace"};

// `string` options drop one pair of outer double quotes (or a compound pair).
std::string bsDequote(std::string v) {
  v = strutil::trim(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  if (v.size() >= 4 && v.rfind("`\"", 0) == 0 && v.compare(v.size() - 2, 2, "\"'") == 0)
    return v.substr(2, v.size() - 4);
  return v;
}

std::vector<std::string> bsWords(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) w.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

// `: word k of LIST`, empty past the end (as Stata's).
std::string bsWord(const std::vector<std::string>& w, size_t k) {
  return k >= 1 && k <= w.size() ? w[k - 1] : std::string();
}

// The number text a `local v = <exp>` stores (%18.0g, the macro layer's rule).
std::string bsNum(double v) {
  if (std::isnan(v)) return ".";
  return eval::stataMacroRealText(v);
}

// `file write (exp)`'s number text: %10.0g with the blanks removed, `.` missing.
std::string bsFileNum(double v) {
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 10, 0);
}

ojson bsText(const std::string& t) { return ojson{{"type", "display"}, {"text", t}}; }

// Stata's regress name column: 12 wide, right-aligned, a long name abbreviated
// with `~` (the regress family's rule, estimation/instrumental_variables.cpp coefStubM4b).
std::string bsStub(const std::string& name) {
  return padLeft(abbrevName(name, 12), 12);
}

// One reportreg regression, as `regress` prints it (log 991-1014 for the
// layout; the numbers are the engine's).
void bsRegressTable(const ojson& reg, const std::string& depName, const std::vector<std::string>& xNames,
                    ojson& out) {
  const double N = num(reg, "N");
  const double dfm = num(reg, "df_m"), dfr = num(reg, "df_r");
  const double mss = num(reg, "mss"), rss = num(reg, "rss"), tss = num(reg, "tss");
  auto ss = [](double v) { return piet::fmt::general(v, 11, 0); };
  auto key = [](const std::string& k, const std::string& v) { return "   " + padRight(k, 16) + "=" + padLeft(v, 10); };
  auto optF = [&](const char* k, int dec) -> std::string {
    if (!reg.contains(k) || !reg.at(k).is_number()) return ".";
    return fixedN(reg.at(k).get<double>(), dec);
  };
  char fbuf[64];
  std::snprintf(fbuf, sizeof fbuf, "F(%lld, %lld)", static_cast<long long>(dfm), static_cast<long long>(dfr));
  const std::string rmse = reg.contains("rmse") && reg.at("rmse").is_number()
                               ? piet::fmt::general(reg.at("rmse").get<double>(), 10, 5)
                               : std::string(".");
  out.push_back(bsText(""));
  out.push_back(bsText("      Source |       SS           df       MS      Number of obs   =" +
                       padLeft(commaGroup(static_cast<int64_t>(N)), 10)));
  out.push_back(bsText("-------------+----------------------------------" + key(fbuf, optF("F", 2))));
  out.push_back(bsText("       Model |" + padLeft(ss(mss), 12) + padLeft(commaGroup(static_cast<int64_t>(dfm)), 10) +
                       padLeft(ss(dfm > 0 ? mss / dfm : std::nan("")), 12) + key("Prob > F", optF("p_F", 4))));
  out.push_back(bsText("    Residual |" + padLeft(ss(rss), 12) + padLeft(commaGroup(static_cast<int64_t>(dfr)), 10) +
                       padLeft(ss(dfr > 0 ? rss / dfr : std::nan("")), 12) + key("R-squared", optF("r2", 4))));
  out.push_back(bsText("-------------+----------------------------------" + key("Adj R-squared", optF("r2_a", 4))));
  out.push_back(bsText("       Total |" + padLeft(ss(tss), 12) + padLeft(commaGroup(static_cast<int64_t>(N - 1)), 10) +
                       padLeft(ss(N > 1 ? tss / (N - 1) : std::nan("")), 12) + key("Root MSE", rmse)));
  out.push_back(bsText(""));
  out.push_back(bsText(std::string(78, '-')));
  out.push_back(bsText(bsStub(depName) + " |" + coefHeader("Std. err.", "t", "95")));
  out.push_back(bsText(std::string(13, '-') + "+" + std::string(64, '-')));
  const ojson coefs = reg.value("coefs", ojson::array());
  for (size_t j = 0; j < coefs.size(); ++j) {
    const ojson& c = coefs[j];
    const std::string nm = j < xNames.size() ? xNames[j] : std::string("_cons");
    if (c.value("omitted", false)) {
      out.push_back(bsText(bsStub("o." + nm) + " |" + padLeft("0", 11) + "  (omitted)"));
      continue;
    }
    auto cell = [&](const char* k) {
      return c.contains(k) && c.at(k).is_number() ? g9cell(c.at(k).get<double>()) : std::string(".");
    };
    auto fx = [&](const char* k, int d) {
      return c.contains(k) && c.at(k).is_number() ? fixedN(c.at(k).get<double>(), d) : std::string(".");
    };
    out.push_back(bsText(bsStub(nm) + " |" + padLeft(cell("b"), 11) + padLeft(cell("se"), 11) +
                         padLeft(fx("t", 2), 9) + padLeft(fx("p", 3), 8) + padLeft(cell("lo"), 13) +
                         padLeft(cell("hi"), 12)));
  }
  out.push_back(bsText(std::string(78, '-')));
}

// Writes `text` to the resolved path; false when the file cannot be opened.
bool bsWriteFile(const std::string& path, const std::string& text) {
  std::ofstream f(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

}  // namespace

ojson runBinscatter(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  // BUG-233: the one copy of this vocabulary lives in the file-scope
  // registration block (see top of file); the dispatcher has already
  // canonicalized the keys against it.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabBinscatter.known, kVocabBinscatter.unimplemented)) {
    return ojson::array({ojson(*err)});
  }
  // varlist(min=2 numeric), ado: several y, then x (ado).
  if (parsed.varlist.size() < 2) return ojson::array({bsRcError("too few variables specified", 102, "syntax_error")});
  // [aweight fweight] (ado): any other weight is Stata's r(101).
  std::string wtype;
  if (parsed.weightType.has_value()) {
    std::string wt = strutil::lower(*parsed.weightType);
    if (wt == "aw" || wt == "aweight") wtype = "aweight";
    else if (wt == "fw" || wt == "fweight") wtype = "fweight";
    else {
      const std::string full = (wt == "pw" || wt == "pweight") ? "pweight"
                               : (wt == "iw" || wt == "iweight") ? "iweight"
                                                                 : wt;
      return ojson::array({bsRcError(full + "s not allowed", 101)});
    }
  }

  // ---- the options, read as Stata's `syntax` reads them (ado)
  std::map<std::string, RawOpt> own;
  std::vector<std::string> passThrough;  // `options' (the catch-all), as typed
  for (auto& o : bsRawOptions(parsed.raw)) {
    const std::string full = bsMatch(o.name);
    if (full.empty()) {
      passThrough.push_back(o.text);
      continue;
    }
    if (kBsServed.count(full) == 0) {
      return ojson::array({ojson(eval::unsupportedOption(
          full, "binscatter's " + full + "() changes the bins, the fit or the files binscatter writes, and this "
                "build does not implement it; it is refused rather than ignored."))});
    }
    own[full] = o;
  }
  auto has = [&](const char* k) { return own.count(k) != 0; };
  auto arg = [&](const char* k) -> std::string {
    auto it = own.find(k);
    return it == own.end() || !it->second.arg.has_value() ? std::string() : *it->second.arg;
  };

  // Nquantiles(integer 20)
  int nquantiles = 20;
  if (has("nquantiles")) {
    const std::string t = strutil::trim(arg("nquantiles"));
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (t.empty() || end == t.c_str() || *end != '\0')
      return ojson::array({bsRcError("option nquantiles() incorrectly specified", 198, "syntax_error")});
    nquantiles = static_cast<int>(v);
  }
  // by(), controls() and absorb() resolve as `syntax` resolves them, before
  // the ado's own checks (FU-1088).
  std::string byName = strutil::trim(arg("by"));
  std::string controls = strutil::trim(arg("controls"));
  std::string absorb = strutil::trim(arg("absorb"));
  if (auto err = bsResolveNames(session, byName, true)) return ojson::array({*err});
  if (auto err = bsResolveNames(session, controls, false)) return ojson::array({*err});
  if (auto err = bsResolveNames(session, absorb, true)) return ojson::array({*err});
  std::string linetype = strutil::trim(arg("linetype"));
  const bool discrete = has("discrete");
  const bool reportreg = has("reportreg");
  const bool replace = has("replace");
  const std::string savedata = has("savedata") ? bsDequote(arg("savedata")) : std::string();
  ojson blocks = ojson::array();

  // ---- the checks before anything is computed (ado)
  if (linetype == "noline") {
    // ado (legacy spelling)
    blocks.push_back(bsText("NOTE: legacy line type 'noline' has been renamed 'none', and is supported only for "
                            "backward compatibility."));
    linetype = "none";
  }
  // Three refusals the ado makes with `di as error` and a bare `exit`: rc 0,
  // nothing computed, e() left as it was (log 973-980 BS_lt_bad, 1084-1091
  // BS_reportreg_none, 1101-1108 BS_disc_nq).
  if (linetype.empty()) linetype = "lfit";
  else if (linetype != "connect" && linetype != "lfit" && linetype != "qfit" && linetype != "none") {
    blocks.push_back(bsText("linetype() must either be connect, lfit, qfit, or none"));
    return blocks;
  }
  const std::string genxq = strutil::trim(arg("genxq"));
  if (!genxq.empty() && discrete) {
    // ado (xq() is refused above, so discrete is the only partner)
    blocks.push_back(bsText("Cannot specify more than one of genxq(), xq(), and discrete simultaneously."));
    return blocks;
  }
  if (!genxq.empty()) {
    // `confirm new variable` (ado)
    if (!strutil::isValidStataIdentifier(genxq)) return ojson::array({bsRcError(genxq + " invalid name", 198)});
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(genxq))
      return ojson::array({bsRcError("variable " + genxq + " already defined", 110)});
  }
  if (nquantiles != 20 && discrete) {
    blocks.push_back(bsText("Cannot specify nquantiles in combination with discrete or an xq variable."));
    return blocks;
  }
  if (reportreg && linetype != "lfit" && linetype != "qfit") {
    blocks.push_back(bsText("Cannot specify 'reportreg' when no fit line is being created."));
    return blocks;
  }
  // savedata() without replace: confirm new file f.csv, then f.do (ado;
  // log 1195-1207 BS_sd_noreplace r(602), the old file left as it was).
  const std::string csvName = savedata + ".csv";
  const std::string doName = savedata + ".do";
  const std::string csvPath = savedata.empty() ? std::string() : resolveUserPath(session, csvName);
  const std::string doPath = savedata.empty() ? std::string() : resolveUserPath(session, doName);
  if (!savedata.empty() && !replace) {
    if (cecon::native_exists(csvPath)) return ojson::array({bsRcError("file " + csvName + " already exists", 602)});
    if (cecon::native_exists(doPath)) return ojson::array({bsRcError("file " + doName + " already exists", 602)});
  }

  // ---- the engine
  const std::vector<std::string> yNames(parsed.varlist.begin(), parsed.varlist.end() - 1);
  const std::string xName = parsed.varlist.back();
  if (!byName.empty()) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(byName)) {
      const std::string t = ds->dtype(byName);
      if (t == "factor" || t.rfind("str", 0) == 0) {
        return ojson::array({ojson(eval::unsupportedOption(
            "by", "a string by() variable is grouped through egen group() with its own labels in binscatter; this "
                  "build serves numeric by() variables only."))});
      }
    }
  }
  std::string yJoined;
  for (const auto& y : yNames) yJoined += (yJoined.empty() ? "" : " ") + y;
  std::vector<std::string> argv = {"binscatter", "--y", yJoined, "--x", xName, "--linetype", linetype};
  if (has("nquantiles")) {
    argv.push_back("--nquantiles");
    argv.push_back(std::to_string(nquantiles));
  }
  if (!byName.empty()) { argv.push_back("--by"); argv.push_back(byName); }
  if (!controls.empty()) { argv.push_back("--controls"); argv.push_back(controls); }
  if (!absorb.empty()) { argv.push_back("--absorb"); argv.push_back(absorb); }
  if (discrete) argv.push_back("--discrete");
  if (has("medians")) argv.push_back("--medians");
  if (has("noaddmean")) argv.push_back("--noaddmean");
  if (reportreg) argv.push_back("--reportreg");
  if (!genxq.empty()) argv.push_back("--genxq");
  // `set type double`: the ado's residual and x^2 variables take c(type)
  // (predict / gen, binscatter.ado), so they stay double.
  if (session.defaultStorageType == "double") argv.push_back("--double");
  if (!wtype.empty()) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wtype);
  }
  pushKeepif(argv, parsed);
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = session.datastore();
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return ojson::array({inRangeRejectError(bad, *parsed.inRange, "binscatter")});
    }
    argv.push_back("--inrange");
    argv.push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
  }
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");

  const bool fit = linetype == "lfit" || linetype == "qfit";
  const bool qfit = linetype == "qfit";
  const bool residualized = !controls.empty() || !absorb.empty();
  // Tempvar names (ado): sortpreserve __000000, marksample __000001,
  // by()'s byvalmatrix the next, then one residual per variable (x first),
  // then qfit's x^2 (log 1038-1062 BS_reportreg_ctl, BS_reportreg_by,
  // BS_reportreg_qfit). DESIGN DECISION (no oracle arm): absorb()'s areg is
  // assumed to take no tempname of the caller's sequence, like regress.
  int tmp = 2 + (byName.empty() ? 0 : 1);
  auto tempname = [&]() {
    char buf[16];
    std::snprintf(buf, sizeof buf, "__%06d", tmp++);
    return std::string(buf);
  };
  std::string xReg = xName;
  std::vector<std::string> yReg = yNames;
  if (residualized) {
    xReg = tempname();
    for (auto& y : yReg) y = tempname();
  }
  const std::string x2Reg = qfit ? tempname() : std::string();
  BsSpec spec;
  spec.ys = yNames;
  spec.x = xName;
  spec.by = byName;
  spec.controls = controls;
  spec.absorb = absorb;
  spec.wtype = wtype;
  spec.wvar = wtype.empty() ? std::string() : strutil::trim(*parsed.weightVar);
  spec.fit = fit;
  spec.qfit = qfit;
  spec.residualized = residualized;
  spec.noaddmean = has("noaddmean");
  spec.xReg = xReg;
  spec.x2Reg = x2Reg;
  spec.yReg = yReg;

  ojson r;
  try {
    r = runCobraJson(argv);
  } catch (const CobraError& ce) {
    // The engine names Stata's rc in the error's detail ("r(134)") where the
    // ado has one (discrete over N/2 values, fastxtile's nquantiles bounds,
    // negative or non-integer weights, no observations).
    const std::string& d = ce.detail;
    if (d.size() > 3 && d.compare(0, 2, "r(") == 0 && d.back() == ')') {
      const std::string digits = d.substr(2, d.size() - 3);
      if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
        blocks.push_back(bsRcError(ce.what(), std::stoi(digits)));
        // r(134)/r(198) come from the bins, after the ado's regressions
        if (digits == "134" || digits == "198") bsReplayRegressions(session, parsed, spec);
        return blocks;
      }
    }
    blocks.push_back(qeError(cobraErrorMessage(ce), ce.code));
    return blocks;
  }

  const size_t ynum = yNames.size();
  const ojson byvalsJ = r.value("byvals", ojson::array());
  std::vector<double> byvals;
  for (const auto& v : byvalsJ) byvals.push_back(v.get<double>());
  const size_t bynum = byName.empty() ? 1 : byvals.size();
  const int nqFinal = static_cast<int>(num(r, "nquantiles", 0));
  const ojson points = r.value("points", ojson::array());
  const ojson coefs = r.value("coefs", ojson::array());

  // by-value text: the value label when the by variable has one (ado)
  std::vector<std::string> byText;
  {
    const auto vl = session.valueLabels.find(byName);
    for (double v : byvals) {
      std::string t = bsNum(v);
      if (vl != session.valueLabels.end() && v == std::floor(v)) {
        const auto it = vl->second.find(static_cast<int>(v));
        if (it != vl->second.end()) t = it->second;
      }
      byText.push_back(t);
    }
  }

  // ---- reportreg tables and the bin warnings, in the ado's order
  size_t ycur = 0;
  for (const auto& m : r.value("messages", ojson::array())) {
    const std::string kind = m.value("kind", std::string());
    if (kind == "group") {
      blocks.push_back(bsText(std::string(255, '-')));
      if (m.contains("byval")) {
        const double bv = m.at("byval").get<double>();
        std::string t = bsNum(bv);
        for (size_t b = 0; b < byvals.size(); ++b)
          if (byvals[b] == bv) t = byText[b];
        blocks.push_back(bsText("-> " + byName + " = " + t));
      }
      ycur = 0;
    } else if (kind == "yvar") {
      blocks.push_back(bsText("y_var = " + m.value("name", std::string())));
      for (size_t i = 0; i < ynum; ++i)
        if (yNames[i] == m.value("name", std::string())) ycur = i;
    } else if (kind == "regress") {
      std::vector<std::string> xs;
      if (qfit) xs.push_back(x2Reg);
      xs.push_back(xReg);
      bsRegressTable(m.at("reg"), yReg[ycur], xs, blocks);
    } else if (kind == "no_obs") {
      blocks.push_back(bsText(reportreg ? "no observations"
                                        : "no observations for one of the fit lines. add 'reportreg' for more info."));
    } else if (kind == "nq_warning") {
      blocks.push_back(bsText("warning: nquantiles(" + std::to_string(m.value("requested", 0)) +
                              ") was specified, but only " + std::to_string(m.value("generated", 0)) +
                              " were generated. see help file under nquantiles() for explanation."));
    } else if (kind == "auto_discrete") {
      // ado: only genxq() is told it was ignored
      if (!genxq.empty())
        blocks.push_back(bsText("note: the x-variable has fewer unique values than the number of bins specified (" +
                                std::to_string(nquantiles) +
                                ").  It will therefore be treated as discrete, and genxq() will be ignored"));
    } else if (kind == "discrete_controls_warning") {
      blocks.push_back(bsText("warning: discrete is specified in combination with controls() or absorb(). note that "
                              "binning takes places after residualization, so the residualized x-variable may "
                              "contain many more unique values."));
    }
  }

  // An engine error after printed output: cmd_binscatter replies {error, rc,
  // messages} and the messages above stand before the error, as the ado
  // prints them (`discrete controls(z)`: the warning of ado, then r(134)
  // at ado). Nothing is posted; e() is the ado's last regression.
  if (r.contains("error")) {
    const ojson& e = r.at("error");
    const int rc = r.value("rc", 0);
    if (rc > 0) blocks.push_back(bsRcError(e.value("message", std::string()), rc));
    else blocks.push_back(qeError("[" + e.value("code", std::string("engine_error")) + "] " +
                                      e.value("message", std::string()),
                                  e.value("code", std::string("engine_error"))));
    if (rc == 134 || rc == 198) bsReplayRegressions(session, parsed, spec);
    return blocks;
  }

  // ---- the graph command (ado)
  std::string colors = strutil::trim(arg("colors"));
  if (colors.empty())
    colors = "navy maroon forest_green dkorange teal cranberry lavender khaki sienna emidblue emerald brown erose "
             "gold bluishgray";
  const std::vector<std::string> colorW = bsWords(colors);
  std::vector<std::string> mcolorW = bsWords(arg("mcolors"));
  std::vector<std::string> lcolorW = bsWords(arg("lcolors"));
  const bool single = ynum == 1 && bynum == 1 && linetype != "connect";
  if (mcolorW.empty()) mcolorW = single ? std::vector<std::string>{bsWord(colorW, 1)} : colorW;
  if (lcolorW.empty()) lcolorW = single ? std::vector<std::string>{bsWord(colorW, 2)} : colorW;
  const std::vector<std::string> msymW = bsWords(arg("msymbols"));
  const std::string connect = linetype == "connect" ? "c(l)" : std::string();

  // point (row, by) of y#: the engine's compacted bins; past the end = missing
  auto pt = [&](size_t yi, size_t b, size_t row, bool wantX) -> double {
    if (yi >= points.size() || b >= points[yi].size()) return std::nan("");
    const ojson& arr = points[yi][b][wantX ? "x" : "y"];
    if (row >= arr.size() || !arr[row].is_number()) return std::nan("");
    return arr[row].get<double>();
  };
  // `local a `a' word` keeps one blank between the old text and the new word
  auto append = [](std::string& macro, const std::string& more) {
    std::string t = macro + " " + more;
    const size_t first = t.find_first_not_of(' ');
    macro = first == std::string::npos ? std::string() : t.substr(first);
  };
  auto trimmed = [](const std::string& t) {
    const size_t a = t.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    const size_t b = t.find_last_not_of(' ');
    return t.substr(a, b - a + 1);
  };
  std::string scatters, sdScatters, legendLabels, order;
  int c = 0, series = 0;
  for (size_t b = 0; b < bynum; ++b) {
    for (size_t yi = 0; yi < ynum; ++yi) {
      ++c;
      double xv = pt(yi, b, 0, true), yv = pt(yi, b, 0, false);
      if (std::isnan(xv) || std::isnan(yv)) continue;
      ++series;
      append(scatters, "(scatteri");
      if (!savedata.empty()) {
        if (byName.empty()) append(sdScatters, "(scatter " + yNames[yi] + " " + xName);
        else
          append(sdScatters, "(scatter " + yNames[yi] + "_by" + std::to_string(b + 1) + " " + xName + "_by" +
                                 std::to_string(b + 1));
      }
      for (size_t row = 0; !std::isnan(xv) && !std::isnan(yv);) {
        append(scatters, bsNum(yv) + " " + bsNum(xv));
        ++row;
        xv = pt(yi, b, row, true);
        yv = pt(yi, b, row, false);
      }
      const std::string symbol = msymW.empty() ? std::string() : "msymbol(" + bsWord(msymW, c) + ")";
      const std::string sopts = trimmed(connect + " mcolor(" + bsWord(mcolorW, c) + ") lcolor(" +
                                        bsWord(lcolorW, c) + ") " + symbol);
      scatters += ", " + sopts + ")";
      if (!savedata.empty()) sdScatters += ", " + sopts + ")";
      if (byName.empty()) {
        if (ynum == 1) legendLabels = "off";
        else append(legendLabels, "lab(" + std::to_string(series) + " " + yNames[yi] + ")");
      } else if (ynum == 1) {
        append(legendLabels, "lab(" + std::to_string(series) + " " + byName + "=" + byText[b] + ")");
      } else {
        append(legendLabels, "lab(" + std::to_string(series) + " " + yNames[yi] + ": " + byName + "=" + byText[b] + ")");
      }
      if (!byName.empty() || ynum > 1) append(order, std::to_string(series));
    }
  }
  std::string fits;
  if (fit) {
    c = 0;
    for (size_t b = 0; b < bynum; ++b) {
      for (size_t yi = 0; yi < ynum; ++yi) {
        ++c;
        // bounds: the group's first x point and its last non-missing one,
        // searched down from row nquantiles (ado)
        const double left = pt(yi, b, 0, true);
        double right = std::nan("");
        for (int row = nqFinal - 1; row >= 0 && std::isnan(right); --row) right = pt(yi, b, static_cast<size_t>(row), true);
        if (yi >= coefs.size() || b >= coefs[yi].size()) continue;
        const ojson& row = coefs[yi][b];
        std::vector<double> cf;
        for (const auto& v : row) cf.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        const std::string quad = qfit ? bsNum(cf.at(0)) : std::string("0");
        const double lin = qfit ? cf.at(1) : cf.at(0);
        const double cons = qfit ? cf.at(2) : cf.at(1);
        if ((qfit && std::isnan(cf.at(0))) || std::isnan(lin) || std::isnan(cons)) continue;
        append(fits, "(function " + quad + "*x^2+" + bsNum(lin) + "*x+" + bsNum(cons) + ", range(" + bsNum(left) +
                         " " + bsNum(right) + ") lcolor(" + bsWord(lcolorW, c) + "))");
      }
    }
  }
  std::string ytitle = yJoined;
  if (ynum == 2) ytitle = yNames[0] + " and " + yNames[1];
  else if (ynum > 2) {
    ytitle.clear();
    for (size_t i = 0; i < ynum; ++i) ytitle += (i ? "; " : "") + yNames[i];
  }
  std::string optionsText;
  for (const auto& t : passThrough) optionsText += (optionsText.empty() ? "" : " ") + t;
  const std::string tail = ", graphregion(fcolor(white))  xtitle(" + xName + ") ytitle(" + ytitle + ") legend(" +
                           legendLabels + " order(" + order + ")) " + optionsText;
  const std::string graphcmd = trimmed("twoway " + scatters + " " + fits + tail);
  const std::string sdGraphcmd = trimmed("twoway " + sdScatters + " " + fits + tail);

  // ---- savedata (ado): f.csv, then f.do, each announced
  if (!savedata.empty()) {
    std::string csv;
    for (int row = 0; row <= nqFinal; ++row) {
      std::vector<std::string> cells;
      for (size_t b = 0; b < bynum; ++b) {
        const std::string bl = byName.empty() ? std::string() : "_by" + std::to_string(b + 1);
        if (row == 0) cells.push_back(xName + bl);
        else cells.push_back(bsFileNum(pt(0, b, static_cast<size_t>(row - 1), true)));
      }
      for (size_t yi = 0; yi < ynum; ++yi)
        for (size_t b = 0; b < bynum; ++b) {
          const std::string bl = byName.empty() ? std::string() : "_by" + std::to_string(b + 1);
          if (row == 0) cells.push_back(yNames[yi] + bl);
          else cells.push_back(bsFileNum(pt(yi, b, static_cast<size_t>(row - 1), false)));
        }
      for (size_t i = 0; i < cells.size(); ++i) csv += (i ? "," : "") + cells[i];
      csv += "\n";
    }
    if (replace && !cecon::native_exists(csvPath)) blocks.push_back(bsText("(file " + csvName + " not found)"));
    if (!bsWriteFile(csvPath, csv))
      return ojson::array({bsRcError("file " + csvName + " could not be opened", 603)});
    blocks.push_back(bsText("(file " + csvName + " written containing saved data)"));

    std::string dofile = "insheet using " + csvName + "\n\n";
    if (!byName.empty()) {
      std::vector<std::string> vars = {xName};
      vars.insert(vars.end(), yNames.begin(), yNames.end());
      for (const auto& v : vars)
        for (size_t b = 0; b < bynum; ++b)
          dofile += "label variable " + v + "_by" + std::to_string(b + 1) + " \"" + v + "; " + byName + "==" +
                    byText[b] + "\"\n";
      dofile += "\n";
    }
    dofile += sdGraphcmd + "\n";
    if (replace && !cecon::native_exists(doPath)) blocks.push_back(bsText("(file " + doName + " not found)"));
    if (!bsWriteFile(doPath, dofile))
      return ojson::array({bsRcError("file " + doName + " could not be opened", 603)});
    blocks.push_back(bsText("(file " + doName + " written containing commands to process saved data)"));
  }

  // ---- e() (ado): `ereturn post, esample(`touse')` replaces the whole
  // of e(); a touse that cannot be rebuilt posts e() without e(sample).
  if (!bsPostEsample(session, parsed, spec, num(r, "N"))) {
    session.lastModel_reset();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.ePostedLive = true;
    session.ePostedEsampleSerial = 0;
  }
  session.eval.setEScalar("N", num(r, "N"));
  session.eval.setEString("graphcmd", graphcmd);
  if (fit) {
    for (size_t k = ynum; k >= 1; --k) {
      eval::EvalMatrix m;
      for (size_t b = 0; b < coefs[k - 1].size(); ++b) {
        std::vector<double> rowv;
        for (const auto& v : coefs[k - 1][b]) rowv.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        m.data.push_back(rowv);
        m.rownames.push_back(byName.empty() ? std::string("=") : "by" + std::to_string(b + 1) + ":=");
      }
      if (qfit) m.colnames = {xName + "^2", xName, "_cons"};
      else m.colnames = {xName, "_cons"};
      session.eval.eMatrices.emplace_back("y" + std::to_string(k) + "_coefs", std::move(m));
    }
  }
  if (!byName.empty()) {
    eval::EvalMatrix m;
    for (size_t b = 0; b < byvals.size(); ++b) {
      m.data.push_back({byvals[b]});
      m.rownames.push_back("by" + std::to_string(b + 1));
    }
    m.colnames = {byName};
    session.eval.eMatrices.emplace_back("byvalues", std::move(m));
  }

  // ---- genxq(): fastxtile's variable (ado): byte up to
  // 100 bins, labelled "<n> quantiles of <x_r>", missing outside the sample.
  std::optional<ojson> genxqMutation;
  if (!genxq.empty() && r.contains("xq") && r.at("xq").is_array()) {
    std::vector<double> vals;
    for (const auto& v : r.at("xq")) vals.push_back(v.is_number() ? v.get<double>() : std::nan(""));
    const std::string hbin = mintScratchPath(session.workdir(), "binscatter");
    forgetScratchMint(hbin);
    const std::string csvPayload = hbin.substr(0, hbin.size() - 5) + ".csv";
    registerScratchMint(csvPayload);
    std::string payload;
    for (double d : vals) {
      if (!std::isnan(d)) payload += std::to_string(static_cast<long long>(d));
      payload += "\n";
    }
    const std::string outHbin = mintScratchPath(session.workdir(), "binscatter");
    if (!bsWriteFile(csvPayload, payload)) {
      blocks.push_back(bsRcError("binscatter: could not write the genxq() values", 603, "data_io"));
      return blocks;
    }
    const CobraRunResult wr = runCobra({"setcol", "--data", *session.currentHbin(), "--var", genxq, "--values",
                                        csvPayload, "--dtype", "f64", "--new", "--out", outHbin});
    if (!wr.ok || !cecon::native_exists(outHbin)) {
      blocks.push_back(bsRcError("binscatter: could not create " + genxq, 603, "data_io"));
      return blocks;
    }
    session.load(outHbin);
    session.implicitFloatVars.erase(genxq);
    session.varMeta[genxq].stataType = nquantiles <= 100 ? "byte" : nquantiles <= 32740 ? "int" : "long";
    session.setVarLabel(genxq, std::to_string(nquantiles) + " quantiles of " + xReg);
    genxqMutation = mutationResult(session, "binscatter", {genxq}, {});
  }

  // ---- the picture (Arc G's twoway block): one scatter layer per series, the
  // fit lines evaluated over their range, connect lines through the points.
  std::vector<ojson> layers;
  int layerIdx = 0;
  for (size_t b = 0; b < bynum; ++b)
    for (size_t yi = 0; yi < ynum; ++yi) {
      ojson pts = ojson::array();
      for (size_t row = 0;; ++row) {
        const double xv = pt(yi, b, row, true), yv = pt(yi, b, row, false);
        if (std::isnan(xv) || std::isnan(yv)) break;
        pts.push_back(ojson::array({xv, yv}));
      }
      if (pts.empty()) continue;
      layers.push_back(qeGraphLayer(linetype == "connect" ? "line" : "scatter", pts, layerIdx));
      if (fit && yi < coefs.size() && b < coefs[yi].size()) {
        std::vector<double> cf;
        for (const auto& v : coefs[yi][b]) cf.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        const double a2 = qfit ? cf[0] : 0.0, a1 = qfit ? cf[1] : cf[0], a0 = qfit ? cf[2] : cf[1];
        const double lo = pts.front()[0].get<double>(), hi = pts.back()[0].get<double>();
        ojson line = ojson::array();
        const int steps = qfit ? 40 : 1;
        for (int s = 0; s <= steps && !std::isnan(a1) && !std::isnan(a0); ++s) {
          const double xv = s == steps ? hi : lo + (hi - lo) * s / steps;
          line.push_back(ojson::array({xv, a2 * xv * xv + a1 * xv + a0}));
        }
        layers.push_back(qeGraphLayer("line", line, layerIdx));
      }
      ++layerIdx;
    }
  ojson graph = qeGraphBlock(layers, std::string(), xName, ytitle, session);
  blocks.push_back(registerQeGraph(std::move(graph), session));

  ojson out;
  out["type"] = "qe_result";
  out["estimator"] = "binscatter";
  out["N"] = r.value("N", 0);
  out["nquantiles"] = nqFinal;
  out["graphcmd"] = graphcmd;
  blocks.push_back(out);
  if (genxqMutation.has_value()) blocks.push_back(*genxqMutation);
  return blocks;
}

}  // namespace handlers
}  // namespace piet
