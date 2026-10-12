# Public beta validation

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
