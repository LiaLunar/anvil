# The script errors panel

A window of Anvil's overlay for modders: the messages of the game's `error.log`, grouped by
kind, with their counts as they happen. It is part of the standard plugin
`anvil-log-throttle` (design §4), off by default, and reads the plugin's own table of
messages rather than the log file. The table is also a service for other plugins,
`anvil-log-throttle.messages` ([`anvil_log_throttle.h`](../include/anvil/anvil_log_throttle.h),
`anvil::log_throttle` in `anvil-sdk`).

The layout comes from the prototype of
[eu4-engine-lab#11](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/11) (three variants
tried on a real Anbennar log of 19 649 lines and 18 831 messages); the reasons are below.

## Turning it on

- The setting **Script errors panel** (`panel`, default off, applies at once) of
  anvil-log-throttle, in the overlay's settings tab or the Manager's.
- Turned on in the game: the table starts at that moment and the window opens. The panel says
  that earlier lines are not counted.
- On at the start: the table starts when the plugin loads, before the game opens its logs, so it
  holds every line of the session, and the window opens with the main menu. Once closed, its
  switch is in the **Status** tab of the Anvil panel, like every plugin's window.
- Off: the window hides and is not refreshed. A table already started keeps counting until the
  game closes (nothing is unloaded in API 2).
- The window needs `anvil-overlay` (an optional dependency) and, for its buttons,
  `anvil-game`, as every overlay panel does. On OpenGL nothing is drawn.

## What it shows

From the top:

1. **Messages**: how many, and how many lines they have in all; **Show only new (N)** and
   **Mark all seen**, the loop of a modder: change, reload, what is new. A message is new while
   its count is above the count it had when everything was marked seen; a heading counts its new
   items, and an item starts with `*`.
2. One heading per kind, with its number of messages and lines, in this order:
   - **With a file and line**: the messages whose text names a file and a line (load errors of
     `.gui` and `.txt` files). Open at the start, since only these can be opened at once.
   - **Script: triggers and effects** (`Script error!`, unknown tags).
   - **Undefined event targets**: one item per target, with the lines of the script and of the
     tooltips together.
   - **Localisation**, **Interface (.gui, .gfx)**, **Graphics and fonts**, **Other**.
   - **Files the game could not open or check**, last: one item per kind of line (`Could not
     open file`, `Invalid file when updating checksum`), which opens to the list of files.
3. Inside an open heading, its items by count, the largest first, at most 30. A click opens an
   item's details:
   - the text, and its count: lines written and lines counted by the throttle;
   - the game's code that wrote it, `[file.cpp:line]`: the game's C++ source, not a mod's file;
   - the file and line, when the game names them, with the layer of the playset that has the
     file (the last of the game folder and the enabled mods of `dlc_load.json`), **Open in VS
     Code** (`vscode://file/<path>:<line>`, opened by Windows) and **Copy the path and line**;
   - for an event target or an unknown tag, **Find '<name>' in the playset's script**: a search
     of `events`, `common`, `decisions`, `missions` and `history/countries` of every layer for the
     name as a whole word, outside comments. The lines that save it as an event target come
     first, then other uses, each a button that opens the file at its line.

The panel's own words come in the overlay's language, from its own table
(`plugins/anvil-log-throttle/src/texts.cpp`), since `anvil-overlay.panel` translates nothing and
draws the glyphs of other scripts from Windows' fonts. The texts of the game and of mods are
shown as they are: UTF-8, or Windows-1252 converted.

## Why this layout

The prototype showed a ranked list (A), headings by kind (B) and headings by file (C). The panel
is B with C's first group, as the prototype's comment recommended, for these reasons:

- **The game rarely names a file and line.** In the Anbennar log it did so in 3 lines of 19 649,
  all while loading (`.gui`, parsing errors). Errors at run time (`Script error! … wrong scope`,
  `Undefined event target`) name only the game's C++ line. A layout by file (C) leaves almost
  everything in "no file". The few messages with a file and line come first, open, because they
  are the only ones a click opens in the editor.
- **The start-up noise dominates B by number of messages.** 18 740 of the 18 831 messages were
  `Could not open file` / `Invalid file when updating checksum` pairs for Anbennar's `history/`
  files, one line each. Folded into one item per kind of line and put last, they no longer push
  everything else down; their heading still says how many files.
- **Find works for event targets, not for triggers.** `destination_province` gave 339 uses in 24
  files with its `save_event_target_as` on top, in 0.9 s; a trigger such as `has_country_flag`
  is used in thousands of places, so its list would be noise. Find is offered only for event
  targets and unknown tags. Leading a run-time error to the event or decision that caused it
  needs a hook of the game's trigger evaluation, which is later work (with a script profiler).
- **Counts as they happen.** The prototype read the file every 2 s and got the throttled counts
  only with the reports, a minute late. The table is filled where the throttle sees each line.
- **Panel v1 is enough.** Collapsed, the headings fit one window at 1280×720; a filter field or
  a table waits until modders ask.

Not in it, on purpose: clearing the panel at a save load, a count of new errors in a
notification, a filter, an editor other than VS Code, a link to Etch.

## The table and the service

- Filled in the throttle's write hook, after the throttle's verdict, only for the logs the
  `logs` setting names. A message is its log, the game's source file name and line, and the text
  without its line end; `written` counts the lines that reached the log, `counted` those the
  throttle only counted. Once the log is closed (or after the next report), error.log gives the
  same count: the plain lines plus the repeats of the reports.
- Kept only from `keep()` on. Until then the hook pays one atomic load per line. Once kept, a
  line costs a hash of its text and a lookup under the table's lock; a new message also the copy
  of its strings. At most 32 768 messages (the Anbennar log had 18 831); lines of further
  messages count only in `untracked`.
- Strings never move and are never freed, so the service's pointers stay valid until the
  process ends, as API 2 requires.
- The panel's thread wakes once a second. While the window is shown and the table's version has
  changed, it reads only the messages whose version is newer, regroups, and hands the overlay
  at most 1 000 rows. A hidden window costs that wake-up and nothing else; with the setting off
  there is no thread.

## Tests

- `anvil_log_throttle_tests`: the table (keep, counts, versions, positions, a full table, stable
  strings, Windows-1252), the panel's model (kinds from the game's texts, the headings and their
  order, the merged event target, the folded missing files, new and seen, the limits of
  `anvil-overlay.panel`), that every text the panel uses has every language with the same
  placeholders, the playset's layers and Find on a folder of files, and the service end to end
  through the plugin's write hook, with the count compared to the lines and reports the log gets.
- The Rust wrapper against a stand-in service (`cargo test -p anvil-sdk`).
- In the game: see the pull request that added the panel.

## Translations

The panel's words were drafted by the agent, in every language; each waits for a native
speaker's check (for Polish, the maintainer's). The `panel` setting's label and description are
translated like the plugin's other settings, in `plugins/anvil-log-throttle/translations/`
(eu4-engine-lab#113).
