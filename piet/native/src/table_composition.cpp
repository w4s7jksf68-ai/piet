// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table composition.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// Split `name(inner)` into its two halves. Returns false when the parentheses
// do not balance, which is a syntax error rather than a name.
bool splitCall(const std::string& item, std::string* head, std::string* inner) {
  const size_t open = item.find('(');
  if (open == std::string::npos) {
    *head = item;
    inner->clear();
    return true;
  }
  if (item.empty() || item.back() != ')') return false;
  *head = item.substr(0, open);
  *inner = item.substr(open + 1, item.size() - open - 2);
  return true;
}


bool parseCells(const std::string& raw, std::vector<CellSpec>* out, std::string* errOut,
                bool* sideBySideOut ) {
  const std::string trimmed = strutil::trim(raw);
  const std::string spec = dequote(trimmed);
  // The tokenizer hands the option's argument over VERBATIM (tokenizer.cpp
  // parseOpts takes the text between the parentheses and only strips
  // whitespace), so the quotes are still here to be read -- and dequote, one
  // line down, is what used to erase them before anything could.
  if (sideBySideOut != nullptr) *sideBySideOut = (spec.size() != trimmed.size());
  if (spec.find('&') != std::string::npos) {
    *errOut =
        "estout: cells() side-by-side notation `&` is not implemented in this build; put each "
        "statistic on its own row instead";
    return false;
  }
  // The grouping notation is a bracket at the TOP LEVEL -- `cells([b se])`.
  // A bracket INSIDE a suboption is something else entirely: `par([ ])` names
  // the two delimiters, and that is what the corpus writes. Measured on the 90
  // packages: `par([` appears 327 times across 20 distinct spellings, and a
  // top-level `cells([` appears ZERO times. The depth-blind test this replaces
  // refused all 327 to guard against none of the 0, and the arm that caught it
  // is one of those lines verbatim (w1b_tables_pins.log c7,
  // `cells(se(par([ ]) fmt(2)))`, rc 0 in Stata).
  {
    int depth = 0;
    for (char ch : spec) {
      if (ch == '(') ++depth;
      else if (ch == ')') { if (depth > 0) --depth; }
      else if ((ch == '[' || ch == ']') && depth == 0) {
        *errOut = "estout: cells() grouping notation `[ ]` is not implemented in this build";
        return false;
      }
    }
  }
  for (const auto& item : splitTopLevel(spec)) {
    std::string head, inner;
    if (!splitCall(item, &head, &inner)) {
      *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
      return false;
    }
    if (head.find(':') != std::string::npos) {
      *errOut = "estout: cells() equation notation `" + head + "` is not implemented in this build";
      return false;
    }
    if (head == "pvalue") {
      *errOut = "estout: cells(pvalue()) is not implemented in this build; use `p`";
      return false;
    }
    CellSpec cs;
    cs.name = head;
    for (const auto& sub : splitTopLevel(inner)) {
      std::string sh, si;
      if (!splitCall(sub, &sh, &si)) {
        *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
        return false;
      }
      if (sh == "star") {
        cs.star = true;
      } else if (sh == "par") {
        cs.par = true;
        std::vector<std::string> lr = splitTopLevel(si);
        if (lr.size() == 1) {
          // `par([ ])` -- the two delimiters are LITERAL TEXT, and `[`/`]` are
          // not nesting here, but splitTopLevel counts them as depth and hands
          // the pair back as one token. Re-split on plain whitespace when that
          // happens, and only accept it when it yields exactly two. This is the
          // corpus's own spelling: `par([` appears 327 times across 20 distinct
          // lines, against zero uses of the top-level grouping the depth rule
          // exists for.
          const std::vector<std::string> ws = splitWs(si);
          if (ws.size() == 2) lr = ws;
        }
        if (lr.size() == 2) {
          cs.parL = dequote(lr[0]);
          cs.parR = dequote(lr[1]);
        } else if (!si.empty() && lr.size() != 0) {
          *errOut = "estout: par() takes either no argument or exactly two delimiters";
          return false;
        }
      } else if (sh == "fmt") {
        bool ok = false;
        cs.fmt = normalizeFmt(si, &ok);
        if (!ok) {
          // `fmt(a3)` is Stata's adaptive-width format. Rendering it as three
          // fixed decimals would be a different number of digits than asked
          // for, so it is refused rather than approximated (decision 4).
          *errOut = "estout: fmt(" + si + ") is not implemented in this build; use fmt(#) or a "
                                          "%fmt such as fmt(%9.3f)";
          return false;
        }
        cs.fmtGiven = true;
      } else if (sh == "label") {
        cs.label = dequote(si);
      } else if (!sh.empty()) {
        *errOut = "estout: cells() sub-option `" + sh + "` is not implemented in this build";
        return false;
      }
    }
    out->push_back(cs);
  }
  if (out->empty()) {
    *errOut = "estout: cells() is empty";
    return false;
  }
  return true;
}


// One body row per (coefficient x cell-spec). A model that does not estimate a
// given coefficient gets a BLANK cell, never a zero -- a mixed-estimator table
// is the plan's own edge case and a zero there is a claim of no effect.
void buildBody(Table* t, const std::vector<Model>& models, const BodyOpts& bo,
               SessionState& session, std::string* errOut ) {
  std::vector<std::string> names = coefNames(models, bo.keep, bo.drop, bo.order, errOut);
  if (bo.consLast) {
    std::vector<std::string> reordered;
    bool sawCons = false;
    for (const auto& n : names) {
      if (n == "_cons") {
        sawCons = true;
      } else {
        reordered.push_back(n);
      }
    }
    if (sawCons) reordered.push_back("_cons");
    names = reordered;
  }
  if (bo.nocons) {
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) {
                                 return n == "_cons" || n == "(intercept)";
                               }),
                names.end());
  }
  // One (model, coefficient, spec) cell. Shared by both layouts so the two can
  // never disagree about a NUMBER -- only about where it sits.
  auto cellFor = [&](const Model& m, const std::string& coef, const CellSpec& cs,
                     const ojson* r) -> std::optional<std::string> {
    if (r == nullptr) return std::nullopt;
    ojson v;
    if (cs.name == "b" || cs.name == "coef") {
      v = r->contains("b") ? (*r)["b"] : ojson(nullptr);
    } else if (cs.name == "se" || cs.name == "stderr") {
      v = r->contains("se") ? (*r)["se"] : ojson(nullptr);
    } else if (cs.name == "t") {
      v = r->contains("t") ? (*r)["t"] : ojson(nullptr);
    } else if (cs.name == "p") {
      v = r->contains("p") ? (*r)["p"] : ojson(nullptr);
    } else if (m.added["matrices"].contains(cs.name)) {
      const ojson& mat = m.added["matrices"][cs.name];
      if (mat.contains("colnames") && mat.contains("data") && mat["data"].is_array() &&
          !mat["data"].empty()) {
        const ojson& cn = mat["colnames"];
        for (size_t j = 0; j < cn.size(); ++j) {
          if (cn[j].is_string() && cn[j].get<std::string>() == coef) {
            if (j < mat["data"][0].size()) v = mat["data"][0][j];
            break;
          }
        }
      }
    }
    if (v.is_null()) return std::nullopt;
    std::string cell;
    if (cs.autoD >= 0) {
      cell = (v.is_number() && !std::isnan(v.get<double>()))
                 ? sigDigits(v.get<double>(), cs.autoD, /*minDec=*/0, /*comma=*/true)
                 : std::string(kMissingCell);
    } else {
      cell = fmtJson(v, cs.fmt);
    }
    if (cs.star && r->contains("p")) cell += starsFor((*r)["p"], bo.stars);
    if (cs.par) cell = cs.parL + cell + cs.parR;
    return cell;
  };

  // BUG-487, the WIDE layout: one row per coefficient, k*n cells, model-major.
  if (bo.sideBySide) {
    for (const auto& coef : names) {
      TRow row;
      row.kind = TRow::kCoef;
      row.label = labelFor(coef, bo.labels, bo.useVarLabels, session);
      if (coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      bool anything = false;
      for (const auto& m : models) {
        const ojson* r = coefRow(m.snap, coef);
        for (const auto& cs : bo.cells) {
          auto c = cellFor(m, coef, cs, r);
          row.cells.push_back(c.value_or(std::string()));
          if (c.has_value() && !c->empty()) anything = true;
        }
      }
      if (anything) t->body.push_back(row);
    }
    return;
  }

  for (const auto& coef : names) {
    for (size_t ci = 0; ci < bo.cells.size(); ++ci) {
      const CellSpec& cs = bo.cells[ci];
      TRow row;
      row.kind = (ci == 0) ? TRow::kCoef : TRow::kSe;
      // Only the FIRST cell spec is labelled: `x / (0.045)` is one coefficient
      // shown two ways, not two coefficients.
      row.label = (ci == 0) ? labelFor(coef, bo.labels, bo.useVarLabels, session)
                            : (cs.label.empty() ? std::string() : cs.label);
      if (ci == 0 && coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      for (const auto& m : models) {
        auto c = cellFor(m, coef, cs, coefRow(m.snap, coef));
        row.cells.push_back(c.value_or(std::string()));
      }
      // A row where every model is blank carries nothing.
      bool anything = false;
      for (const auto& c : row.cells) {
        if (!c.empty()) anything = true;
      }
      if (anything) t->body.push_back(row);
    }
  }
}


// The footer: `stats(N r2 ...)` in the order given. A key that is an estadd
// LOCAL renders as text ("Controls Yes"), which is decision 2's whole point.
void buildFoot(Table* t, const std::vector<Model>& models, const std::vector<std::string>& stats,
               const std::string& fmt, size_t stride ) {
  for (const auto& key : stats) {
    TRow row;
    row.kind = TRow::kStat;
    // BUG-486, axis 7. estout labels a stats() row with the KEY AS TYPED --
    // `r2`, not `R-squared` (w1b_estout_defaults.log g3: `r2 .2933891`). The
    // pretty label belongs to `stats(..., labels(...))`, which the caller has
    // not given. `statLabel` stays for that path and for esttab, which builds
    // its own footer and whose 09-09 capture verified those labels.
    row.label = key;
    bool anyText = false;
    for (const auto& m : models) {
      bool isText = false;
      // ...and `N` follows the SAME running format as everything else: `74`
      // under %9.0g (g3), `74.000` under fmt(3) (g4). The `%9.0f` special case
      // here was a guess and the capture refutes it.
      row.cells.push_back(statCell(m, key, fmt, &isText));
      if (isText) anyText = true;
      // BUG-487: under the wide layout a model owns `stride` columns and the
      // statistic sits in the FIRST of them, the rest blank -- `N  74    74  `
      // against a four-column body (w1b_estout_cells.log h7).
      for (size_t pad = 1; pad < stride; ++pad) row.cells.emplace_back();
    }
    if (anyText) row.kind = TRow::kText;
    t->foot.push_back(row);
  }
}


// Column heads: `mlabels()`/`mtitles()` if given, else the model's depvar (what
// esttab prints), else the stored name.
// `nameHeads` picks which side of the model a derived head comes from. estout
// heads its columns with the STORED NAME (`m1`, `m2` -- e2.txt/e_plain.txt row
// 1); esttab heads them with the dependent variable. This build derived the
// depvar for both, which is BUG-486's second axis.
void buildHeads(Table* t, const std::vector<Model>& models,
                const std::optional<std::string>& mlabels, bool numbers, bool heads,
                bool nameHeads ) {
  if (numbers) {
    for (size_t i = 0; i < models.size(); ++i) {
      t->colNums.push_back("(" + std::to_string(i + 1) + ")");
    }
  }
  if (!heads) return;
  std::vector<std::string> labels;
  if (mlabels.has_value()) {
    // SPLIT FIRST, DEQUOTE EACH TOKEN -- the rule `parseLabelPairs` states and the
    // fourth site in this tree to have broken it. `mlabels("Model One" "Two")`
    // dequoted FIRST becomes `Model One" "Two`, whose remaining quotes make the
    // splitter's quote tracking swallow the separator, so the two labels came out
    // as `Model` and `One" "Two` (w1b_estout_tex.log r8).
    labels = splitTopLevel(*mlabels);
    for (auto& l : labels) l = dequote(l);
  }
  for (size_t i = 0; i < models.size(); ++i) {
    if (i < labels.size()) {
      t->colHeads.push_back(labels[i]);
      continue;
    }
    if (nameHeads) {
      t->colHeads.push_back(models[i].name);
      continue;
    }
    const std::string dv = models[i].snap.value("depvar", std::string());
    t->colHeads.push_back(dv.empty() ? models[i].name : dv);
  }
}
}
