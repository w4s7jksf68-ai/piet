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
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "results/stored_estimates_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

// ---------------------------------------------------------------------------
// `estimates table [NAME...] [, b se]` — Stata's own model-comparison table.
//
// Layout is pinned to real Stata 19.5 (P4 answerbook,
// internal capture p4_stata.log): a 13-character label
// field ending in the `|` rule, then 13 characters per model. The numeric cell
// is Stata's %9.0g, whose real rule — measured off that log rather than assumed
// — is "as many significant digits as fit in 9 characters once the leading zero
// of a |x|<1 value is dropped"; a minus sign is allowed to overflow that width
// (the log's -.19581818 is 10 characters). estFmt9 below implements exactly that.
namespace {

std::string estFmt9(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  for (int prec = 9; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    std::string s(buf);
    // Stata prints .5, never 0.5 — and the dropped zero is what buys the digit
    // that makes the 9-character budget work out.
    if (s.rfind("0.", 0) == 0) s.erase(0, 1);
    else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
    const size_t visible = s.size() - (s[0] == '-' ? 1 : 0);
    if (visible <= 9) return s;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return std::string(buf);
}

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string padLeft(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

std::string padRight(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}

std::string centerIn(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  if (n >= w) return s;
  // Odd leftovers go to the LEFT (W:P8: a 6-character model name in the
  // 11-wide header cell is `   p8_ols  `, not `  p8_ols   `). P4's names are
  // 5 characters, which splits evenly and so never discriminated the two
  // roundings — its header stays byte-identical either way.
  const size_t left = (w - n + 1) / 2;
  return std::string(left, ' ') + s + std::string(w - n - left, ' ');
}

// W:P8. A coefficient named `<level>.<var>` is one level of factor variable
// `<var>`; Stata's `estimates table` breaks those out under an indented
// variable header instead of listing `2.region` as a flat row name. Returns
// false for everything else, including interactions (`2.region#c.x1`), which
// stay ordinary rows rather than being half-grouped on a guess.
bool splitFactorLevel(const std::string& name, std::string& var, std::string& level) {
  const size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return false;
  for (size_t i = 0; i < dot; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  const std::string rest = name.substr(dot + 1);
  // A variable name: bytes >= 0x80 are name bytes (core/ident.h), so `2.قيمة` splits.
  if (!cecon::ident::isIdentStartByte(rest[0])) return false;
  for (char c : rest) {
    if (!cecon::ident::isIdentByte(c)) return false;
  }
  var = rest;
  level = name.substr(0, dot);
  return true;
}

// W:P8. The equation label a model's rows sit under. Measured on StataBE 19.5
// rather than assumed: `regress`'s e(b) columns are bare (`mpg`, `_cons`) and
// print under `_`, while every ML single-equation estimator probed (logit,
// probit, poisson) names its equation after the dependent variable
// (`hi:mpg`, `hi:_cons`) and prints under `hi`. Two `regress` models with
// DIFFERENT depvars still share `_` and therefore still print flat — the
// discriminating case, checked explicitly, and the reason this keys on the
// estimator family rather than on the depvar alone.
std::string equationLabel(const ojson& snap) {
  std::string estimator = snap.value("estimator", std::string());
  for (auto& c : estimator) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (estimator == "feols" || estimator == "ols") return "_";
  const std::string depvar = snap.value("depvar", std::string());
  return depvar.empty() ? "_" : depvar;
}

}  // namespace

ojson runEstimatesTable(const std::vector<std::string>& modelNamesIn,
                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                        SessionState& session) {
  // W:P8. `se` ADDS the standard-error row to the coefficient; it does not
  // replace it — live-verified, `estimates table M1 M2, se` prints b and se
  // and the b/se legend exactly as `, b se` does. The previous reading
  // ("b or se") printed an SE-only table for `, se`, which is a wrong table
  // at rc 0. The coefficient is therefore always shown and `wantSe` is the
  // only real switch.
  bool wantSe = false;
  std::vector<std::string> statKeys;
  for (const auto& [k, v] : opts) {
    if (k == "b" && v.isBool && v.boolValue) continue;
    if (k == "se" && v.isBool && v.boolValue) { wantSe = true; continue; }
    if (k == "stats" && !v.isBool) {
      std::string cur;
      for (char c : v.strValue + " ") {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
          if (!cur.empty()) { statKeys.push_back(cur); cur.clear(); }
        } else {
          cur += c;
        }
      }
      continue;
    }
    // B4 P1: `unsupported_option`, which eval::coarseRc maps to r(198) — the
    // code the standing rule reserves for an option this build will not take.
    // It defaulted to `engine_error`, i.e. rc 1, so `estimates table m1, st(N)`
    // — r(198) in StataBE (oracle_p.log:176-180) — refused under a code no
    // Stata command ever answers with, and `capture` saw 1 instead of 198.
    return engineErrorBlockEt("estimates table: option `" + k + "` is not supported in this "
                              "version of PIET (only `b`, `se` and `stats()` are wired)",
                              "unsupported_option");
  }
  for (const auto& s : statKeys) {
    if (s != "N" && s != "r2") {
      return engineErrorBlockEt("estimates table: stats(" + s + ") is not supported in this "
                                "version of PIET (only `N` and `r2` are wired)");
    }
  }

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
  const std::vector<std::string> names = std::move(ex.names);
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
  std::vector<const ojson*> models;
  for (const auto& n : names) {
    if (n == ".") {
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt(
            "estimates table: the active estimation results (.) carry no coefficient table "
            "this version of PIET can render; store them first (`estimates store NAME`)");
      }
      liveSnaps.push_back(makeSnapshot(*session.lastEstBlock));
      models.push_back(&liveSnaps.back());
      continue;
    }
    const ojson* m = session.estimatesGet(n);
    if (m == nullptr) {
      std::string avail;
      for (const auto& a : session.estimatesNames()) { if (!avail.empty()) avail += ", "; avail += a; }
      return engineErrorBlockEt("estimates table: model `" + n + "` not found in estimates store. "
                                "Available: " + (avail.empty() ? std::string("(none)") : avail));
    }
    models.push_back(m);
  }

  // Each model's rows sit under its equation label; models sharing one label
  // share rows. When every model lands on the same label the table is flat and
  // no equation header is printed at all (P4's two-`regress` case, and the
  // reason that pin stays byte-unchanged through this rewrite).
  std::vector<std::string> modelEq(models.size());
  std::vector<std::string> eqOrder;
  for (size_t i = 0; i < models.size(); ++i) {
    modelEq[i] = equationLabel(*models[i]);
    if (std::find(eqOrder.begin(), eqOrder.end(), modelEq[i]) == eqOrder.end()) {
      eqOrder.push_back(modelEq[i]);
    }
  }
  const bool grouped = eqOrder.size() > 1;

  // Coefficient rows = union across models, in first-seen order (Stata keeps
  // the first model's order and appends anything new from later ones), kept
  // per equation so a grouped table never leaks one equation's names into
  // another's block.
  std::map<std::string, std::vector<std::string>> coefOrderByEq;
  std::vector<std::map<std::string, std::pair<double, double>>> cells(models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    std::vector<std::string>& order = coefOrderByEq[modelEq[i]];
    for (const auto& row : models[i]->value("coeftable", ojson::array())) {
      const std::string nm = row.value("name", std::string());
      if (nm.empty()) continue;
      if (std::find(order.begin(), order.end(), nm) == order.end()) order.push_back(nm);
      const double b = row.contains("b") && !row.at("b").is_null() ? row.at("b").get<double>()
                                                                   : std::nan("");
      const double se = row.contains("se") && !row.at("se").is_null() ? row.at("se").get<double>()
                                                                      : std::nan("");
      cells[i][nm] = {b, se};
    }
  }

  // Stata puts the intercept LAST, whatever the engine's own coeftable order is
  // (PIET's leads with it). Reordering here rather than at the producer keeps
  // every other consumer of that coeftable byte-unchanged.
  for (auto& [eq, order] : coefOrderByEq) {
    (void)eq;
    auto isCons = [](const std::string& s) { return s == "_cons" || s == "(intercept)"; };
    std::stable_partition(order.begin(), order.end(),
                          [&](const std::string& s) { return !isCons(s); });
  }

  const size_t labelW = 13;   // 12-wide right-aligned name + one space, then '|'
  const size_t cellW = 13;    // 11-wide right-aligned value + two spaces
  const size_t totalW = labelW + 1 + cellW * models.size();  // +1 = the '|' rule itself
  const std::string ruleFull(totalW, '-');
  const std::string ruleSplit = std::string(labelW, '-') + "+" +
                                std::string(totalW - labelW - 1, '-');
  // A structural row (equation header, factor header, in-group blank) stops at
  // the '|' with no trailing cell padding — measured, not stylistic.
  const std::string blankLabelRow = std::string(labelW, ' ') + "|";

  std::vector<std::string> lines;
  lines.push_back(ruleFull);
  {
    std::string h = padLeft("Variable", labelW - 1) + " |";
    // The unstored active results head their column "Active" (StataBE 19.5,
    // `estimates table` after a fit).
    for (const auto& n : names)
      h += centerIn(n == "." ? std::string("Active") : n, cellW - 2) + "  ";
    lines.push_back(h);
  }
  lines.push_back(ruleSplit);

  // One coefficient's b (and optionally se) row, cells blank for any model
  // that never estimated it.
  auto emitCoefRows = [&](const std::string& cn, const std::string& eq,
                          const std::string& labelCell) {
    std::string bRow = labelCell;
    std::string seRow = blankLabelRow.substr(0, labelW - 1) + " |";
    for (size_t i = 0; i < models.size(); ++i) {
      auto it = cells[i].find(cn);
      const bool present = it != cells[i].end() && modelEq[i] == eq;
      // A model that never estimated this coefficient gets a BLANK cell, not 0.
      bRow += present ? padLeft(estFmt9(it->second.first), cellW - 2) + "  "
                      : std::string(cellW, ' ');
      seRow += present ? padLeft(estFmt9(it->second.second), cellW - 2) + "  "
                       : std::string(cellW, ' ');
    }
    lines.push_back(bRow);
    if (wantSe) lines.push_back(seRow);
  };

  for (size_t g = 0; g < eqOrder.size(); ++g) {
    const std::string& eq = eqOrder[g];
    if (grouped) {
      if (g > 0) lines.push_back(ruleSplit);
      lines.push_back(padRight(eq, labelW) + "|");
    }
    const std::vector<std::string>& order = coefOrderByEq[eq];
    std::string openFactor;
    for (size_t r = 0; r < order.size(); ++r) {
      const std::string& cn = order[r];
      std::string fvar, flevel;
      const bool isLevel = splitFactorLevel(cn, fvar, flevel);
      if (isLevel && fvar != openFactor) {
        lines.push_back(blankLabelRow);
        lines.push_back(padLeft(fvar, labelW - 1) + " |");
        openFactor = fvar;
      } else if (!isLevel && !openFactor.empty()) {
        lines.push_back(blankLabelRow);
        openFactor.clear();
      }
      // A level row indents the level under its variable header; every other
      // row right-aligns its own name in the label field.
      const std::string labelCell = isLevel ? padLeft(flevel, labelW - 2) + "  |"
                                            : padLeft(cn, labelW - 1) + " |";
      emitCoefRows(cn, eq, labelCell);
    }
  }

  if (!statKeys.empty()) {
    lines.push_back(ruleSplit);
    lines.push_back(padRight("Statistics", labelW) + "|" +
                    std::string(cellW * models.size(), ' '));
    for (const auto& s : statKeys) {
      std::string row = padLeft(s, labelW - 1) + " |";
      for (size_t i = 0; i < models.size(); ++i) {
        const ojson& snap = *models[i];
        const char* key = (s == "N") ? "n_obs" : "r2";
        const bool present = snap.contains(key) && snap.at(key).is_number();
        row += present ? padLeft(estFmt9(snap.at(key).get<double>()), cellW - 2) + "  "
                       : std::string(cellW, ' ');
      }
      lines.push_back(row);
    }
  }

  lines.push_back(ruleFull);
  // The legend names the per-cell statistics, so it appears only when there is
  // more than one of them — `, b` alone prints no legend (live-verified).
  if (wantSe) lines.push_back(padLeft("Legend: b/se", totalW));

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

}  // namespace piet::handlers
