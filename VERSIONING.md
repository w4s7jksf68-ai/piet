# PIET public versions

The public product is **PIET**. Its first named public prerelease is
**PIET 1.0 Beta 1**, with version string `1.0.0-beta.1` and Git tag
`v1.0.0-beta.1`.

`piet/native/CMakeLists.txt` defines the compiled application version in
`PIET_VERSION_VALUE`. The API and `piet_native --version` return that complete
string. About formats it as the product name and shows the CoBrA engine
version separately. The engine keeps its own version sequence.

Further beta cuts use `1.0.0-beta.2`, `1.0.0-beta.3`, and so on. A release
candidate uses `1.0.0-rc.1`; a stable release uses `1.0.0`. Advancing a name
does not by itself certify readiness. Build, packaging, and release records
must refer to the same source commit and compiled version.

The earlier `beta-2026.10.12.1` tag identifies the original source snapshot.
It remains historical and is not moved to the newly named version.
