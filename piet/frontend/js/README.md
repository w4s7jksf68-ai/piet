# Frontend maintenance

`app.js` composes controllers and boots the DOM/native bridge. The `create*Controller`
modules own their own mutable state. They receive only the service/callbacks they use;
they do not import `app.js` or share a mutable application-context bag. Service modules
(`console.js`, `panels.js`, `find.js`, `gutter.js`, `grid.js`, `api.js`, `status-flash.js`)
hold module-level state of their own. `panels.js` and `gutter.js` are re-imported with
cache-busting queries by the DOM runner, so their state must stay in those files.

| Feature | Owner |
|---|---|
| Do-file contents, selection, editing and dirty state | `editor.js` |
| Single-line input and command history navigation | `command-input.js` |
| Command/do execution, busy state, dataset mutation and file routing | `execution.js` |
| Progress polling/events, cancel control and dropped-file events | `run-progress.js` |
| Kernel input requests and completion dialogs | `request-dialog.js` |
| Pane composition, split ratios and view tabs | `layout.js` |
| Appearance, settings cache/consent and native UI-state persistence | `settings.js` |
| Recent-file storage/order/menu | `recent-files.js` |
| Gallery navigation and viewing | `graphics-gallery.js` |
| File exports, clearing results and image conversion | `file-export.js` |
| Top-level keyboard/menu navigation | `app-navigation.js` |
| Result-block summaries and command text helpers | `result-blocks.js`, `input-text.js` |
| Results console: append, scrollback cap, fold, copy bar, per-block DOM | `console.js` |
| Scrollback/gallery byte and block budgets | `console-budget.js` |
| Coefficient table and model header/footer text | `estimation-format.js` |
| correlate / summarize / tabulate / describe / esttab text | `table-format.js` |
| Plain-language error hint line | `error-hints.js` |
| Graph block store for the gallery, disk spill | `graphics-gallery-store.js`, `graph_spill.js` |
| Graph block to SVG | `graph.js` |
| Bridge client, routing keys, version cache | `api.js`, `api-error.js` |
| Demo-mode fixtures and mock commands | `api-mock.js` |
| Data browser grid, cell display formats | `grid.js` |
| Variables pane, detail card, History pane | `panels.js`, `varstats.js` |
| Pane resizers, find bar, editor gutter, number/string helpers | `panes.js`, `find.js`, `gutter.js`, `util.js` |
| About/legal overlay, status flash | `about-panel.js`, `status-flash.js` |

Callbacks referencing a later controller are intentionally deferred until interaction.
Do not invoke those callbacks while constructing an earlier controller. `getClient`
and `getGrid` similarly defer access until boot has installed those services.

Run `node piet/frontend/test/dom_runner.mjs` from the repository root. For controller
wiring/native restoration changes, also run the actual native `--ui-smoke` journey.
