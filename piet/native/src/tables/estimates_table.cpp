// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
#include "piet/handlers/estimates_tables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/cclass.h"       // settingCValue
#include "piet/display_hardening.h"  // evalScalarExpr
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "results/stored_estimates_internal.h"
#include "estout_internal.h"
#include "tables_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

namespace table_detail {
// tables/estout_models.cpp: the r() `estimates table` leaves (est_table.ado).
void estTablePostR(SessionState& session, const std::vector<std::string>& names,
                   const std::vector<EoModel>& ems, const std::vector<EoKey>& rows,
                   const std::vector<std::string>& stats);
}  // namespace table_detail

using table_detail::EoCoefVals;
using table_detail::EoKey;
using table_detail::EoModel;

// ---------------------------------------------------------------------------
// `estimates table [NAME...] [, options]` -- Stata's own model-comparison table (est_table.ado).
//
// The coefficients are read the way estout reads them (the models' e(b) stripes, base levels and
// omitted columns marked), merged over the models in the order mat_capp gives, narrowed by
// keep() / drop(), and written in est_table's one-line layout: a variable column of varwidth()
// characters, a bar, and one column of modelwidth() characters per model. The cell formats, the
// star legend, the statistics panel and the wrapping of wide tables follow its Display program;
// every layout figure below is read off that program, not measured.
// ---------------------------------------------------------------------------
namespace {

// A Stata display format: %[-][0][W][.D](f|g|e)[c]; `len` is the width est_table measures.
bool chkFormat(const std::string& fmt, double probe, size_t* len) {
  static const std::regex re(R"(^%(-?)(0?)(\d*)(\.(\d+))?([feg])(c?)$)");
  std::smatch m;
  if (!std::regex_match(fmt, m, re)) return false;
  if (m[3].length() > 0) {
    *len = static_cast<size_t>(std::stoi(m[3].str()));
  } else {
    *len = table_detail::fmtNum(probe, fmt).size();
  }
  return true;
}

// Stata's %W.0g: as many significant digits as fit in W-1 characters once the leading zero of a
// |x|<1 value is dropped; a minus sign may stand outside that width (est_table's default %10.0g
// prints -.19581818 and 1.7465592; vs-stata bank 634ddb60, 29561649).
std::string generalFmt(double v, size_t width) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  const size_t budget = width > 1 ? width - 1 : 1;
  for (size_t prec = budget; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", static_cast<int>(prec), v);
    std::string t(buf);
    if (t.rfind("0.", 0) == 0) t.erase(0, 1);
    else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
    const size_t visible = t.size() - (t[0] == '-' ? 1 : 0);
    if (visible <= budget) return t;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

std::string fmtCell(double v, const std::string& fmt) {
  {
    static const std::regex g0(R"(^%(\d+)\.0g$)");
    std::smatch m;
    if (std::regex_match(fmt, m, g0)) return generalFmt(v, static_cast<size_t>(std::stoi(m[1].str())));
  }
  std::string s = table_detail::fmtNum(v, fmt);
  // a format with a width pads on the left; est_table right-aligns anyway
  size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  return s.substr(a);
}

size_t cps(const std::string& s) { return cecon::ident::codePointCount(s); }

std::string padLeft(const std::string& s, size_t w) {
  const size_t n = cps(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

std::string padRight(const std::string& s, size_t w) {
  const size_t n = cps(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}

// {center w:s}: the odd leftover goes to the right (a model name `A` in the 10-wide column of
// `estimates table A B` has 4 blanks before it and 5 after; vs-stata bank 5b6813ac).
std::string centerIn(const std::string& s, size_t w) {
  const size_t n = cps(s);
  if (n >= w) return s;
  const size_t left = (w - n) / 2;
  return std::string(left, ' ') + s + std::string(w - n - left, ' ');
}

// The first `n` characters of a UTF-8 string.
std::string firstChars(const std::string& s, size_t n) {
  size_t i = 0, k = 0;
  while (i < s.size() && k < n) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++k;
  }
  return s.substr(0, i);
}

// Stata's abbrev(s, w): a string longer than w keeps its first w-2 characters, a tilde and its last
// character (`displace~t`; the 24-wide variable label `This variable label is~e` of vs-stata bank
// 66877d9d).
std::string abbrevName(const std::string& s, size_t w) {
  if (cps(s) <= w) return s;
  if (w <= 3) return firstChars(s, w);
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
  }
  return firstChars(s, w - 2) + "~" + s.substr(starts.back());
}

// The label cell of a factor level or an interaction atom: too long for its column it keeps the
// head and ends in `..` (`Average c..`).
std::string dotAbbrev(const std::string& s, size_t w) {
  if (cps(s) <= w) return s;
  if (w <= 2) return firstChars(s, w);
  return firstChars(s, w - 2) + "..";
}

// ---- the options -----------------------------------------------------------------------------
struct StatFmt {
  bool on = false;
  std::optional<std::string> fmt;
};

struct EtOpts {
  StatFmt b, se, t, p;
  std::optional<std::string> stfmt;
  bool star = false;
  std::optional<std::string> starList;
  bool eform = false;
  std::optional<int> varwidth, modelwidth;
  bool varlabel = false;
  std::optional<std::string> title;
  bool newpanel = false;
  bool allbase = false, base = false, noomitted = false, noemptycells = false;
  bool nofvlabel = false;
  bool nounderscore = false;
  std::vector<std::string> stats;
  std::optional<std::string> keep, drop;
  std::optional<std::string> equations;
};

struct OptSpec {
  const char* name;
  size_t minLen;
};

// est_table's syntax lines: the capitals of each option name are its minimum abbreviation.
const OptSpec kEtOptSpecs[] = {
    {"b", 1},          {"bfmt", 2},      {"se", 2},        {"sefmt", 3},     {"t", 1},
    {"tfmt", 2},       {"p", 1},         {"pfmt", 2},      {"stfmt", 3},     {"star", 4},
    {"eform", 5},      {"varwidth", 3},  {"modelwidth", 5}, {"varlabel", 4}, {"style", 3},
    {"title", 2},      {"newpanel", 3},  {"markdown", 5},  {"eqlabel", 5},   {"plabel", 4},
    {"nounderscore", 7}, {"stats", 5},   {"statistics", 4}, {"keep", 1},     {"drop", 1},
    {"coded", 3},      {"equations", 2}, {"grlabel", 5},   {"nofvnotes", 4}, {"withci", 6},
    {"allbaselevels", 7}, {"baselevels", 4}, {"noomitted", 6}, {"omitted", 4},
    {"noemptycells", 7}, {"emptycells", 5}, {"fvlabel", 5}, {"nofvlabel", 7}, {"vsquish", 7},
    {"nolstretch", 10}, {"cformat", 7},    {"pformat", 7}, {"sformat", 7},   {"fvwrap", 6},
    {"fvwrapon", 8},
};

// The spelling of `typed` among est_table's options, or empty.
std::string resolveEtOpt(const std::string& typedIn) {
  std::string typed = typedIn;
  for (auto& c : typed) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& sp : kEtOptSpecs) {
    const std::string full = sp.name;
    if (typed.size() >= sp.minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0) {
      return full;
    }
  }
  return std::string();
}

std::vector<std::string> splitTokens(const std::string& raw) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw + " ") {
    if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  return out;
}

// Reads the options; an error block for what est_table refuses.
std::optional<ojson> readEtOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                EtOpts* o) {
  auto bad = [](const std::string& msg, int rc) { return std::optional<ojson>(table_detail::tblErrorRc(msg, rc)); };
  auto statFmt = [&](StatFmt* sf, const eval::OptValue& v) {
    sf->on = true;
    if (!v.isBool && !v.strValue.empty()) sf->fmt = table_detail::dequote(v.strValue);
  };
  bool starGiven = false, starListGiven = false;
  for (const auto& [k, v] : opts) {
    const std::string name = resolveEtOpt(k);
    if (name.empty()) {
      // B4 P1: `unsupported_option`, which eval::coarseRc maps to r(198) -- the code the standing
      // rule reserves for an option est_table does not take (`st(N)` is r(198) in StataBE,
      // oracle_p.log:176-180).
      return std::optional<ojson>(
          ojson(eval::errorBlock("unsupported_option", "option `" + k + "` not allowed")));
    }
    const std::string arg = v.isBool ? std::string() : table_detail::dequote(v.strValue);
    if (name == "b" || name == "bfmt") statFmt(&o->b, v);
    else if (name == "se" || name == "sefmt") statFmt(&o->se, v);
    else if (name == "t" || name == "tfmt") statFmt(&o->t, v);
    else if (name == "p" || name == "pfmt") statFmt(&o->p, v);
    else if (name == "stfmt") {
      if (v.isBool) return bad("option stfmt() incorrectly specified", 198);
      o->stfmt = arg;
    } else if (name == "star") {
      if (v.isBool) starGiven = true;
      else {
        starListGiven = true;
        o->starList = arg;
      }
    } else if (name == "eform") o->eform = true;
    else if (name == "varwidth" || name == "modelwidth") {
      static const std::regex isInt(R"(^\s*[+-]?\d+\s*$)");
      if (v.isBool || !std::regex_match(arg, isInt)) {
        return bad("option " + name + "() incorrectly specified", 198);
      }
      (name == "varwidth" ? o->varwidth : o->modelwidth) = std::stoi(arg);
    } else if (name == "varlabel") o->varlabel = true;
    else if (name == "title") {
      if (v.isBool) return bad("option title() incorrectly specified", 198);
      o->title = arg;
    } else if (name == "newpanel") o->newpanel = true;
    else if (name == "allbaselevels") o->allbase = true;
    else if (name == "baselevels") o->base = true;
    else if (name == "noomitted") o->noomitted = true;
    else if (name == "noemptycells") o->noemptycells = true;
    else if (name == "nounderscore") o->nounderscore = true;
    else if (name == "nounderscore") o->nounderscore = true;
    else if (name == "nofvlabel") o->nofvlabel = true;
    else if (name == "fvlabel") o->nofvlabel = false;
    else if (name == "omitted" || name == "emptycells" || name == "vsquish" || name == "nolstretch") {
      // the defaults / spacing of _ms_display: no effect on this layout
    } else if (name == "stats" || name == "statistics") {
      if (v.isBool) return bad("option " + name + "() incorrectly specified", 198);
      for (const auto& s : splitTokens(arg)) {
        if (std::find(o->stats.begin(), o->stats.end(), s) == o->stats.end()) o->stats.push_back(s);
      }
    } else if (name == "keep") o->keep = arg;
    else if (name == "drop") o->drop = arg;
    else if (name == "equations") o->equations = arg;
    else {
      return std::optional<ojson>(ojson(eval::unsupportedOption(
          name, "estimates table: option " + name + "() is not implemented in this version of PIET")));
    }
  }
  if (starGiven && starListGiven) return bad("options star and star() may not be combined", 198);
  o->star = starGiven || starListGiven;
  if (o->star && (o->se.on || o->t.on || o->p.on)) {
    return bad("option star may not be combined with se, t, or p", 198);
  }
  return std::nullopt;
}

// Stata's missing-value kinds as est_table keeps them in its coefficient matrix.
enum class CellKind { kZ, kBase, kEmpty, kOmit, kNum };

struct EtCell {
  CellKind kind = CellKind::kZ;
  double b = std::nan("");
  double var = std::nan("");
};

// `<level><b|n|o>.<var>` -> level, var; the other atoms (`c.x`, `x`, `co.x`) carry no level.
struct Atom {
  bool factor = false;
  std::string level, var, text;
};

Atom parseAtom(const std::string& a) {
  Atom at;
  at.text = a;
  const size_t dot = a.find('.');
  if (dot == std::string::npos || dot == 0) {
    at.var = a;
    return at;
  }
  const std::string head = a.substr(0, dot);
  size_t i = 0;
  while (i < head.size() && (std::isdigit(static_cast<unsigned char>(head[i])) || head[i] == '-')) ++i;
  bool marks = i > 0 && head.size() - i <= 2;
  for (size_t j = i; marks && j < head.size(); ++j) marks = head[j] == 'b' || head[j] == 'n' || head[j] == 'o';
  if (marks) {
    at.factor = true;
    at.level = head.substr(0, i);
    at.var = a.substr(dot + 1);
  } else {
    at.var = a.substr(dot + 1);
  }
  return at;
}

std::vector<Atom> splitAtoms(const std::string& name) {
  std::vector<Atom> out;
  size_t a = 0;
  while (a <= name.size()) {
    size_t h = name.find('#', a);
    if (h == std::string::npos) h = name.size();
    out.push_back(parseAtom(name.substr(a, h - a)));
    a = h + 1;
  }
  return out;
}

// One term of the stub (`_ms_display`): the header lines above it, and its own label.
struct StubTerm {
  std::vector<std::string> header;  // ralign lines
  std::string key;                  // terms with the same key share one header
  std::string label;
  bool indented = false;
};

}  // namespace

ojson runEstimatesTable(const std::vector<std::string>& modelNamesIn,
                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                        SessionState& session) {
  EtOpts o;
  if (auto err = readEtOpts(opts, &o)) return *err;

  // W2 esttabnames (BUG-552): a written namelist goes through the one
  // shared expansion: `A_*`, `_all`, `.`; a
  // token matching nothing is r(111) "estimation result NAME not found".
  // With no namelist Stata tables `.`, the active results alone, whatever
  // is stored (StataBE 19.5: `estimates store m1`, a second fit, `estimates table`
  // -> one "Active" column), and with no active results that is r(301) "last
  // estimates (.) not found" (BUG-1248; it was rc 1, "no stored estimates").
  SessionState::EstNamelist ex = session.expandEstimatesNamelist(
      modelNamesIn.empty() ? std::vector<std::string>{"."} : modelNamesIn);
  if (ex.rc != 0) return estNamelistError(ex);
  // est_table: `local names : list uniq names` -- a model named twice is tabulated once
  std::vector<std::string> names;
  for (const auto& n : ex.names) {
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  // The active, unstored results (`.`): snapshotted locally, as a read must not
  // store anything. std::deque so the pointers taken below stay valid.
  std::deque<ojson> liveSnaps;
  if (names.empty()) {  // `_all` over an empty store
    return engineErrorBlockEt("estimates table: no stored estimates. Use `estimates store NAME` "
                              "after fitting a model.");
  }

  // Resolve every requested model FIRST: reporting a missing name only after
  // half a table has been printed would leave the user reading a partial
  // comparison as if it were complete.
  std::vector<const ojson*> snaps;
  for (const auto& n : names) {
    if (n == ".") {
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt(
            "estimates table: the active estimation results (.) carry no coefficient table "
            "this version of PIET can render; store them first (`estimates store NAME`)");
      }
      ojson snap = makeSnapshot(*session.lastEstBlock);
      snap["escalars"] = escalarsFrom(*session.lastEstBlock);
      mergeLiveEScalars(snap["escalars"], session);
      liveSnaps.push_back(std::move(snap));
      snaps.push_back(&liveSnaps.back());
      continue;
    }
    const ojson* m = session.estimatesGet(n);
    if (m == nullptr) {
      std::string avail;
      for (const auto& a : session.estimatesNames()) { if (!avail.empty()) avail += ", "; avail += a; }
      return engineErrorBlockEt("estimates table: model `" + n + "` not found in estimates store. "
                                "Available: " + (avail.empty() ? std::string("(none)") : avail));
    }
    snaps.push_back(m);
  }

  // est_table.ado Mkemat: results without e(b) and e(V) (an estpost posting)
  // are refused, r(321) -- `estimates table` never tabulates a summary posting
  // as coefficients.
  for (size_t i = 0; i < snaps.size(); ++i) {
    const ojson& snap = *snaps[i];
    if (snap.contains("posts_e_b") && snap["posts_e_b"].is_boolean() &&
        !snap["posts_e_b"].get<bool>()) {
      const std::string who = names[i] == "." ? std::string("current") : names[i];
      return ojson(eval::errorBlock("engine_error",
                                    who + " estimation results do not have e(b) and e(V)",
                                    std::nullopt, std::nullopt, std::nullopt,
                                    std::string("r(321)")));
    }
  }

  // equations(): a spec that numbers each model's equations. Single-equation models have just the
  // one, so only a spec that names that first equation (a plain `1`, as in `equations(1)`) is
  // decided here; anything else would re-pair the equations of a model that has several.
  if (o.equations.has_value()) {
    bool single = true;
    for (size_t i = 0; i < snaps.size() && single; ++i) {
      const ojson& sn = *snaps[i];
      single = !(sn.contains("outcomes") && sn["outcomes"].is_array() && sn["outcomes"].size() > 1);
    }
    bool plain = !splitTokens(*o.equations).empty();
    for (const auto& tkn : splitTokens(*o.equations)) {
      if (tkn != "1") plain = false;
    }
    if (!single || !plain) {
      return ojson(eval::unsupportedOption(
          "equations", "estimates table: equations() other than equations(1) on single-equation "
                       "models is not implemented in this version of PIET"));
    }
  }

  // The coefficients, as estout reads them: e(b)'s stripe with its base levels and omitted columns.
  const size_t M = snaps.size();
  std::vector<table_detail::Model> ms(M);
  std::vector<EoModel> ems;
  ems.reserve(M);
  for (size_t i = 0; i < M; ++i) {
    ms[i] = table_detail::materializeModel(session, names[i], *snaps[i]);
  }
  for (size_t i = 0; i < M; ++i) {
    ems.push_back(table_detail::eoModelFrom(ms[i], names[i], session, nullptr, i,
                                            /*dropOmitted=*/false, /*withBase=*/true));
    ems.back().model = &ms[i];
  }
  // est_table Mkemat: a model whose e(b) has no coefficient is `no coefficients found in NAME`.
  for (size_t i = 0; i < M; ++i) {
    if (ems[i].coefRows.empty()) {
      return ojson(eval::errorBlock("engine_error", "no coefficients found in " + names[i],
                                    std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
    }
  }
  // equations(1) on single-equation models: the one equation of each is named `#1` (est_table
  // AdjustRowEq), so their rows meet under it and r(coef) names them `#1:mpg` (vs-stata bank
  // e6c325d4)
  if (o.equations.has_value()) {
    for (auto& em : ems) {
      std::vector<EoKey> rows;
      std::map<EoKey, EoCoefVals> vals;
      std::map<EoKey, std::string> marked;
      for (const auto& k : em.coefRows) rows.emplace_back("#1", k.second);
      for (const auto& [k, v] : em.vals) vals[EoKey{"#1", k.second}] = v;
      for (const auto& [k, v] : em.ebMarked) marked[EoKey{"#1", k.second}] = v;
      em.coefRows = std::move(rows);
      em.vals = std::move(vals);
      em.ebMarked = std::move(marked);
    }
  }
  std::vector<EoKey> B;
  for (const auto& em : ems) table_detail::eoCapp(&B, em.coefRows);

  // A model's cell for a row: the number, or what est_table's matrix holds for a base level
  // (.b), an omitted column (.o) and a row the model lacks (.z).
  auto cellOf = [&](size_t i, const EoKey& k) {
    EtCell c;
    const EoModel& em = ems[i];
    auto v = em.vals.find(k);
    const bool has = v != em.vals.end() &&
                     std::find(em.coefRows.begin(), em.coefRows.end(), k) != em.coefRows.end();
    if (!has) return c;
    const EoCoefVals& cv = v->second;
    auto mk = em.ebMarked.find(k);
    if (cv.zeroVar && cv.b == 0.0 && mk != em.ebMarked.end()) {
      c.kind = mk->second.find("b.") != std::string::npos ? CellKind::kBase : CellKind::kOmit;
      return c;
    }
    c.kind = CellKind::kNum;
    c.b = cv.b;
    c.var = cv.zeroVar ? 0.0 : (std::isnan(cv.se) ? cv.se : cv.se * cv.se);
    return c;
  };

  // ---- keep() / drop() (est_table Keep, Drop) -------------------------------------------------
  // A name given as `x` matches the row of that name; `i.x` the rows of every level of x; `#.x` that
  // level; a name with `*` or `?` every row it matches. A name no row has is r(198).
  const std::vector<std::string> rowNames = table_detail::eoStripeNames(B);
  auto plainName = [](const std::string& stripe) {
    // the display spelling of a stripe name: level marks (`1b.`, `2n.`, `0bn.`) read as the level
    std::string s;
    std::vector<Atom> at = splitAtoms(stripe);
    for (size_t i = 0; i < at.size(); ++i) {
      if (i) s += "#";
      s += at[i].text;
    }
    return s;
  };
  auto specRows = [&](const std::string& spec, std::vector<size_t>* out) {
    // exact spellings first: the stripe name, then the name without its marks
    std::string eqPart, term = spec;
    if (const size_t colon = spec.find(':'); colon != std::string::npos) {
      eqPart = spec.substr(0, colon);
      term = spec.substr(colon + 1);
    }
    const bool glob = term.find_first_of("*?") != std::string::npos;
    const bool iForm = term.size() > 2 && term[0] == 'i' && term[1] == '.';
    for (size_t r = 0; r < B.size(); ++r) {
      if (!eqPart.empty()) {
        const std::string eq = B[r].first == "__" ? std::string("_") : B[r].first;
        if (eq != eqPart) continue;
      }
      const std::string st = rowNames[r];
      const std::string pl = plainName(st);
      // the same name with its level marks removed (`1bn.rep78` -> `1.rep78`)
      std::string unmarked;
      {
        std::vector<Atom> at = splitAtoms(st);
        for (size_t i = 0; i < at.size(); ++i) {
          if (i) unmarked += "#";
          if (at[i].factor) unmarked += at[i].level + "." + at[i].var;
          else unmarked += at[i].text;
        }
      }
      bool hit = false;
      if (glob) {
        hit = piet::strutil::globMatch(term, st) || piet::strutil::globMatch(term, unmarked) ||
              piet::strutil::globMatch(term, B[r].second);
      } else if (iForm) {
        const std::vector<Atom> at = splitAtoms(st);
        hit = at.size() == 1 && at[0].factor && at[0].var == term.substr(2);
      } else {
        hit = term == st || term == unmarked || term == pl || term == B[r].second;
      }
      if (hit) out->push_back(r);
    }
    return !out->empty();
  };
  std::vector<size_t> sel(B.size());
  for (size_t r = 0; r < B.size(); ++r) sel[r] = r;
  if (o.keep.has_value()) {
    std::vector<size_t> kept;
    for (const auto& spec : splitTokens(*o.keep)) {
      std::vector<size_t> rows;
      if (!specRows(spec, &rows)) {
        return ojson(eval::errorBlock("engine_error",
                                      "coefficient " + spec + " does not occur in any of the models",
                                      std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
      }
      const bool eqSpec = spec.find(':') != std::string::npos;
      // est_table Keep takes the row rownumb() finds -- the first one -- unless the whole
      // equation is named; wildcards and i.x forms select all they match.
      const bool many = spec.find_first_of("*?") != std::string::npos ||
                        (spec.size() > 2 && spec[0] == 'i' && spec[1] == '.');
      if (many || eqSpec) kept.insert(kept.end(), rows.begin(), rows.end());
      else kept.push_back(rows.front());
    }
    sel = kept;
  }
  if (o.drop.has_value()) {
    for (const auto& spec : splitTokens(*o.drop)) {
      std::vector<size_t> rows;
      if (!specRows(spec, &rows)) {
        return ojson(eval::errorBlock("engine_error",
                                      "coefficient " + spec + " does not occur in any of the models",
                                      std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
      }
      std::vector<size_t> rest;
      for (size_t r : sel) {
        if (std::find(rows.begin(), rows.end(), r) == rows.end()) rest.push_back(r);
      }
      sel = rest;
    }
  }
  std::vector<EoKey> shown;
  for (size_t r : sel) shown.push_back(B[r]);

  // ---- formats and widths (Display) -------------------------------------------------------------
  bool hasOmit = false;
  for (const auto& k : shown) {
    for (size_t i = 0; i < M; ++i) {
      if (cellOf(i, k).kind == CellKind::kOmit) hasOmit = true;
    }
  }
  size_t vw = std::max<size_t>(8, static_cast<size_t>(o.varwidth.value_or(o.varlabel ? 24 : 12)));
  size_t cw = hasOmit ? 9 : 7;
  size_t len = 0;
  auto invalidFmt = [&](const std::string& f) {
    return ojson(eval::errorBlock("engine_error", "invalid display format " + f, std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(120)")));
  };
  std::string bfmt = "%10.0g";
  if (o.b.fmt.has_value()) {
    if (!chkFormat(*o.b.fmt, 1.0, &len)) return invalidFmt(*o.b.fmt);
    bfmt = *o.b.fmt;
    cw = std::max(cw, len);
  } else {
    cw = 10;
  }
  std::string sefmt = bfmt, tfmt = "%7.2f", pfmt = "%7.4f";
  if (o.se.fmt.has_value()) {
    if (!chkFormat(*o.se.fmt, 1.0, &len)) return invalidFmt(*o.se.fmt);
    sefmt = *o.se.fmt;
    cw = std::max(cw, len);
  }
  if (o.t.fmt.has_value()) {
    if (!chkFormat(*o.t.fmt, -123.45, &len)) return invalidFmt(*o.t.fmt);
    tfmt = *o.t.fmt;
    cw = std::max(cw, len);
  }
  if (o.p.fmt.has_value()) {
    if (!chkFormat(*o.p.fmt, 1.0, &len)) return invalidFmt(*o.p.fmt);
    pfmt = *o.p.fmt;
    cw = std::max(cw, len);
  }
  std::string stfmt = bfmt;
  if (!o.stats.empty() && o.stfmt.has_value()) {
    if (!chkFormat(*o.stfmt, 1.0, &len)) return invalidFmt(*o.stfmt);
    stfmt = *o.stfmt;
    cw = std::max(cw, len);
  }
  // star / star(): the p-value cut-offs, smallest first
  double pcut[3] = {0.001, 0.01, 0.05};
  size_t lsym = 0;
  std::string legend;
  if (o.star) {
    if (o.starList.has_value()) {
      std::vector<double> v;
      for (const auto& tkn : splitTokens(*o.starList)) {
        char* end = nullptr;
        const double d = std::strtod(tkn.c_str(), &end);
        if (end == nullptr || *end != '\0') {
          return table_detail::tblErrorRc("invalid specification of star()", 198);
        }
        v.push_back(d);
      }
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
      if (v.size() != 3 || v[0] <= 0 || v[2] >= 1) {
        return table_detail::tblErrorRc("invalid specification of star()", 198);
      }
      for (int i = 0; i < 3; ++i) pcut[i] = v[static_cast<size_t>(i)];
    }
    auto g = [](double x) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.15g", x);
      return std::string(buf);
    };
    // the cut-offs as typed (`.1 .05 .01`), else the defaults
    std::string c1 = "0.05", c2 = "0.01", c3 = "0.001";
    if (o.starList.has_value()) {
      std::vector<std::pair<double, std::string>> t;
      for (const auto& tkn : splitTokens(*o.starList)) t.emplace_back(std::strtod(tkn.c_str(), nullptr), tkn);
      std::sort(t.begin(), t.end());
      t.erase(std::unique(t.begin(), t.end(), [](const auto& a, const auto& b2) { return a.first == b2.first; }),
              t.end());
      c3 = t[0].second;
      c2 = t[1].second;
      c1 = t[2].second;
    }
    (void)g;
    legend = "* p<" + c1 + "; ** p<" + c2 + "; *** p<" + c3;
    lsym = 3;
  }
  if (o.modelwidth.has_value()) cw = std::max(cw, static_cast<size_t>(std::max(0, *o.modelwidth)));
  const size_t cwT = cw + lsym;

  // ---- the stub ---------------------------------------------------------------------------------
  // The equation rows sit under: regress's `_` (shown under no header) and, for the ML
  // estimators, the dependent variable's name.
  auto eqName = [](const EoKey& k) { return k.first == "__" ? std::string("_") : k.first; };
  std::vector<std::string> uniqEq;
  for (const auto& k : shown) {
    const std::string e = eqName(k);
    if (std::find(uniqEq.begin(), uniqEq.end(), e) == uniqEq.end()) uniqEq.push_back(e);
  }
  const bool grouped = uniqEq.size() > 1;

  auto fvLabel = [&](const std::string& var, const std::string& level) {
    if (o.nofvlabel) return level;
    auto vl = session.valueLabels.find(var);
    if (vl == session.valueLabels.end()) return level;
    try {
      const int code = std::stoi(level);
      auto it = vl->second.find(code);
      return it != vl->second.end() ? it->second : level;
    } catch (...) {
      return level;
    }
  };
  auto varLab = [&](const std::string& var) {
    bool ex = false;
    return table_detail::eoVarLabel(session, var, &ex);
  };

  // The stub of one row: header lines (shared by consecutive rows of one term), the row's label.
  auto stubOf = [&](const std::string& stripe) {
    StubTerm st;
    const std::vector<Atom> at = splitAtoms(stripe);
    size_t nFactor = 0;
    for (const auto& a : at) nFactor += a.factor ? 1 : 0;
    if (at.size() == 1 && !at[0].factor) {
      // an ordinary variable (or `c.x`, the constant)
      std::string nm = at[0].text;
      if (o.varlabel) {
        if (nm == "_cons") nm = "Constant";
        else if (nm.find('.') == std::string::npos) {
          const std::string l = varLab(nm);
          if (!l.empty()) nm = l;
        }
      }
      st.label = abbrevName(nm, vw);
      return st;
    }
    if (nFactor == 1) {
      // `L.v` or `L.v#c.x`: the header holds the variable (and the factor's continuous partners,
      // one line each), the row the level
      std::string var, level;
      std::vector<std::string> before, after;
      bool seen = false;
      for (const auto& a : at) {
        if (a.factor && !seen) {
          var = a.var;
          level = a.level;
          seen = true;
        } else if (!seen) {
          before.push_back(a.text);
        } else {
          after.push_back(a.text);
        }
      }
      std::string head = o.varlabel && !varLab(var).empty() ? varLab(var) : var;
      st.key = stripe.substr(stripe.find('.') == std::string::npos ? 0 : 0);
      std::vector<std::string> lines;
      std::string tail;
      for (const auto& a : before) tail += a + "#";
      std::string headText = tail + head;
      std::string whole = headText;
      for (const auto& a : after) whole += "#" + a;
      if (cps(whole) <= vw) {
        lines.push_back(whole);
      } else {
        if (!after.empty()) headText += "#";
        lines.push_back(headText);
        for (size_t i = 0; i < after.size(); ++i) lines.push_back(after[i] + (i + 1 < after.size() ? "#" : ""));
      }
      st.header = lines;
      st.key.clear();
      for (const auto& l : lines) st.key += l + "|";
      st.label = dotAbbrev(fvLabel(var, level), vw - 1);
      st.indented = true;
      return st;
    }
    // several factors, or an interaction of continuous variables: the name as it is
    st.label = abbrevName(plainName(stripe), vw);
    return st;
  };

  // ---- the statistics (r(stats) and the footer) ------------------------------------------------
  const size_t nStat = o.stats.size();
  std::vector<std::vector<double>> statVals(nStat, std::vector<double>(M, std::nan("")));
  std::vector<std::vector<bool>> statHas(nStat, std::vector<bool>(M, false));
  for (size_t s = 0; s < nStat; ++s) {
    for (size_t i = 0; i < M; ++i) {
      const table_detail::EoV v = table_detail::eoStatValue(ems[i], o.stats[s], &session);
      if (v.kind == table_detail::EoV::kNum || v.kind == table_detail::EoV::kMissing) {
        statHas[s][i] = true;
        statVals[s][i] = v.kind == table_detail::EoV::kNum ? v.v : std::nan("");
      }
    }
  }

  // ---- the p-values of star ---------------------------------------------------------------------
  auto dfOf = [&](size_t i) {
    const ojson& e = ms[i].escalars;
    if (e.contains("df_r") && e["df_r"].is_number()) return e["df_r"].get<double>();
    return std::nan("");
  };
  auto pOf = [&](double b, double var, double df) {
    const double z = std::fabs(b) / std::sqrt(var);
    char buf[160];
    if (std::isnan(df)) std::snprintf(buf, sizeof(buf), "2*(1-normal(%.17g))", z);
    else std::snprintf(buf, sizeof(buf), "2*ttail(%.17g,%.17g)", df, z);
    const auto r = piet::evalScalarExpr(buf, session);
    return r.has_value() ? *r : std::nan("");
  };

  // ---- the cells ---------------------------------------------------------------------------------
  // b, se, t, p of one model's cell; the text est_table prints (blank for .z).
  auto bText = [&](const EtCell& c) -> std::string {
    switch (c.kind) {
      case CellKind::kZ: return std::string();
      case CellKind::kBase: return "(base)";
      case CellKind::kEmpty: return "(empty)";
      case CellKind::kOmit: return "(omitted)";
      case CellKind::kNum: break;
    }
    return fmtCell(o.eform ? std::exp(c.b) : c.b, bfmt);
  };
  auto otherText = [&](const EtCell& c, char which, double df) -> std::string {
    if (c.kind != CellKind::kNum) return std::string();
    switch (which) {
      case 's': {
        const double se = std::sqrt(c.var);
        return fmtCell(o.eform ? se * std::exp(c.b) : se, sefmt);
      }
      case 't': return fmtCell(c.b / std::sqrt(c.var), tfmt);
      default: return fmtCell(pOf(c.b, c.var, df), pfmt);
    }
  };

  // ---- the layout --------------------------------------------------------------------------------
  const size_t labelW = vw + 1;        // the variable column and the blank before the bar
  const size_t cellW = cwT + 3;        // one model: ` ` + cw + the symbols + two blanks
  const std::string blankLabelRow = std::string(labelW, ' ') + "|";
  size_t linesize = 80;
  if (const auto cv = settingCValue("linesize", session)) {
    if (!cv->isString) {
      try {
        linesize = static_cast<size_t>(std::stoi(cv->text));
      } catch (...) {
      }
    }
  }
  const size_t hcol1 = vw + 1;
  const size_t hcolr = cwT + 3;
  const size_t nModBlock = std::max<size_t>(1, (linesize - 1 - hcol1) / hcolr);

  std::vector<std::string> lines;
  if (o.title.has_value() && !o.title->empty()) {
    // `{p}` wraps the title at the line width
    std::string cur;
    for (const auto& w : splitTokens(*o.title)) {
      if (cur.empty()) cur = w;
      else if (cps(cur) + 1 + cps(w) <= linesize) cur += " " + w;
      else {
        lines.push_back(cur);
        cur = w;
      }
    }
    if (!cur.empty()) lines.push_back(cur);
    lines.push_back(std::string());
  }

  auto colLabel = [&](size_t i) { return names[i] == "." ? std::string("Active") : names[i]; };
  // the rows that survive est_table's Check4Continue: a row whose every cell is .z, a base level
  // (unless baselevels / allbaselevels was typed), an empty cell under noemptycells, an omitted
  // one under noomitted is not printed
  auto rowVisible = [&](const EoKey& k) {
    const bool baseShown = o.allbase || o.base;
    bool any = false;
    for (size_t i = 0; i < M; ++i) {
      const EtCell c = cellOf(i, k);
      if (c.kind == CellKind::kZ) continue;
      if (c.kind == CellKind::kBase && !baseShown) continue;
      if (c.kind == CellKind::kEmpty && o.noemptycells) continue;
      if (c.kind == CellKind::kOmit && o.noomitted) continue;
      any = true;
    }
    return any;
  };

  // one block of models m1..m2 (inclusive, 0-based)
  auto block = [&](size_t m1, size_t m2) {
    const size_t n = m2 - m1 + 1;
    // header: a plain rule, the model names, the rule with the bar
    auto plainRule = [&]() { return std::string(labelW + 1 + n * cellW, '-'); };
    auto splitRule = [&]() { return std::string(labelW, '-') + "+" + std::string(n * cellW, '-'); };
    lines.push_back(plainRule());
    {
      std::string h = padLeft("Variable", labelW - 1) + " |";
      for (size_t i = m1; i <= m2; ++i) {
        h += " " + centerIn(abbrevName(colLabel(i), cwT), cwT) + "  ";
      }
      lines.push_back(h);
      lines.push_back(splitRule());
    }
    // coefficient panel
    std::string openHeader;
    bool inFactorGroup = false;
    bool firstRow = true;
    for (size_t g = 0; g < uniqEq.size(); ++g) {
      const std::string& eq = uniqEq[g];
      if (grouped) {
        if (g > 0) lines.push_back(splitRule());
        if (!(eq == "_" && o.nounderscore)) lines.push_back(padRight(eq, labelW) + "|");
        openHeader.clear();
        inFactorGroup = false;
        firstRow = true;
      }
      for (const auto& k : shown) {
        if (eqName(k) != eq) continue;
        if (!rowVisible(k)) continue;
        // the stub of the stripe name
        std::string stripe;
        for (size_t r = 0; r < B.size(); ++r) {
          if (B[r] == k) stripe = rowNames[r];
        }
        if (const size_t colon = stripe.find(':'); colon != std::string::npos &&
                                                   (stripe.substr(0, colon) == k.first || k.first == "__")) {
          stripe = stripe.substr(colon + 1);
        }
        const StubTerm st = stubOf(stripe);
        if (!st.header.empty()) {
          if (st.key != openHeader) {
            if (!firstRow) lines.push_back(blankLabelRow);
            for (const auto& hl : st.header) {
              // a line that ends in `#` carries the bar right behind it
              lines.push_back(!hl.empty() && hl.back() == '#' ? padLeft(hl, labelW) + "|"
                                                              : padLeft(hl, labelW - 1) + " |");
            }
            openHeader = st.key;
            inFactorGroup = true;
          }
        } else if (inFactorGroup) {
          lines.push_back(blankLabelRow);
          openHeader.clear();
          inFactorGroup = false;
        }
        firstRow = false;
        const std::string stubCell = st.indented ? padLeft(st.label, labelW - 2) + "  |"
                                                  : padLeft(st.label, labelW - 1) + " |";
        std::string bRow = stubCell;
        for (size_t i = m1; i <= m2; ++i) {
          const EtCell c = cellOf(i, k);
          const std::string f = bText(c);
          std::string fs;
          if (o.star && c.kind == CellKind::kNum) {
            const double p = pOf(c.b, c.var, dfOf(i));
            if (p < pcut[0]) fs = "***";
            else if (p < pcut[1]) fs = "**";
            else if (p < pcut[2]) fs = "*";
          }
          bRow += " " + padLeft(f, cw) + (o.star ? padRight(fs, lsym) : std::string()) + "  ";
        }
        lines.push_back(bRow);
        auto extra = [&](char which) {
          std::string row = std::string(labelW, ' ') + "|";
          for (size_t i = m1; i <= m2; ++i) {
            row += " " + padLeft(otherText(cellOf(i, k), which, dfOf(i)), cw) + "  ";
          }
          lines.push_back(row);
        };
        if (o.se.on) extra('s');
        if (o.t.on) extra('t');
        if (o.p.on) extra('p');
      }
    }
    // statistics panel
    if (nStat > 0) {
      if (o.newpanel) {
        lines.push_back(plainRule());
        lines.push_back(std::string());
        lines.push_back(plainRule());
        std::string h = padLeft("Statistics", labelW - 1) + " |";
        for (size_t i = m1; i <= m2; ++i) h += " " + centerIn(abbrevName(colLabel(i), cwT), cwT) + "  ";
        lines.push_back(h);
        lines.push_back(splitRule());
      } else {
        lines.push_back(splitRule());
        if (grouped) {
          lines.push_back(padRight("Statistics", labelW) + "|" + std::string(n * cellW, ' '));
        }
      }
      for (size_t s = 0; s < nStat; ++s) {
        const std::string ufmt = o.stats[s] == "N" ? "%" + std::to_string(cw) + ".0f" : stfmt;
        std::string row = padLeft(o.stats[s], labelW - 1) + " |";
        for (size_t i = m1; i <= m2; ++i) {
          const std::string f = statHas[s][i] && !std::isnan(statVals[s][i]) ? fmtCell(statVals[s][i], ufmt)
                                                                              : std::string();
          row += " " + padLeft(f, cw) + (o.star ? std::string(lsym, ' ') : std::string()) + "  ";
        }
        lines.push_back(row);
      }
    }
    lines.push_back(plainRule());
    const size_t twidth = 1 + hcol1 + n * hcolr;
    if (o.star) {
      lines.push_back(padLeft("Legend: " + legend, twidth));
    } else if (o.se.on || o.t.on || o.p.on) {
      std::string lg = "b";
      if (o.se.on) lg += "/se";
      if (o.t.on) lg += "/t";
      if (o.p.on) lg += "/p";
      lines.push_back(padLeft("Legend: " + lg, twidth));
    }
  };
  for (size_t m1 = 0; m1 < M; m1 += nModBlock) {
    block(m1, std::min(M - 1, m1 + nModBlock - 1));
    if (m1 + nModBlock < M) lines.push_back(std::string());
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  table_detail::estTablePostR(session, names, ems, shown, o.stats);
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}


// ---------------------------------------------------------------------------
// `estimates stats [namelist] [, n(#)]` (estimates.ado Stats): one row per model with
// N = n() (else e(N_ic), else e(N)), e(ll_0), e(ll), df = e(df_ic) (else e(rank), else the
// rank of e(V)), AIC = -2 ll + 2 df and BIC = -2 ll + ln(N) df; models without a likelihood
// print `.` for it and for both criteria.
// ---------------------------------------------------------------------------
ojson runEstimatesStats(const std::vector<std::string>& modelNamesIn,
                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                        SessionState& session) {
  std::optional<double> nOpt;
  for (const auto& [k, v] : opts) {
    if (k == "n") {
      char* end = nullptr;
      const std::string t = v.isBool ? std::string() : table_detail::dequote(v.strValue);
      const double d = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(d > 0)) {
        return table_detail::tblErrorRc("option n() incorrectly specified", 198);
      }
      nOpt = d;
    } else {
      return ojson(eval::errorBlock("unsupported_option", "option `" + k + "` not allowed"));
    }
  }
  SessionState::EstNamelist ex = session.expandEstimatesNamelist(
      modelNamesIn.empty() ? std::vector<std::string>{"."} : modelNamesIn);
  if (ex.rc != 0) return estNamelistError(ex);
  const std::vector<std::string> names = std::move(ex.names);
  if (names.empty()) return engineErrorBlockEt("estimates stats: no stored estimates.");
  std::deque<ojson> liveSnaps;
  std::vector<const ojson*> snaps;
  for (const auto& n : names) {
    if (n == ".") {
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt("estimates stats: no active estimation results");
      }
      ojson snap = makeSnapshot(*session.lastEstBlock);
      snap["escalars"] = escalarsFrom(*session.lastEstBlock);
      mergeLiveEScalars(snap["escalars"], session);
      liveSnaps.push_back(std::move(snap));
      snaps.push_back(&liveSnaps.back());
      continue;
    }
    const ojson* m = session.estimatesGet(n);
    if (m == nullptr) {
      return engineErrorBlockEt("estimates stats: model `" + n + "` not found in estimates store.");
    }
    snaps.push_back(m);
  }
  const size_t M = snaps.size();
  std::vector<table_detail::Model> ms(M);
  std::vector<EoModel> ems;
  ems.reserve(M);
  for (size_t i = 0; i < M; ++i) ms[i] = table_detail::materializeModel(session, names[i], *snaps[i]);
  for (size_t i = 0; i < M; ++i) {
    ems.push_back(table_detail::eoModelFrom(ms[i], names[i], session, nullptr, i, false, true));
    ems.back().model = &ms[i];
  }
  const double nan = std::nan("");
  auto scalar = [&](size_t i, const char* k) {
    const table_detail::EoV v = table_detail::eoStatValue(ems[i], k, &session);
    return v.kind == table_detail::EoV::kNum ? v.v : nan;
  };
  eval::EvalMatrix S;
  S.colnames = {"N", "ll0", "ll", "df", "AIC", "BIC"};
  std::vector<std::vector<double>> rows;
  for (size_t i = 0; i < M; ++i) {
    double n = nOpt.value_or(nan);
    if (std::isnan(n)) n = scalar(i, "N_ic");
    if (std::isnan(n)) n = scalar(i, "N");
    const double ll0 = scalar(i, "ll_0"), ll = scalar(i, "ll");
    double df = scalar(i, "df_ic");
    if (std::isnan(df)) df = scalar(i, "rank");
    double aic = nan, bic = nan;
    if (!std::isnan(ll) && !std::isnan(df)) {
      aic = -2.0 * ll + 2.0 * df;
      if (!std::isnan(n)) bic = -2.0 * ll + std::log(n) * df;
    }
    rows.push_back({n, ll0, ll, df, aic, bic});
    S.rownames.push_back(names[i] == "." ? std::string(".") : names[i]);
  }
  S.data = rows;
  auto g7 = [](double v) {
    if (std::isnan(v)) return std::string(".");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.7g", v);
    std::string t(buf);
    if (t.rfind("0.", 0) == 0) t.erase(0, 1);
    else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
    return t;
  };
  std::string text;
  text += "\nAkaike's information criterion and Bayesian information criterion\n\n";
  text += std::string(77, '-') + "\n";
  text += "       Model |          N   ll(null)  ll(model)      df        AIC        BIC\n";
  text += std::string(13, '-') + "+" + std::string(63, '-') + "\n";
  for (size_t i = 0; i < M; ++i) {
    text += padLeft(names[i], 12) + " |";
    const int widths[6] = {11, 11, 11, 8, 11, 11};
    for (int c = 0; c < 6; ++c) text += padLeft(g7(rows[i][static_cast<size_t>(c)]), static_cast<size_t>(widths[c]));
    text += "\n";
  }
  text += std::string(77, '-');
  if (!nOpt.has_value()) text += "\nNote: BIC uses N = number of observations. See [R] IC note.";
  eval::replaceRClass(session.eval);
  session.eval.rClassCmd = "estimates";
  session.eval.rMatrices.emplace_back("S", std::move(S));
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

}  // namespace piet::handlers
