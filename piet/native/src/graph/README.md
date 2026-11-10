# Graph maintenance

Start with the affected command or output, rather than reading every graph file.

| Change | Owner |
|---|---|
| Scatter, line, time/panel and margins graph blocks | `commands.cpp` |
| Histogram, density and distribution diagnostics | `distribution_commands.cpp` |
| Distribution grids, kernels and numerical helpers | `distribution_math.cpp` |
| Twoway layers, fits and composition | `twoway.cpp` |
| SVG serialization and geometry | `svg.cpp` |
| Save, combine and export files | `files.cpp` |
| Live-view point thinning | `view.cpp` |
| Shared option/column handling | `support.cpp` |

`support.h` and `distribution.h` are private declarations for these owners.
Graph storage remains in the existing session/graph store; rendering must not add
another mutable graph store. Command dispatch stays in `dispatch/route_graphics.cpp`.
Frontend rendering remains separate from native SVG serialization.

All sources retain the original `-ffp-contract=off` setting: moving histogram edges
or confidence intervals must not change floating-point rounding. Validate with the
existing graphics selftests, output/file comparisons and the native UI journey.
