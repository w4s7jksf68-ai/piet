# PIET 1.0 Beta 1 — v1.0.0-beta.1

This adds an Apple Silicon DMG to the first public source snapshot of PIET + CoBrA.
It has a new Git history containing only the public distribution tree.
No private development history, licensed reference datasets, reference-software
binaries, reference logs, or internal automation configuration are included.
Corresponding source: [public beta tag](https://github.com/w4s7jksf68-ai/piet/tree/v1.0.0-beta.1)
and the source archives on its GitHub release page.

## Changes for public distribution

- PIET now reports `1.0.0-beta.1` and shows **PIET 1.0 Beta 1** in About;
  CoBrA keeps its separate `0.41.0` engine version.
- Downloadable macOS app containing the native interface, CoBrA engine,
  synthetic sample, GPL text, and third-party notices.
- The DMG is ad-hoc signed and not notarized. macOS may require the user to
  allow the first launch in System Settings → Privacy & Security.
- Source build instructions and one-command macOS build/test entry point.
- Synthetic sample in place of a third-party research dataset.
- Original dependency licenses and a consolidated attribution file, including
  Howard Hinnant's public-domain civil-date algorithms.
- `egen concat`, `egen cut`, and `bcal create` implementations withheld pending
  provenance clearance; invocation fails explicitly. Calendar reading remains.
- Browser fixture mode disabled: statistical output requires native computation.
- Self-contained thread/determinism tests and independent OLS/data IO checks.

## Validation scope

See the GitHub Actions result for this exact public commit. The earlier source
snapshot's local checks are recorded in VALIDATION.md; this DMG's checks are
recorded on the GitHub release page. The original private reference corpus is
not part of this release and was not rerun as public-release certification.

No Developer ID signed or notarized installer is provided. The DMG is for
Apple Silicon, requires macOS 13.3 or newer, and was tested on macOS 26.6.2.
Older macOS versions, Windows, and Linux are not validated release targets.
Compatibility is incomplete;
passing the included checks does not certify all estimators or command syntax.
PIET reports `1.0.0-beta.1`; CoBrA reports `0.41.0` separately. Include the
public tag and commit when reporting issues.
