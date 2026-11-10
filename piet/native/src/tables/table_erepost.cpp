// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table erepost.
#include "tables_internal.h"
#include "piet/handlers/estimates_tables.h"
namespace piet::handlers {
using namespace table_detail;


// ===========================================================================
// erepost
// ===========================================================================

ojson runErepost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabErepost.known,
                                            kVocabErepost.unimplemented)) {
    return ojson(*err);
  }
  if (!session.lastEstBlock.has_value()) {
    return tblErrorRc("erepost: no estimation result in memory. Run a regression first.", 301);
  }
  std::string args = argsAfterHead(parsed.raw);
  if (args.empty()) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");

  // `erepost b = M` / `erepost V = M`. Both spellings are accepted; only `b` is
  // implemented, and `V` is refused BY NAME rather than accepted-and-ignored,
  // because a table built from a reposted b with the OLD V would report
  // standard errors that belong to different coefficients.
  const size_t eq = args.find('=');
  if (eq == std::string::npos) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");
  const std::string lhs = strutil::trim(args.substr(0, eq));
  const std::string rhs = strutil::trim(args.substr(eq + 1));
  if (lhs == "V" || lhs == "v") {
    return ojson(eval::unsupportedCommand("erepost V=", std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (lhs != "b") return tblSyntax("erepost: only `erepost b = MATRIX' is supported");

  const eval::EvalMatrix* mat = session.eval.findMatrix(rhs);
  if (mat == nullptr) return tblError("matrix " + rhs + " not found");
  if (mat->data.empty()) return tblError("erepost: matrix " + rhs + " is empty");

  // Stata's e(b) is 1 x k. A k x 1 column vector is not read as the same vector:
  // erepost posts M with `ereturn post`, which takes a row vector, so it is r(503)
  // whatever its row names (StataBE 19.5, BUG-1494: `matrix Zc = (7\8\9)`, named
  // `x1 x2 _cons` or not, after `regress y x1 x2`). Anything with two real
  // dimensions says so with both shapes named.
  std::vector<double> vals;
  std::vector<std::string> names;
  if (mat->data.size() == 1) {
    vals = mat->data[0];
    names = mat->colnames;
  } else if (mat->data[0].size() == 1) {
    return tblErrorRc("conformability error", 503);
  } else {
    return tblError("erepost: " + rhs + " is " + std::to_string(mat->data.size()) + "x" +
                    std::to_string(mat->data[0].size()) + " but e(b) is a vector");
  }

  ojson block = *session.lastEstBlock;
  if (!SessionState::blockPostsEb(block) || !session.lastModel().has_value()) {
    // eb-order T-P3: the live e() has no e(b) (an estpost posting), or one with
    // no fitted model behind it (a first erepost on such a posting), so erepost
    // posts M as the new e(b) under M's own names -- erepost.ado `eret post `b''
    // with no e(V) to carry (w2v_estpost_ematrix.log ep14e_colnames_eb `c1 c2`,
    // ep14e_local_bc1 7, ep14e_local_bx1 r(111); a second repost takes M2's
    // names, ep14e2_colnames_eb `x1 x2`, ep14e2_local_bx1 7).
    // The posting's macros and other matrices stay (ep14e_ecmd).
    ojson coeftable = ojson::array();
    for (size_t i = 0; i < vals.size(); ++i) {
      ojson row;
      row["name"] = i < names.size() && !names[i].empty() ? names[i] : ("c" + std::to_string(i + 1));
      row["b"] = std::isnan(vals[i]) ? ojson(nullptr) : ojson(vals[i]);
      row["se"] = nullptr;
      row["t"] = nullptr;
      row["p"] = nullptr;
      coeftable.push_back(row);
    }
    block["coeftable"] = coeftable;
    block.erase("posts_e_b");
    // eb-order ROUND 3: the rows ARE the posted M (names, order, values), so
    // the e(b) resolver reads them as posted, not through a command's stripe
    // row or its omitted-column guard (a reposted 0 has no se).
    block["reposted_b"] = true;
    session.lastEstBlock = block;
    // BUG-1202: a fit that posted e(b) as a matrix (glm, oaxaca, teffects) is
    // read from that matrix by `matrix b = e(b)` and el(e(b),i,j), so it becomes
    // M too (StataBE: el(e(b),1,1) and _b[x1] are both M's 4 after glm).
    for (auto& p : session.eval.eMatrices) {
      if (p.first != "b") continue;
      eval::EvalMatrix nb;
      nb.data = {vals};
      nb.rownames = p.second.rownames.empty() ? std::vector<std::string>{"y1"} : p.second.rownames;
      for (size_t i = 0; i < vals.size(); ++i)
        nb.colnames.push_back(i < names.size() && !names[i].empty() ? names[i]
                                                                   : "c" + std::to_string(i + 1));
      p.second = std::move(nb);
    }
    return displayBlock("(e(b) reposted from " + rhs + "; e(V) is not reposted)");
  }
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) {
    return tblError("erepost: the last estimation has no coefficient vector");
  }
  const size_t k = block["coeftable"].size();
  // BUG-1203: Stata's e(b) after tobit/ologit/nbreg (eval::ebHasUnsentColumns)
  // has ancillary columns the table lacks, so an M no wider than the table is
  // Stata's conformability error (StataBE: a 3-column M after `tobit yc x1 x2`
  // is r(503) and _b is unchanged); a wider one cannot be laid over a table
  // without those columns, and says so.
  if (eval::ebHasUnsentColumns(session.eval)) {
    if (vals.size() <= k) return tblErrorRc("conformability error", 503);
    return tblErrorRc("erepost: e(b) after this estimation has ancillary columns this version "
                      "of PIET does not hold, so it cannot be reposted",
                      198);
  }
  // BUG-1202: glm, oaxaca and teffects post e(b) as a matrix, which `matrix b =
  // e(b)` and el(e(b),i,j) read ahead of the table. That matrix is Stata's e(b):
  // M is conformable with it, the repost writes it, and each table row (what
  // _b[] reads) takes the value of its column by name.
  eval::EvalMatrix* postedB = nullptr;
  for (auto& p : session.eval.eMatrices)
    if (p.first == "b") postedB = &p.second;
  if (postedB != nullptr && (postedB->data.size() != 1 ||
                             postedB->colnames.size() != postedB->data[0].size()))
    postedB = nullptr;
  if (vals.size() != (postedB != nullptr ? postedB->data[0].size() : k))
    return tblErrorRc("conformability error", 503);  // StataBE: a 1x2 M after a 3-column fit

  // M is positional over e(b) as Stata lists it -- the canonical order, `_cons`
  // last -- not over the engine's coeftable, which puts `(intercept)` first:
  // after `regress y x1 x2`, `erepost b = (1,2,3)` gives `_b[x1]` 1 and
  // `_b[_cons]` 3 (w2g_ebmat.log e8_bx1 +1.0X+000, e8_bcons +1.8X+001).
  std::vector<size_t> posOf(k);
  for (size_t i = 0; i < k; ++i) posOf[i] = i;
  {
    std::string why;
    const std::optional<eval::EvalMatrix> canon = eval::canonicalEMatrix(block, 'b', &why);
    if (canon.has_value() && canon->colnames.size() == k) {
      for (size_t i = 0; i < k; ++i) {
        std::string nm = block["coeftable"][i].value("name", "");
        const size_t colon = nm.find(':');
        const std::string eq = colon == std::string::npos ? std::string() : nm.substr(0, colon + 1);
        std::string bare = colon == std::string::npos ? nm : nm.substr(colon + 1);
        if (bare == "(intercept)") bare = "_cons";
        const auto it = std::find(canon->colnames.begin(), canon->colnames.end(), eq + bare);
        if (it != canon->colnames.end())
          posOf[i] = static_cast<size_t>(it - canon->colnames.begin());
      }
    }
    // A repeated name maps two rows to one column: keep the engine's positions.
    std::vector<char> seen(k, 0);
    bool perm = true;
    for (size_t i = 0; i < k && perm; ++i) {
      if (seen[posOf[i]]) perm = false;
      seen[posOf[i]] = 1;
    }
    if (!perm)
      for (size_t i = 0; i < k; ++i) posOf[i] = i;
  }

  const bool rename = hasFlag(parsed.opts, "rename");
  // BUG-1494: erepost posts M together with the fit's own e(V) (`ereturn post b
  // V`), which takes M only under e(b)'s column names, in e(b)'s order: an
  // unnamed M (c1 c2 ...), other names, or e(b)'s names permuted is r(507) "name
  // conflict" with e(b) left alone, and an M narrower than e(b) (a factor fit's
  // base column) r(503). `rename` gives V M's names instead. StataBE 19.5,
  // scratch-laneA2/agents/rprog/x1494.do: case counts (`X1` is 507), an `o.`
  // name counts (`x1 x3 _cons` against `o.x1 x3 _cons` is 507), the default
  // equation `_:` is no equation (`_:x1 _:x2 _:_cons` posts), a row name does not
  // matter. e(b)'s names are read as `matrix Z = e(b)` copies them.
  if (!rename) {
    std::vector<std::string> ebNames;
    if (const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b"))
      ebNames = eb->colnames;
    else if (const std::optional<eval::EvalMatrix> canon = eval::canonicalEMatrix(block, 'b'))
      ebNames = canon->colnames;  // e(b) the resolver will not build (a renamed `_cons`)
    const auto stripeName = [](const std::vector<std::string>& nm, size_t i) {
      std::string s = i < nm.size() ? nm[i] : std::string();
      if (s.empty()) return "c" + std::to_string(i + 1);
      if (s.compare(0, 2, "_:") == 0) s.erase(0, 2);
      return s;
    };
    if (!ebNames.empty()) {
      if (ebNames.size() != vals.size()) return tblErrorRc("conformability error", 503);
      for (size_t i = 0; i < vals.size(); ++i)
        if (stripeName(names, i) != stripeName(ebNames, i)) return tblErrorRc("name conflict", 507);
    }
  }
  if (rename) {
    // The rows take M's names, so canonicalEMatrix can no longer find `_cons`
    // by name: lay the rows out in e(b)'s order first, so `: colnames e(b)`
    // lists M's names in M's order.
    ojson reordered = ojson::array();
    for (size_t p = 0; p < k; ++p) reordered.push_back(ojson());
    for (size_t i = 0; i < k; ++i) reordered[posOf[i]] = block["coeftable"][i];
    block["coeftable"] = reordered;
    for (size_t i = 0; i < k; ++i) posOf[i] = i;
  }
  // An omitted (collinear) column stays omitted after the repost -- Stata's e(b)
  // keeps its `o.` name (w2v_estripe_census.log esc_regress_omit `o.x1 x2 x3c
  // _cons`) -- but the b 0 / se 0 pair that marks it is about to be
  // overwritten, so record it on the row first for the e(b) resolver's
  // omitted-column guard (eval/macros.cpp resolveEMatrixFor G1). A row marked
  // by an earlier repost keeps its mark; a repost's own 0 is not an omission.
  if (!block.value("reposted_b", false)) {
    for (auto& row : block["coeftable"]) {
      if (!row.is_object()) continue;
      const auto bIt = row.find("b");
      const auto seIt = row.find("se");
      const bool bZero = bIt != row.end() && bIt->is_number() && bIt->get<double>() == 0.0;
      const bool seZero = seIt == row.end() || seIt->is_null() ||
                          (seIt->is_number() && seIt->get<double>() == 0.0);
      if (bZero && seZero) row["omitted"] = true;
    }
  }
  // Each row's column of M: its canonical position, or for a posted e(b) the
  // column of the same name (`_cons` for `(intercept)`, an equation prefix
  // the row does not carry ignored).
  std::vector<std::optional<size_t>> colOf(k);
  for (size_t i = 0; i < k; ++i) {
    if (postedB == nullptr) {
      colOf[i] = posOf[i];
      continue;
    }
    std::string nm = block["coeftable"][i].value("name", "");
    if (nm == "(intercept)") nm = "_cons";
    for (size_t j = 0; j < postedB->colnames.size() && !colOf[i]; ++j) {
      const std::string& c = postedB->colnames[j];
      const size_t colon = c.find(':');
      if (c == nm || (colon != std::string::npos && nm.find(':') == std::string::npos &&
                      c.compare(colon + 1, std::string::npos, nm) == 0))
        colOf[i] = j;
    }
  }
  for (size_t i = 0; i < k; ++i) {
    ojson& row = block["coeftable"][i];
    if (colOf[i]) {
      const size_t p = *colOf[i];
      row["b"] = std::isnan(vals[p]) ? ojson(nullptr) : ojson(vals[p]);
      if (rename && p < names.size() && !names[p].empty()) row["name"] = names[p];
    }
    // NO V was reposted, so se/t/p no longer belong to these coefficients.
    // They become NULL -- Stata's missing -- and NOT 0: a zero standard error
    // reads as an infinitely precise estimate, and a zero p as significance.
    row["se"] = nullptr;
    row["t"] = nullptr;
    row["p"] = nullptr;
    if (row.contains("ci_lo")) row["ci_lo"] = nullptr;
    if (row.contains("ci_hi")) row["ci_hi"] = nullptr;
  }
  // The posted e(b) as M (written below, after the LastModel refresh, which
  // reinstalls the fit's own e() stores).
  std::optional<eval::EvalMatrix> newPostedB;
  if (postedB != nullptr) {
    newPostedB = *postedB;
    newPostedB->data[0] = vals;
    if (rename)
      for (size_t j = 0; j < newPostedB->colnames.size() && j < names.size(); ++j)
        if (!names[j].empty()) newPostedB->colnames[j] = names[j];
  }
  block["reposted_b"] = true;  // eb-order ROUND 3: see the estpost branch above
  session.lastEstBlock = block;
  // `_b[x]` in both layers reads this block (eb-order T-P3,
  // EvalSession::coefSource), so the write above is all `_b[]` needs. LastModel
  // still caches the raw result separately, mirrored into
  // `eval.lastModelResult` for the fitted e-scalars, so the copy below keeps
  // that cache on the reposted coefficients (a copy keeps the fit serial, so
  // setLastModel leaves this block in place).
  if (session.lastModel().has_value()) {
    LastModel lm = *session.lastModel();
    if (lm.result.has_value()) {
      ojson res = *lm.result;
      if (res.contains("coeftable") && res["coeftable"].is_array() &&
          res["coeftable"].size() == k) {
        for (size_t i = 0; i < k; ++i) {
          res["coeftable"][i] = block["coeftable"][i];
        }
        lm.result = res;
        session.setLastModel(lm);
      }
    }
  }
  if (newPostedB.has_value()) {
    bool put = false;
    for (auto& p : session.eval.eMatrices) {
      if (p.first != "b") continue;
      p.second = *newPostedB;
      put = true;
    }
    if (!put) session.eval.eMatrices.emplace_back("b", std::move(*newPostedB));
  }
  return displayBlock("(e(b) reposted from " + rhs +
                      "; e(V) is not reposted, so e(se) is missing)");
}
}
