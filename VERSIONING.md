# Versions

PIET (the application) and CoBrA (the engine) carry separate version strings.

- **PIET** uses a letter line: `A.y.z` is the official release line. `A.0.0`
  (2026-11-05) is the first official release; `A.0.1` (2026-11-06) is its first
  patch cut, `A.0.2` adds the Windows build, and `A.1.0` (2026-11-18) is the
  first minor release of the line (new capability; nothing removed). Patch cuts
  are `A.y.z`, minor releases `A.y.0`. `A.1.1` adds Linux downloads; the
  engine remains CoBrA `1.1.0`. `A.2.0` expands compatibility and ships CoBrA `1.2.0` with Contract A `1.28.0` and Contract B `2.12.0`.
  Earlier `B.y.z` versions were beta cuts and `2.y.z` the MVP line.
- **CoBrA** uses ordinary semantic versions (`1.0.0` at PIET A.0.0, `1.0.1` at A.0.1, `1.0.2` at A.0.2, `1.1.0` at A.1.0).
- `piet_native --version` prints the compiled PIET version; About shows the
  engine version separately.

The public tag `v1.0.0-beta.1` (2026-10-12) identifies an earlier experimental
source snapshot published under a different naming scheme. It remains
historical; it is not part of the `A` line and is not moved.
