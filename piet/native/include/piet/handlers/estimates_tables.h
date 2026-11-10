// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: stored estimates, the esttab/estout family, transform().
// Implemented under piet/native/src/tables/; eststo and estimates save/use/dir/drop
// in src/results/.
#ifndef PIET_NATIVE_HANDLERS_ESTIMATES_TABLES_H
#define PIET_NATIVE_HANDLERS_ESTIMATES_TABLES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// ---- AER-SCOPE1 bundle E (TABLES, 2026-09-04) ------------------------------
// The `regress -> estadd -> eststo -> estout|esttab using f.tex` pipeline, plus
// the other table commands that write a FILE. Before this bundle `runEsttab`
// returned a JSON block and wrote nothing to disk, and `makeSnapshot` carried no
// e() scalar bag, so `estadd` had nowhere to attach.
//
// Two shared pieces keep the five renderers from becoming five copies of
// `\begin{tabular}`: one `Table` model built from the stored estimates, and
// three renderers over it (TeX / fixed-width / delimited). They live inside the
// TU; only the entry points are declared here.

// `estadd sub ...` -- attach a local, scalar or matrix to the ACTIVE e() (Stata
// semantics: estadd runs BEFORE eststo, and the corpus writes it that way 5,725
// times). Stored in SessionState::estAdded, keyed by a fingerprint of the active
// estimate so a later, different regression discards the bag instead of letting
// it bleed onto the wrong model.
nlohmann::ordered_json runEstadd(const eval::ParsedCommand& parsed, SessionState& session);
// `estpost summarize|tabstat ...` -- a pseudo-estimate whose e(mean)/e(sd)/...
// 1xk matrices are what `esttab, cells(mean)` then reads. Values come from the
// SAME summarize/tabstat cores the native commands use, never a second
// implementation of the statistics.
nlohmann::ordered_json runEstpost(const eval::ParsedCommand& parsed, SessionState& session);
// `estout [NAME...] using f [, style() cells() stats() ...]`.
nlohmann::ordered_json runEstout(const eval::ParsedCommand& parsed, SessionState& session);
// `outreg2 using f [, replace|append tex excel ctitle() addtext() addstat()]`.
// The `.txt` ledger is the source of truth for `append`, exactly as in the
// original: Stata's outreg2 appends across SESSIONS, so an in-memory ledger
// would answer differently on the second run of the same do-file.
nlohmann::ordered_json runOutreg2(const eval::ParsedCommand& parsed, SessionState& session);
// `texsave varlist using f.tex [, ...]` -- the DATASET as a LaTeX table.
nlohmann::ordered_json runTexsave(const eval::ParsedCommand& parsed, SessionState& session);
// `texdoc init|close` plus the `tex <text>` line verb that writes into the open
// file. Split in two because `tex` is its own registry row.
nlohmann::ordered_json runTexdoc(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session);
// `frmttable using f, statmat(M) tex ...` -- TeX only. The default output is
// RTF/.doc, which this build has no writer for and refuses by name rather than
// writing a differently-formatted file under the requested extension.
nlohmann::ordered_json runFrmttable(const eval::ParsedCommand& parsed, SessionState& session);
// `regsave [using f.dta]` / `parmest, saving(f.dta)` -- the estimates as a
// DATASET. Same builder, different column vocabulary.
nlohmann::ordered_json runRegsave(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runParmest(const eval::ParsedCommand& parsed, SessionState& session);
// `mat2txt, matrix(M) saving(f) [replace append title() format() note()]`.
nlohmann::ordered_json runMat2txt(const eval::ParsedCommand& parsed, SessionState& session);
// `erepost b = M [, rename]` -- overwrite the active e(b) from a matrix.
nlohmann::ordered_json runErepost(const eval::ParsedCommand& parsed, SessionState& session);
// The `using` half of `esttab`. Takes the block `runEsttab` already built and
// writes it, so the on-screen table and the file can never disagree: there is
// one renderer and the file is what it produced.
nlohmann::ordered_json writeEsttabFile(const nlohmann::ordered_json& esttabBlock,
                                        const std::string& path,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        SessionState& session);
// The factor base levels of a fit's coefficient table, as estout shows them
// (`0.clinic` 0 / (.)): [{var, level, name, before, after}], read from the
// data and the live e(sample). Empty when there is none or no data.
nlohmann::ordered_json factorBaseLevels(const nlohmann::ordered_json& coeftable,
                                        SessionState& session);
// The screen form of `esttab` (no `using`): the same translation and writer run
// without a file, so the options are validated exactly as the file form does
// (mode exclusivity, keep() patterns) and r() is posted. Null on success.
nlohmann::ordered_json esttabScreen(const nlohmann::ordered_json& esttabBlock,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// -- M4a item 3: stored estimates + tables (ROADMAP M4a) ---------------------

// `eststo [NAME]` / `est store [NAME]` / `estimates store [NAME]` — snapshot
// the current estimation result (session.lastEstBlock, falling back to
// lastModel().result) into the named estimates store. C++ port of
// dispatcher.py's "eststo" handler + esttab.py's make_snapshot.
// B4-W3b: `title("...")` is a real `estimates store` option (73 corpus uses,
// `est store r`i', title("")`). Before this it was in kVocabEstimates's
// UNIMPLEMENTED set and answered r(198); it now lands on the snapshot under
// the "title" key and `estimates dir` prints it. It is NOT coefplot's
// `title()` — that one (graph/coefplot_estimates_block.cpp) is the plot's own graph title,
// read from coefplot's opts and never from a stored snapshot.
// W2 eststoprefix (BUG-521): `autoPrefix` is eststo's `prefix()` option (the
// stem of the auto-name, estout's default "est"), and `viaEststo` says whether
// the call is estout's `eststo` (which owns the auto-name counter) or official
// `estimates store` (which never touches it) -- see SessionState::eststo.
nlohmann::ordered_json runEststo(const std::optional<std::string>& name,
                                 const std::optional<std::string>& title, SessionState& session,
                                 const std::string& autoPrefix = "est", bool viaEststo = true);

// `estimates save FILE [, replace]` / `estimates use FILE` (B4-W3b, BUG-470).
// The round trip Stata runs at rc 0 on every link: save writes a PIETEST1
// envelope carrying BOTH the e()-block snapshot AND the full LastModel, and
// use restores both — so `predict` after a `use` predicts off the RESTORED
// model, not off whatever regression happens to still be live. That last part
// is the whole point of the entry: an rc-only round trip passes for the wrong
// reason when a model is still in memory.
nlohmann::ordered_json runEstimatesSave(const std::string& fileArg, bool replace,
                                        SessionState& session);
nlohmann::ordered_json runEstimatesUse(const std::string& fileArg, SessionState& session);

// `eststo dir` / `est dir` / `estimates dir` — list stored estimate names.
nlohmann::ordered_json runEststoDir(SessionState& session);
// Every stored estimate and the `_est_NAME` variables written for them, as
// `estimates clear` and `clear matrix` (its `_est drop _all`) drop them (BUG-1212).
void clearStoredEstimates(SessionState& session);

// `eststo drop|clear [NAME]` / `est drop|clear [NAME]` / `estimates
// drop|clear [NAME]`.
nlohmann::ordered_json runEststoManage(const std::string& sub, const std::optional<std::string>& name,
                                        SessionState& session);

// `esttab [NAME...] [, se|t b(%fmt) star(...) nostar label]` — multi-column
// regression table from the stored estimates (all stored models if no names
// given). C++ port of esttab.py's parse_esttab_opts + render_esttab.
nlohmann::ordered_json runEsttab(const std::vector<std::string>& modelNames,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  SessionState& session);

// `estimates table [NAME...] [, b se]` (P4 wave) — Stata's own side-by-side
// comparison of stored estimates, distinct from `esttab` (a user-written
// package with a different layout). Coefficient rows are the UNION of the
// models' names in first-seen order; a model missing a coefficient leaves its
// cell blank rather than printing a zero, because a blank means "not in this
// model" while a 0 would read as an estimated null effect. With no names, the
// whole store is used, matching Stata.
nlohmann::ordered_json runEstimatesTable(const std::vector<std::string>& modelNames,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);

// ---------------------------------------------------------------------------
// esttab / estout `transform()` -- 18 corpus uses over four papers (12 on estout
// in paper 016, 6 on esttab in 015/083/104), and measurably ONE option: every
// esttab arm in tests/eval/golden/aer_w1/w1b_transform2.log has an estout twin
// with the same numbers (y3/y4/y5/y6).
//
// Declared HERE rather than kept private to tables/table_estout.cpp because the
// two writers build their rows in different translation units --
// tables/esttab_block.cpp for esttab, tables/table_estout.cpp for estout -- and
// BUG-496 is what a rule implemented twice in exactly that shape costs. One
// semantics, two call sites, both pinned by selftest arms.
//
// The grammar is estout `_TransformCoefs`, read before implementing and
// then measured: the value is a flat whitespace-separated list taken as TRIPLES
// (pattern, f, df), and a trailing group of two is (f, df) with no pattern -- so
// a one- or two-token list applies to every row.
struct TransformGroup {
  std::string pat;  // coefficient pattern; empty or `*` matches every row
  std::string f;    // REPLACES the coefficient; `@` is that coefficient
  std::string df;   // MULTIPLIES the se by abs(); `@` is the COEFFICIENT, not
                    // the se. Empty leaves the se alone -- measured, not
                    // assumed (w1b_transform.log r1 doubles b and holds se).
};

struct TransformSpec {
  std::vector<TransformGroup> groups;
  // Per-model words from `pattern()`. EMPTY means every model, which covers both
  // an absent pattern() and an explicitly empty one (w1b_transform2.log z4); a
  // word missing for a model means "do not transform it" (z1, where pattern(1)
  // over two models leaves the second alone).
  std::vector<std::string> pattern;
  bool empty() const { return groups.empty(); }
  bool appliesToModel(size_t i) const {
    if (pattern.empty()) return true;
    return i < pattern.size() && pattern[i] == "1";
  }
};

// Parses the INSIDE of `transform(...)`; false + `err` for what estout itself
// rejects (a pattern() word that is neither 0 nor 1 is r(198) there).
bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err);

struct TransformCell {
  bool matched = false;
  bool bMissing = false;           // the expression did not evaluate finitely
  std::optional<double> b;         // the new coefficient
  std::optional<double> seFactor;  // multiply the se by this; nullopt = leave it
};

// One (model, coefficient) cell. `snap` is THAT model's own snapshot and is made
// current while the expression is evaluated, because `_b[]` inside a transform
// resolves per model: `transform(@/(-_b[weight]))` over two models divides each
// column by ITS OWN weight coefficient (w1b_transform2.log y1).
TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_ESTIMATES_TABLES_H
