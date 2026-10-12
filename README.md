# PIET + CoBrA — experimental public beta

PIET is a local desktop interface for econometric analysis. CoBrA is its C++
computation engine. The application accepts a subset of Stata-style commands
and reads and writes supported data formats without requiring Stata at runtime.

**Source beta, 2026-10-12.** This release is for evaluation and bug reporting.
Compatibility is incomplete. Validate statistical results independently before
using them in research or decisions. No signed installer is included.

## Build on macOS

Requirements: Xcode Command Line Tools (C++17), CMake 3.20+, Python 3.
The build uses Apple's Accelerate framework and native WebKit.
From a fresh clone:

```sh
sh scripts/build-macos.sh
COBRA_BIN="$PWD/build/cobra" ./piet/native/build/piet_native
```

Keep the repository's `piet/frontend` and sample files alongside the build.
The command above builds both executables and runs the included tests.
Other platforms have source branches but are not certified by this beta.

## Try it

In the application, choose **File → Open sample dataset**, or enter:

```stata
sysuse synthetic_panel
summarize fatal beertax unemp
regress fatal beertax unemp, robust
```

The sample is entirely synthetic. Its names do not describe real observations.
You can open your own CSV or supported Stata dataset through the file menu.
Run only scripts you trust: commands can read and write local files and invoke
system programs with your user permissions.

## Beta scope

- Desktop interface, native command processing, CoBrA computation engine.
- Local build and self-contained public tests; no licensed reference software
  or third-party research datasets are required by those tests.
- `egen concat()`, `egen cut()`, and `bcal create` are unavailable in this
  first public beta pending implementation-provenance clearance. They report
  an explicit error. Existing business-calendar reading remains available.
- The test suite covers a bounded set of numerical and data-operation checks.
  It is not a certification of every accepted command or of full Stata parity.
- Internal version strings are inherited from the development snapshot.
  The public Git tag identifies this release; see [release notes](RELEASE_NOTES.md).

## License and acknowledgments

Project code: **GPL-3.0-or-later**, [LICENSE.txt](LICENSE.txt).
Bundled dependencies retain their respective licenses; see
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and [NOTICE.txt](NOTICE.txt).
PIET and CoBrA are not affiliated with or endorsed by StataCorp.

Please report reproducible problems through GitHub Issues, using synthetic data
and the public release tag. Do not upload credentials or private datasets.
See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).
