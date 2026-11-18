// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Survey commands.
#include "piet/handlers/estimation.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

// ---------------------------------------------------------------------------
// svyset
// ---------------------------------------------------------------------------
//
//     svyset [psu] [weight] [, strata() fpc() weightN()] [|| ssu [, ...]] [, global options]
//     svyset                       the design in memory (or "no survey characteristics are set")
//     svyset, clear                forget it
//
// The design is kept whole (SessionState::SvySettings) and shown the way Stata shows it;
// what the engine's `svy:` can compute from it is a smaller set, and `svy:` refuses the
// rest. Every call that builds a design starts from nothing (the previous design goes
// unless noclear or clear(...) is given), and a call that fails leaves the old design.
namespace {

using SvySt = SessionState::SvySettings;

ojson svyErr(const std::string& msg, int rc) {
  return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// Splits at top-level occurrences of `sep` (outside parentheses, brackets and quotes).
std::vector<std::string> svySplitTop(const std::string& s, const std::string& sep) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inq = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') inq = !inq;
    if (!inq) {
      if (c == '(' || c == '[') ++depth;
      if (c == ')' || c == ']') --depth;
      if (depth == 0 && s.compare(i, sep.size(), sep) == 0) {
        out.push_back(cur);
        cur.clear();
        i += sep.size() - 1;
        continue;
      }
    }
    cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

struct SvyOpt {
  std::string name;  // lower case
  std::string arg;
  bool hasArg = false;
};

// `name`, `name(args)` tokens of an option list, in order.
std::vector<SvyOpt> svyOptTokens(const std::string& s, bool* bad) {
  std::vector<SvyOpt> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
    if (j == i) {
      *bad = true;
      return out;
    }
    SvyOpt o;
    o.name = lowerM4(s.substr(i, j - i));
    i = j;
    if (i < s.size() && s[i] == '(') {
      int depth = 0;
      bool inq = false;
      size_t k = i;
      for (; k < s.size(); ++k) {
        if (s[k] == '"') inq = !inq;
        if (inq) continue;
        if (s[k] == '(') ++depth;
        if (s[k] == ')' && --depth == 0) break;
      }
      if (k >= s.size()) {
        *bad = true;
        return out;
      }
      o.arg = s.substr(i + 1, k - i - 1);
      o.hasArg = true;
      i = k + 1;
    }
    out.push_back(o);
  }
  return out;
}

// `typed` abbreviates `full` to at least `minLen` characters.
bool svyAbbrev(const std::string& typed, const std::string& full, size_t minLen) {
  return typed.size() >= minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
}

std::vector<std::string> svyWords(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  std::string w;
  while (is >> w) out.push_back(w);
  return out;
}

std::string svyJoin(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& x : v) out += (out.empty() ? "" : " ") + x;
  return out;
}

// One variable name that must exist (numeric when `numeric`). A unique abbreviation resolves (varabbrev
// on, the `syntax varname` rule), and `*name` is left holding the full name so the design stores it.
bool svyVar(SessionState& session, std::string* name, bool numeric, ojson* err,
            const std::string& inOption) {
  Datastore* ds = session.datastore();
  bool ambiguous = false;
  std::optional<std::string> full;
  if (ds != nullptr) full = resolveVarName(*name, session, &ambiguous);
  if (!full) {
    *err = svyErr(ambiguous ? "ambiguous abbreviation " + *name
                            : "variable " + *name + " not found" +
                                  (inOption.empty() ? "" : "\n(error in option " + inOption + "())"),
                  111);
    return false;
  }
  *name = *full;
  if (numeric && ds->dtype(*name) != "f64") {
    *err = svyErr("type mismatch", 109);
    return false;
  }
  return true;
}

// A varlist option (brrweight() ...): names, ranges and wildcards, all numeric.
bool svyVarlist(SessionState& session, const std::string& spec, std::vector<std::string>* out,
                ojson* err) {
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) {
    *err = ex.error;
    return false;
  }
  Datastore* ds = session.datastore();
  // A name the dataset does not hold exactly is a unique abbreviation of one, or r(111): the varlist
  // rule. Wildcard and range spellings already expanded to full names above.
  for (auto& v : ex.tokens) {
    if (ds == nullptr || ds->has(v)) continue;
    bool ambiguous = false;
    std::optional<std::string> full = resolveVarName(v, session, &ambiguous);
    if (!full) {
      *err = svyErr(ambiguous ? v + " ambiguous abbreviation" : "variable " + v + " not found", 111);
      return false;
    }
    v = *full;
  }
  for (const auto& v : ex.tokens)
    if (ds != nullptr && ds->has(v) && ds->dtype(v) != "f64") {
      *err = svyErr("type mismatch", 109);
      return false;
    }
  *out = ex.tokens;
  return true;
}

void svyClearAll(SvySt& d) { d = SvySt(); }

// clear(list): the named parts only.
bool svyClearParts(SvySt& d, const std::string& list, ojson* err) {
  for (const auto& w : svyWords(lowerM4(list))) {
    if (svyAbbrev(w, "pweight", 2) || svyAbbrev(w, "iweight", 1) || svyAbbrev(w, "weight", 1)) {
      d.wtype.clear();
      d.wvar.clear();
    } else if (w == "vce") {
      d.vce.clear();
    } else if (w == "mse") {
      d.mse.clear();
    } else if (svyAbbrev(w, "brrweight", 3)) {
      d.brr.clear();
    } else if (w == "fay") {
      d.fay.clear();
    } else if (svyAbbrev(w, "bsrweight", 3)) {
      d.bsr.clear();
    } else if (w == "bsn") {
      d.bsn.clear();
    } else if (svyAbbrev(w, "jkrweight", 3)) {
      d.jkr.clear();
    } else if (svyAbbrev(w, "sdrweight", 3)) {
      d.sdr.clear();
      d.sdrfpc.clear();
    } else if (w == "dof") {
      d.dof.clear();
    } else if (svyAbbrev(w, "dofsubpop", 6)) {
      d.dofsubpop.clear();
    } else if (svyAbbrev(w, "poststrata", 5) || svyAbbrev(w, "postweight", 5)) {
      d.poststrata.clear();
      d.postweight.clear();
    } else if (svyAbbrev(w, "rake", 4) || svyAbbrev(w, "regress", 3)) {
      d.calmethod.clear();
      d.calmodel.clear();
      d.calopts.clear();
    } else if (svyAbbrev(w, "singleunit", 6)) {
      d.singleunit.clear();
    } else {
      *err = svyErr("option clear() invalid", 198);
      return false;
    }
  }
  return true;
}

// rake()/regress(): `model [, totals() ll() ul() iterate() tolerance() force noconstant ...]`.
bool svyCalibration(const std::string& method, const std::string& arg, SvySt& d, ojson* err) {
  auto bad = [&](const std::string& why) {
    *err = svyErr("option " + method + "() invalid;\n" + why, 198);
    return false;
  };
  const std::vector<std::string> parts = svySplitTop(arg, ",");
  const std::string model = strutil::trim(parts[0]);
  std::string opts;
  for (size_t i = 1; i < parts.size(); ++i) opts += (i > 1 ? "," : "") + parts[i];
  opts = strutil::trim(opts);
  if (model.empty()) return bad("varlist required");
  bool badTok = false;
  std::vector<SvyOpt> toks = svyOptTokens(opts, &badTok);
  if (badTok) return bad("option invalid");
  bool haveTotals = false;
  for (const auto& t : toks) {
    if (svyAbbrev(t.name, "totals", 3)) {
      haveTotals = true;
      for (const auto& w : svyWords(t.arg))
        if (w.find('=') == std::string::npos)
          return bad("totals: list of numbers requires copy option");
    } else if (!(t.name == "ll" || t.name == "ul" || svyAbbrev(t.name, "iterate", 4) ||
                 svyAbbrev(t.name, "tolerance", 3) || t.name == "force" ||
                 svyAbbrev(t.name, "noconstant", 3) || svyAbbrev(t.name, "copy", 4))) {
      return bad("option " + t.name + " not allowed");
    }
  }
  if (!haveTotals) return bad("option totals() required");
  d.calmethod = method;
  d.calmodel = model;
  d.calopts = opts;
  return true;
}

// The design as Get builds r(): r(settings), r(su#) ... in svyset.ado's order.
struct SvyGet {
  std::string settings;
  std::vector<std::pair<std::string, std::string>> macros;
  int stages = 0, stagesWt = 0;
};

bool svyGet(const SvySt& d, SvyGet* g, ojson* err) {
  if (!d.isSet()) {
    g->settings = ", clear";
    return true;
  }
  auto& m = g->macros;
  std::string wt;
  if (!d.wvar.empty()) {
    wt = "[" + d.wtype + "= " + d.wvar + "]";
    m.push_back({"wtype", d.wtype});
    m.push_back({"wexp", "= " + d.wvar});
    m.push_back({"wvar", d.wvar});
  }
  std::string gsets;
  auto add = [&](const std::string& t) { gsets += (gsets.empty() ? "" : " ") + t; };
  if (!d.brr.empty()) {
    add("brrweight(" + d.brr + ")");
    m.push_back({"brrweight", d.brr});
  }
  if (!d.fay.empty() && d.fay != "0") {
    add("fay(" + d.fay + ")");
    m.push_back({"fay", d.fay});
  }
  if (!d.bsr.empty()) {
    add("bsrweight(" + d.bsr + ")");
    m.push_back({"bsrweight", d.bsr});
  }
  if (!d.bsn.empty() && d.bsn != "1") {
    add("bsn(" + d.bsn + ")");
    m.push_back({"bsn", d.bsn});
  }
  if (!d.jkr.empty()) {
    add("jkrweight(" + d.jkr + ")");
    m.push_back({"jkrweight", d.jkr});
  }
  if (!d.sdr.empty()) {
    add("sdrweight(" + d.sdr + (d.sdrfpc.empty() ? "" : ", fpc(" + d.sdrfpc + ")") + ")");
    m.push_back({"sdrweight", d.sdr});
    if (!d.sdrfpc.empty()) m.push_back({"sdrfpc", d.sdrfpc});
  }
  if (!d.dof.empty()) {
    add("dof(" + d.dof + ")");
    m.push_back({"dof", d.dof});
  }
  if (!d.dofsubpop.empty()) {
    add(d.dofsubpop);
    m.push_back({"dofsubpop", d.dofsubpop});
  }
  if (!d.poststrata.empty() || !d.postweight.empty()) {
    add("poststrata(" + d.poststrata + ")");
    add("postweight(" + d.postweight + ")");
    m.push_back({"poststrata", d.poststrata});
    m.push_back({"postweight", d.postweight});
  }
  if (!d.calmethod.empty()) {
    const std::string spec = d.calmodel + ", " + d.calopts;
    add(d.calmethod + "(" + spec + ")");
    m.push_back({"calmethod", d.calmethod});
    m.push_back({"calmodel", d.calmodel});
    m.push_back({"calopts", d.calopts});
    m.push_back({d.calmethod, spec});
  }
  const std::string su = d.singleunit.empty() ? std::string("missing") : d.singleunit;
  add("singleunit(" + su + ")");
  m.push_back({"singleunit", su});
  const std::string vce = d.vce.empty() ? std::string("linearized") : d.vce;
  add("vce(" + vce + ")" + (d.mse.empty() ? "" : " mse"));
  m.push_back({"vce", vce});
  if (!d.mse.empty()) m.push_back({"mse", "mse"});

  g->stages = d.stages;
  g->stagesWt = d.stagesWt;
  if (g->stagesWt > 0 && !d.wtype.empty()) {
    *err = svyErr("invalid weight specification;\n" + d.wtype + "s not allowed with stage-level weight() options", 198);
    return false;
  }
  for (const char* w : {"brr", "bsr", "jkr", "sdr"}) {
    const std::string& v = std::string(w) == "brr" ? d.brr : std::string(w) == "bsr" ? d.bsr : std::string(w) == "jkr" ? d.jkr : d.sdr;
    if (g->stagesWt > 0 && !v.empty()) {
      *err = svyErr(std::string("invalid weight specification;\noption ") + w + "weight() not allowed with stage-level weight() options", 198);
      return false;
    }
  }
  const int I = std::max(g->stages, g->stagesWt);
  std::string sets;
  std::string oror;
  std::string comma = ",";
  for (int i = 0; i < I; ++i) {
    const SvySt::Stage st = i < static_cast<int>(d.stage.size()) ? d.stage[i] : SvySt::Stage();
    const std::string n = std::to_string(i + 1);
    if (!st.su.empty()) {
      if (st.su == "_n" && i + 1 < g->stages) {
        *err = svyErr("invalid use of _n; observations can only be sampled in the final stage", 198);
        return false;
      }
      if (st.su != "_n") m.push_back({"su" + n, st.su});
    } else if (i + 1 < I) {
      *err = svyErr("invalid survey characteristics;\nsampling unit variable is not set for stage " + n + " of " + std::to_string(I) + " stages", 459);
      return false;
    }
    sets += oror + (st.su.empty() ? std::string("_n") : st.su);
    if (!wt.empty()) {
      sets += " " + wt;
      wt.clear();
    }
    if (!st.strata.empty()) {
      m.push_back({"strata" + n, st.strata});
      sets += comma + " strata(" + st.strata + ")";
      comma.clear();
    }
    if (!st.weight.empty()) {
      m.push_back({"weight" + n, st.weight});
      sets += comma + " weight(" + st.weight + ")";
      comma.clear();
    }
    if (!st.fpc.empty()) {
      m.push_back({"fpc" + n, st.fpc});
      sets += comma + " fpc(" + st.fpc + ")";
      comma.clear();
    } else if (i + 1 < g->stages) {
      *err = svyErr("invalid survey characteristics;\nFPC variable is not set for stage " + n + " of " + std::to_string(g->stages) + " stages", 459);
      return false;
    }
    oror = " || ";
  }
  if (!gsets.empty()) sets += comma + " " + gsets;
  g->settings = sets;
  return true;
}

std::string svyFirstLast(const std::string& list) {
  const std::vector<std::string> w = svyWords(list);
  if (w.size() == 1) return w[0];
  if (w.size() > 2) return w.front() + " .. " + w.back();
  return list;
}

// What `svyset` prints for the design in memory.
std::string svyDisplay(const SvySt& d, const SvyGet& g) {
  if (g.settings == ", clear") return "no survey characteristics are set";
  const int I = std::max(g.stages, g.stagesWt);
  const std::string wlabel = d.wtype == "iweight" ? "Importance weights" : "Sampling weights";
  size_t maxlen = std::max(wlabel.size(), std::string("Sampling unit " + std::to_string(I)).size());
  if (!d.bsr.empty()) maxlen = std::max<size_t>(maxlen, 17);
  if (!d.jkr.empty()) maxlen = std::max<size_t>(maxlen, 17);
  if (!d.bsn.empty()) maxlen = std::max<size_t>(maxlen, 17);
  std::string out = "\n";
  auto line = [&](const std::string& label, const std::string& val) {
    out += std::string(label.size() < maxlen ? maxlen - label.size() : 0, ' ') + label + ": " + val + "\n";
  };
  line(wlabel, d.wvar.empty() ? std::string("<none>") : d.wvar);
  const std::string vce = d.vce.empty() ? std::string("linearized") : d.vce;
  line("VCE", vce);
  if (vce != "linearized") line("MSE", d.mse.empty() ? "off" : "on");
  if (!d.brr.empty()) line("BRR weights", svyFirstLast(d.brr));
  if (!d.fay.empty()) line("Fay's adjustment", d.fay);
  if (!d.bsr.empty()) line("Bootstrap weights", svyFirstLast(d.bsr));
  if (!d.bsn.empty()) line("Bootstrap samples", d.bsn);
  if (!d.jkr.empty()) line("Jackknife weights", svyFirstLast(d.jkr));
  if (!d.sdr.empty()) line("SDR weights", svyFirstLast(d.sdr));
  if (!d.dof.empty()) line("Design df", d.dof);
  else line("Subpop. df", d.dofsubpop == "dofsubpop" ? "on" : "off");
  if (!d.poststrata.empty()) line("Poststrata", d.poststrata);
  if (!d.postweight.empty()) line("Post. pop. sizes", d.postweight);
  if (!d.calmethod.empty()) line("Calibration", d.calmethod);
  line("Single unit", d.singleunit.empty() ? std::string("missing") : d.singleunit);
  for (int i = 0; i < I; ++i) {
    const SvySt::Stage st = i < static_cast<int>(d.stage.size()) ? d.stage[i] : SvySt::Stage();
    const std::string n = std::to_string(i + 1);
    line("Strata " + n, st.strata.empty() ? std::string("<one>") : st.strata);
    line("Sampling unit " + n, (st.su.empty() || st.su == "_n") ? std::string("<observations>") : st.su);
    line("FPC " + n, st.fpc.empty() ? std::string("<zero>") : st.fpc);
    if (!st.weight.empty()) line("Weight " + n, st.weight);
    else if (g.stagesWt > 0) line("Weight " + n, "<none>");
  }
  out.pop_back();
  return out;
}

void svyPost(SessionState& session, const SvyGet& g, const SvySt& d) {
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (g.settings == ", clear") {
    session.eval.rStrings.set("settings", g.settings);
    posted.push_back("settings");
    eval::declareRPosted(session.eval, "svyset", posted);
    return;
  }
  if (g.stages > 0) {
    session.eval.rValues.set("stages", static_cast<double>(g.stages));
    posted.push_back("stages");
  }
  session.eval.rValues.set("stages_wt", static_cast<double>(g.stagesWt));
  posted.push_back("stages_wt");
  if (!d.dof.empty()) {
    try {
      session.eval.rValues.set("dof", std::stod(d.dof));
      posted.push_back("dof");
    } catch (const std::exception&) {
    }
  }
  session.eval.rStrings.set("settings", g.settings);
  posted.push_back("settings");
  for (const auto& [k, v] : g.macros) {
    if (k == "dof") continue;  // posted above as a number
    if (k == "fay" || k == "bsn") {  // numbers in r()
      try {
        session.eval.rValues.set(k, std::stod(v));
        posted.push_back(k);
        continue;
      } catch (const std::exception&) {
      }
    }
    session.eval.rStrings.set(k, v);
    posted.push_back(k);
  }
  eval::declareRPosted(session.eval, "svyset", posted);
}

// Keeps the engine's view (psu, pweight, strata) in step with the design.
void svySyncEngine(SvySt& d) {
  d.psu.clear();
  d.strata.clear();
  d.pweight.clear();
  if (!d.stage.empty()) {
    d.psu = d.stage[0].su == "_n" ? std::string() : d.stage[0].su;
    d.strata = d.stage[0].strata;
  }
  if (d.wtype == "pweight") d.pweight = d.wvar;
}

}  // namespace

ojson runSvyset(const eval::ParsedCommand& parsed, SessionState& session) {
  // The line after the command word.
  std::string args = strutil::trim(parsed.raw);
  {
    size_t i = 0;
    while (i < args.size() && !std::isspace(static_cast<unsigned char>(args[i])) && args[i] != ',') ++i;
    args = strutil::trim(args.substr(i));
  }
  SvySt cur = session.svyDesign.value_or(SvySt());
  auto finish = [&](const SvySt& d, bool show, std::vector<ojson> pre) -> ojson {
    SvyGet g;
    ojson err;
    if (!svyGet(d, &g, &err)) return err;
    ojson blocks = ojson::array();
    for (auto& b : pre) blocks.push_back(std::move(b));
    if (show) {
      ojson b;
      b["type"] = "display";
      b["text"] = svyDisplay(d, g);
      blocks.push_back(b);
    }
    session.svyDesign = d.isSet() ? std::optional<SvySt>(d) : std::nullopt;
    svyPost(session, g, d);
    return blocks;
  };

  // `svyset` alone, or only eclass/clear/mi: the design in memory, or forget it.
  {
    bool onlyFlags = args.empty();
    bool clear = false;
    if (!args.empty() && args[0] == ',') {
      bool bad = false;
      const auto toks = svyOptTokens(args.substr(1), &bad);
      onlyFlags = !bad;
      for (const auto& t : toks) {
        if (t.hasArg) onlyFlags = false;
        else if (svyAbbrev(t.name, "clear", 5)) clear = true;
        else if (!(svyAbbrev(t.name, "eclass", 6) || t.name == "mi")) onlyFlags = false;
      }
      if (toks.empty()) onlyFlags = false;
    }
    if (onlyFlags) {
      if (clear) {
        SvySt d;
        return finish(d, false, {});
      }
      return finish(cur, true, {});
    }
  }
  if (!session.hasData()) return svyErr("no variables defined", 111);

  // ---- stages and options -------------------------------------------------
  const std::vector<std::string> segs = svySplitTop(args, "||");
  struct Seg {
    std::string pre;
    std::vector<SvyOpt> opts;
  };
  std::vector<Seg> segments;
  std::vector<SvyOpt> globalOpts;
  for (const auto& sg : segs) {
    Seg s;
    const std::vector<std::string> cp = svySplitTop(sg, ",");
    s.pre = strutil::trim(cp[0]);
    std::string rest;
    for (size_t i = 1; i < cp.size(); ++i) rest += (i > 1 ? "," : "") + cp[i];
    bool bad = false;
    const std::vector<SvyOpt> toks = svyOptTokens(rest, &bad);
    if (bad) return svyErr("option invalid", 198);
    for (const auto& t : toks) {
      const std::string& n = t.name;
      const bool isGlobal = n == "vce" || n == "mse" || svyAbbrev(n, "brrweight", 3) || n == "fay" ||
                            svyAbbrev(n, "bsrweight", 3) || n == "bsn" || svyAbbrev(n, "jkrweight", 3) ||
                            svyAbbrev(n, "sdrweight", 3) || svyAbbrev(n, "singleunit", 6) || n == "dof" ||
                            svyAbbrev(n, "nodofsubpop", 6) || svyAbbrev(n, "dofsubpop", 6) ||
                            svyAbbrev(n, "poststrata", 5) || svyAbbrev(n, "postweight", 5) || n == "rake" ||
                            svyAbbrev(n, "regress", 3) || svyAbbrev(n, "clear", 5) || svyAbbrev(n, "noclear", 3) ||
                            n == "mi";
      if (isGlobal) globalOpts.push_back(t);
      else s.opts.push_back(t);
    }
    segments.push_back(std::move(s));
  }
  int stageN = static_cast<int>(segments.size());
  if (stageN == 1 && segments[0].pre.empty()) stageN = 0;

  // ---- global options (ParseGlobals) ---------------------------------------
  SvySt d = cur;
  bool noclear = false, clearGiven = false;
  std::string clear1;
  std::string vceArg, brrArg, bsrArg, jkrArg, sdrArg, dofArg, singleArg, rakeArg, regressArg;
  std::string poststrata, postweight;
  bool mse = false, dofsubpop = false, nodofsubpop = false;
  std::string fayArg, bsnArg;
  for (const auto& t : globalOpts) {
    const std::string& n = t.name;
    if (n == "vce") vceArg = strutil::trim(t.arg);
    else if (n == "mse") mse = true;
    else if (svyAbbrev(n, "brrweight", 3)) brrArg = t.arg;
    else if (n == "fay") fayArg = strutil::trim(t.arg);
    else if (svyAbbrev(n, "bsrweight", 3)) bsrArg = t.arg;
    else if (n == "bsn") bsnArg = strutil::trim(t.arg);
    else if (svyAbbrev(n, "jkrweight", 3)) jkrArg = t.arg;
    else if (svyAbbrev(n, "sdrweight", 3)) sdrArg = t.arg;
    else if (svyAbbrev(n, "singleunit", 6)) singleArg = strutil::trim(t.arg);
    else if (n == "dof") dofArg = strutil::trim(t.arg);
    else if (svyAbbrev(n, "nodofsubpop", 6)) nodofsubpop = true;
    else if (svyAbbrev(n, "dofsubpop", 6)) dofsubpop = true;
    else if (svyAbbrev(n, "poststrata", 5)) poststrata = strutil::trim(t.arg);
    else if (svyAbbrev(n, "postweight", 5)) postweight = strutil::trim(t.arg);
    else if (n == "rake") rakeArg = t.arg;
    else if (svyAbbrev(n, "regress", 3)) regressArg = t.arg;
    else if (svyAbbrev(n, "noclear", 3)) noclear = true;
    else if (svyAbbrev(n, "clear", 5)) {
      if (t.hasArg) clear1 = t.arg;
      else clearGiven = true;
    } else if (n == "mi") {
      return ojson(eval::errorBlock("unsupported_option",
                                    "Option `mi` is a valid Stata option that this version of PIET does not implement for `svyset`.",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }
  // Old single-stage spellings, only when no stage was typed.
  std::string oldPsu, oldStrata, oldFpc;
  bool oldSrs = false;
  if (stageN == 0 && !segments.empty()) {
    std::vector<SvyOpt> keep;
    for (const auto& t : segments[0].opts) {
      if (t.name == "psu") oldPsu = strutil::trim(t.arg);
      else if (svyAbbrev(t.name, "strata", 3) && stageN == 0) oldStrata = strutil::trim(t.arg);
      else if (t.name == "fpc") oldFpc = strutil::trim(t.arg);
      else if (t.name == "srs") oldSrs = true;
      else keep.push_back(t);
    }
    segments[0].opts = keep;
    if (!keep.empty()) return svyErr("option " + keep[0].name + " not allowed", 198);
  }

  ojson err;
  if (!dofArg.empty() && (dofsubpop || nodofsubpop))
    return svyErr("options dof() and " + std::string(dofsubpop ? "dofsubpop" : "nodofsubpop") + " may not be combined", 198);
  if (dofsubpop && nodofsubpop) return svyErr("options dofsubpop and nodofsubpop may not be combined", 198);
  if (!poststrata.empty() || !postweight.empty()) {
    if (postweight.empty()) return svyErr("option poststrata() requires option postweight()", 198);
    if (poststrata.empty()) return svyErr("option postweight() requires option poststrata()", 198);
    if (!rakeArg.empty()) return svyErr("options poststrata() and rake() may not be combined", 198);
    if (!regressArg.empty()) return svyErr("options poststrata() and regress() may not be combined", 198);
  } else if (!rakeArg.empty() && !regressArg.empty()) {
    return svyErr("options rake() and regress() may not be combined", 198);
  }
  if (clearGiven && !clear1.empty()) return svyErr("options clear and clear() may not be combined", 198);
  if (clearGiven && noclear) return svyErr("options clear and noclear may not be combined", 198);

  // Clear (everything, or the parts named) unless noclear.
  if (!noclear && clear1.empty()) {
    svyClearAll(d);
  } else {
    if (!svyClearParts(d, clear1, &err)) return err;
    bool stageOpts = stageN > 1 || !oldPsu.empty() || !oldStrata.empty() || !oldFpc.empty() || oldSrs;
    if (stageOpts) return svyErr("option noclear is not allowed with design_options", 198);
    noclear = true;
  }

  // noclear (or clear(...)) with a stage text: only a weight is allowed there.
  std::string wtypeNew, wvarNew;
  auto parseWeight = [&](std::string* text, ojson* e, bool* ok) {
    *ok = true;
    const size_t lb = text->find('[');
    if (lb == std::string::npos) return;
    const size_t rb = text->find(']', lb);
    if (rb == std::string::npos) {
      *e = svyErr("invalid syntax", 198);
      *ok = false;
      return;
    }
    std::string inner = strutil::trim(text->substr(lb + 1, rb - lb - 1));
    const size_t eq = inner.find('=');
    std::string wt = lowerM4(strutil::trim(eq == std::string::npos ? inner : inner.substr(0, eq)));
    std::string ex = eq == std::string::npos ? std::string() : strutil::trim(inner.substr(eq + 1));
    std::string full;
    if (svyAbbrev(wt, "pweight", 2)) full = "pweight";
    else if (svyAbbrev(wt, "iweight", 1)) full = "iweight";
    else {
      *e = svyErr(wt + " not allowed", 101);
      *ok = false;
      return;
    }
    if (ex.empty()) {
      *e = svyErr("weight required", 401);
      *ok = false;
      return;
    }
    wtypeNew = full;
    wvarNew = ex;
    *text = strutil::trim(text->substr(0, lb) + " " + text->substr(rb + 1));
  };
  if (noclear && stageN > 0) {
    std::string pre = segments[0].pre;
    bool ok = true;
    parseWeight(&pre, &err, &ok);
    if (!ok || !pre.empty() || !segments[0].opts.empty())
      return svyErr("option noclear is not allowed with design_options", 198);
    if (!svyVar(session, &wvarNew, true, &err, "")) return err;
    d.wtype = wtypeNew;
    d.wvar = wvarNew;
    stageN = 0;
  }

  // Global settings.
  if (!vceArg.empty() || mse) {
    std::string v = lowerM4(vceArg);
    if (v.empty() || svyAbbrev(v, "linearized", 3)) v = "linearized";
    else if (svyAbbrev(v, "jackknife", 4)) v = "jackknife";
    else if (svyAbbrev(v, "bootstrap", 4)) v = "bootstrap";
    else if (v != "brr" && v != "sdr") return svyErr("vce(" + vceArg + ") invalid", 198);
    d.vce = v;
    d.mse = mse ? "mse" : "";
  }
  if (!brrArg.empty()) {
    std::vector<std::string> v;
    if (!svyVarlist(session, brrArg, &v, &err)) return err;
    d.brr = svyJoin(v);
  }
  if (!fayArg.empty()) {
    char* endp = nullptr;
    const double f = std::strtod(fayArg.c_str(), &endp);
    if (*endp != '\0' || f < 0 || f > 2) return svyErr("option fay() incorrectly specified", 125);
    d.fay = fayArg;
  }
  if (!bsrArg.empty()) {
    std::vector<std::string> v;
    if (!svyVarlist(session, bsrArg, &v, &err)) return err;
    d.bsr = svyJoin(v);
  }
  if (!bsnArg.empty()) {
    char* endp = nullptr;
    const long b = std::strtol(bsnArg.c_str(), &endp, 10);
    if (*endp != '\0' || b <= 0) return svyErr("option bsn() incorrectly specified", 125);
    d.bsn = bsnArg;
  }
  if (!jkrArg.empty()) {
    // varlist [, stratum() fpc() multiplier() reset]
    const std::vector<std::string> cp = svySplitTop(jkrArg, ",");
    std::vector<std::string> v;
    if (!svyVarlist(session, strutil::trim(cp[0]), &v, &err)) return err;
    if (v.empty()) return svyErr("option jkrweight() incorrectly specified", 198);
    d.jkr = svyJoin(v);
  }
  if (!sdrArg.empty()) {
    const std::vector<std::string> cp = svySplitTop(sdrArg, ",");
    std::vector<std::string> v;
    if (!svyVarlist(session, strutil::trim(cp[0]), &v, &err)) return err;
    if (v.empty()) return svyErr("option sdrweight() incorrectly specified", 198);
    d.sdr = svyJoin(v);
    d.sdrfpc.clear();
    if (cp.size() > 1) {
      bool bad = false;
      for (const auto& t : svyOptTokens(strutil::trim(cp[1]), &bad))
        if (t.name == "fpc") d.sdrfpc = strutil::trim(t.arg);
      if (bad) return svyErr("option sdrweight() incorrectly specified", 198);
    }
  }
  if (!dofArg.empty()) {
    char* endp = nullptr;
    const double f = std::strtod(dofArg.c_str(), &endp);
    if (*endp != '\0' || dofArg.empty()) return svyErr("dof() invalid -- invalid number", 121);
    (void)f;
    d.dof = dofArg;
    d.dofsubpop.clear();
  }
  if (dofsubpop) d.dofsubpop = "dofsubpop";
  if (nodofsubpop) d.dofsubpop = "nodofsubpop";
  if (!poststrata.empty()) {
    if (!svyVar(session, &poststrata, false, &err, "")) return err;
    if (!svyVar(session, &postweight, true, &err, "")) return err;
    d.poststrata = poststrata;
    d.postweight = postweight;
  }
  if (!rakeArg.empty() && !svyCalibration("rake", rakeArg, d, &err)) return err;
  if (!regressArg.empty() && !svyCalibration("regress", regressArg, d, &err)) return err;
  if (!singleArg.empty()) {
    const std::string sv = lowerM4(singleArg);
    std::string full;
    for (const char* f : {"missing", "certainty", "scaled", "centered"})
      if (svyAbbrev(sv, f, 1)) full = f;
    if (full.empty()) return svyErr("option singleunit() invalid", 198);
    d.singleunit = full;
  }

  // ---- the stages -----------------------------------------------------------
  std::vector<ojson> pre;
  if (stageN == 0 && !noclear) {
    // Only the old one-stage spellings, if any, make a stage here.
    std::string su = oldPsu;
    if (oldSrs) {
      if (!oldStrata.empty()) return svyErr("options strata() and srs may not be combined", 198);
      if (!oldPsu.empty()) return svyErr("options psu() and srs may not be combined", 198);
      su = "_n";
    }
    if (!su.empty() || !oldStrata.empty() || !oldFpc.empty()) {
      segments.assign(1, Seg());
      segments[0].pre = su;
      if (!oldStrata.empty()) segments[0].opts.push_back({"strata", oldStrata, true});
      if (!oldFpc.empty()) segments[0].opts.push_back({"fpc", oldFpc, true});
      stageN = 1;
    }
  }
  int wgtN = 0;
  int stagesKept = stageN;
  d.stage.clear();
  for (int i = 1; i <= stageN; ++i) {
    Seg& sg = segments[static_cast<size_t>(i - 1)];
    std::string text = sg.pre;
    std::string wtypeI, wvarI;
    {
      bool ok = true;
      wtypeNew.clear();
      wvarNew.clear();
      parseWeight(&text, &err, &ok);
      if (!ok) return err;
      wtypeI = wtypeNew;
      wvarI = wvarNew;
    }
    std::string su = strutil::trim(text);
    std::string strata, fpc, weight;
    for (const auto& t : sg.opts) {
      const std::string& n = t.name;
      if (svyAbbrev(n, "strata", 3)) strata = strutil::trim(t.arg);
      else if (n == "fpc") fpc = strutil::trim(t.arg);
      else if (n == "weight" || n == "weight" + std::to_string(i) || svyAbbrev(n, "weight", 1)) weight = strutil::trim(t.arg);
      else if (n == "psu" && i == 1) {
        if (!su.empty() && su != strutil::trim(t.arg)) return svyErr("option psu() invalid", 198);
        su = strutil::trim(t.arg);
      } else {
        return svyErr("option " + n + " not allowed", 198);
      }
    }
    if (su.empty()) {
      if (weight.empty() && wvarI.empty() && strata.empty() && fpc.empty()) return svyErr("psuid required", 198);
      su = "_n";
    }
    if (su == "_n" && i < stageN)
      return svyErr("invalid use of _n; observations can only be sampled in the final stage", 198);
    if (!wvarI.empty()) {
      if (!d.wvar.empty()) return svyErr("sampling weights may only be specified once", 198);
      if (!svyVar(session, &wvarI, true, &err, "")) return err;
      d.wtype = wtypeI;
      d.wvar = wvarI;
    }
    if (su != "_n") {
      // `confirm variable`: a unique abbreviation names the variable; the design keeps the full name.
      Datastore* dsx = session.datastore();
      std::optional<std::string> suFull;
      if (dsx != nullptr) suFull = resolveVarName(su, session);
      if (!suFull) return svyErr(su + " invalid " + (i == 1 ? "psu" : "ssu") + " variable name", 111);
      su = *suFull;
    }
    if (!strata.empty() && !svyVar(session, &strata, false, &err, "strata")) return err;
    if (!fpc.empty() && !svyVar(session, &fpc, true, &err, "fpc")) return err;
    if (!weight.empty() && !svyVar(session, &weight, true, &err, "weight" + std::to_string(i))) return err;
    SvySt::Stage st;
    st.su = su;
    st.strata = strata;
    st.fpc = fpc;
    st.weight = weight;
    d.stage.push_back(st);
    // Without an FPC the stage is sampled with replacement: later stages do not count.
    if (fpc.empty() && stageN > i && stagesKept == stageN) {
      pre.push_back(ojson{{"type", "display"},
                          {"text", "note: stage " + std::to_string(i) +
                                       " is sampled with replacement; further stages will be ignored for\n"
                                       "      variance estimation."}});
      stagesKept = i;
    }
    if (!weight.empty()) wgtN = i;
  }
  if (!noclear && !d.stage.empty()) {
    d.stages = stagesKept == 0 ? 1 : stagesKept;
  } else if (d.stage.empty() && cur.stages > 0 && noclear) {
    d.stage = cur.stage;
    d.stages = cur.stages;
    d.stagesWt = cur.stagesWt;
  }
  if (wgtN > 0) d.stagesWt = wgtN;
  else if (!noclear) d.stagesWt = 0;
  svySyncEngine(d);
  return finish(d, true, std::move(pre));
}

// `svy: CMD ...` — rewrite into the ordinary engine call plus the design flags.
ojson runSvyPrefix(const std::string& innerRaw, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (!session.svyDesign.has_value())
    return ojson(eval::errorBlock(
        "engine_error", "data not set up for svy, use svyset", std::nullopt, std::nullopt,
        std::nullopt, "r(119)"));
  const auto& d = *session.svyDesign;
  // The engine computes one-stage linearized designs with a probability weight, strata and
  // no finite-population correction; every other design setting changes the estimates, so
  // it is refused rather than dropped.
  {
    std::string unsupported;
    if (d.wtype == "iweight") unsupported = "an importance weight";
    else if (d.pweight.empty()) unsupported = "a design without a probability weight";
    else if (d.psu.empty()) unsupported = "a design sampled by observation (no sampling unit variable)";
    else if (d.stages > 1 || d.stagesWt > 0) unsupported = "a multistage design";
    else if (!d.stage.empty() && !d.stage[0].fpc.empty()) unsupported = "a finite-population correction (fpc())";
    else if (!d.vce.empty() && d.vce != "linearized") unsupported = "vce(" + d.vce + ")";
    else if (!d.mse.empty() || !d.brr.empty() || !d.fay.empty() || !d.bsr.empty() || !d.bsn.empty() ||
             !d.jkr.empty() || !d.sdr.empty())
      unsupported = "replication weights";
    else if (!d.dof.empty()) unsupported = "dof()";
    else if (d.dofsubpop == "dofsubpop") unsupported = "dofsubpop";
    else if (!d.poststrata.empty() || !d.postweight.empty()) unsupported = "poststratification";
    else if (!d.calmethod.empty()) unsupported = "calibration (rake()/regress())";
    else if (!d.singleunit.empty() && d.singleunit != "missing") unsupported = "singleunit(" + d.singleunit + ")";
    if (!unsupported.empty())
      return ojson(eval::errorBlock(
          "unsupported_option",
          "`svy:` with " + unsupported + " in the survey design is a valid Stata construction that this "
          "version of PIET does not implement (its survey prefix covers one-stage linearized designs "
          "with a probability weight and strata).",
          std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }

  eval::ParsedCommand p;
  try {
    p = eval::tokenize(innerRaw);
  } catch (const std::exception&) {
    return engineErrorBlockM4("syntax_error: could not parse the command after `svy:`",
                              "syntax_error");
  }
  const std::string inner = lowerM4(trimM4(p.cmd));
  if (p.weightVar.has_value())
    return engineErrorBlockM4(
        "svy: weights may not be given on the command line — the design's [pweight=] applies.",
        "unsupported_option");

  std::vector<std::string> argv;
  if (inner == "regress" || inner == "reg" || inner == "r" || inner == "feols") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: regress requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"feols", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else if (inner == "mean") {
    if (p.varlist.empty())
      return engineErrorBlockM4("syntax_error: svy: mean requires at least one variable",
                                "syntax_error");
    argv = {"mean", "--vars", joinCommaM4(p.varlist)};
  } else if (inner == "logit" || inner == "logistic") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: logit requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"logit", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else {
    return engineErrorBlockM4(
        "`svy: " + p.cmd + "` is a valid Stata construction that this version of PIET does not "
        "implement (the survey prefix covers regress, mean and logit).",
        "unsupported_command");
  }
  if (p.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*p.ifExpr);
  }
  argv.push_back("--weight");
  argv.push_back(d.pweight);
  argv.push_back("--wtype");
  argv.push_back("pw");
  argv.push_back("--svy");
  argv.push_back("--psu");
  argv.push_back(d.psu);
  // A design without strata() is one stratum for every svy verb the engine
  // serves (BUG-1424: its mean and logit paths take it, as feols did).
  if (!d.strata.empty()) {
    argv.push_back("--strata");
    argv.push_back(d.strata);
  }
  // e(cmd) after `svy: regress` is `regress` (w2t_boottest_family.log:2497
  // H1_svy ecmd=regress); the engine's verb stays the cobraCmd every replay
  // keys on. mean and logit keep publishing the engine's word as before.
  ojson raw;
  ojson block = runP7MleAsM4(argv[0], argv, session, &raw,
                             argv[0] == "feols" ? std::string("regress") : std::string());
  // FU-471: as in runLogitProbit, the logit reply's `df_m` stays on the cached
  // reply (e(df_m)) and off the block, which never carried it.
  if (argv[0] == "logit" && block.is_object()) block.erase("df_m");
  // The design's counts from the reply's svy block (BUG-1424: StataBE e(N_psu) 20,
  // e(N_strata) 1, e(df_r) 19 after `svy: mean` / `svy: logit` on a one-stratum design).
  if (block.value("type", std::string()) != "error" && raw.is_object() && raw.contains("svy") &&
      raw["svy"].is_object()) {
    const ojson& sv = raw["svy"];
    for (const auto& [key, name] : std::vector<std::pair<const char*, const char*>>{
             {"n_psu", "N_psu"}, {"n_strata", "N_strata"}, {"design_df", "df_r"},
             {"pop_size", "N_pop"}})
      if (sv.contains(key) && sv[key].is_number())
        putEScalarNL(session, name, sv[key].get<double>());
    // BUG-1513 (StataBE 19.5, one stratum and strata(s)): `svy: regress` e(df_m) 1, the
    // numerator dof of the design-adjusted F the reply sends; e(prefix) svy; e(title)
    // "Survey: " + the command's own title ("Linear regression", "Mean estimation",
    // "Logistic regression" after svy: logit and svy: logistic). e(vce)/e(vcetype)
    // linearized/Linearized come with the model (cacheLastModelM4, setLastModel).
    if (argv[0] == "feols" && sv.contains("f_df1") && sv["f_df1"].is_number())
      putEScalarNL(session, "df_m", sv["f_df1"].get<double>());
    putEStringNL(session, "prefix", "svy");
    putEStringNL(session, "title", argv[0] == "feols"  ? "Survey: Linear regression"
                                   : argv[0] == "mean" ? "Survey: Mean estimation"
                                                       : "Survey: Logistic regression");
  }
  return block;
}

// ===========================================================================
// P7 — survival: stset / stcox / streg
//
// `stset` is an ENGINE verb, not PIET state: it materializes `_t0/_t/_d/_st` as
// real columns, which is data computation, and every later st command then reads
// them as ordinary variables — the same arrangement Stata has. What PIET keeps is
// the DECLARATION (session.stContext), which is what lets stcox/streg reject
// honestly on un-stset data and lets `vce(robust)` cluster on the subject id.


}  // namespace piet::handlers
