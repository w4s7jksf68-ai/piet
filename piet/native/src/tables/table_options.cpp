// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from outreg2 2.3.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
#include "tables_internal.h"
#include "estout_internal.h"
#include "outreg2_internal.h"
namespace piet::handlers::table_detail {


const piet::eval::OptVocab& kVocabEstadd = piet::eval::registerOptVocab(
    {"estadd"},
    // W2AK: estadd_ysumm's statistic flags (estadd).
    {{"replace", "prefix", "quietly", "mean", "sum", "min", "max", "range", "sd", "var", "cv",
      "semean", "skewness", "kurtosis", "median", "p1", "p5", "p10", "p25", "p50", "p75", "p90",
      "p95", "p99", "iqr", "q", "all"},
     {}});


const piet::eval::OptVocab& kVocabEstpost = piet::eval::registerOptVocab(
    {"estpost"},
    {{"statistics", "stat", "stats", "columns", "by", "listwise", "casewise", "nototal", "quietly", "detail",
      // tabstat's by-group handling: `missing` makes a missing by-value a group of its own and
      // `elabels` names every group by its value and saves the labels in e(labels)
      // (vs-stata bank 6542bb10, 14c66079).
      "missing", "elabels"},
     {"escal"}});


const piet::eval::OptVocab& kVocabEstout = piet::eval::registerOptVocab(
    {"estout"},
    {{"style", "replace", "append", "cells", "b", "se", "t", "p", "fmt", "stats", "mlabels",
      "collabels", "keep", "drop", "order", "star", "starlevels", "varlabels", "labels", "label",
      "title", "note",
      // B4-W5: the discriminating pair the oracle itself identified. `estout
      // nonumbers` alone produced a file byte-identical to the plain arm
      // (pins_oracle_t2.do's own header says so), because estout's DEFAULT
      // header is the model NAME -- so the arm that separates them is
      // `numbers` vs `nonumbers`, e_numbers.txt vs e_numbers_off.txt. Before
      // this both spellings were unknown options and answered rc 198.
      "numbers", "nonumbers",
      // `transform()` -- 12 of the 18 corpus uses are on this command. `eform` is
      // NOT added with it: it is measurably `transform(exp(@) exp(@))` PLUS its
      // own removal of `_cons` (w1b_transform.log u2 has no intercept row where
      // u1 does), it has zero corpus uses on either writer, and accepting it as
      // an alias would drop that second half silently.
      "transform",
      // W1a-4: the two geometry knobs of `style(tex)`. Both are MINIMA -- a
      // longer label or number overflows unpadded (w1b_estout_tex2.log s25,
      // w1b_estout_tex.log rc) -- and `nolabel` is here because it is what
      // decides the stub's default between 20 and 12, winning over `label` when
      // both are given (w1b_estout_opts.log q3).
      "varwidth", "modelwidth", "nolabel",
      // W1a-4, all measured in w1b_estout_opts{,2}.log. The four seam hooks are
      // ADDITIVE here, unlike esttab's, where they replace a block (BUG-500);
      // `begin()` prefixes every row; `legend` prints the ladder DESCENDING and
      // only when the cells carry stars; `extracols()` inserts empty columns
      // after the columns it names; `rename()` is a row-label map whose misses
      // are no-ops (y b).
      "prehead", "posthead", "prefoot", "postfoot", "begin", "legend", "extracols", "rename",
      // W2AK (2026-10-21): estout now runs on the derived writer (eoRender),
      // so the rest of estout's option list and MoreOptions' switches
      // are honoured as the ado reads them.
      "unstack", "nounstack", "eqlabels", "indicate", "refcat", "substitute", "delimiter",
      "end", "lastend", "mgroups", "labcol2", "incelldelimiter", "interaction", "hlinechar",
      "lz", "nolz", "abbrev", "noabbrev", "wrap", "nowrap", "omitted", "noomitted",
      "baselevels", "nobaselevels", "nolegend", "type", "notype", "matrix", "showtabs",
      "noshowtabs", "outfilenoteoff", "nooutfilenoteoff", "level", "dmarker", "msign",
      "stardrop", "starkeep", "margin", "nomargin", "discrete", "nodiscrete", "eform",
      "noeform", "equations", "topfile", "bottomfile",
      // The screen form's smcl style already switches all four on (estout: defaults smcl ->
      // smcltags, smclrules, and through it smclmidrules and smcleqrules), so spelling one is
      // a no-op there; any other style or a `using` file would change the text and is refused
      // by name in runEstout (vs-stata bank 46aec76d, f9f7a396, b06f4ec7, 6dc98d73).
      "smcltags", "smclrules", "smclmidrules", "smcleqrules", "stardetach", "dropped"},
     {"transpose", "wide", "defaults"},
     // MoreOptions spells Label and NOLabel with the capitals L and NOL: `l` and `nol`
     // are theirs alone (legend, lz need two letters; nolegend, nolz more), which the
     // unique-prefix rule cannot tell (vs-stata bank b4d3851c, 08c7be76).
     {{"l", "label"}, {"nol", "nolabel"}, {"c", "cells"}}});

// `esttab`'s option vocabulary is NOT registered here. It has exactly one owner
// (`esttab_block.cpp`'s kVocabEsttab), because a command whose option set is
// declared in two translation units is a command with two answers about what it
// accepts. `cells` and `p` are listed there as UNIMPLEMENTED: `runEsttab` reads
// nostar/b/se/t/star and neither of those two, so accepting them would be the
// accept-and-ignore class this campaign exists to prevent (bundle A shipped it
// once as `ivreg ..., ro`). `estout` DOES implement cells() -- that handler is
// in this file and reads the spec -- so the honest statement is per command,
// not per option name.

const piet::eval::OptVocab& kVocabOutreg2 = piet::eval::registerOptVocab(
    {"outreg2"},
    {{"replace", "append", "ctitle", "addtext", "addstat", "label", "tex", "excel", "nocons",
      "keep", "dec", "bdec", "sdec", "nor2", "noobs", "adjr2", "noaster",
      // w2at_table_writers (outreg2.ado's own syntax lines :418-468): the notes
      // pair, the R-squared and p-value decimals, drop() and sortvar().
      "nonotes", "addnote", "rdec", "pdec", "drop", "sortvar",
      // B4-W5: the four content options, each pinned to a captured arm in
      // tests/eval/golden/aer_b4/tables/ (o_tstat, o_tstat_tdec4, o_auto4,
      // o_stats). `noauto` comes with them because it is the switch outreg2's
      // own source uses to turn the adaptive number format off.
      "tstat", "tdec", "auto", "noauto", "stats",
      // `adec(#)` is the added-statistic decimal count -- 106 corpus uses over
      // three papers. `afmt()` is deliberately NOT here: it is the ado's only
      // escape from the separator bug, it has ZERO corpus uses, and its non-`f`
      // suffixes are unmeasured, so it stays an unknown option rather than an
      // accepted one this build would have to guess at.
      "adec",
      // `text` asks for the plain-text ledger, which this build ALWAYS writes
      // (the `.txt` file is its append source of truth), so it is accepted and
      // is a no-op rather than a rejection. Every one of the five captured
      // outreg2 arms spells it.
      "text",
      // BUG-1467: `se` is the positive form of outreg2's noSE option (the
      // default; listed so it is not read as an abbreviation of `seeout`), and
      // `nose` drops the standard-error rows (outreg2 se_skip).
      "se", "nose",
      // pvalue, bracket, paren()/parenthesis() and bracket(): outreg2.ado's own statistics options
      // (vs-stata bank 64eb4ba9, 93e5a3cb, cf08e52b, df9fe134)
      "pvalue", "bracket", "parenthesis", "ci", "cdec"},
     {"word", "alpha", "e", "sideway", "onecol", "seeout", "symbol",
      "title", "beta", "eform", "skip", "long", "sum", "dta"},
     // Pvalue (`p`), Onecol (`o`): the capitals of outreg2's syntax line
     {{"p", "pvalue"}, {"o", "onecol"}, {"par", "parenthesis"}, {"pare", "parenthesis"},
      {"paren", "parenthesis"}}});


const piet::eval::OptVocab& kVocabTexsave = piet::eval::registerOptVocab(
    {"texsave"},
    {{"replace", "title", "headerlines", "frag", "hlines", "align", "nofix", "footnote",
      "varlabels",
      // W2AK TW-TEXSAVE: the rest of texsave's syntax line, derived from it.
      "size", "width", "marker", "label", "location", "geometry", "landscape", "nonames",
      "autonumber", "rowsep", "sw", "noendash", "delimiter", "preamble", "headlines",
      "tablelines", "headerlines2", "footlines", "rowstretch", "rowheight", "colwidth",
      "headersep", "dataonly", "bold", "italics", "underline", "slanted", "smallcaps",
      "sansserif", "monospace", "emphasis", "decimalalign", "valuelabels"},
     {}});


// `force` is texdoc's "overwrite even if the file is open" flag; `cmdstrip` and
// friends belong to `texdoc do`/`texdoc stlog`, which this build does not run.
const piet::eval::OptVocab& kVocabTexdoc = piet::eval::registerOptVocab(
    {"texdoc"},
    {{"replace", "append", "force", "nologdir", "logdir", "nologall", "logall", "noprefix", "prefix",
      "nostpath", "stpath", "nodo", "do", "nolog", "log", "nocmdlog", "cmdlog", "nobeamer",
      "beamer", "noverbatim", "verbatim", "nohardcode", "hardcode", "nokeep", "keep", "nocustom",
      "custom", "nocmdstrip", "cmdstrip", "nolbstrip", "lbstrip", "nogtstrip", "gtstrip",
      "nomatastrip", "matastrip", "nooutput", "output", "noltrim", "ltrim", "gropts", "grdir",
      "alert", "tag", "nocertify", "certify", "linesize"},
     {}});


// `tex` (texdoc's line verb) takes no options at all: everything after the
// verb is literal TeX. Registering an EMPTY vocabulary is not a no-op -- it is
// what makes any `tex ..., foo` reach the net as an unknown key rather than
// being silently swallowed.
const piet::eval::OptVocab& kVocabTexLine =
    piet::eval::registerOptVocab({"tex"}, {{}, {}});


const piet::eval::OptVocab& kVocabFrmttable = piet::eval::registerOptVocab(
    {"frmttable"},
    {{"statmat", "sdec", "tex", "replace", "rtitles", "ctitles", "title", "note", "using",
      "fragment",
      // W1a-5h. `nocenter` drops the `\begin{center}` wrapper and nothing else
      // (w1b_frmttable_base.log h7 against h1); `sfmt()` names the numeric
      // format whose `fc` spelling is ALREADY the default this writer uses, so
      // `sfmt(fc,...)` is byte-identical to leaving it out (cells2 k7 against
      // k6) -- accepted as a measured no-op for that spelling, and any other
      // suffix is refused below rather than silently treated as `fc`.
      "nocenter", "sfmt",
      // W2AK TW-FRMT: the stored table (frmttable) -- `store()`
      // names the table a statmat() call builds (default: the unnamed one) and
      // `replay[()]` writes a stored table with the new output options.
      "replay", "store"},
     // `replay`/`replay1`/`store` are frmttable's stored-table accumulator
     // (frmttable keeps the built table in a Mata external
     // `_FrmtT<name>`): `store()` names it, `append`/`merge` accumulate into it,
     // and `replay` writes it out. Measured in w1b_frmttable_replay.log -- append
     // STACKS rows (g2) and merge adds COLUMNS (g4) -- and named unimplemented
     // rather than unknown, because the model is understood and absent, not
     // unrecognised. `multicol()` is the tex span option paper 004 pairs with
     // replay (gb).
     {"merge", "append", "varlabels", "substat", "annotate", "asymbol", "coljust", "hlines",
      "doubles", "multicol", "addrows"},
     // frmttable's syntax line spells Statmat(), Note() and Title() with the capitals
     // S, N, T: the shortest spelling that is theirs alone. Several no... and s...
     // options share these prefixes (noBLankrows, SDec, ...), which the unique-prefix
     // rule cannot tell apart (vs-stata bank 3cef0e2c, 094ed42e, 635d981c, 305082d0).
     {{"n", "note"}, {"no", "note"}, {"s", "statmat"}, {"st", "statmat"}, {"t", "title"}}});


const piet::eval::OptVocab& kVocabRegsave = piet::eval::registerOptVocab(
    {"regsave"},
    {{"replace", "append", "ci", "se", "nose", "pval", "tstat", "addlabel", "level", "cmdline", "autoid",
      "double", "detail", "covar"},
     {"table", "saveold", "rtable", "df", "addvar", "coefmat", "varmat"}});


const piet::eval::OptVocab& kVocabParmest = piet::eval::registerOptVocab(
    {"parmest"},
    {{"saving", "norestore", "fast", "list", "omit", "label", "idnum", "idstr", "level", "eform",
      "rename", "format", "flist", "escal", "emac", "stars", "bmatrix", "vmatrix"},
     {"frame", "dfmatrix", "dof", "msetype", "empty", "ylabel",
      "pupper", "plower", "clnumber", "mcompare", "mcomci", "evec", "erows", "ecols", "float",
      "nodouble", "nozerop", "nullvalue"}});


const piet::eval::OptVocab& kVocabMat2txt = piet::eval::registerOptVocab(
    {"mat2txt"}, {{"matrix", "saving", "replace", "append", "title", "format", "note"}, {}});


const piet::eval::OptVocab& kVocabErepost = piet::eval::registerOptVocab(
    {"erepost"}, {{"rename", "noesample", "properties", "obs", "dof", "esample", "cmd"}, {"nob", "nov", "se", "drop"}});


// `tabout` is refused at the ARM (dispatcher.cpp), so it never reaches a
// handler here. It still gets a vocabulary entry so that
// `canonicalizeForCommand` has something to answer for the token and the
// registry inventory guard sees a complete bundle. Everything is
// `unimplemented`: the whole grammar is what is missing, not a set of options.
const piet::eval::OptVocab& kVocabTabout =
    piet::eval::registerOptVocab({"tabout"}, {{}, {"using", "replace", "append", "style", "cells",
                                                  "format", "clab", "sum", "svy"}});




// The registrations above are consumed by the reject nets below; naming them
// here keeps `-Wunused-variable` honest about the two that no handler in this
// TU reads (tabout is refused at the arm, and `tex` has an empty vocabulary).
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {
    &kVocabTabout, &kVocabTexLine};


// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// estout core (W2AK, 2026-10-21): ONE table writer for esttab and estout.
//
// esttab.ado is "translate the options through a per-mode table, then call
// estout" (esttab is the table, the translation), and this
// build used to carry two independent hand-built writers instead -- the old
// `writeEsttabFile` and `runEstout` -- which agreed with neither the ados nor
// each other on rules, gaps, hooks, widths or csv quoting. What follows is
// derived from estout.ado 3.33's WRITE phase (estout and the
// helpers it calls: WriteCaption, WriteBegin/WriteEnd, WriteEqrow,
// WriteStrRow, vFormat, SignificantDigits, Stars, Abbrev, InsertAtVariables,
// CompileVarl, ExpandEqVarlist, DropOrKeep, Order) driven by an
// `EoSpec` the caller fills. esttab's translation lives in `esttabSpec` below.
//
// Reading the ado is the spec: every rule here follows the behaviour read from it.
// What is NOT derived is said where it would have been (rtf/html/smcl
// writers, element-level keep()/drop(), statslayout, margin/discrete).
// ===========================================================================


const double kEoNaN = std::numeric_limits<double>::quiet_NaN();


// Stata's missing value inside this port.
const double kO2Missing = std::numeric_limits<double>::quiet_NaN();


// ===========================================================================
// texdoc / tex
//
// The open file is a PATH held in `session.eval.globals`, and every `tex` line
// opens-appends-closes. At the five-line scale the corpus uses that costs
// nothing measurable, and it avoids adding a SessionState field on a surface
// this lane does not own. The cost, stated: `$__piet_texdoc_path` is visible to
// `macro list`. That is the trade the plan made (decision 9).
// ===========================================================================


const char* const kTexdocPathKey = "__piet_texdoc_path";


const char* const kFrmtStorePrefix = "__piet_frmttable_";
}
