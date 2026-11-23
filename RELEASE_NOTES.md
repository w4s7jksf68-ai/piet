# PIET A.2.0 / CoBrA 1.2.0 — Stata command compatibility update

PIET expands support for GMM, frame management, postestimation, result tables,
and command options. Long `display` output now wraps in the console and in
plain-text logs. Stata's example datasets are permanently unavailable through
PIET's `sysuse`. `require` returns r(601) for unavailable packages.

The recorded Stata comparisons omit graph internals, timestamps, elapsed
times and serialization-dependent archive sizes as described in the
[compatibility policy](https://github.com/w4s7jksf68-ai/piet/blob/A.2.0/docs/piet/COMPATIBILITY-POLICY.md).
Some Stata commands and options remain unsupported. The policy also lists
follow-ups for eigenvector signs, temporary names, graph schemes, signed ranks,
table abbreviations and checksums.

Downloads are available for macOS arm64, Windows x64 and Ubuntu 24.04+ x86-64.
The release workflow builds each file from this repository at tag `A.2.0`.
SHA-256 files are provided alongside the downloads.

# PIET A.1.1 / CoBrA 1.1.0 — Linux desktop release

PIET A.1.1 adds Linux downloads alongside macOS and Windows. The computation
engine remains CoBrA 1.1.0; contracts A 1.27.0 and B 2.11.0 are unchanged.
Corresponding source: this repository at tag `A.1.1`.

**Linux downloads**

- Ubuntu 24.04 LTS or newer, x86-64 with SSE4.2 (roughly 2009 onward).
- `PIET-A.1.1-linux-x64.deb`: install with
  `sudo apt install ./PIET-A.1.1-linux-x64.deb`, then launch PIET from the
  application menu or run `piet`.
- `PIET-A.1.1-linux-x64.tar.xz`: extract the entire folder and run `./PIET`.
  Install its dependencies first with
  `sudo apt install libwebkit2gtk-4.1-0 libgtk-3-0 libopenblas0 liblapacke`.
- Both include the engine, frontend, sample dataset, teaching examples and
  licence notices. Each download has a SHA-256 file beside it.

The GTK desktop backend now opens and saves files through native dialogs,
loads the bundled interface's JavaScript modules and resolves its resources
from the executable's location. The x86-64 engine launcher selects OpenBLAS's
fixed Nehalem arithmetic path so CPU dispatch does not change the last step of
a nearly converged probit. Package dependencies enforce the tested Ubuntu
24.04 runtime floor; Ubuntu 22.04 is not supported.

Statistical algorithms, reference values and test tolerances are unchanged
from A.1.0. Its capabilities and known gaps remain as documented below.

**Validation of the Linux port:** 67 self-contained engine tests passed;
45 tests requiring unshipped private fixtures were skipped. The public smoke
test checked OLS against an independent solve, a .dta roundtrip and Hangul
paths/text. Native file-picker checks passed, and both extracted and installed
packages passed all 25 GUI-journey steps. macOS and Windows CI also passed.
The release workflow rebuilds the downloads from this tag and checks them
before publishing.

# PIET A.1.0 / CoBrA 1.1.0 — checked against 39,290 Stata 19.5 cells: 32,148 now match

The app reports PIET `A.1.0`, the engine CoBrA `1.1.0`; the engine contract moves to 1.27.0 and the
app contract to 2.11.0, both additive (nothing renamed or removed). Corresponding source: this
repository at tag `A.1.0`.

**What this release is.** On 2026-11-10 StataBE 19.5 ran 39,290 small do-files ("cells") covering
319 commands with their options, abbreviations, error cases and stored results, and everything it
printed and stored was kept. A.1.0 is the result of running PIET against every one of those cells and
repairing what differed. A cell matches when the return code, every stored result in `r()`, `e()`
and `s()` (within the tolerances of `tests/eval/TOLERANCE.md`) and, for display cells, the printed
text all agree with Stata's.

| cells | A.0.2 code | A.1.0 |
|---|---|---|
| match Stata | 18,526 | **32,148** |
| Stata runs it, PIET answers "not implemented in this version" | 8,699 | 3,872 |
| a stored number differs | 1,315 | 141 |
| Stata refuses it, PIET runs it | 2,469 | 301 |
| both refuse, with another return code | 1,785 | 240 |
| a stored result is missing, or a stored name or text differs | 3,830 | 741 |
| a command PIET does not have | 2,384 | 1,596 |

Both columns come from the same harness; two rules added during the work (values Stata itself cannot
reproduce, such as its internal data checksum and the addresses of graph objects, are not compared)
account for 122 of the new matches. 327 cells are not counted as targets: their Stata record is an artifact of the recording session
(state left by an earlier cell, a package that failed to install there) or a limit only Stata/BE has.
Outside them 32,114 of 38,963 cells match (82.4%).

**Estimation**

- `ivreg2`, `xtivreg2` and `ivreghdfe` post their whole set of stored results (fit statistics, F,
  Sargan/Hansen J, under- and weak-identification statistics, first-stage results). `reghdfe` posts
  `e(ic)` with two or more absorbed factors; `prais`, `xtreg, be`, `sureg` and `reg3` post what Stata
  posts.
- New: the random-effects `xtpoisson` model (gamma effects, Stata's default) with `vce(robust)`;
  `heckman` by full maximum likelihood (Stata's default) with `select(varlist)`, `predict`,
  `mills()` and `nshazard()`; `tobit, vce(cluster)` and `predict` after `tobit`.
- `ologit`, `oprobit` and `mlogit` take weights and `offset()`, and `predict` after them gives `xb`,
  `stdp` and `pr`. `mean` and `proportion` post their full results and take `over()`,
  `vce(cluster)`, standardization and `citype()`. `pca` and `sktest` take weights.
- `logit`, `probit` and the other maximum-likelihood fits publish `e(gradient)`, `e(ilog)` and ml's
  settings, and `tobit` follows Stata's ml iteration.
- `test`, `lincom` and `nlcom` now answer after fits the engine cannot re-fit (qreg, glm, ivregress
  GMM/LIML, xtreg re, sureg, the ml fits above), from the posted `e(b)` and `e(V)`.
- `level()` is honoured when abbreviated, down to `l(90)`.

**Tables and Settings**

- New setting, **Table size limits**: the largest one-way and two-way tables `tabulate` draws and the
  cells of `tabulate, summarize()`. Empty means Stata's limit, that of Stata/SE and MP (12,000 rows;
  1,200 x 80; 375 cells); type a larger number to raise one. No table may exceed 1,000,000 cells
  (200,000 with `summarize()`), which bounds memory.
- Large tables are built in linear time: a one-way table of 100,000 rows took 19 s and now takes 0.2 s.
- `table` and `tabstat` follow Stata in more forms (pages for a third dimension, `tabstat` options).

**Data, language, files, graphs**

- Many data-management commands now leave Stata's `r()` results (`egen`, `isid`, `ipolate`,
  `duplicates`, `contract`, `gsort`, `collapse`, `append`, the gtools family and others); new `char`
  command; `findname` selects as Stata does.
- `#delimit ;`, `set varabbrev off`, more extended macro functions, SMCL in `display`, the detail codes
  of `%t` display formats, more matrix functions, and a second `program define` of a standing
  program is refused (r(110)).
- `infile`, `infix` and `input` read lines as Stata does; `import delimited` reports the encoding Stata
  reports and posts its `r()`; `zipfile` and `zipsave` compress as zlib does.
- More options of `twoway`, `binscatter`, `coefplot` and `event_plot` are supported, and `by()` graphs
  draw one panel per group; `esttab` writes HTML and Markdown; `estimates table` takes its whole
  option set.
- Thousands of refusals now carry Stata's own return code and message, including the option
  abbreviations Stata accepts and refuses (measured from 13,520 cells).

**Changes that can move results you already have**

- `import delimited` stores a non-integer numeric column as `float`, as Stata does, unless you give
  `asdouble`.
- `heckman ..., twostep`: an observation that is selected but has a missing outcome now leaves the
  estimation sample, as Stata's `heckman` marks it out. Estimates on such data change.
- `table`: a Total margin is computed over the table's own rows (those with every dimension present).
- `frames reset` clears the data in memory.
- Where Stata/BE and Stata/SE-MP differ, PIET follows SE/MP: `set maxvar` is accepted and the
  `tabulate` limits are SE/MP's (adjustable in Settings).
- An option whose only effect is output the app cannot draw yet stays a named "not implemented"
  refusal instead of being accepted and silently ignored.

**Known gaps**: 3,872 cells are still answered "not implemented" and 1,596 use commands PIET does not
have. The differences found and not yet fixed are registered as defects, for example the order in
which `label list` lists value labels after `reshape wide`, and `tabulate a b, summarize(x)`, which
is computed but not drawn. Stata's own example datasets (`sysuse auto` and others) are not bundled;
PIET's own `fatalities` is.

**Code adopted from other projects** (listed in `THIRD_PARTY_NOTICES.txt`): zlib's deflate algorithm
(Zlib licence), so that `zipfile` / `zipsave` report the sizes Stata reports; ICU's charset-detection
tables (Unicode License v3), for `import delimited`'s encoding guess.

**Checks on this tree:** before publication, the full internal gate (application self-test, the ten
wave answer books, 339 frozen outputs, 112 engine unit tests, 148 correctness gates, registry,
transport and probe checks) passed. In GitHub Actions, as before: engine unit tests,
`tests/public_smoke.py` and the real-window `--ui-smoke` journey on macOS and Windows, and the
downloads built from the commit at this tag by `.github/workflows/release.yml`.

# PIET A.0.2 / CoBrA 1.0.2 — first Windows build; do-files open in their own folder

The app reports PIET `A.0.2`, the engine CoBrA `1.0.2`; contracts unchanged at
1.26.0 / 2.10.0. Corresponding source: this repository at tag `A.0.2`.

**New on every platform: do-files open in their own folder, unless you chose a
directory yourself.** PIET still starts in Documents. Choosing a do-file with Run .do
or Open do-file, dropping one on the window, or reopening one from Recent now moves
the working directory to that file's folder, and the Results pane says so, so a
relative `use "mydata.dta"` finds the data beside the do-file. Once you have run `cd`,
or if you set a default working directory in Settings, opening a do-file leaves the
directory alone, so a project that runs `code/analysis.do` from its root behaves as
in Stata. A `do FILE` you type or call from a script never moves it. To match Stata
fully, turn off **Open do-files in their own folder** in Settings.

**New: Windows 10/11 x64.** `PIET-A.0.2-setup.exe` installs the app and, if the machine
needs it, Microsoft's WebView2 Runtime. `PIET-A.0.2-windows-x64-portable.zip` runs
from any folder on a machine that already has WebView2, as Windows 11 does. Neither
is code-signed, so SmartScreen warns on first launch: see the README's "Download
for Windows".

The Windows build gives the same numbers on every CPU. Its linear algebra uses one
fixed set of OpenBLAS kernels (SSE4.2) rather than choosing by processor, because
choosing changed results on nearly separated logit and probit samples at about 1e-8.
The cost is speed on very large matrix work, compared with kernels tuned to a newer CPU.

Known differences from the macOS build, both named rather than hidden:

- `ustrregexra()` and `ustrregexm()` refuse a character above U+FFFF, such as an emoji,
  with r(199). Windows matches text in 16-bit units, so `.` would otherwise match half
  of such a character. Hangul and all other text through U+FFFF behave as on macOS.
- `sdid, vce(bootstrap)` standard errors can differ from the macOS build's in about the
  fourth significant digit for the same seed; the point estimate does not. A few
  resampled panels are near-degenerate, which makes those replicates sensitive to
  whether the CPU fuses `a*b + c` into one rounding step. The macOS build does and the
  Windows build, which targets CPUs without that instruction, does not.

Also fixed: a built tree on Windows now finds its own engine, and the engine's
x86-64 baseline is now applied on native Windows builds.

**Checks on this tree:** engine unit tests and `tests/public_smoke.py` (now including
Hangul folder and file names) on both macOS and Windows in GitHub Actions, where the
same 45 of 112 engine tests are skipped on both for lack of private fixtures; the
app's real-window `--ui-smoke` journey on both, now covering the do-file folder
behaviour through the drop path.

**Build provenance:** `.github/workflows/release.yml` built every download from the
commit at this tag: `scripts/build-macos.sh` with `scripts/package-macos-dmg.sh`, and
`scripts/build-windows.sh` with `scripts/package-windows.sh`. Verify downloads with the
attached `.sha256` files.

# PIET A.0.1 / CoBrA 1.0.1 — patch stamp of 2026-11-06 (tag `A.0.1`)

Patch cut of the official line: the same tree as the 2026-11-06 macOS build
below, now carrying the version strings that identify it (the app reports
PIET `A.0.1`, the engine CoBrA `1.0.1`; contracts unchanged at 1.26.0 / 2.10.0).
What changed since A.0.0 / 1.0.0: BUG-1939 (dev-tree paths resolve from the
executable; `sysuse` and the frontend were lost under a `-ffile-prefix-map`
build), BUG-1940 (`hausman` when V_b-V_B is not positive definite answered
chi2 ~ 1e16 or refused; now Stata's generalized-inverse rule with its note),
adoption declarations and SPDX headers across the tree, the demo tour
corrections, and the public packaging scripts. The `.dmg` on the release page
is rebuilt from this tag. Corresponding source: this repository at tag `A.0.1`.

# PIET A.0.0 / CoBrA 1.0.0 — macOS build of 2026-11-06 (tag `A.0.0-public-20261106`)

First downloadable build from the public tree: `PIET-A.0.0-20261106-macos-arm64.dmg`
on the release page, built with `scripts/build-macos.sh` + `scripts/package-macos-dmg.sh`
from the tagged commit on an Apple Silicon Mac (macOS 26.6, Xcode command-line
tools), ad-hoc signed, not notarized (see README, "Download for macOS").
The app reports PIET `A.0.0` and CoBrA `1.0.0`; the tree it was built from
includes the fixes landed after those version stamps (BUG-1939 dev-tree path
resolution, BUG-1940 `hausman` on a difference matrix that is not positive
definite, demo tour corrections), so the tag and commit on the release page,
not the version string alone, identify the build. Corresponding source: this
repository at the tag.

# PIET A.0.0 / CoBrA 1.0.0 — public source cut of 2026-11-06

This cut replaces the experimental 2026-10 source beta with the full current
source tree of PIET A.0.0 and CoBrA 1.0.0 (the compiled version strings), as
prepared for public distribution on 2026-11-06. The whole tree is replaced in
one commit on top of the beta's own public history; no private development
history, private oracle logs, licensed reference datasets, or internal
automation is included.

- Every statistical routine ships as the project's own implementation. Code
  adapted from GPL-compatible open-source projects is declared in
  `THIRD_PARTY_NOTICES.txt` and in a header comment of the file that carries it.
- Tests and self-test arms that depend on private oracle fixtures report
  themselves as declared skips in a public clone; the fixtures they name are
  not required to build or use the software.
- Corresponding source for any binary published on the release page is this
  repository at the release tag.

# PIET 1.0 Beta 1 — v1.0.0-beta.1 (historical)

The first public source snapshot (2026-10-12) with an Apple Silicon DMG. It
used a different version naming scheme and withheld several functions pending
provenance review; those functions are present in the current tree as the
project's own implementations.
