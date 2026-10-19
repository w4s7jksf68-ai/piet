# Public beta validation

## Unreleased restoration

Checked locally on 2026-10-20 using macOS 26.6.2 on Apple Silicon and
AppleClang 21. The public regression suite constructs its own synthetic inputs
and expected results; it does not need licensed reference software, private
test logs, or third-party datasets.

| Check | Result |
|---|---|
| CoBrA and PIET native Release builds | PASS |
| Existing CTest determinism and thread-budget tests | 3/3 PASS |
| Restored-function regression groups | 15/15 PASS |
| Existing independent OLS/SE, DTA roundtrip, unknown-command smoke | PASS |
| Real native WebView UI smoke | 24 steps, GREEN |
| Gitleaks 8.30.1 source scan | No leaks found |

Restored-function checks cover formatting, decoded labels and UTF-8 byte limits,
ties, floating-point
boundaries, storage metadata, DTA roundtrips, malformed options, selection,
calendar omissions and extensions, fresh-process reload, and replacement-cache
refresh. A direct engine check also rejected nine malformed label/option
requests without persisting an output dataset.

Build and run the public checks with `sh scripts/build-macos.sh`. To run only the
restored-function regressions after both executables have been built:

```sh
python3 tests/restored_functions.py
```

`PIET_BIN` and `COBRA_BIN` can select alternate native and engine builds.
Coverage and known behavior limits are described in
[RESTORED_FUNCTIONS.md](docs/RESTORED_FUNCTIONS.md). No new package validation or
full compatibility certification is implied.

## Historical source beta

The following results describe the original source release, not the unreleased
restoration above.

Release: beta-2026.10.12.1. Checked locally on 2026-10-12.
Host: macOS 26.6.2, Apple Silicon, AppleClang 21.

| Check | Result |
|---|---|
| CoBrA Release build from this public tree | PASS |
| PIET native Release build from this public tree | PASS |
| CTest: morsel_determinism, thread_determinism, threads_budget | 3/3 PASS |
| OLS coefficients and standard errors vs independent Python calculation | PASS |
| Synthetic CSV → DTA save → reload → repeated OLS | PASS |
| Withheld functions and unknown command return errors | PASS |
| Native UI journey with synthetic sample | GREEN, 24 reported steps including final journey |
| JavaScript syntax check with Node | PASS |
| Gitleaks 8.30.1, default rules, redacted staged-source scan | No findings |

Reproduce build and public tests with `sh scripts/build-macos.sh`.
For the native UI journey (requires a graphical macOS session):

```sh
COBRA_BIN="$PWD/build/cobra" ./piet/native/build/piet_native --ui-smoke
```

A path-free UI result summary is in `tests/ui-smoke-result.json`. That journey
checks rendering, editor interactions, file save/read and bridge routing; its
PNG check validates computed paint styles, not a pixel-decoded exported image.

Builds emit pre-existing compiler warnings. Passing these checks does not imply
warning-free code, complete numerical coverage, full Stata compatibility,
security certification, or legal clearance. There are no certified Windows or
Linux artifacts, and no signed/notarized installer in this source beta.
