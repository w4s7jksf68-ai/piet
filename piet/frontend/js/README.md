# Frontend maintenance

`app.js` composes controllers and boots the DOM/native bridge. Feature controllers own
their own mutable state. They receive only the service/callbacks they use; they do not
import `app.js` or share a mutable application-context bag.

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
| Graphics collection and gallery navigation | `graphics-gallery.js` |
| File exports, clearing results and image conversion | `file-export.js` |
| Top-level keyboard/menu navigation | `app-navigation.js` |
| Result-block summaries and command text helpers | `result-blocks.js`, `input-text.js` |

Callbacks referencing a later controller are intentionally deferred until interaction.
Do not invoke those callbacks while constructing an earlier controller. `getClient`
and `getGrid` similarly defer access until boot has installed those services.

Run `node piet/frontend/test/dom_runner.mjs` from the repository root. For controller
wiring/native restoration changes, also run the actual native `--ui-smoke` journey.
