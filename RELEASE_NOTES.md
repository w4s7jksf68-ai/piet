# Experimental public beta — beta-2026.10.12.1

This is the first public **source** snapshot of PIET + CoBrA.
It has a new Git history containing only the public distribution tree.
No private development history, licensed reference datasets, reference-software
binaries, reference logs, or internal automation configuration are included.

## Changes for public distribution

- Source build instructions and one-command macOS build/test entry point.
- Synthetic sample in place of a third-party research dataset.
- Original dependency licenses and a consolidated attribution file, including
  Howard Hinnant's public-domain civil-date algorithms.
- `egen concat`, `egen cut`, and `bcal create` implementations withheld pending
  provenance clearance; invocation fails explicitly. Calendar reading remains.
- Browser fixture mode disabled: statistical output requires native computation.
- Self-contained thread/determinism tests and independent OLS/data IO checks.

## Validation scope

See the GitHub Actions result for this exact public commit. The local release
checks are recorded in VALIDATION.md. The original private reference corpus is
not part of this release and was not rerun as public-release certification.

No signed or notarized installer is provided. Windows and Linux are not
validated release targets for this snapshot. Compatibility is incomplete;
passing the included checks does not certify all estimators or command syntax.
Internal PIET/CoBrA version output is inherited from development; use the public
tag and commit when reporting issues.
