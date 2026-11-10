// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool commandNeedsNoData(const std::string& cmd) {


  // ------------------------------------------------------------------
  // 2. Guard: data must be loaded for most commands (dispatcher.py §2's
  // allow-list). M3a widens the M2 slice's `use`-only list to every
  // data-free command this milestone's handler surface actually routes
  // (xtset/tsset/display/di/scalar/label/timer/cd/pwd/preserve/restore
  // /tempfile/tempname); the FULL oracle allow-list (sysuse/webuse/clear/
  // set/import/version/compress/graph/eststo/…) stays M3b until those
  // commands themselves are ported — listing a command here that has no
  // handler arm below would silently bypass the no-data guard with nothing
  // to route it to, so this set is intentionally scoped to M3a's surface.
  static const std::set<std::string> kNoDataCommands = {
      "use",       "sysuse", "xtset",     "tsset",   "display", "di",     "cd",
      "pwd",       "timer",  "label",     "preserve", "restore", "tempfile",
      "tempname",  "scalar",
      // append.ado appends into an empty dataset too: the result is the using
      // data (w2aw_data_combine_family.log dc_a_empty rc 0, N=2);
      // runAppendFiles handles the empty master itself.
      "append",
      // M3b: foundational commands that must work with zero data loaded
      // (clear/set-obs are the `clear -> set obs N -> gen` data-construction
      // idiom's first two steps; version/compress/set are pure no-ops;
      // egen/bysort/postfile/label define et al. can run before `use` too).
      "clear",     "set",    "version",   "compress", "exit",   "egen",
      "bysort",    "by",     "postfile",  "post",     "postclose",
      // BUG-028 + U7/S9 offline shims: `log using`/`log close|off|on`, plus
      // which/ssc/confirm/mkdir/erase, must all work before any `use`
      // (analysis.do's line 52-108 preamble opens a log, checks/installs
      // packages, and confirms the input file exists, all before `use`).
      // BUG-180 (P2-PREP): "rmdir" joins its mkdir/erase neighbours here —
      // scratch-directory teardown must work before any `use`, exactly like
      // the setup half.
      "log",       "which",  "ssc",       "confirm",  "mkdir",  "erase",  "rm",  "rmdir",
      // M4a: stored-estimates commands operate on session state (the
      // estimates store / lastModel), not the live dataset (dispatcher.py
      // §2's own no-data allow-list: "eststo", "est", "estimates",
      // "marginsplot", "graph", "coefplot"). "graph" itself (export/save/
      // combine/drop sub-commands read session.last_graph, not live data;
      // `graph twoway`/plot-type forms DO need data — the graph handler
      // arm re-checks hasData() for those internally, matching
      // dispatcher.py's own `if ds is None: return no_data_loaded()` INSIDE
      // the "graph" handler body, not at this outer guard).
      "eststo",    "est",    "estimates", "marginsplot", "coefplot", "graph",
      // Mission 5 (cosmetic no_data_loaded divergence, native-paths
      // investigation): real Stata's `describe`/`desc` with no dataset in
      // memory answers an EMPTY description (0 vars, 0 obs), not an error.
      // Deliberately does NOT include "codebook" — real Stata's codebook
      // needs actual variable statistics and errors with no data, so it
      // stays on the outer no-data guard, unaffected. The "describe"/"desc"
      // arm below special-cases !session.hasData() itself rather than
      // calling handlers::runDescribe() (descriptive/describe.cpp),
      // whose own internal guard still returns
      // noDataLoaded() unconditionally.
      // BUG-105 abbreviation stopgap: `d` is listed here for the same
      // reason `desc` is — an alias that routes to the describe arm but
      // dies on the outer no-data guard would answer no_data_loaded where
      // its own canonical answers an empty description, i.e. it would be
      // an alias in name only.
      "describe",  "desc",   "d",
      // P1 wave: `import delimited|excel` LOADS a dataset, so it must run with
      // nothing in memory — the same reason `use`/`sysuse` are here. (`export`
      // genuinely needs data and stays on the outer guard.)
      "import",
      // BUG-343: `file open|write|read|close` is text I/O against the filesystem
      // and never touches the dataset. Registering the row without listing it
      // here would have made every `file` line answer "No dataset is loaded" —
      // the same wrong-class error BUG-342's second layer produced, and the
      // reason a registry row is never the whole of adding a command.
      "file",
      // AER-SCOPE1 bundle D (ENV): every environment verb runs with nothing in
      // memory -- most of them ARE the first line of the do-file, before any
      // `use`. Omitting a row here is BUG-343 exactly: the registry row and the
      // arm are both correct and the command still answers "No dataset is
      // loaded". testAerCampaignRatchets() probes this axis directly.
      "shell",   "winexec", "type",    "dir",      "ls",      "net",
      "ado",     "adopath", "sysdir",  "findfile", "sleep",   "pause",
      "cls",     "gr_edit", "error",   "bcal",     "twoway__histogram_gen",
      "frames",
      // AER-SCOPE1 bundle B: `return`, `svmat` and `matlist` read the session's
      // r()/matrix stores, never the dataset -- `svmat A` on an empty session is
      // how Stata builds a dataset FROM a matrix. `mkmat` is deliberately absent
      // for the mirror reason: it reads variables, so no data is a real error.
      "return",  "svmat",   "matlist",
      // W2D-T01: `ereturn clear/post/local` act on e(), never the dataset;
      // `ereturn clear` opens the golden do-file's helpers before any data
      // exist (w2d_uncovered.log 211-212). The `list` arm re-checks hasData()
      // itself to keep its earlier answer.
      "ereturn",
      // ml-getmata T4: `getmata` reads the Mata store, and on an empty session
      // it is how the dataset comes into existence (getmata: an empty dataset
      // implies force; oracle arm getmata_nodata).
      "getmata", "ml",
      // `matrix`/`mat` join them: a matrix is session state, not data. Stata
      // accepts `matrix A = (1,2\\3,4)` with nothing in memory, and `svmat A`
      // on an empty session is the documented way to build a dataset FROM a
      // matrix -- refusing the definition made that whole path unreachable.
      "matrix",  "mat",
      // `tset` is deliberately ABSENT: like the `tsset`/`xtset` it re-spells, it
      // validates its variables against the data, so running it with nothing in
      // memory is a genuine no-data error rather than a route that was blocked.
      // AER-SCOPE1 bundle E (TABLES): the table verbs that render STORED
      // ESTIMATES or a session MATRIX and write a file. `eststo`/`esttab` are
      // already above for the same reason. The question this list answers is
      // "does this verb READ THE DATA?" (bundle D's lesson), and for these the
      // answer is no -- an estimate outlives the `clear` that follows it, so
      // `regress ... ; clear ; estout using t.tex` is a real corpus shape.
      "estadd",  "estout",   "outreg2",  "frmttable", "mat2txt", "erepost",
      "regsave", "parmest",
      // texdoc holds an open FILE; `tex` writes a literal line into it. Neither
      // has anything to do with the dataset, and `texdoc init` is routinely the
      // first line of the do-file.
      "texdoc",  "tex",
      // Deliberately ABSENT, all three for the same reason `tset` is: they read
      // the dataset. `estpost summarize` summarises variables, `texsave` renders
      // the dataset as a table, and `tabout` cross-tabulates it -- with nothing
      // in memory each is a genuine no-data error, not a blocked route.
      // AER-SCOPE1 bundle F (RESHAPE): `input` and `drawnorm` CREATE the data
      // they operate on, exactly like `use` -- `clear` then `input x y` is the
      // documented way to type a dataset in, and `drawnorm u v, n(50) clear` is
      // how a simulation starts. Every OTHER verb in bundle F reshapes or
      // samples an existing frame and is deliberately absent from this list.
      "input",   "drawnorm",
      // AER-SCOPE1 bundle G (IO-TEXT): the three readers LOAD data, and the
      // file/environment verbs never touch the dataset at all -- `copy`,
      // `filefilter` and `putexcel set` all routinely appear in a preamble
      // before any `use`. Deliberately ABSENT: `outsheet`, `outfile` and
      // `saveold`, which WRITE the frame out, so running them with nothing in
      // memory is a genuine no-data error rather than a blocked route.
      "insheet", "infile",   "infix",    "putexcel", "filefilter", "copy",
      "unzipfile", "zipfile", "odbc",    "spshape2dta",
      // AER tier-1: file, estimate-store, label-store, matrix-store and display
      // verbs that read no dataset (zipsave alone needs one, and checks itself).
      "zipuse", "require", "_dots", "postutil", "lrtest", "matsort", "matselrc", "numlabel",
      "findname",  // rc 0 and silent with no data (StataBE, lane-meta probe mx5)
      // AER-SCOPE1 bundle I (UTIL): `filelist` builds its dataset FROM the
      // filesystem, so like `use` it must run with nothing in memory. Every
      // other verb in bundle I reads or rewrites an existing frame.
      "filelist",
      // AER-SCOPE1 bundle J (GRAPH-X): `grstyle` touches nothing at all, and
      // `grc1leg2`/`event_plot` read named graphs and stored estimates rather
      // than the frame -- all three routinely appear before any `use`.
      // `geodist`, `geonear` and `cdfplot` read the data and stay on the guard.
      "grstyle", "grc1leg2", "event_plot",
      // AER-SCOPE2 bundle K1: `beep`/`window` touch nothing at all, `grc1leg`
      // reads named graphs like its grc1leg2 sibling above, and `parallel`'s
      // initialize/clean subcommands are process bookkeeping. All four appear in
      // do-file preambles before any `use`.
      "beep", "window", "grc1leg",
      // AER scope 2 K1c: `parallel initialize`/`setclusters`/`clean` are
      // process bookkeeping and appear in a driver's preamble, before any
      // `use`. The `do` forms below them load their own data (or run with
      // none), so the whole name is on the guard.
      "parallel",
      // serset-twoway SS: `serset` reads the session's serset store, and with
      // nothing in memory it is rc 0 with r(id) -1 (w2al_twoway_family.log:177);
      // `serset use` is how a serset BECOMES the data.
      "serset",
      // T7 L1: `lookfor x` with nothing in memory is rc 0 and silent in StataBE
      // (a bare `lookfor` is still r(198)); the handler owns both answers.
      "lookfor",
  };
return kNoDataCommands.count(cmd) != 0;
}

}
