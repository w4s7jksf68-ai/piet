# PIET compatibility policy

PIET will never distribute Stata's example datasets. `sysuse auto`,
`sysuse lifeexp`, `sysuse census` and `sysuse citytemp` return r(601).
`sysuse dir` lists PIET's sample data. `use` can read datasets supplied
separately by the user.

`require` returns r(601) for an unavailable package, including requests
with `, install`. Available native commands have PIET's own versioning.
PIET performs no package downloads.

The console and plain-text log wrap long `display` output at
`c(linesize)`, using `> ` for continuation lines. The structured result
retains the complete display string.

Compatibility checks compare deterministic results against the recorded
reference. They omit internal graph `r(name)`, dates and times from
`graph describe`, elapsed times from `timer` and `geonear`'s `r(t56)`,
and `zipsave` byte counts and ratios that depend on serialization.
Return codes, timer counts, actual graph metadata and archive member
counts still matter.

PIET targets the numerical optimum. Differences caused by Stata's
optimizer stopping earlier can remain visible in strict comparisons.

## Review in the next patch

| Item | Current behavior and follow-up |
| --- | --- |
| `drawnorm` eigenvectors | Signs in two-variable `r(L)` follow the recorded convention. Check larger and repeated-eigenvalue cases, preserving covariance and seeded behavior. |
| Temporary variables | Start at `__000000`, use uppercase base 36 and skip existing columns. Check allocation across nested scopes, frames and temporary-name commands. |
| Unknown graph schemes | `set scheme` reports r(111) for an unavailable scheme. Review discovery of user-supplied schemes. |
| `signrank` with one observation | The exact probabilities are all 1, following the recorded Stata result. Review edge cases with ties and zero differences. |
| `esttab` abbreviations | Bare `non`, `nob` and `obs` are accepted without altering the table. Check the abbreviation rules against neighboring spellings. |
| `datasignature` | PIET tracks data changes with its own deterministic fingerprint. A reference-compatible checksum still needs a reproducible specification and evidence across types and storage formats. |
