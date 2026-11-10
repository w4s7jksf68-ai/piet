# Graph maintenance

This directory builds graph blocks (the JSON the frontend draws), the sersets kept
beside them, the SVG form of a graph, and the files `graph export` writes. Start with
the command or output in the table; `dispatch/route_graphics.cpp` is where a typed line
reaches these handlers, and the entry points are declared in
`include/piet/handlers/graph.h`.

| Command or symptom | Owner |
|---|---|
| `scatter`, `graph line`, `xtline`, `tsline`, `marginsplot` blocks | `commands.cpp` |
| Parenthesised `twoway (...) (...)`: layers, fits, functions, `scatteri`, legend block | `twoway.cpp` |
| Splitting `twoway` text into layers, global options, `if`, `in` | `twoway_parse.cpp` |
| Stata option to style translation: colors, line width/pattern, marker symbol/size | `twoway_opts.cpp` |
| `histogram`, `hist`, `kdensity`, `twoway__histogram_gen`: `by()` panels, `gen()`, `r()` | `distribution_commands.cpp` |
| Histogram binning, kernel functions, kernel density values, default bandwidth | `distribution_math.cpp` |
| `graph bar`, `hbar`, `box`, `hbox` and their serset | `graph_bar.cpp` |
| `coefplot` grammar (subgraph, plot, model, option table) | `coefplot_syntax.cpp` |
| `coefplot` data pipeline, `generate()`, `r()` (`runCoefplotJann`) | `coefplot.cpp` |
| Older `runCoefplot` block built from stored estimates (also used by `event_plot`) | `coefplot_estimates_block.cpp` |
| `binscatter`: command and files, option vocabulary, `e(sample)` and replayed regressions | `binscatter.cpp`, `binscatter_syntax.cpp`, `binscatter_ereturn.cpp` |
| Twoway block shared by `rdplot` and `binscatter` | `graph_point_layers.cpp` |
| `grc1leg2`, `cdfplot`, `event_plot` | `grc1leg.cpp`, `cdfplot.cpp`, `event_plot.cpp` |
| Helpers shared by those three and `geodist` | `graph_extension_support.cpp` |
| `graph save`, `graph use`, `graph combine`, `graph export` entry, `saving()` option | `files.cpp` |
| `graph describe`, `copy`, `rename`, `display`/`draw`, `graph dir` | `named_graphs.cpp` |
| Graph name(), registration, describe record, scheme sizes, sersets a graph owns | `graph_registration.cpp` |
| `serset` command family | `serset_command.cpp` |
| Serset store, column constructors, data-plot serset builder, `.gph` serset JSON | `serset_store.cpp` |
| SVG drawing of every block kind (`buildGraphSvg`) | `svg.cpp` |
| Export formats: dispatch, SVG reader, PDF/EPS encoders, PNG/JPEG encoders | `graph_export_formats.cpp`, `graph_export_svg_reader.cpp`, `graph_export_vector.cpp`, `graph_export_raster.cpp` |
| Console-only point thinning for large scatter and line blocks | `view.cpp` |
| Option and column helpers shared by the command files | `graph_support.cpp` |

Private headers. `graph_support.h` (helpers in `graph_detail`) is included by the
command, twoway, distribution and file sources; `distribution.h` by the two
distribution files and `twoway.cpp`. `serset_internal.h` serves the serset sources,
`graph_registration.cpp` and `named_graphs.cpp`. `coefplot_syntax.h` joins
`coefplot_syntax.cpp` and `coefplot.cpp`; `binscatter_internal.h` joins the three
`binscatter` sources. `graph_export_scene.h` is the display list passed from the SVG
reader to the encoders. `graph_extension_internal.h` is also included by
`utility/geodesic_distance.cpp`, and `graph_point_layers.h` by
`estimation/regression_discontinuity.cpp`; changing either affects that file.

State. Graphs live in `SessionState`: `lastGraph`, `namedGraphs`, `graphMeta`,
`currentGraph` and `sersets`; `graph_registration.cpp` writes them together. A serset
belongs to the graph that made it, takes the lowest free id of 0 to 1999 and is dropped
with its graph; one over 8 million cells keeps only its shape. Sersets are never
written into the emitted block. `view.cpp` runs after the full block is copied into
`lastGraph` and `namedGraphs`, so it may thin only the console copy; export and save
must see every point. `svg.cpp` formats numbers through one file-static buffer, so it is
not reentrant. `grc1leg.cpp`, `cdfplot.cpp` and `event_plot.cpp` register their option
vocabularies once through `registerOptVocab`; an accepted option must be read.

Compile flags. `-ffp-contract=off` is set (in `piet/native/CMakeLists.txt`) only for
`graph_support.cpp`, `commands.cpp`, `distribution_math.cpp`, `distribution_commands.cpp`,
`twoway.cpp`, `svg.cpp`, `files.cpp`, `view.cpp`, `twoway_opts.cpp` and
`coefplot_estimates_block.cpp`; no graph file has an in-file pragma. The CMake comment
gives the reason: bin and grid edges such as `xmin + step*i`, and the coefplot
confidence bounds `b +/- z*se`, must round the multiply and the add separately as numpy
does. A file keeps its flag if it moves or is split; the other files here, including
`graph_bar.cpp`, `coefplot.cpp` and `cdfplot.cpp`, are not on the list.

Tests. `selftest_graphics.cpp`, `selftest_graphics_bar_and_coefplot.cpp`,
`selftest_graphics_binscatter.cpp` and `selftest_graphics_histogram_kdensity.cpp` cover
this directory. Elsewhere: browser drawing is `piet/frontend/js/graph.js`, not `svg.cpp`;
`lpoly`/`lowess` (tested by `selftest_graphics_lpoly_lowess.cpp`) are in
`estimation/lowess_lpoly_smoothing.cpp`, `rdplot` in
`estimation/regression_discontinuity.cpp`, `geodist`/`geonear` in
`utility/geodesic_distance.cpp`.
