# PIET + CoBrA — experimental public beta

PIET is a local desktop interface for econometric analysis. CoBrA is its C++
computation engine. The application accepts a subset of Stata-style commands
and reads and writes supported data formats without requiring Stata at runtime.

**PIET 1.0 Beta 1.** This release is for evaluation and bug reporting.
Compatibility is incomplete. Validate statistical results independently before
using them in research or decisions.

## Download for macOS

[Download PIET 1.0 Beta 1 for Apple Silicon (.dmg)](https://github.com/w4s7jksf68-ai/piet/releases/download/v1.0.0-beta.1/PIET-1.0.0-beta.1-macos-arm64.dmg)
and drag **PIET.app** to Applications. This build is ad-hoc signed, but is not
Apple Developer ID signed or notarized. macOS may block the first launch; use
[Apple's Open Anyway instructions](https://support.apple.com/102445) if you
trust this download. The app requires macOS 13.3 or newer; only macOS 26.6.2
on Apple Silicon was tested. See the [release page](https://github.com/w4s7jksf68-ai/piet/releases/tag/v1.0.0-beta.1)
for the SHA-256 checksum and corresponding source.

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

## Published Beta 1 scope

- Desktop interface, native command processing, CoBrA computation engine.
- Local build and self-contained public tests; no licensed reference software
  or third-party research datasets are required by those tests.
- `egen concat()`, `egen cut()`, and `bcal create` are unavailable in this
  first public beta pending implementation-provenance clearance. They report
  an explicit error. Existing business-calendar reading remains available.
- The test suite covers a bounded set of numerical and data-operation checks.
  It is not a certification of every accepted command or of full Stata parity.
- PIET reports `1.0.0-beta.1`. CoBrA reports its own engine version `0.41.0`.
  The public Git tag identifies the exact release; see [release notes](RELEASE_NOTES.md).

## Unreleased source changes

The development source restores `egen concat()`, `egen cut()`, and `bcal create`
with independent replacement implementations. These changes are not included in
the Beta 1 download above. Labels used by `concat(), decode` and created by
`cut(), label` are carried between the native interface and engine.

The public tests use synthetic examples and do not require Stata. Their new
validation results are pending; the historical results in [VALIDATION.md](VALIDATION.md)
apply to the original source beta. See [the behavior and provenance record](docs/RESTORED_FUNCTIONS.md)
for the implementation approach, verification command, and current limits.
In particular, `bcal create, personal` and `generate()` combined with `if` or
`in` remain unsupported. An extended calendar range must keep endpoints on
weekdays represented in the source data and satisfy the maximum-gap limit.

## License and acknowledgments

Project code: **GPL-3.0-or-later**, [LICENSE.txt](LICENSE.txt).
Bundled dependencies retain their respective licenses; see
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and [NOTICE.txt](NOTICE.txt).
PIET and CoBrA are not affiliated with or endorsed by StataCorp.

Please report reproducible problems through GitHub Issues, using synthetic data
and the public release tag. Do not upload credentials or private datasets.
See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).
