# PROTOTYPE: script errors panel (throwaway)

Branch `prototype/script-errors-panel`, never merged. It answers
happy-dwarf-team/eu4-engine-lab#11: what a script-error panel for modders in
Anvil's overlay shows, how it groups, how it leads to the file and line, and how
it connects to Etch.

Three variants in one panel (`anvil-overlay.panel` v1: text, button, separator);
**Next variant >** flips them:

- **A, worst first:** one list ranked by occurrences; a row opens its details.
- **B, by kind:** a heading per kind (script, event targets, missing files,
  interface, load errors, localisation, graphics) with totals; a heading opens.
- **C, by file:** files the game names with a line first (Open line N), then
  files the game could not open, then everything without a file, by name.

Details of a message: the full text, the game's C++ source line, the count
(written + counted by `anvil-log-throttle`), the file and line when the game
names one, resolved to a layer of the playset, **Open in VS Code**
(`vscode://file/<path>:<line>`), **Copy**, and **Find '<name>' in the playset's
script**, which lists `save_event_target_as` lines first, then uses.

It reads the log file every 2 s (no service of the throttle yet).

## Run

Without the game, the rows of every variant as text:

```
cmake --build build --config Release --target prototype_script_errors_dump
build/plugins/prototype-script-errors/Release/prototype_script_errors_dump.exe <error.log> <dlc_load.json> <user dir> <game dir>
```

In the game: install `build/plugin/prototype-script-errors` next to
`anvil-overlay` and `anvil-game`. Settings (`anvil/data/prototype-script-errors/settings.toml`):
`source` (a saved log to replay instead of the game's), `playset` (a
`dlc_load.json` to resolve files with), `variant` (0–2), `open_links`.
