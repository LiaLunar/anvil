# Anvil: design

Status: **draft 3, accepted 2026-10-19.** It specifies Anvil 1.0 with plugin API 2 and collects the decisions of [Map: Anvil 2 specification](https://github.com/happy-dwarf-team/anvil/issues/12). Each section links the ticket that holds the reasoning. How it is delivered is in §22. This document describes the target; the code reaches it through the pull requests of §22.3. Stage 1 (Anvil 0.2, API 1) is described by draft 2 in the git history.

Words in **bold** at first use are defined in [GLOSSARY.md](../GLOSSARY.md).

## 1. Goals

For players:
- Native plugins load into EU4 1.37.5 (Windows, 64-bit) **without modifying `eu4.exe` or any game file**. Removing Anvil returns the game to vanilla.
- An unknown game build, a broken plugin or a crash never prevents the game from starting, and the player is told what happened and why.
- Plugins that change the simulation never act in ironman or multiplayer unless both the author and the player allow it.

For plugin authors:
- A modder who already writes hooks ports one hook in an evening. Every game address is declared once, verified before load and typed in code. The author writes no register-save assembly, no byte profiles and no own proxy DLL.
- Mistakes surface early: a signature mismatch fails to compile, and `anvil check` finds byte, memory and conflict problems without starting the game.

For both:
- Anvil's own cost stays within published budgets (§17), and everything Anvil does is visible in its log.

Non-goals: Linux/macOS, 32-bit, other Paradox games, game builds other than 1.37.5, loading EU4DLL-style `plugins/*.dll` (EU4DLL is treated as legacy), a scripting layer for modders without C/C++.

## 2. Injection: proxy `version.dll` (unchanged)

`eu4.exe` imports `VERSION.dll`, which is not in `KnownDLLs`. Anvil ships as `version.dll` next to `eu4.exe`:
- it forwards the 17 exports to `%SystemRoot%\System32\version.dll`;
- `DllMain` checks that the process is `eu4.exe` and overwrites the executable's entry point with a jump to Anvil;
- at the entry point Anvil restores those bytes, runs the `early` phase and continues into the game.

This is verified for direct starts and through the Paradox launcher (EU4 engine lab, `docs/anvil-real-folder.md`).

## 3. Folder layout

```
<EU4>/eu4.exe
<EU4>/version.dll                      Anvil (proxy + core)
<EU4>/AnvilManager.exe                 player tool, also the installer (§15)
<EU4>/anvil/
    anvil.toml                         enabled, disabled_plugins, profile, log_level, allow_ironman/allow_multiplayer (§13)
    anvil.log, anvil.prev.log
    trust.toml                         approvals of plugins from mods (§14)
    crashes/<time>.json|.txt|.dmp      last 10 crash reports with minidumps (§12)
    plugins/<plugin-id>/               local plugins: trusted by placement
        plugin.toml  symbols.toml  settings.toml  translations/<code>.toml  <entry>.dll  (+ read-only files)
    data/<plugin-id>/                  writable per plugin; settings.toml holds the player's values
    data/anvil/approved/<id>/<hash>/   the approved copy a plugin from a mod loads from (§14)
    data/anvil/unfinished-starts.txt   starts in a row that ended before the game was ready (§10)
    data/anvil/crash-shown.txt         the last crash report the overlay announced (§15)
<mod folder of an enabled mod>/anvil/<plugin-id>/   plugins from mods, Workshop or local (§14)
```

## 4. Core and standard plugins

Decided in [What belongs in the Anvil core and what in library plugins](https://github.com/happy-dwarf-team/anvil/issues/17).

**Rule:** a feature belongs to the **core** only if it must work before any plugin loads or needs a view across all plugins at once.

Core:
- discovery, resolution and load order;
- build identity (the core's only game knowledge);
- symbol verification;
- the registry of changes and the chains;
- services and events, and its own services `anvil.control` (§15) and `anvil.benchmark` (the start-up benchmark, [docs/spec/benchmark-v1.md](spec/benchmark-v1.md) §5);
- the approval decision for plugins from mods;
- crash capture and attribution;
- suspending gameplay plugins (§13);
- plugin status (`anvil.status`) and the log.

The core never shows a blocking dialog.

**Standard plugins** are library plugins maintained by the project and released with Anvil. They load, fail and can be disabled or replaced like any other plugin; dependents of a missing one are skipped with the reason.
- `anvil-game`: game readiness, a frame event, running work on the main thread, a session event (ironman, multiplayer), the date's and the player's events, and read-only provinces and countries. Its C interface is [`include/anvil/anvil_game.h`](../include/anvil/anvil_game.h); details in §13.1.
- `anvil-overlay`: notifications, the panel and the settings UI (§15).
- `anvil-log-throttle`: a line that repeats in the game's logs is written a few times, then counted, with periodic reports of the count ([#172](https://github.com/happy-dwarf-team/anvil/issues/172)).
  - **How it is built** (`plugins/anvil-log-throttle`, `phase = "early"`, `affects_gameplay = false`, an optional dependency on `anvil-overlay` for its panel): every log file of the game (`logs/<name>.log`) is a file sink of one class (vtable `0x1D94BA8`, on GOG `CFileLogger`'s `0x1D7CF78`), whose write (slot `+8`, `0x1562BE0`) prefixes `[<file>:<line>]: ` and calls `PHYSFS_write` and `PHYSFS_flush` for each line. The plugin swaps that slot and the deleting destructor (slot 0); the game's router, which picks the sink by channel, counts the line and echoes it to a debugger, is untouched. A sink's path (file object at `+0x20`, path at `+0x08`, as the router reads it) decides whether its lines are throttled (`logs`, default `error.log`).
  - **The table of messages and the script errors panel** (1.20, [docs/script-errors-panel.md](script-errors-panel.md), eu4-engine-lab#11): once something asks for it (the `panel` setting at load or when turned on, or `keep()` of the service `anvil-log-throttle.messages` version 1, [`anvil_log_throttle.h`](../include/anvil/anvil_log_throttle.h)), the write hook also counts every line of a throttled log by message (log, the game's source file and line, text), written or counted, in a table that never forgets and is read with a version, so a reader gets only what changed. The panel, a window of `anvil-overlay.panel` that a thread of the plugin refreshes once a second while it is shown, groups the messages by kind: those with a file and line first, the start-up noise of missing files last and folded.
  - A message is the sink, the channel, the source file and line, and the text. Its first `repeats` lines (10) are written; the next one is replaced by `[anvil-log-throttle] logged 10 times; further repeats are counted and reported every 60 s: <text>`, and anvil.log names the message (the first 20). Further lines are counted; a count `report_seconds` (60) old is written as `[anvil-log-throttle] repeated <n> more times in the last <s> s: <text>` at the next line of a throttled log (looked for at most once a second), and the rest when the game deletes the sink at exit, with a line of totals per log in anvil.log. `full_log` writes every line. A crash or a killed process loses the counts since the last report.
  - At most 16 384 messages are tracked: a full table forgets the messages that are not counted yet, and once every tracked one is counted, new messages are written untracked. A line is counted only under a message with the same text, so a hash collision writes it. The game still builds every line (the trigger's stream, the log mutex) before the plugin sees it: the plugin saves the formatting and the write with its flush, not the evaluation.
  - It is not one of the plugins safe mode keeps (§10), and the Manager's start-up benchmark compares it like any other plugin.
  - **Decided** in [#172](https://github.com/happy-dwarf-team/anvil/issues/172) (1 November 2026):
    - It is enabled by default for `error.log`. A player or modder who wants every line sets `full_log`, or disables the plugin.
    - Readers of a throttled log (Tongs' `subscribe error_log`, and Crucible on top of it) see the written lines and the two notices, not every repeat. The notices keep the fixed formats above, which carry the counts. The plugin offers no service with its counts; one is added when Tongs or Crucible needs it.
- `anvil-map`: map modes from plugins, the service `anvil-map.modes` version 1 ([ADR 0007](adr/0007-plugin-map-modes.md), [docs/spec/map-modes-v1.md](spec/map-modes-v1.md); its C interface is [`include/anvil/anvil_map.h`](../include/anvil/anvil_map.h)). A plugin mode is a button on the minimap's list, a colour for each province and lines in the province's tooltip, on a number of the game's map mode enum it borrows for the process (80–83, then the debug modes 50, 57–60, 66, 68, 71 and 84).
  - **How it is built** (`plugins/anvil-map`, `phase = "main"`, `affects_gameplay = false`, depends on `anvil-game` and optionally on Mei for `mei.text`; 16 symbols, the lock committed and checked in CI). Five function hooks and one call redirect, each of which answers for a borrowed number and passes every other number to the game unchanged:
    - `UpdateMapColorProvinceBuffer` (`0x12F0B40`, GOG `0x12EACA0`) paints the plugin's last buffer as BGRA by province ID, bounded by the count the function itself uses (`map+0x6F0`, one `CProvince` of 0x2E10 bytes); where the buffer leaves a province without a colour, the game first paints the political mode (1) into it;
    - `GetMapModeNameOrDesc` and `GetMapModeSpriteName` answer the button's name, tooltip and sprite with the game's own `CString` constructor; the call of the latter in `CSettings::WriteMembers` answers `GFX_mapmode_notselected` for a borrowed number, which the writer skips, so `settings.txt` never keeps a plugin mode under a debug mode's sprite;
    - `CMinimapPanel::AddMapModes` gets a copy of the tab's array with the tab's plugin modes appended. `ShowMapModesList` calls it for the quick bar and then only for the tab shown (bits 1, 2, 4 and 8 of its argument, the arrays `tab_1` to `tab_4`), so the copy is made each time the player opens a tab, and a minimap rebuilt by Hamweard gets the buttons again;
    - `CProvince::SetupMapmodeTooltip` writes the plugin's kept lines (`CString::append` on `CToolTip::_Instant`) instead of the borrowed debug mode's, and notes which province was asked for.
  - **The step** is its handler of `anvil-game.frame`: it reads the session's current map mode (`session+0x2C`, GOG `+0x24`, from `SetMapModeDirty`'s bytes), publishes `anvil-map.hidden` and `anvil-map.shown` when it changes to or from a borrowed number, calls `SetMapModeDirty(number)` once after a new buffer of the shown mode, and calls the plugin's `describe` for the province the tooltip asked for. Painting and tooltips also run in frames nested in the tick, where `anvil-game.world` may not be read; the hooks read only what the step and `set_colors` left. `anvil-game.session` `ENDED` drops every buffer and kept text.
  - Text is UTF-8 from plugins, written unchanged when Mei's `mei.text` says the game's text is UTF-8, else in Windows-1252 with `?` for what it lacks. The service calls no game function that builds text, so it posts no command into the session and needs no suspension in multiplayer or ironman.
  - With `eu4agent.dll` (europagiga) in the process, which hooks the same functions and lends the same numbers, `anvil-map` installs no hooks when it finds it at load, passes everything on when it finds it at `anvil-game.ready`, and refuses plugin modes.
  - `examples/player-cores` is a plugin mode on `anvil-game.world` only: the player's cores, held or lost.
  - **Tried in the game** (lab run, 2026-11-09, GOG 1.37.5.2 and Steam 1.37.5): `player-cores` on number 84 in tab 4, picked from its button and from Tongs, painting the player's cores and saying "Your core, held by you" or "Not a core of yours" in the tooltip; buttons on all four tabs, painting from the buffer at speed 5 (14 paints in a session with 184 days at speed 5 and six mode changes: the game repaints only when a mode is marked dirty), tooltips with the plugin's lines on the name's line, `mapmode continent` showing the plugin mode that holds 50, the fourteenth registration refused, buttons and painting again after a new session, `$` literal and `§` written as `?`, and an empty button frame without a sprite. A year at speed 5 took as long with `anvil-map` and no plugin mode as without it (two runs each, within their spread), and the tab's list had no extra button. Not tried: a plugin mode on the quick bar and `settings.txt`, Bellows' frame pacing series, Etch's corpus, Mei's UTF-8 text, europagiga, GOG 1.37.5.0.
- `anvil-script`: gameplay plugins add effects of their own to the game's script language, which mods write in events, decisions and scripted effects ([ADR 0008](adr/0008-plugin-script-effects.md), [docs/spec/script-effects-v1.md](spec/script-effects-v1.md)), and are called back at the Day end, once a day after the game's day ([ADR 0009](adr/0009-day-end.md), [docs/spec/day-end-v1.md](spec/day-end-v1.md)); mods test whether a plugin acts with the game's `has_dlc = "anvil:<id>"` ([docs/spec/has-dlc-v1.md](spec/has-dlc-v1.md)). Its C interface is [`include/anvil/anvil_script.h`](../include/anvil/anvil_script.h).
  - **How it is built** (`plugins/anvil-script`, `phase = "early"`, `affects_gameplay = false`, no dependencies, 15 symbols per build, Steam and both GOG builds; each service, and has_dlc, needs only its own symbols, so a build without one set still gets the others): a function hook on the start of `CEffect::ReadMember` (`0x5DEC50`), the switch through which every container effect parses its members, compares the member's key (`CReader::_Toklhs`) with the registered keys. For one of them, with a value and the parser's token for a name the lexer does not know (`0xF`, seen in the game), it builds its own `CEffect`: the game's constructor, a copy of the game's vtable (19 slots) with the deleting destructor, `Assign`, `GetDesc` and `Execute` replaced, the parent's `CArray::Append`, the token. Every other member goes on to the game; a block or a word of the game on a registered key is left to the game, with one line in anvil.log per key and reason.
  - `register_effect` asks the core's `anvil.control` (`plugin_of`, appended in 1.16) who the caller is: only an `early` plugin with `affects_gameplay = true` registers, with keys `<its id>_<name>`, until the game parses its first effect. At each `Execute` and `GetDesc` it asks whether the owner is suspended (§13); a suspended owner's effects do nothing.
  - `Execute` passes the value and the scope to the plugin (the province and country of the game's `CEventScope`, read from its fields as the game's own effects read them: `GetScopeProvinceId` and `GetScopeCountryTag` give 0 and `---` for a plain scope [seen in the game]); `GetDesc` lets the base free the caller's prefix, then builds the plugin's line as the game's string with `CString::CString(const char*)`, in UTF-8 when Mei's `mei.text` says the game keeps UTF-8 and in Windows-1252 otherwise, by anvil-map's text rule (its `text.cpp`: literal, `§` becomes `?`, one line). The destructor runs `~CEffect` and frees the object from the plugin's heap.
  - **The Day end** (`anvil-script.day_end` version 1, 1.18): a function hook on `CGameState::HandleDailyTick` (`0x758430`, GOG `0x7575F0`), whose one caller is the day's command `CDailyTickCommand::Execute`, calls the game's day and then the subscribers, on the main thread, after every `ParallelFor` and the monthly and yearly updates of the day, before the command's `MakeSyncCheck`. A subscriber gets the date at `CGameState+0x1DD0` (advanced by 24 hours at the start of the day, the offset taken from `HandleDailyTick+0x91`) with the flags of a new month and a new year. Only a plugin with `affects_gameplay = true` subscribes (`plugin_of`, as for effects), in any phase, until the first Day end of the process, which fixes the order (subscription, so load order); a suspended owner's callback is skipped at each Day end. With no subscriber the detour calls the game's day and returns.
  - **has_dlc for plugin names** (1.20, anvil#316): a function hook on `CHasDLCTrigger::Assign` (`0xCC5BA0`, GOG `0xCC4350`) takes a name `anvil:<plugin id>`, interns the id, writes its index into the trigger's padding (`+0x6C`) and gives the trigger a copy of the game's vtable (16 slots) with `Evaluate` replaced by one atomic load. The answers are fixed on `anvil-game.session`: a plugin loaded and not suspended in a single-player session is true, everything else false, every plugin in multiplayer and outside a session. The game's own `Assign` logs nothing for a name it does not know and its `Evaluate` answers false, so without Anvil the same script is false and silent. It changes no file and no DLC list, so the game's checksum is the same, and in multiplayer every machine answers false, as one without Anvil does. Details in [has-dlc-v1.md](spec/has-dlc-v1.md).
  - **Tried in the game** (lab run, 2026-11-11, Steam 1.37.5 and GOG 1.37.5.2, single player): a lab mod event run from the console with a lab plugin loaded: `anvil:anvil-game` and the lab plugin's id true, an id not installed false, `NOT` of a true gate false, `anvil:Anvil-Game` false (not an id, the game's answer), a real DLC name still the game's; anvil.log lists the gates at the game's start and their answers at the session event. Without Anvil the same mod loads with no line about `has_dlc` in `error.log`. Not tried: multiplayer, ironman, a suspended plugin, GOG 1.37.5.0 in the game.
  - **Tried in the game** (lab run, 2026-11-10, Steam 1.37.5 and GOG 1.37.5.2, eu4-engine-lab#136): a lab plugin and `hello-script` subscribed in their loads, in load order. From the save of 17 March 1719 to January 1720 at speed 5: one Day end per day (293 on Steam, 300 on GOG), all on the main thread, each with the date `anvil-game.day` then reported and its count of days, the new month flag on every 1st and both flags on 1 January; the first Day end of a loaded session was the save's date plus one day; none in the menu, on a pause, or in a session in which no day passed. A subscription after the first Day end got `WRONG_PHASE`, and a plugin with `affects_gameplay = false` got `WRONG_KIND`. A year at speed 5 on Steam took 37.2 s and 33.8 s with `anvil-script` and no subscriber, 32.9 s and 34.2 s without it: within the spread of two runs. Not tried: suspension in ironman or multiplayer (unit tests only), GOG 1.37.5.0 in the game (its bytes are checked by `symbols.lock`).
  - It is not one of the plugins safe mode keeps (§10): a safe start parses no plugin effect and calls no Day end.
- `anvil-inspector` (1.21, [docs/inspector.md](inspector.md), eu4-engine-lab#155): for modders, a window of `anvil-overlay.panel` with the flags, variables and timed modifiers of the province whose view is open, of its owner and of the player's country, and the countries' rulers, heirs and consorts.
  - **How it is built** (`plugins/anvil-inspector`, `affects_gameplay = false`, required dependencies on `anvil-game` and `anvil-overlay`, no symbols): everything comes from `anvil-game.world`, read in main-thread work (`run_on_main_thread`) that the plugin's thread queues once a second while the window is shown. The work compares the session, the date, the province view and the player with the last read and reads the three objects again only when one of them changed, the window opened, or the player clicked Refresh; the rows are built from the copy on the plugin's thread. The `window` setting is off by default; then the plugin starts no thread and subscribes to nothing.
- `anvil-script-profiler` (1.22, [docs/script-profiler.md](script-profiler.md), eu4-engine-lab#158, #163): for modders, a window of `anvil-overlay.panel` that times the game's triggers and effects by their owner (event, event option, decision, on_action, casus belli, triggered modifier, mission) and role, and writes each profiling span to `logs/script_profile_<date>.csv`.
  - **How it is built** (`plugins/anvil-script-profiler`, `affects_gameplay = false`, a required dependency on `anvil-game` and an optional one on `anvil-overlay`): loaded and idle, with no change and no thread. A span, started from the window, puts in two detours as one group, `CAndTrigger::Evaluate` and `CEffect::Execute`, and removes them at its end; each game thread times its root blocks in a lock-free table of its own, and the owners come from member offsets and the owner classes' vtables in `symbols.toml`. Its own controls run on the main thread: clicks, days and sessions from `anvil-game`.
  - **Tried in the game** (lab run, 2026-11-12, Steam 1.37.5 and GOG 1.37.5.2, eu4-engine-lab#164): `0 change(s)` at every load; the window's switch, Start, Stop and Measure 30 days; owner names as the prototype's on vanilla and Anbennar; a load gave `interrupted_session`, Exit Game `interrupted_exit`; −1.7% days per second on vanilla and −1.0% on Anbennar during a span. Not tried: GOG 1.37.5.0 in the game, ironman, multiplayer.

Knowledge that only some plugins need (GUI tree, console, text) stays in those plugins and their services.

## 5. A plugin package

### 5.1 `plugin.toml` (manifest v2)

```toml
id = "hello"                        # [a-z0-9-]+, max 64, unique
name = "Hello"
version = "0.1.0"                   # SemVer
authors = ["Your Name"]
description = "..."
api = 2
anvil = ">=1.0.0"                   # optional SemVer range of Anvil
entry = "hello.dll"
phase = "main"                      # "early" | "main"
symbols = "symbols.toml"            # optional; its `build` replaces game_builds
settings = "settings.toml"          # optional
affects_gameplay = false            # REQUIRED (§13)
allow_ironman = false               # false | "opt-in"
allow_multiplayer = false           # false | "opt-in"
load_after = []
issues = "https://github.com/you/hello/issues"   # optional; shown with crash reports
repository = "https://github.com/you/hello"      # optional; shown when approving

[[dependencies]]
id = "anvil-game"
version = "^1"
optional = false

[[conflicts]]                       # this plugin is skipped while a listed one is enabled
id = "other-text-renderer"
version = "<2"

[[modules]]                         # optional, at most 32 (ADR 0011)
id = "drill_all"                    # [a-z][a-z0-9_]*, max 32, unique in the plugin
name = "Drill all selected armies"
description = "..."                 # optional
affects_gameplay = true             # REQUIRED, as for the plugin
allow_ironman = "opt-in"            # false | "opt-in"; only with affects_gameplay = true
allow_multiplayer = "opt-in"
default = true                      # on until the player chooses (anvil.toml)
```

**Modules** ([ADR 0011](adr/0011-plugin-modules.md)). A plugin that bundles features of different kinds declares each as a module and acts for it through `host->module(self, "<id>", &out)`, a handle with its own owner in the change registry; the plugin's own `affects_gameplay` covers what it does through `self`. §13 applies to each handle by its own flags, and anvil.toml names a module `<plugin>.<module>`.

Unknown keys are errors ([Offline anvil check CLI and plugin CI](https://github.com/happy-dwarf-team/anvil/issues/23)). `game_builds` and `[[expect]]` are gone: a plugin without `symbols` runs on every build; with `symbols` it runs only on that file's `build`.

### 5.2 `symbols.toml`

Decided in [One source of truth for game addresses](https://github.com/happy-dwarf-team/anvil/issues/18), extended by [Plugin authoring experience](https://github.com/happy-dwarf-team/anvil/issues/21) and [API 2 gaps from the paper port](https://github.com/happy-dwarf-team/anvil/issues/32).

```toml
build = "eu4-1.37.5-steam-win64"

[symbol.speed_object]                         # a function
rva = 0x8024D0
bytes = "48 8B 81 18 04 00 00 C3"             # hex pairs, ?? = any byte
memory = "code"                               # code | read | write
signature = "void* (void* session)"           # typed symbol in C/C++/Rust
note = "session vtable slot 0x100: returns the game-speed object"

[symbol.person_name_block]                    # a block for replace_block (Unicode Patch's person_name_hook)
rva = 0xA4B48C
bytes = "48 8D 55 B8 48 83 7D D0 10 48 0F 43 55 B8 4C 8B 45 C8 48 8B CB E8 4A 7E 64 FF"
memory = "code"
# resume = "<symbol>"                         # optional; default: the end of the block (0xA4B4A6 here)

[symbol.frontend_tick_slot]                   # a pointer slot
rva = 0x1D5A8E8
memory = "read"
points_to = 0x10D2E80

[symbol.root_and_date]                        # values taken from verified bytes
rva = 0x14321FC
bytes = "48 8B 05 75 DC F0 00 8B 98 D0 1D 00 00"
memory = "code"
values.root        = { at = 3, rip_relative = 7, type = "Root" }   # -> sym::root, Data<Root> at 0x233FE78
values.date_offset = { at = 9, size = 4 }                          # -> sym::root_and_date.date_offset = 0x1DD0

[symbol.physfs_get_write_dir]                 # by export name instead of RVA
export = "PHYSFS_getWriteDir"
signature = "const char* ()"
```

- **Kinds:** a symbol is a function (`signature`), a pointer slot (`points_to`), data (`type`) or a site (anything else).
- **Values:** they must lie on fixed bytes.
- **Digests:** decided in [ADR 0003](adr/0003-symbol-digests.md) (2026-11-04). A symbol can state `sha256`, `size` and optional `any` instead of `bytes`, so the file carries no bytes of the game. The digest is SHA-256 over `size` bytes with each `any` offset set to 0. `bytes` and `sha256` exclude each other per placement. A value's `is` states what `anvil gen` otherwise reads from the bytes. Needs Anvil 1.10; an older Anvil reports the key as unknown and skips the plugin. `bytes` stays valid for all of API 2.
- **Exports:** an `export` symbol is resolved through the export table; `bytes`, if given, check the prologue.
- **Schema:** unknown keys are errors. Names match `[a-z][a-z0-9_]*` (they become identifiers in generated headers), and a symbol's id is its position in the file. A symbol has exactly one of `rva` and `export`; one at an `rva` needs `memory` and `bytes` or `points_to` (a data symbol may have neither). `signature`, `points_to` and `type` exclude each other. A value has `at` and either `size` (1, 2, 4 or 8, little-endian) or `rip_relative`, the offset of the instruction's end, at least `at + 4`: it yields the RVA `rva + rip_relative + disp32`. `resume` names another symbol and needs `bytes`, the block it ends.

**Several builds.** Decided in [#160](https://github.com/happy-dwarf-team/anvil/issues/160) (2026-11-02). A file for more than one build lists them in `builds` instead of `build`, and keeps one declaration per symbol: what the symbol is (kind, `memory`, `signature`, `type`, `values`, `resume`, `note`, `export`) is shared, and only where it is (`rva`, `bytes`, `points_to`) may differ, in a `build."<id>"` table of the symbol:

```toml
builds = ["eu4-1.37.5-steam-win64", "eu4-1.37.5.2-gog-win64"]

[symbol.frame]                                # the same bytes, another address
bytes = "48 89 5C 24 08 48 89 6C 24 10"
memory = "code"
signature = "uintptr_t (void* app, uint8_t outer)"
build."eu4-1.37.5-steam-win64" = { rva = 0x14C25A0 }
build."eu4-1.37.5.2-gog-win64" = { rva = 0x14BC400 }

[symbol.speed_object]                         # other bytes: a table per build
memory = "code"
values.speed = { at = 3, size = 4 }           # 0x418 on Steam, 0x408 on GOG

[symbol.speed_object.build."eu4-1.37.5-steam-win64"]
rva = 0x8024D0
bytes = "48 8B 81 18 04 00 00 C3"

[symbol.speed_object.build."eu4-1.37.5.2-gog-win64"]
rva = 0x801850
bytes = "48 8B 81 08 04 00 00 C3"
```

- **Rules:** each of `rva`, `bytes` and `points_to` is given for every build or per build, not both; a `build` table may hold only these three and only for a build `builds` lists. Every build must satisfy the schema above on its own (an error names the build), and a symbol has the same kind in every build. A symbol whose keys are all shared, such as an export with the same prologue, needs no tables.
- **Ids and values:** a symbol keeps one id in every build, so plugin code does not change with the build. The loader selects the running game's build (§7); `anvil check --exe` the exe's (§16). `anvil gen` writes a value as a constant only when every build has the same one; a value that differs is read when the game loads (`symbol_value`, `Host::value`), as for an export's RVA.
- **Absent symbols.** Decided on 2026-11-03 ([#193](https://github.com/happy-dwarf-team/anvil/issues/193)): code that one store's build lacks, such as the Steam lobby the GOG build 1.37.5.2 does not have, is declared absent there rather than left out of the file or the build:

  ```toml
  [symbol.lobby_close]
  memory = "code"
  signature = "void (void* lobby)"
  rva = 0x1658D50
  bytes = "40 53 48 83 EC 20"
  absent = ["eu4-1.37.5.2-gog-win64"]
  ```

  - `absent` lists builds of `builds`; a file with one `build` has none, a symbol absent on every build is an error (remove it), and an absent build gets no `build."<id>"` table. The shared keys and the rules above apply to the builds the symbol is in.
  - On an absent build the symbol is not verified and has no address: `address_of` returns NULL, `symbol_value` and every change on it give `ANVIL_ERROR_NOT_DECLARED`, and the log names the symbol as absent. A plugin checks before it uses one: `host->address_of(self, SYM_X) != NULL` in C, `host.present(sym::x)` in C++. A `resume` must name a symbol that is in every build its block is in.
  - `anvil check --exe` lists the symbols absent on the exe's build; `anvil gen` names the absent builds in each symbol's comment, and a value is a constant when the builds the symbol is in agree.
  - Nothing is skipped silently: a symbol the file declares for a build must verify there, or the plugin is skipped as before. Only `absent` exempts it, and only on the builds it names.
  - Within API 2 ([ADR 0001](adr/0001-plugin-api-2-compatibility.md)): a new key, no new host field, and `address_of` already returned NULL for a symbol it cannot give. An older Anvil reads `absent` as an unknown key and skips the plugin, so a plugin that uses it declares `anvil = ">=1.9.0"`.
- **Why one file:** the issue weighed one file per build against this. Here the shared part, which is most of a declaration, is written once and cannot drift between builds, and `anvil check` still names the build of every problem. A build is added by adding it to `builds` and a table to each symbol whose address or bytes differ, which `anvil port` (§16) does; the second GOG build (1.37.5.0) came this way ([#187](https://github.com/happy-dwarf-team/anvil/issues/187)). If several plugins come to need the same per-build tables, the shared address library declined in §21 is the next question.

### 5.3 `settings.toml`

Decided in [Plugin configuration convention and the overlay settings panel](https://github.com/happy-dwarf-team/anvil/issues/35).

```toml
[setting.ram_prefetch_budget_mb]
type = "int"                  # bool | int | float | enum | string | key
default = 1024
min = 64
max = 8192
label = "RAM prefetch budget (MB)"
description = "RAM for unpacked archive members the game has not taken yet."
group = "RAM prefetch"
restart = true                # true: applies at the next start

[setting.mode]
type = "enum"
values = [{ name = "off", label = "Off" }, { name = "verify", label = "Verify only" }, { name = "on", label = "On" }]
default = "verify"
label = "Mode"
restart = false               # false: applies at once; the plugin may subscribe

[setting.panel_key]
type = "key"
default = "Ctrl+Shift+F12"
label = "Panel key"
```

- **Schema:** unknown keys are errors, as in the other files. Names match `[a-z][a-z0-9_]*` (max 64; they become identifiers in generated headers), and a setting's id (`AnvilSetting`) is its position in the file. `type`, `default` and `label` are required; `description` and `group` (empty: ungrouped) are optional, and `restart` defaults to `true`, the safe choice for a plugin that reads a value once. The overlay and the Manager show the ungrouped settings first, then each group once under its name, the groups in the order of their first setting in the file and the settings of a group in file order, so a group's settings need not stand together in the file.
- **Types and values:** `bool`; `int` (64-bit) and `float` (finite; an integer is accepted) with optional `min`, `max` and `step` (the editor's increment, greater than 0, not a rule for values); `enum` with `values`, an array of `{ name, label }` (the label defaults to the name), whose `default` and stored values are option names; `string` (UTF-8, no NUL); `key`, a binding such as `"Ctrl+Shift+F12"` or `""` for none. `min`/`max` belong to `int` and `float` only, `values` to `enum` only, and `default` must satisfy the limits.
- **Keys:** modifiers `Ctrl` (or `Control`), `Shift`, `Alt`, each at most once, then one key: `A`–`Z`, `0`–`9`, `F1`–`F24`, `Space`, `Enter`, `Escape`, `Tab`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, the arrows (`Up`, `Down`, `Left`, `Right`), `Pause`, `PrintScreen`, `CapsLock`, `NumLock`, `ScrollLock`, `Numpad0`–`Numpad9`, `NumpadAdd`/`Subtract`/`Multiply`/`Divide`/`Decimal`, and the US-layout punctuation keys `Semicolon`, `Equals`, `Comma`, `Minus`, `Period`, `Slash`, `Backquote`, `LeftBracket`, `Backslash`, `RightBracket`, `Quote`; names are case-insensitive. The value a plugin reads is the Windows virtual-key code in bits 0–7 with `0x100` Shift, `0x200` Ctrl and `0x400` Alt, as the Win32 hot-key control encodes it.

### 5.4 `translations/<code>.toml`

Decided in [eu4-engine-lab#113](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/113), [ADR 0010](adr/0010-plugin-translations.md). A **plugin translation** gives one language's words for the texts of the plugin the player reads in the overlay and the Manager: `description` of `plugin.toml` and the `label`, `description`, `group` and option labels of `settings.toml`. The plugin's `name` stays.

```toml
# translations/fr.toml
status = "draft"            # "checked": a native speaker checked it
[text]
"Panel key" = "Touche du panneau"
"Opens and closes the Anvil panel." = "Ouvre et ferme le panneau d'Anvil."
```

- **Files:** one per language, named with the codes of `ANVIL_LANGUAGE` (§15): `fr`, `de`, `es`, `zh-Hans`, `zh-Hant`, `ja`, `ko`, `pl`. English has none. `plugin.toml` does not name the folder, so an Anvil before 1.20 ignores it and the plugin needs no newer `anvil` range for it.
- **Schema:** `status` (required, `draft` or `checked`) and `[text]`, a table of English texts and their translations, both strings, neither empty, no NUL; any other key is an error. UTF-8, at most 16 MB.
- **Lookup by the English words,** as Anvil's own catalogs do: a text whose English words changed finds no translation and shows in English until the file follows. A text without a translation, or every text in a language without a file, is shown in English, without a mark.
- **The loader** reads the player's language's file of each resolved plugin and each plugin in a mod once at start, before any plugin loads. A file that does not parse is left out with a warning in `anvil.log`; the plugin loads as it would without it. The overlay gets the words through `anvil.control` (`plugin_text`, `mod_plugin_text`), the Manager reads the files itself.
- **Fonts:** Chinese, Japanese and Korean words of a plugin come from the fonts of Windows (§15), since Anvil's subsets cover only its own texts.
- **`anvil check`** (§16): a file that does not parse, a name that is not `<code>.toml` of those codes, and an entry no text of the plugin has are errors; a text without a translation and a missing language are warnings, errors with `--complete-translations`. `anvil check --game` leaves translations out.
- `anvil_add_plugin` (§16) copies `translations/*.toml` into the built plugin folder.

## 6. Game build identity

Anvil reads the PE header of the running image (TimeDateStamp, SizeOfImage, CheckSum, entry point) and maps it to a build id from a compiled-in table. The full SHA-256 is checked off the critical path and cached. With an unknown build, plugins with `symbols` are skipped with "game build unknown (EU4 updated?)", or in a GOG install (`goggame-2057001589.info` in the game folder) with "GOG build unknown: Anvil runs on {0}. If yours is older, update it in GOG Galaxy; if it is newer, wait for an Anvil update.", where `{0}` is the last GOG row of the table, so the words cannot go stale, and the overlay says so: it runs on every build. `anvil-game` is skipped with the rest, so there is no badge in the menu and the notice starts with the first frame, on the loading screen; it is one notice for all those plugins and the ones that need them, such as a plugin with no addresses of its own that depends on `anvil-game`, and it stays until the player closes it, whatever the notification time. A plugin with `symbols` loads only on a build its file declares (§5.2). When Anvil knows the build and a plugin does not declare it yet, which lasts from an Anvil release to that plugin's, the same holds: one notice, "N plugins wait for this game version", until closed, and an amber dot on the badge.

| Build id | Store | `eu4.exe` SHA-256 |
| --- | --- | --- |
| `eu4-1.37.5-steam-win64` | Steam | `9AD3EFE1…D771A` |
| `eu4-1.37.5.0-gog-win64` | GOG, 1.37.5.0 of 2024-10-03 | `5F4145CE…88EE` |
| `eu4-1.37.5.2-gog-win64` | GOG, 1.37.5.2 of 2025-11-13 | `D29CF745…5319` |

New ids carry the store and all four parts of the version, since GOG ships two builds of 1.37.5 ([#160](https://github.com/happy-dwarf-team/anvil/issues/160)); the Steam id keeps its name, which every plugin declares. The other GOG build, 1.37.5.0, is known since [#187](https://github.com/happy-dwarf-team/anvil/issues/187): its code is laid out as 1.37.5.2's (same sections, entry point and function table; about 1,700 bytes of code differ, in the custom nation commands, Nakama and GOG Galaxy code), so every symbol of `anvil-game` is at the same address. GOG rows stay oldest first, because the advice to a GOG player on an unknown build names the last one. The Microsoft Store and Epic builds are not planned (§21, [#36](https://github.com/happy-dwarf-team/anvil/issues/36)).

## 7. Verification before loading

For each plugin, before its DLL is loaded and before any plugin has changed anything, Anvil:
1. parses `plugin.toml`, `symbols.toml` and `settings.toml`; an unknown key or a schema error skips the plugin, and so does a `symbols.toml` that does not declare the running build; with several builds it takes that build's addresses and bytes (§5.2);
2. resolves `export` symbols;
3. compares every symbol with the image: `(actual & mask) == (expected & mask)`, the memory kind against the section flags, and `points_to` against the stored pointer;
4. computes `values`.

Bytes already changed by other plugins are compared in their original form, so two plugins can depend on the same function. A mismatch skips the plugin, logging the symbol, the RVA and the first differing byte. `anvil check` runs the same code offline (§16).

## 8. Changes

Decided in [Hook API 2: hook kinds, chaining and conflict policy](https://github.com/happy-dwarf-team/anvil/issues/19) and [API 2 gaps from the paper port](https://github.com/happy-dwarf-team/anvil/issues/32). Facts: [SafetyHook research](https://github.com/happy-dwarf-team/anvil/issues/14).

### 8.1 Seven kinds of change

| Kind | Call | Notes |
| --- | --- | --- |
| Function hook | `hook_function(sym, detour, &next)` | detour with its own `next` |
| Mid hook | `mid_hook(sym, callback, user)` | callback gets `AnvilCpuContext`; the original code runs afterwards |
| Block replacement | `replace_block(sym, callback, user)` | the declared block never runs; resume at `resume` or at the block's end |
| Call redirect | `redirect_call(sym, target, &next)` | `call rel32` sites |
| Byte patch | `patch_bytes(sym, bytes, size)` | expected bytes come from the declaration |
| Pointer swap | `swap_pointer(sym, value, &previous)` | atomic; also in read-only memory |
| Import replacement | `replace_import(module, function, replacement, &next)` | `eu4.exe` imports |

In addition, `address_of(sym)` gives a verified address to call or read.

Every call that touches `eu4.exe` takes a declared symbol, never a raw RVA. Raw pointers are accepted only **outside** the `eu4.exe` image (`hook_function_at` and friends, e.g. Direct3D 9 vtables). Those still go through the registry and chains, with an optional prologue check.

**Pointer swap.**
- It takes a slot symbol whose pointer still holds its `points_to`; the slot must be 8-byte aligned.
- The slot is written with one atomic store. In read-only memory, Anvil changes the page's protection only for that store, keeping it executable if it was.
- `previous` is a value, not code, so there is no stub to re-point. Instead Anvil keeps the plugin's `previous` location: it writes the next value of the chain there at the request and again, atomically, whenever the chain changes.
  - When an inner link leaves, the link outside it reads the following value through its `previous`.
  - Anvil stops writing to a location once its change is removed.
  - The location must therefore stay valid while the change lives (a global or a field of a long-lived object), and the plugin reads it each time it passes on, never a copy.
- `previous` may be NULL when the plugin does not pass on.
- When the last link leaves, the slot gets back the pointer it held before the chain began. A slot that the game rewrote while nobody swapped it starts the next chain from its current value.

### 8.2 Chains

- Anvil owns one SafetyHook hook per address and chains the plugins itself.
- Order follows the load order (dependencies, `load_after`): the earlier plugin is outermost. There are no priorities.
- Every link has its own `next`. Redirects and pointer swaps chain with `next` being the previous target or value; a pointer swap's `previous` follows the chain as §8.1 describes.
- Links can be removed in any order. Executable memory (stubs, trampolines) is never freed.
- At one address, mid hooks run first, then a block replacement or the function-hook chain or the redirected call.
- A mid hook that changes `rip` ends the chain; the first occurrence per address and session is logged.

### 8.3 Conflicts and unsafe sites

- A change whose bytes overlap another plugin's change at a different address is refused, and both owners are logged. A block replacement owns its whole block.
- The one exception: identical byte patches share ownership.
- A hook whose stolen bytes contain `JRCXZ` or `LOOP*` is refused, because SafetyHook mis-encodes them.

### 8.4 Staging, handles, suspension

- Changes requested during `anvil_plugin_load` are staged and enabled together after it returns `ANVIL_OK`; on failure none are enabled.
- Later changes are atomic one by one or as an explicit group.
- Every change returns a handle and can be removed. A failed load rolls back everything the plugin had.
- The core can **suspend and resume all changes of a plugin**: it bypasses its links, reverts its patches and restores its pointers (§13).
  - A suspended link keeps its place in its chain. Its own `next` (or `previous`) still leads to the rest, so a thread inside it passes on.
  - A byte patch shared with a plugin that is not suspended stays written.
  - Suspended changes keep their bytes reserved: a suspended block replacement still owns its block. Changes the plugin requests while suspended are enabled suspended.
  - Suspending and resuming are idempotent. The plugin alone gets `anvil.suspended` after its changes are out of the game and `anvil.resumed` after they are back, once per change of state, so it can stop and restart its threads.
  - Removing a suspended change or rolling the plugin back works as for any other change.
- All kinds are allowed in every phase; SafetyHook's page trapping covers threads.

### 8.5 `AnvilCpuContext`

- The layout is identical to SafetyHook's `Context64` (408 bytes): XMM0–15, RFLAGS, the GPRs, `rsp` (read-only), a reserved slot (`trampoline_rsp`) and `rip` (writable). It is pinned with `static_assert`s.
- YMM upper halves, x87 and MXCSR are not saved.
- Anvil runs a copy of SafetyHook 0.7's stub, so a later SafetyHook cannot change the ABI: one context save for all mid hooks at an address and the block replacement. One copy serves every address; each address enters it through its own 24-byte thunk that pushes the address's state as `rip`, which the dispatch loop replaces with the resume address before any callback runs.
- A lighter, volatile-only context is not offered (§17); the Unicode Patch port counts its text blocks' calls per frame to decide whether one is needed ([#104](https://github.com/happy-dwarf-team/anvil/issues/104#issuecomment-6407336807)).

## 9. Plugin API 2

The C ABI is [`include/anvil/anvil_plugin.h`](../include/anvil/anvil_plugin.h) (`ANVIL_API_VERSION 2`); plugins vendor it. API 1 is gone: no bridge, and the manifest accepts only `api = 2` (§22.2).
- **Host table:** log, `plugin_dir`, `data_dir`, game info (build id, image base and size, the SHA-256 once verified), the change calls of §8, `remove`, groups, `address_of`, `symbol_value`, services, events (`anvil.plugins_loaded`, `anvil.suspended`, `anvil.resumed`), and settings (§11). Anvil fills every field.
  - A change handle (`AnvilChange*`) is the registry's change id; ids are never reused, so a stale handle is `ANVIL_ERROR_NOT_FOUND`. While the process exits, `remove` does nothing.
  - Inside `anvil_plugin_load` the load is the plugin's group: `begin_group`/`commit_group` return `ANVIL_ERROR_WRONG_PHASE` there.
  - Results map the registry's one to one; `NOT_FOUND` and `ALREADY_EXISTS` (services) are the last two codes.
  - Events are named like services: a plugin publishes only `<its id>.<name>`, so no plugin can forge another's events (the core acts on `anvil-game.session`, §13); `anvil.` names are Anvil's. Any plugin may subscribe to any name.
- **Versioning:** the `size` and `api_version` fields keep it extensible; within API 2 functions are only appended (`ANVIL_HOST_HAS`) after the "end of API 2.0" line. The same contract covers every service (one **service version** only grows), nothing is unloaded in API 2, documented behaviour counts like layout, and the C++ and Rust layers stay source-compatible within an Anvil major version; an API 3 comes with a new Anvil major version that still loads `api = 2` plugins ([ADR 0001](adr/0001-plugin-api-2-compatibility.md)).
- **Generated headers:** `anvil gen` produces `symbols.gen.h/.hpp` (symbol ids, typed function symbols, values) and `settings.gen.h/.hpp` (typed accessors).
- **C++ wrapper** ([`anvil.hpp`](../include/anvil/anvil.hpp), header-only, C++20):
  - `ANVIL_PLUGIN(Type)` with `Type : anvil::Plugin`; a failing call in `load()` throws `anvil::Error` (the host's result and a message), the macro turns any exception into a result, and Anvil rolls back;
  - callables the wrapper adapts (mid hooks, block replacements, event handlers) must not throw: the adapter catches, logs the first exception per change and the game continues. Function-hook detours, redirect targets and import replacements are plain functions the game calls directly; they must not throw (`noexcept`);
  - `anvil::HookOf<sym::x>` (a `FunctionHook` whose `next` Anvil writes before enabling), `PointerSwap<T>` (keeps `previous` where Anvil can rewrite it); mid hooks and block replacements take any callable, kept for the process;
  - a signature mismatch (detour, call arguments, hook object) fails with a `static_assert`; each symbol has its own generated type (`sym::Types::<name>`), so the compiler's note names it;
  - `Host` is a stateless value (`anvil::Host{}` works anywhere once the plugin is loading); typed `call`, `function`, `data` (`Data<T>`, also typed `rip_relative` values) and `value`;
  - `Change` is a move-only handle: destroying it removes the change, `release()` keeps it.
- **Rust** ([Rust SDK tier and timing](https://github.com/happy-dwarf-team/anvil/issues/22), [the safe crate](https://github.com/happy-dwarf-team/anvil/issues/159)): `anvil-sys` (the raw API and the service headers), `anvil gen --lang rust`, the safe crate `anvil-sdk` and two Rust examples in CI.
  - **`anvil-sys`** (`sdk/rust/anvil-sys`, `no_std`, no dependencies, `rust-version` 1.82, which CI does not test: it builds with 1.99.1):
    - `src/bindings.rs` is bindgen's output for [`anvil_plugin.h`](../include/anvil/anvil_plugin.h) and the headers of the services Anvil ships, `anvil_game.h`, `anvil_panel.h`, `anvil_input.h`, `anvil_benchmark.h`, `anvil_map.h`, `anvil_script.h` and `anvil_log_throttle.h` (`sdk/rust/bindings.h` includes them), committed. `sdk/rust/regen-bindings.ps1` writes it (pinned bindgen, target `x86_64-pc-windows-msvc`, bindgen's own formatter, so the output does not depend on the machine); CI runs it with `-Check` and fails when the file differs, so a change of a header comes with its bindings.
    - C enums are integer constants (`AnvilResult`, `ANVIL_OK`), not Rust enums: a value the host sends is never invalid. Host table fields are `Option<unsafe extern "C" fn>`; `ANVIL_HOST_HAS` is a C macro, so a Rust plugin that needs a field added after API 2.0 compares `size` with `offset_of!` itself.
    - `AnvilCpuContext` is `#[repr(C)]`; the crate asserts at compile time its size (408), alignment, the offsets of `rflags`, `rax`, `rsp` and `rip`, and `ANVIL_API_VERSION`. `AnvilPluginLoadFn` is the type of the one function a plugin exports; the crate declares no extern functions.
    - Its version is Anvil's (release-please updates it with `version.txt`), and the release workflow publishes it to crates.io with the `CARGO_REGISTRY_TOKEN` secret. Without the secret the step only warns; the release is then published by hand at its tag.
  - **`anvil-sdk`** (`sdk/rust/anvil-sdk`, library name `anvil` because `anvil` on crates.io is another project; it depends only on `anvil-sys`):
    - `anvil::plugin!(Type)`, a declarative macro like `ANVIL_PLUGIN(Type)` (no proc-macro crate), exports `anvil_plugin_load`; `Plugin::load(host: &mut Host) -> Result<Self, anvil::Error>` returns the plugin, which is kept for the process, or an error whose code becomes the result Anvil rolls back on. A panic in `load` is `ANVIL_ERROR_INTERNAL` with the message in the log.
    - Every adapter (mid hooks, block replacements, events, settings, panel clicks, work for the main thread) runs the plugin's closure under `catch_unwind`: a panic is logged once per callback with its message and place, and the game carries on. A panic hook keeps the message for the adapter. Function-hook detours are plain functions the game calls; `anvil::catch_panic` is for code inside them.
    - `Host` is `Copy` (`Host::current()` anywhere once the plugin is loading). `Change` removes its change on `Drop`; `keep()` keeps it. `FunctionHook<sym::NameFn>` and `PointerSwap` live in a `static`, where Anvil writes `next` or `previous`; `FnPtr` covers `unsafe extern "C" fn` types only, so calling `next` is always an `unsafe` call. `Cpu` is the `#[repr(transparent)]` context: reading it is safe, `regs_mut` is `unsafe`.
    - The `unsafe` boundary: host and service calls are safe. What reaches the game's memory by address, or puts raw bytes and pointers into it, is an `unsafe fn` with its contract: `Host::function`, `Host::data`, `Cpu::regs_mut`, `patch_bytes`, `swap_pointer`, `redirect_call` and `replace_import` (whose sites declare no signature), and the changes at raw addresses (`hook_at`, `mid_hook_at`). `hook`, `mid_hook` and `replace_block` on declared symbols are safe: a detour has the generated signature's type.
    - Typed wrappers for every service Anvil ships: `anvil::game` (`anvil-game.game`, and its events as typed `Event<P>` constants), `anvil::world` (`anvil-game.world` with the 1.15 modifiers, units and selection), `anvil::commands` (`anvil-game.commands`, 1.17), `anvil::panel` (`anvil-overlay.panel`), `anvil::input` (`anvil-overlay.input` and `Game::keyboard`, 1.18), `anvil::benchmark` (`anvil.benchmark`), `anvil::map` (`anvil-map.modes`, with a tooltip callback that cuts its text at a whole character, and the `shown`/`hidden` events), `anvil::script` (`anvil-script.day_end`, 1.18) and `anvil::log_throttle` (`anvil-log-throttle.messages`, 1.20). A function past a table's `size` answers `ErrorCode::Unsupported`; an event payload is read only as far as its `size` says; names and lists ask for their size first.
    - Its own `0.x` version, bumped in the pull request that changes it; ADR 0001's source compatibility covers it from 1.0, after the first real Rust plugin. It depends on the `anvil-sys` of the same release (release-please writes that version). The release workflow publishes a version crates.io does not have yet, once the first one is there; the first one is published by hand after a game test and the maintainer's approval.
    - `cargo test` runs it against a fake host table: a panic in an adapter is logged once and does not unwind, `Drop` removes the change, and a payload of an older publisher is read only up to its size.
  - **`anvil gen --lang rust`** writes `symbols.gen.rs` (and `settings.gen.rs`) only, to be included in a module (`mod sym { include!(…) }`, as the Rust hello's `build.rs` does):
    - `pub const <NAME>: u32` ids (an `anvil_sys::AnvilSymbol`, written as `u32` so the file needs no crate path and works under `anvil-sdk` alone), `pub type <Name>Fn = unsafe extern "C" fn(…) -> …` from `signature`, and `pub const <SYMBOL>_<VALUE>: u64` values (RVAs for `rip_relative`), as in the C header. A detour assigned to a `<Name>Fn` is checked by the compiler, the counterpart of the C++ `static_assert`. Two names that become the same Rust name are an error.
    - Types translated: pointers (with their `const`), `char`, `short`, `int`, `long` (32 bits), `long long` with `signed`/`unsigned`, `float`, `double`, `bool`, `void`, `<stdint.h>` types, `size_t`, `intptr_t`, `uintptr_t`, `ptrdiff_t`, `wchar_t`, and `...`. A pointer to any other type is `*mut c_void` (`*const c_void` for `const`), quietly. Anything else by value (a struct, a function pointer, an array) is `*mut c_void` too, with a warning that names the symbol (`symbols.toml:<line>: warning: …`, also as a comment in the file) because the ABI may not match.
    - `settings.gen.rs` (for a `settings.toml` in the same call): `pub const <NAME>: u32` ids (`anvil_sys::AnvilSetting`), `pub const <NAME>_<OPTION>: i64` indices of enum options, and with a key setting `KEY_SHIFT`, `KEY_CTRL`, `KEY_ALT` and `KEY_VK_MASK`, as `settings.gen.h` has them. The plugin reads values with `Host::setting_bool`, `setting_int`, … of the safe crate (or the host table). A name two settings or options both give is an error.
  - **`examples/hello-rust`** (`cdylib`, static CRT): the hello of the C and C++ examples on the safe crate, with `unsafe` only in the detour that calls the rest of the chain. **`examples/panel-rust`**: no game addresses and no `unsafe`; the player's country from the world service at a session's start, every month and on a click, in an overlay panel. CI builds both for `x86_64-pc-windows-msvc` with clippy, rustfmt and their unit tests, loads them through the same host table as the other examples (`plugin_tests`, with fake game, world and panel services for `panel-rust`), and ships them in the package next to them. `tests/rust/gen-compile` assigns Rust functions to the generated types, so a wrong translation does not compile.

## 10. Load order and failure policy

1. The proxy is set up (always); the entry-point hook is placed in `eu4.exe` only.
2. At the entry point: no `anvil/` folder means nothing happens. `ANVIL_DISABLE=1` or `enabled = false` means the log only. `ANVIL_BENCHMARK=with:<id>` or `without:<id>` makes the start a benchmark start ([docs/spec/benchmark-v1.md](spec/benchmark-v1.md) §2); in the arm without, plugin `<id>` is disabled next to `disabled_plugins`, without writing `anvil.toml`.
3. **Safe mode:** two consecutive crashes before the game is ready (`anvil-game.ready`, §12, §13.1) mean this start loads no plugins except the standard ones the overlay needs to explain it.
   - Each start counts itself in `anvil/data/anvil/unfinished-starts.txt` before anything can crash. `anvil-game.ready` removes the file, unless this start already wrote a crash report: EU4's handler may let the game go on to the menu after a crash on another thread, and such a start stays counted. An exit before readiness without a crash puts the previous count back (a crash: a report was written, or a fatal-looking exception came in the last 30 s, which the game's own handler may have caught). A start that crashed, hung or was killed before the game was ready stays counted, whatever caught the crash.
   - A count of 2 makes this start safe: the file is removed and the log says why. The plugins are listed as skipped ("safe mode"). The next start loads them again.
   - Without `anvil-game` (disabled or missing) there is no ready signal, so a crash at any time of a session counts.
   - The standard plugins `anvil-game` and `anvil-overlay` still load in safe mode, so the overlay can explain it. One of them is left out ("safe mode; the crash before it was …") when a report of the starts that led to safe mode puts the crash in its code or its change (`in_plugin_code`, `in_plugin_change`), so the safe start cannot end the same way. Those reports are the newest ones written before the game was ready, at most one per counted start; a start that hung or was killed wrote none, so an older report may stand in for it.
4. The build is identified.
5. Plugins are discovered: local `anvil/plugins/*`, and plugins in the mods enabled in the playset (approved versions only, from their approved copies, §14).
6. Resolve and verify:
   - API version and the `anvil` range;
   - the build of `symbols`;
   - duplicates (a local plugin wins over plugins from mods with the same id; two mods with one id are decided by the approval, §14);
   - disabled ids;
   - the symbols (§7);
   - `conflicts` (the declaring plugin is skipped, see below);
   - dependencies and cycles. A plugin with `affects_gameplay = true` depends on `anvil-game` (any version) without declaring it, because only `anvil-game` tells Anvil about ironman and multiplayer (§13): without it the plugin is skipped with "affects gameplay, so it needs plugin 'anvil-game', which is not loaded".

   **Conflicts.** A plugin that declares a `conflicts` entry is skipped when a plugin with that id and a version in the range is among the candidates that passed the checks above; the other plugin loads normally. The log says `conflicts with <id> <version>; disable one of them`. Whatever depends on a skipped plugin is skipped by the dependency check, as usual. Edge cases are resolved the same way on every start, whatever the discovery order:

   - A plugin that is itself skipped for a conflict does not count as a target. Plugins that conflict with nothing alive are kept first, and whoever declares a conflict with one of them is skipped. So with `a` conflicting with `b` and `b` with `c`, `b` is skipped and `a` and `c` load.
   - When only cycles are left, the smallest id (byte order) is skipped and the rest is resolved again. Two plugins that conflict with each other leave the one with the larger id; in `a -> b -> c -> a`, `a` is skipped, then `b` because of `c`, and `c` loads.
   - A conflict with a plugin that is absent, disabled, rejected by the API, `anvil` or symbols checks, or outside the version range is ignored.
   - A candidate that is skipped later, because its own dependency is missing or in a cycle, still counts as a conflict target: conflicts are resolved before dependencies.
7. `early` plugins are loaded at the entry point, then `main` plugins on an Anvil thread. Each plugin's staged changes are enabled when its load returns. A plugin whose required dependency failed to load is not loaded either ("requires plugin '<id>', which failed to load").
8. `anvil.plugins_loaded` is published and the status is recorded.

## 11. Settings

- Defaults live in `settings.toml` in the plugin folder, read-only (§5.3). A plugin whose `settings.toml` does not parse is skipped with the parser's reason, like a broken `symbols.toml` (§7); `anvil check` reports it at its line.
- The player's values live in `anvil/data/<id>/settings.toml`, only the values that differ from the default; the file can be edited by hand. It is a flat table, `name = value` in the types of §5.3 (an enum option's name, a key binding as text):
  ```toml
  ram_prefetch_budget_mb = 2048
  mode = "on"
  ```
- An invalid value (wrong type, out of range, not an option, not a key) falls back to the default, with a warning in the log and the overlay; so does a name the plugin does not declare (ignored), and a file that does not parse leaves every setting at its default.
- `ANVIL_<ID>_<NAME>` environment variables override values for testing (the plugin id and the name in upper case, `-` as `_`), and Anvil logs each one: `true`/`false`/`1`/`0`, a decimal int, a number, an option's name, any string, a key binding. An invalid one is a warning and is ignored. An override changes what the plugin reads in this session, never the player's file.
- The values are read once, before the plugin loads, and are always valid when the plugin sees them.
- Plugins read validated values through generated accessors (§16): `cfg::<name>.get()` in C++; `host->setting_bool/int/float/string(self, CFG_<NAME>)` in C. `setting_int` also reads a bool (0/1), an enum (the option's index, `CFG_<NAME>_<OPTION>`) and a key (the code); `setting_float` an int; `setting_string` an enum (the option's name) and a key (`"Ctrl+F12"`). Any other combination, and an unknown id, reads 0, 0.0 or `""`. A string stays valid for the process.
- For `restart = false` settings plugins can subscribe to changes (`subscribe_setting`, or `anvil::Host::subscribe_setting(cfg::<name>, handler)`, which does not compile for a `restart = true` setting). The host refuses an unknown setting with `ANVIL_ERROR_NOT_DECLARED` and a `restart = true` one with `ANVIL_ERROR_WRONG_KIND`. Callbacks run synchronously on the thread that made the change, after the new value is in place; a plugin whose load fails loses its subscriptions.
- **Changing a value while the game runs** (the overlay, 1.1): the core holds each resolved plugin's settings in a `PluginSettings` (`src/core/plugin_settings.hpp`), found by `anvil::plugin_settings(id)` (`runtime.hpp`). `set(id, value)` (or `set_text`, `reset`) checks the value, saves the player's file (atomically, through a temporary file; with only defaults it is removed), and for a `restart = false` setting makes it the current value and runs the callbacks; a `restart = true` setting keeps its current value until the next start (`current` and `saved` tell the editor whether to show "applies at the next start"). A change replaces an environment override for the rest of the session. The overlay, a plugin itself, reaches these calls through the core's private service `anvil.control` (§15); the warnings of the start-up are in `warnings()`.
- **Outside the game** (the Manager, 1.2): `anvil_core` has the same rules without the running game: `load_settings`, `load_player_settings` and `write_player_settings` (`src/core/settings.hpp`), `parse_setting_text` and `setting_toml` for text, `check_setting_value`. Edits are safe only while the game is closed: a running game rewrites the file on the next change in the overlay.
- Settings are global per plugin; there are no per-playset or per-save profiles (§21).

## 12. Crash capture and attribution

Decided in [Crash attribution to plugins](https://github.com/happy-dwarf-team/anvil/issues/24).

- **Capture**
  - The core registers a VEH at the entry point. It records fatal-looking exceptions in memory only: no IO, at most 8.
  - An unhandled-exception filter chained ahead of EU4's writes the report, then hands over to EU4's own crash reporter.
    - Anvil sets its filter before the game's C runtime starts and replaces eu4.exe's import of `SetUnhandledExceptionFilter` (1.37.5 imports it from `KERNEL32.dll`). The game's own calls then only record their filter as Anvil's next one and get back the filter they replaced. Anvil's filter stays on top: it has the report written, calls the game's filter and returns its result.
    - At `anvil-game.ready` the core checks that its filter is still the top-level one. A filter set through another module's import stays on top, and the log names it.
  - A helper thread prepared at start-up writes every report while the crashed thread waits. It has its own stack, `MiniDumpWriteDump` loaded and the minidump file already open.
    - The report is built in fixed buffers: no allocation and no lock. The registry is asked with try-locks, and the crashed thread's memory is read with `ReadProcessMemory`. A fault while writing leaves a partial report with a note.
    - After the report the helper adds a small minidump (`MiniDumpNormal` with thread info and unloaded modules) and rewrites the report to name it. A dump writer stuck on a lock of the crashed thread costs only the dump. Until then the file waits as `pending-<pid>.dmp`, which the system deletes when the process ends without a kept dump.
    - On a stack overflow the VEH hands over at once, because the filter may have no stack left. The game's main thread gets a 64 KB stack guarantee for the handlers.
    - Other threads keep the default guarantee, which leaves about 12 KB after an overflow. Windows first puts the CPU state of the exception there: 1.7 KB with AVX and 3.2 KB with AVX-512, but 11.5 KB where it enables AMX (some server and workstation Xeons). Then it cannot dispatch the exception and ends the process before any handler runs. So on such a host (a context over 8 KB) `version.dll` keeps its thread calls, and every thread that starts after it loads gets a guarantee of the context plus 8 KB, rounded up to pages (20 KB for AMX), when its stack reservation is at least 256 KB. That costs such a thread about 8 KB of usable stack (0.2% of the game's 4 MB) and nothing elsewhere: on other hosts `DllMain` still turns thread calls off. The log says when it applies.
    - Windows calls the filter from a thread's start, which it reaches by unwinding, and it cannot unwind through a context stub (no unwind data). So the dispatch loop of mid hooks and block replacements calls the unhandled-exception filter itself, as a thread's start does, for an exception a callback leaves unhandled; without it such a crash ended the game with no report (1.0).
  - Not caught: `__fastfail` and `/GS` failures (they bypass both), `TerminateProcess`, and an exception that a frame handler of the game catches before the filter. On an AMX host, also a stack overflow on a thread with less than 256 KB of stack, or on one that started before `version.dll` loaded or skips thread calls (Windows ends the process before any handler runs). One report per process.
- **Report:** `anvil/crashes/<time>.json` and `.txt`, with the minidump `<time>.dmp`, keeping the last 10. It contains:
  - the code and the address as module + offset;
  - for `eu4.exe` addresses, the nearest declared symbol and whether a plugin's change covers it;
  - for stubs and trampolines, the change, plugin and symbol;
  - the unwound stack with every frame attributed;
    - frames come from unwind data (`RtlVirtualUnwind`). Anvil's stubs have none: a return address in the context stub continues from its saved `AnvilCpuContext` at the site (the stub is shared, so the dispatch loop leaves the site's state in a spare slot of the stub's frame for the walk), and a trampoline is unwound as the game instructions it copies. Anything else without unwind data (other stubs, code of no module) switches to a scan of the stack for return addresses, marked "scan" (they may be stale);
    - repeated frames (a recursion) are counted, not listed;
  - the registers, the plugins with versions and `issues` links, the build, the earlier fatal-looking exceptions of the VEH, and the tail of the log.
- **Levels:** "in plugin X's code", "in plugin X's change", "plugin X on the stack", or "no plugin involved". Never "X caused it".
  - In X's code: the crash address is in X's DLL. In X's change: it is in game bytes, a stub or a trampoline of X's change (every owner of a shared one). On the stack: a later frame is in X's DLL or change. The JSON has the level as `in_plugin_code`, `in_plugin_change`, `plugin_on_stack` or `no_plugin`.
- **Next start:** a notice in the overlay with "disable X at the next start" (only on click), and safe mode after two crashes before the game is ready (`anvil-game.ready`). Nothing is uploaded; "copy report" plus the `issues` link.

## 13. Gameplay-affecting plugins

Decided in [Gameplay-affecting plugins in ironman, achievements and multiplayer](https://github.com/happy-dwarf-team/anvil/issues/29).

- **`affects_gameplay`** is required: `true` if the plugin can change the simulation's state or results.
- **Suspension:** `anvil-game` publishes a session event. In ironman or multiplayer the core suspends every change of gameplay plugins (§8.4), the plugin is told so it can stop its threads, and the changes come back after the session.
- **Opt-in:** `allow_ironman` / `allow_multiplayer = "opt-in"` let the player allow a plugin in that mode; the choice is stored in `anvil.toml`:
  ```toml
  allow_ironman = ["my-plugin"]       # ids; only plugins whose plugin.toml offers allow_ironman = "opt-in"
  allow_multiplayer = []
  ```
  A session that is both ironman and multiplayer needs both. An id listed for a mode its plugin does not offer, or for a plugin that does not affect gameplay, does nothing; the log says so when the plugin is resolved.
- **Achievements:** with an ironman opt-in active, the overlay shows a permanent notice that achievements stay enabled but may break Paradox's rules.
- **Multiplayer:** the overlay shows a hash of the active gameplay plugin set for players to compare by hand.
- **Without `anvil-game`** nothing would report sessions, so gameplay plugins depend on it implicitly (§10): they are skipped when it is missing, disabled or skipped, and not loaded when it fails to load.
- **Modules** ([ADR 0011](adr/0011-plugin-modules.md)). Each module of §5.1 is an owner of its own: the policy holds an entry for the plugin's handle and one for each module that is on at this start, and suspends each by its own flags; `anvil.suspended` reaches what that handle subscribed. The player lists a module as `<plugin>.<module>` in `allow_ironman` and `allow_multiplayer`, and chooses modules for the next start with `enabled_modules` (one whose `default` is `false`) and `disabled_modules`, which wins; `module()` answers `ANVIL_ERROR_DISABLED` for a module that is off, so its code never runs. The hash has a line `<plugin>.<module> <version>` for each module that affects gameplay and acts in the session. `anvil.control`'s `plugin_of` answers for a module's handle with the module's key, flags and state, so `anvil-game.commands` and `anvil-script` gate the module. A plugin depends on `anvil-game` implicitly when it or any module affects gameplay.

**How the core applies it** (`src/core/gameplay_policy.cpp`):
- The core subscribes to `anvil-game.session` before any plugin loads, so it acts before plugins hear of a session. Only `anvil-game` can publish that name (§9).
- On a session start it computes, for each gameplay plugin in load order, whether the mode allows it, suspends the others (`anvil.suspended` after their changes are out) and logs each decision with the reason and the number of changes; plugins it allows are logged as such. On the end it resumes them (`anvil.resumed` after the changes are back). A start that follows a start (the mode changed) suspends or resumes only the difference.
- A plugin that loads while a session runs starts suspended: its changes are enabled suspended. A plugin whose load failed is left alone.

### 13.1 `anvil-game`

A standard plugin (`plugins/anvil-game`, released in `anvil/plugins/anvil-game/`), `phase = "early"` so that early plugins can depend on it, `affects_gameplay = false`. Its `symbols.toml` declares every address and offset it reads (44 symbols, 21 for the loop, the session and the GUI and 23 for the world, checked with `anvil check --exe`; the lock is committed and checked in CI). It hooks the frame function (`0x14C25A0(app, outer)`) and the root's session setter (`0x756A30(root, session)`, see 2.); after each frame of the main loop (`outer = 1`) on the main thread (the thread of the first such frame) while the game is not quitting (`app+0x83`, `app+0x2F8`) it does, in this order:

1. **Readiness:** the first frame after the game sets its ready flag (the byte at `0x233FF4E`, set once at the end of start-up) publishes `anvil-game.ready`. The core logs `game ready at … ms` from it; the core itself no longer knows the flag.
2. **Session:** a session runs when the root object (`0x233FE78`) holds a session of the game's class (vtable `0x1CB8460`, verified through its slot `+0x100`), that session has a game-speed object, and the date is set. Ironman is the root byte `+0x24D0`, multiplayer the game's own check `0x6EAA10(session)`, the two inputs of the console's multiplayer/ironman gate. A change of the session object, of ironman or of multiplayer ends the session and starts a new one, and so does a new install: a save loaded from the game menu puts the same session object back into the root, inside one frame of the main loop, so the hook of the setter counts the sessions the game puts in (5 calls per load in the lab, none while a game runs) and a changed count counts as a new session; `anvil-game.session` carries `STARTED` or `ENDED` with the flags and a number. Reads that fault (a half-built root) change nothing. Quitting from a session publishes no `ENDED`.
3. **Main menu:** outside a session the root's session is the front end (its class's vtable `0x1D5A8C8`, verified through slot 0), whose state machine (`CFrontEnd::ChangeState` in GOG's names) keeps its state at `frontend+0x900` (GOG `+0x8F0`): 0 is the main menu, 3 the nation selection (checked live, 1.37.5 Steam, [#184](https://github.com/happy-dwarf-team/anvil/pull/184)). A change between "main menu" and anything else publishes `anvil-game.main_menu` and logs `main menu shown` or `main menu left (front end state N)`. In a session the front end is not read.
4. **Work:** what `run_on_main_thread` queued before this frame, in order.
5. **Campaign:** while a session runs, the date, the player's country and the player's pending events are compared with the last frame (below). The pending list itself is read right after the session step, before anything of the frame is published, so every handler of the frame sees the same entries counted.
6. **Out of sync:** while a session runs, the flags are compared with the last frame and a change publishes `anvil-game.oos` (1.14.0). Two bytes hold them. `DETECTED` is the flag of the session's out-of-sync handler (the pointer at `session+0x4D8`, GOG `+0x4C8`, may be null; then handler`+0x08`), which a client sets at its first checksum mismatch. `PAUSED` is the game-speed object's `+0x19`. The host compares no checksums, so it never has `DETECTED`. `CGameState::_bOutofsynch` in GOG's `eu4.pdb` looks like the place, but the code only clears it, so it is not read. `true` has not been seen in a real desync, which takes two computers.
7. **Frame:** `anvil-game.frame` with the game's frame counter (`app+0x320`).

Frames drawn inside a game update or a save load (`outer = 0`) and frames on other threads are passed on untouched; anvil-game only counts the nested ones that run, so that `main_menu()` is 0 while one draws (the loading screen of a save loaded from the menu). Saves load inside one frame of the main loop, so a session starts at the end of the frame that loaded it, before the next game update. Checked in the game (anvil#168): while a save loaded from the game menu, no frame of the main loop and no nested frame ran; the root's session was null for about 2 s and then held the same object as before, and the game menu never switches between ironman and not: its load list offers no ironman saves, and in an ironman game its Load Game is disabled.

Service `anvil-game.game` version 1: `ready()`, `main_thread_id()`, `frame_number()`, `run_on_main_thread(work, context)`, `session(&out)` and, appended in 1.6.0 (`ANVIL_GAME_HAS`), `date(&out)`: the root's date (`root+0x1DD0`, hours since 1 January -5000 in 365-day years, decoded as Tongs does for `tongs_state`) as year, month and day with the session's number, as of the last frame of the main loop; 0 while no session runs (the menu, a save loading from it). While a save loads from a running game no frame runs, so the old session and its last date stay until the load ends (checked in the game, anvil#170). Appended in 1.8.0: `main_menu()`, nonzero while the main menu is up as of the last frame of the main loop and no nested frame is drawing, with the event `anvil-game.main_menu` (payload NULL) when that changes; the overlay's badge (§15) uses them. Queued work never runs once the game quits, and frames stop while a save loads, so callers wait with a limit. Appended in 1.18.0: `keyboard(&flags)`, whether the game's GUI takes the keyboard ([#248](https://github.com/happy-dwarf-team/anvil/issues/248)), read live rather than as of a frame. `TEXT` is a focused text field: `CEditBox::GiveFocus` stores the box's text receiver in `CGui::_pCurrentTextInputReceiver` (the global `CGui::_pGui`, then `+0x210`) and `RemoveFocus` clears it, and `CGui::HandleKeyEvent` gives a key to the focused object first. `SHORTCUT` is the shortcut editor, which takes every key while it is on (`+0x230`) and has listeners (`+0x248`, the size of their set). Two symbols, `gui_text_receiver` in `RemoveFocus` and `gui_shortcut_pick` in `HandleKeyEvent`, both may be declared absent; then it answers 0. The GUI belongs to the main thread, where the window procedure also runs, which is where the overlay asks.

**The campaign** ([ADR 0004](adr/0004-plugins-without-addresses.md), 1.11.0). All of it only reads, for a plugin with no game addresses; none of it is code in the game's tick.

- Service `anvil-game.world` version 1: `player`, `province_limit`, `province`, `province_name`, `province_cores`, `country`, `country_provinces`, `war_ids`, `war`, `war_name`, `war_side` and, appended in 1.15.0 (`ANVIL_GAME_WORLD_HAS`), `modifier_id`, `country_modifier`, `province_modifier`, `country_units`, `unit`, `unit_name`, `unit_leader_name` and `selected_units`, and in 1.20.0 `monarch`, `monarch_name`, `monarch_dynasty`, `flag_id`, `flag_name`, `country_flags`, `province_flags`, `monarch_flags`, `global_flags`, `country_variable`, `province_variable`, `country_variable_names` and `province_variable_names`, and in 1.21.0 `pending_events`, `pending_event`, `pending_event_key`, `pending_event_title`, `country_statistics`, `country_timed_modifiers`, `country_timed_modifier_keys`, `province_timed_modifiers`, `province_timed_modifier_keys` and `province_view`, and in 1.22.0 `country_relations`, `subject_type`, `diplomacy_status`, `country_rivals`, `country_overlord` and `country_subjects` ([`anvil_game.h`](../include/anvil/anvil_game.h)). `src/world.cpp` is Tongs' walk (its design, "World reads"): each call starts from the root, copies memory under a guard (`read_raw`, SEH), checks a province by its own ID and a country by its own tag, calls no game function and keeps nothing, so a load leaves no stale pointer. A fault or a failed guard costs that answer (`ANVIL_ERROR_INTERNAL`).
- A call is allowed only on the main thread, inside the step above (a handler of an `anvil-game` event or queued work), while a session runs; anything else is `ANVIL_ERROR_WRONG_PHASE`. Another thread sees days half applied (tongs#107), and so does a frame drawn inside the tick.
- `AnvilGameProvince` and `AnvilGameCountry` take their `size` from the caller and only that much is written. Strings and lists are the caller's buffers: filled as far as they fit, with the whole size answered. Text is UTF-8: valid UTF-8 stays, as Mei keeps it, and anything else is Windows-1252 without colour codes.
- The country's values (`treasury`, `stability`, `prestige`, `manpower`, `monthly_income`) are fixed points read from `CCountry`. `monthly_income` is `_vEstimatedMonthlyIncome`, which `CCountry::MonthlyUpdateSerial` sets to 0, sums and clamps once a month: the income total of the economy view, before expenses (checked in the game: 1.553 against 1.55).
- A modifier is asked for by its script key, once, and then by its id, the index of its `CModifierDefinition`. `country_modifier` answers `CModifier::GetValue` on the sum at `country+0x28C8+8` (`CCountryCachedData::_Modifiers`, whose first field is its state), and `province_modifier` on `province+0x998` (`CProvince::GetModifier`), which holds only what the province has itself. This is the one game function the service calls: 112 of the 814 definitions carry a transformer that changes the value when a DLC is off, mostly to 0, so bytes read directly would disagree with the game. The function and all 79 transformer classes only read: they write to the caller's value and call nothing but checks of DLC state, and the walk of their code found no other write than the thread's first use of its thread-local storage, which the main thread is long past. The game's console command `print_modifiers` prints the values before the transformer, so it agrees with these reads only for what no DLC gates: on Steam with the DLCs on, every entry of two dumps (439,590) was equal; on GOG, 21 keys differed, each with a transformer. The windows show the values after it. `CCountryCachedData::GetModifiers` itself is not called, because it logs an error and hands out an empty set while the modifiers are being worked out; `country_modifier` answers `ANVIL_ERROR_INTERNAL` then, which on the main thread after a frame should not happen. A key is looked up by walking the definitions rather than the game's hash map, so nothing depends on the map's layout; a lookup is meant to be done once, because an id holds as long as the game runs.
- `anvil-game.day`, `.month` and `.year` carry `AnvilGameDateChange`: at most one of each per frame, with `days`, because `CGameState::Tick` posts one day per `CInGameIdler::Idle` and a multiplayer client may run several queued ones. The monthly and yearly updates run inside the same `HandleDailyTick`, so they are done by then. `anvil-game.player` carries `AnvilGamePlayerChange`. Neither comes for the first value of a session, across a load, or for a date that goes back.
- `anvil-game.war` carries `AnvilGameWarChange`. On each `day` (and once at a session's start, as the baseline) the running wars at `root+0x1E30` are compared by address with the last list; what a war was when it began (its tags and name) is kept for its end, when the object is gone from the list. A war is one array read a day plus the reads of a new one. An id from a plugin is looked up in the list before it is used, so a stale id answers `ANVIL_ERROR_NOT_FOUND`.
- Commands (1.17.0, anvil#256): service `anvil-game.commands` version 1, `post(self, command, &result)` with the types `DRILL` (an intent: `ANVIL_GAME_COMMAND_START` or stop), `MERGE` and `SPLIT`. It is the one part of `anvil-game` that changes the game, and only through the game's own commands: it allocates with the game's `operator new`, builds the command with the constructor the game's AI calls (the drill and merge buttons build theirs in place, with the same fields; split's button calls the constructor), and hands it to `CSession::Post`, the function every button calls, with the session the buttons take (`idler+0x330` Steam, `+0x328` GOG, then `+0x370`). The pure part is `src/commands.cpp` (who may call, the units, intents, the limits of 64 units a call and 256 commands a frame, freeing a refused command through its deleting destructor) behind a backend that `src/plugin.cpp` fills and the tests fake. The caller is checked through `anvil.control`'s `plugin_of`, as `anvil-script` does (ADR 0008): `affects_gameplay = false` or no plugin is `ANVIL_ERROR_UNSUPPORTED`, a suspended plugin `ANVIL_ERROR_SUSPENDED`. Units are looked up in the commanding country's list (the root's player tag, never the observed one), so another country's unit, or any unit in observe mode, is `ANVIL_ERROR_NOT_FOUND`. While `CSession::_bIsHotjoin` (`+0x7D7`) is set the game would drop the command on every machine, so the service answers `ANVIL_ERROR_WRONG_PHASE`. `CSession::Post` refuses without a server, and while `_bIsAllowedToPost` (`+0x80D`) is 0 unless the session is a dummy (single player). The session starts with it set; `CInGameIdler::Idle` clears it only in an observer's game, around the UI's update, and puts the value back at its end (eu4-engine-lab#36, #90), so a post after the frame passes in multiplayer too, and `refused` is in practice an observer's. Whether the game then runs it alike on every machine is step 3 of anvil#256. The command symbols of a type may be declared absent: that type answers `ANVIL_ERROR_NOT_DECLARED`. Version 2 (eu4-engine-lab#126, #138) adds `EVENT_OPTION` with `pending_event_id` and `option_index` appended to `AnvilGameCommand`, and anvil-game registers the same table as versions 1 and 2. It does what the option's button does: it finds the local player's `CEventWindow` among the idler's pop-ups (`idler+0x78` GOG, `+0x80` Steam, minus 8 for the head) by its vtable and `_PendingEventID` (`+0x120`), takes the option's button from `_Buttons` (`+0x208`, the name `<event id>_<index>` at `+0x38` as `CEventWindow::Select` parses it) and its state (`+0x124`, 3 = disabled), and calls `CEventWindow::OptionSelected(window, option)`, which builds `CSelectEventOptionCommand`, posts it through an inlined copy of `CSession::Post` and clears `_KeepAlive` (`+0x124`). That function clears `_KeepAlive` and leaks the command also when the post is refused, so the service first checks the post's conditions itself (a server, and `_bIsDummy` or `_bIsAllowedToPost`) and answers `refused` without touching the window. A command posted beside the window would leave it open, and `Execute` checks neither the pending entry nor the option's trigger, so a click would run the effects twice (eu4-engine-lab#90): there is no other path, and a pending event without a window (the console's `msg`) cannot be chosen through the service.
- Game speed (commands version 3, 1.22.0, anvil#337): `SPEED` with `speed` (1 .. 5) appended to `AnvilGameCommand`; anvil-game registers the same table as versions 1 to 3. It does what the speed bar's buttons do (`CInGameIdler::IncreaseGameSpeed` and `DecreaseGameSpeed`): nothing while the in-game menu is shown (`idler+0x12E0` GOG, `+0x12F0` Steam, its byte `+0xF5`) or without `CEU3Idler::HasAdminRights` (a multiplayer client), which the service answers as `refused`; otherwise one `CIncreaseGameSpeedCommand` (0x50 bytes) or `CDecreaseGameSpeedCommand` (0x70) per step from the root's speed (`root+0x1E08`, 0 = the bar's 1), built by the constructors the game registers its commands with and posted as the other types are. Its `Execute` calls `CGameState::SetGamespeed`, so the speed changes on every machine when the session runs it, also while paused.
  - **Tried in the game** (lab run, 2026-11-12, Steam 1.37.5, single player, eu4-engine-lab `docs/anvil-modules-game-test.md`): from speed 2, `speed = 5` posted 3 and the game was at 5 in the next frame, paused; `5` again was `skipped`; `1` posted 4; Smithy's `speed_keys` module on a key set the speed to 3 (2 posted) and the game did not see the key; with the in-game menu shown `refused` 1 and nothing posted, after closing it 2 posted. Not tried: multiplayer (host and client), GOG in the game (its bytes are checked by `symbols.lock`).
- Units (1.15.0, anvil#251): `country_units`, `unit`, `unit_name`, `unit_leader_name` and `selected_units`. A unit is found in its country's linked list and checked by its id, so a read costs that list. Its kind (army, navy) and its mission's type come from virtual getters that are `xor eax,eax; ret` or `mov eax,imm32; ret` in both builds: the service reads the getter's 3 or 6 bytes from the vtable slot, only inside `eu4.exe`, and decodes them, so units call no game function. Any other getter gives `ANVIL_GAME_MISSION_UNKNOWN`, or `ANVIL_ERROR_INTERNAL` for the kind. The selection is the session's map input (an offset that differs between the builds, taken from bytes) and its list, with provinces and other selectable things left out. The 13 unit symbols may be declared absent together: the five calls then answer `ANVIL_ERROR_NOT_DECLARED`.
- Rulers, heirs and consorts (1.20.0, eu4-engine-lab#7): `CCountry::GetMonarchOfType` gives the three pointers (`+0x1280`, `+0x1298`, `+0x12B0`, by the game's `EMonarchType`). A country without one does not hold null but the game's placeholder of that kind (`CNullMonarch`, `CNullHeir`, `CNullQueen`): one object per process, made on first use, shared by every such country and never reset, so an effect on a missing heir writes into it and the next campaign of the same process reads it back (eu4-engine-lab#150). The service reads the three globals (from the `mov rax,[global]` at the start of each `GetNull*`) and answers `ANVIL_ERROR_NOT_FOUND` for a null pointer or any placeholder, reading nothing from it. A country no longer on the map keeps its last ruler and heir, as the game stores them, and the service gives them like `country` gives such a country (`province_count` 0). A monarch's id is its `CReferenceObject` id (`+0x10`, the same symbol as a leader's), name `+0x28`, skills `+0x48`, birth `+0x70`, sex `+0x74`, regency `+0x78`, the leader it is `+0x98` (a `CID` whose type, the low half, is `0x31`), flags `+0xA0`, and an heir's claim `+0x148`. `GetName` gives a fixed text in an interregnum when the name is empty; the service gives the empty name. The dynasty follows `CMonarch::GetDynasty`: a monarch with a regency whose country has a valid heir shows the heir's dynasty, unless `IsQueenRegent` (vtable slot `0x60`) is true. That getter is decoded from its 3 bytes (`xor al,al; ret` false, `mov al,1; ret` true) inside `eu4.exe`, never called; any other code counts as true, so the monarch's own dynasty stands. The 14 monarch symbols may be declared absent together.
- Flags (1.20.0): a `CIntegerFlags` holds a `std::vector<int16>` of flag indexes (`+0x10`) and a parallel vector of dates (`+0x28`), as `CIntegerFlags::GetFlagDate` reads them. The index is the position of the name in the global `_AllFlagsReverse` (an array of `std::string`), to which `CreateFlagIndex` only appends, so an id holds while the game runs; a name no effect or save has set yet in this run has none. A country's flags are at `+0x270` (`GetFlags`), a province's at `+0x1B8` and its permanent ones at `+0x1F8`, which `CProvince::IsFlagSet` both checks and the service answers together (each index once), a monarch's at `+0xA0` and the global ones in the root at `+0xB38` (`CHasGlobalFlagTrigger`). The 6 flag symbols may be declared absent together.
- Variables (1.20.0): `CVariables` is an `unordered_map<CString, CFixedPoint>` from `+0x10`, at a country's `+0x330` and a province's `+0x238` (`AccessVariablesForScope`). The service reads the map's list, never its hash: the sentinel node at `+0x18` (`GetVariable` compares the found node with it), the count after it, and in each node the next node at `+0`, the name before the value and the value at `+0x30`. A walk that does not end at the sentinel after the count's nodes is `ANVIL_ERROR_INTERNAL`. The 3 variable symbols may be declared absent together.
- Pending events (1.21.0, eu4-engine-lab#152): `CGameState::_PendingEvents` is a `CPdxArray<CPendingEvent>` in the root (data `+0x1EA0`, count `+0x1EAC`, entries of `0x28` bytes, from the loop in `AddPendingEvent`). An entry holds the event (`+0x10`), its `CEventScope` (`+0x18`, owned), `_AutoSelectDate` (`+0x20`) and the id (`+0x24`), which `AddPendingEvent` takes from a counter and `EVENT_OPTION` takes back. The list holds every country's events with options: an AI's leave in the frame after they came, a hidden player's event too, and a second copy of an event whose window the player already has leaves in one frame through `CRemovePendingEventCommand`; a player's visible entry stays until a click, `EVENT_OPTION`, or the day after its auto-select date; a load replaces the list without the add and remove functions (eu4-engine-lab#31). So the service does not guess an entry's kind: an entry counts once it was in the list at two outer frames in a row, told apart by its id, event and scope, and a session's first list counts whole. A real event costs at most one frame. The country is the game's own rule from `CEventManager::TriggerEvent`: the scope's `_OverrideTag` (`+0x44`) when its valid byte (`+0x4B`) is set, else `_Country` (`+0x2C`), and for a tag whose valid byte is clear the owner of `_nProvince` (`+0x28`); province events have no country of their own. From the event: `_DebugKey` (`+0x20`, the key `CEventWindow::OptionSelected` logs), `_Title` (`+0x7E0`, a localisation key), the options (`+0x790`, counted) and `_IsHidden` (`+0xA61`). Translated texts and whether a button is enabled stay the window's (Tongs reads them). `pending_events(NULL)` is the commanding country's (the root's player, never the observed one), as `EVENT_OPTION` acts for it. `anvil-game.pending_event` (`AnvilGamePendingChange`, `ADDED` or `REMOVED`) compares the counted entries of that country after each outer frame, not only on a new day, since a choice runs as a command also while the game is paused; a new session or a change of the player's country is a new baseline that publishes nothing. A save keeps the entries that were waiting when it was written, an AI's and hidden ones included (an autosave of 1 January 1720 held one of each), so after a load those count too while the game has not run them, and the game reopens a window for a hidden one as well: `ANVIL_GAME_PENDING_HIDDEN` tells them apart. In multiplayer the list holds the other humans' events too, which any tag can read: documented and kept out of the catalog, not filtered (ADR 0004, decision 4). The 8 pending event symbols may be declared absent together.
- Statistics (1.21.0): four `CGraphStatistics` in the root, income `+0x1ED8`, score `+0x1F60`, nation size `+0x1FE8` and inflation `+0x2070`, filled only by `CGameState::YearlyUpdate` through `AddDataByKey` and from a save (`ReadMember`). Each is an `unordered_map<CString, SGraphStatisticsItem>` keyed by the tag's text, read as its list like the variables (the sentinel at `+0x18`); a node holds the key at `+0x10`, the values (`std::vector<int>`) at `+0x60` and the years (`GetYear` of the date) at `+0x78`, appended together, so a node whose two vectors differ in length is `ANVIL_ERROR_INTERNAL`. The values are whole units after the game's own division by 1000 (truncated) and the nation size a count of provinces, not the service's usual ×1000; every year end since the history began has a point, 0 before the country had a value, where the save writes only the other years. The yearly update runs on 1 January and stores that year: the point 1720 holds the value at the end of 1719, as the save writes it. A read walks every country's node, about 15 µs with 1380 countries. The 6 statistics symbols may be declared absent together.
- Tried in the game (lab run, 2026-11-12, Steam 1.37.5 and GOG 1.37.5.2, eu4-engine-lab#152): from the save of 17 March 1719 loaded from the menu, the four series of RUS, TUR, SWE and POL equalled the save's `*_statistics` value for value, the other years 0; after the year end the new point 1720 equalled the yearly autosave's. A lab event fired from the console gave the player's two visible events (a country event and a province event in the capital, whose owner gave the country) at once in `pending_events`, the same ids as Tongs read from their windows, and one `ADDED` each; a hidden player event and an AI's event it fired were never counted or told. `EVENT_OPTION` and Tongs' choice through the service each gave one `REMOVED` and the windows closed. The autosave written with two events waiting gave, after its load, the same ids at the session's first frame and no event; a hidden event the save kept counted too, with its flag. Both builds gave the same answers. 1000 `pending_events` calls took 23 µs and 1000 `country_statistics` calls about 15 ms. Not tried: GOG 1.37.5.0 in the game, an event with `_OverrideTag`, multiplayer, observe mode live.
- Tried in the game (lab run, 2026-11-11, Steam 1.37.5 and GOG 1.37.5.2, eu4-engine-lab#7): from the save of 17 March 1719, the rulers, heirs and consorts of 15 tags, their flags, country, province and global flags and country and province variables matched the save's own fields; a missing heir or consort gave `NOT_FOUND`; flags and variables an event set appeared with their date and value. Both builds gave the same answers except flag ids, whose order of registration differs between builds. 1000 `monarch` calls took 73–80 µs and 1000 `country_flags` calls of 110 flags 3.2 ms. Not tried: GOG 1.37.5.0 in the game, a regency council or interregnum live, a flag on a missing heir, a second campaign in one process.
- Timed modifiers (1.21.0, eu4-engine-lab#155): a country's `CTimedModifier`s are a linked list at `+0x1F0` (`CCountry::HasModifier`: the head node, the object at `+0`, the next node at `+0x10`), a province's a vector at `+0x2F0` (`CProvince::HasModifier`). In each, `+0x30` is the `CStaticModifier`, whose key is the `std::string` at `+0x158` that `CTimedModifier::WriteMembers` writes as `modifier`; the end date (`+0x44`) and the start date (`+0x40`) are hours, `0x29C77F8` (1.1.1) for none, which the save leaves out. A modifier given for good (a negative duration) ends at the game's date for never, a global that `CCountry::AddTemporaryModifier` reads and the save writes as `date=-1.1.1`; the service reads that global at each call and answers both as `ANVIL_GAME_TIMED_NO_END` (an unset start as `_NO_START`); `+0x48` is the ruler modifier's bool and `+0x4B` `hidden`. The list and its keys are two calls, so a key never has to fit a fixed field; on the main thread they answer the same list. The 7 timed modifier symbols may be declared absent together.
- The province view (1.21.0): the session's selection object (`session+0x1220` Steam, `+0x1210` GOG), its province view at `+0x10`, the province it shows at `+0x958` and its window at `+0xA0`, whose widget (`+0x30`) is shown while its byte `+0xC5` is set: the path Tongs reads, with Tongs' sites. `province_view` answers the province's ID only while that window is shown, and `ANVIL_ERROR_NOT_FOUND` otherwise; a pointer that is not a province of the array is `ANVIL_ERROR_INTERNAL`. The 6 view symbols may be declared absent together.
- Diplomacy (1.22.0, eu4-engine-lab#153): `CCountry::_Diplomacy` is a `CCountryDiplomacy` embedded at `+0x14F8` (`CDeclareWarAction::ExecuteDerived`). Each kind of relation is a `CPdxArray<pair<int, T*>>` in it, 16-byte entries whose int is the other country's index, as the game's getters read them: royal marriages `+0x68`, alliances `+0x80`, military access `+0xB0`, giving military access `+0xC8`, subsidies `+0x110`, dependencies `+0x128`, guarantees `+0x1A0`, warnings `+0x1B8`, each with its count 12 bytes on. `CAlliance::Link` puts the same `CRelation` into both countries' lists, so the direction is the relation's `_First` (`+0x10`) and `_Second` (`+0x18`), as the save writes them, and only military access splits by side: a country's `_MilitaryAccess` holds the relations in which it is first, `_GivingMilitaryAccess` those in which it is second. A relation that names neither the country asked is `ANVIL_ERROR_INTERNAL`. Its dates are `+0x20` and `+0x24`, the unset date (1.1.1) for none. A dependency's `_pSubjectType` (`+0x40`) is a `CSubjectType` whose script key is at `+0x10`. `_Statuses` (`+0x1510`, a vector of `CDiplomacyStatus`, 0x48 bytes, by the other's index) holds what `CDiplomacyStatus::DoWriteMembers` writes under `active_relations`: `_nCachedSum` (`+0x1A`, int16), trust (`+0x1E`), favors (`+0x20`, ×1000) and the truce bit (`+0x45`, bit 0); the service checks its parent (`+0`) and the other country (`+0x10`) before it reads. The opinion is `_OpinionCache` (`+0x2380`, a vector of int16 by the other's index), the value `CCountry::GetOpinionValue` returns without a description below the cache's size; above it the game works the value out, and the service answers `ANVIL_ERROR_NOT_FOUND` there instead, as it does for a country that owns no province (whose row the game no longer refreshes, eu4-engine-lab#30). The cache equals the diplomacy window's number (eu4-engine-lab#53), but it is refreshed at least monthly for a human and yearly for an AI, and the window's tooltip works it out again; a number worked out now would call the game (`CalculateOpinionValuesOnly`), which this service does not. The timed opinion modifiers behind it (`_TimedOpinions2`) stay out of version 1 (eu4-engine-lab#29): the shared empty `_NullTimedOpinions` and `_NullCasusBellis` of `CCountryDiplomacy` would have to be read as none, as the monarch placeholders are. Rivals are `_Rivals` (`+0x2248`, a list: the tag at `+0x10`, the next node at `+0x28`), every node as `CCountry::IsRival` counts them, its deleted flag not looked at; the overlord `_OverLord` (`+0x18F0`, a tag) and the subjects `_Subjects` (`+0x1A18`, a vector of tags). Every country's statuses, trust, favors and opinions can be read, also in multiplayer: documented and kept out of the catalog, not filtered (ADR 0004, decision 4). The 21 diplomacy symbols may be declared absent together.
- Tried in the game (lab run, 2026-11-12, Steam 1.37.5 and GOG 1.37.5.2, eu4-engine-lab#153): from the save of 17 March 1719 loaded from the menu, a lab plugin read the 88 countries that own a province: their 402 relations (140 alliance, 96 royal marriage, 52 dependency, 52 military access and 52 giving military access entries, 6 guarantee, 2 subsidies, 2 warning; a dependency in both the overlord's and the subject's list) with their dates and the 26 subject types, 104 rivals, the overlords and subjects, and the status of all 7656 pairs, each equal to the save's `diplomacy`, `active_relations` (289 truces, 993 trusts other than 50, 161 favors) and `opinion_cache`, with 0 differences; an alliance of 1373 keeps its end date of 1701.9.1, as the save does. After 290 days at speed 5 the same read on 1 January 1720 equalled the yearly autosave of that day the same way on each build. `timed_sum` ran from -1166 to 394, the opinion from -200 to 200. The same country twice gave `INVALID_ARGUMENT`; a tag without provinces (BYZ) `NOT_FOUND` from either side, and no relations. 1000 `diplomacy_status` calls took 0.1 ms, 1000 reads of RUS's 13 dependencies 0.5 ms and 1000 of three rivals 0.13 ms. Not tried: GOG 1.37.5.0 in the game, a cache that does not reach a country yet (a release; unit tests only), multiplayer.
- **Symbols absent on a build.** `anvil-game` is what suspension and safe mode depend on, so the world must not hold up a port: a port may declare these 111 symbols `absent` (§5.2). Without one of the 13 of the reader the service is not registered and `get_service` gives NULL; without a country value its bit in `AnvilGameCountry.values` is clear; without a modifier symbol the three modifier calls, without a unit symbol the five unit calls, without a monarch, flag, variable, pending event, statistics, timed modifier, province view or diplomacy symbol the calls of that group, and without a war symbol the war calls, answer `ANVIL_ERROR_NOT_DECLARED`; no `anvil-game.war` comes without the war symbols and no `anvil-game.pending_event` without the pending event symbols.
- It shows everything, terra incognita and any country in multiplayer included: Anvil cannot hide what a native plugin can read for itself. A plugin that shows such values in multiplayer does not enter the catalog (ADR 0002).

In a start-up benchmark start (`anvil.benchmark` active, [docs/spec/benchmark-v1.md](spec/benchmark-v1.md) §2) it subscribes to the core's `anvil.benchmark_quit` and, after the next frame, pushes `SDL_QUIT` through SDL 2.0.4's `SDL_PushEvent` (`sdl_dynapi_init`, `sdl_push_event`), as the menu's Quit does; it does nothing when SDL's dynamic API slot 88 no longer points at that function.

## 14. Workshop distribution and trust

Decided in [Workshop distribution and trust model](https://github.com/happy-dwarf-team/anvil/issues/25). Facts: [Workshop research](https://github.com/happy-dwarf-team/anvil/issues/16).

[Run the Workshop experiments E1 and E2](https://github.com/happy-dwarf-team/anvil/issues/34) found:
- **E1:** a mod with only `descriptor.mod` + `anvil/**` keeps the 1.37.5 checksum and allows ironman achievements.
- **E2:** a private upload through the Paradox launcher delivered the DLL byte-identical, with no warnings; the launcher only adds `remote_file_id` to `descriptor.mod`.
- **E3:** the Paradox forum's [rules for user mods](https://forum.paradoxplaza.com/forum/threads/updated-14-06-18-rules-for-user-made-mods-and-edits-of-pds-games.708039/) (last updated 2018-06-14, read 2026-10-26) govern mods listed on the forum or the Workshop and say nothing about native code. What concerns plugins:
  - a mod does not distribute the EXE file (rule 3): plugins are DLLs, and Anvil itself is not on the Workshop;
  - a mod includes no license or claim of copyright, only credits (rule 4), and no third-party material without its holder's permission (rule 6). This conflicts with open-source licenses such as MIT, whose notice must ship with every copy: Bellows' Workshop item carries its own `LICENSE` and the notices of mimalloc and toml++. The notices stay, as the known risk of a moderator removing an item, and the question is put to Paradox;
  - no fees (rule 8);
  - a mod "should be exclusive" to the forum and the Workshop (rule 9): plugins are also released on GitHub on purpose, because the Manager installs them from zips and the Workshop cannot install Anvil.
  The binding terms are Paradox's and Valve's agreements, which forbid malware and cheats but not native code ([#16](https://github.com/happy-dwarf-team/anvil/issues/16)); Bellows has been public on the Workshop since 0.9.

- **Discovery:** only mods enabled in the current playset (`dlc_load.json` → `.mod` → `path`), in `anvil/<plugin-id>/` folders, several per mod. This also gates a plugin on its mod being enabled.
- **Trust:**
  - an approval covers a SHA-256 over the whole plugin folder;
  - `trust.toml` records the id, version, Workshop id, hash and date;
  - the Manager can revoke an approval.
- **Updates:** a new hash is pending; the previously approved copy (`data/anvil/approved/`) keeps running until the new one is approved.
- **First approval:**
  - the overlay asks once the game runs, and the plugin loads from the next start;
  - the Manager can approve before launch;
  - the prompt shows the name, authors, version, mod, `affects_gameplay`, symbol count, hash, `repository` and a native-code warning.
- **Anvil itself** cannot be installed by the Workshop (items never land next to `eu4.exe`); the Manager installs it (§15).

### 14.1 How it is built (1.3)

Decided in [Workshop discovery and trust](https://github.com/happy-dwarf-team/anvil/issues/60) ([decisions](https://github.com/happy-dwarf-team/anvil/issues/60#issuecomment-6295248386)). The code is in `src/core/playset.cpp`, `plugin_hash.cpp`, `trust.cpp` and `mod_plugins.cpp`, which are part of `anvil_core`, so the loader, `anvil check --game` and the Manager use the same rules.

- **The playset**
  - In the game, EU4's user folder is the one `-userdir=<path>` names on its command line, as for the game itself. Otherwise, and for the Manager and `anvil check`, it is the path in `<EU4>/userdir.txt` when that file names one; otherwise it is `Documents\Paradox Interactive\Europa Universalis IV` (`SHGetFolderPathW`, which eu4.exe imports), or `Europa Universalis IV GOG` in a GOG install: GOG's eu4.exe adds " GOG" to the name, and the Paradox launcher's `gameDataPath` names the same folder, so the mods and settings of a GOG game are not Steam's.
  - Anvil reads the `enabled_mods` of `dlc_load.json` there, in order.
  - Each entry names a descriptor (`mod/ugc_123.mod`). Anvil reads only its top-level `name`, `path` and `archive`; blocks such as `tags = { … }` and comments are skipped. A relative `path` is in the user folder.
  - A zipped mod (`archive` without `path`) cannot carry plugins and is skipped without a warning. A descriptor that cannot be read is a warning. A `dlc_load.json` that does not parse means no plugins from mods.
  - A mod is a Workshop item only when its folder is `…/workshop/content/236850/<id>`. A local mod whose descriptor got a `remote_file_id` from an upload stays local.
  - Disabling a mod stops its plugins and keeps their approvals.
- **The folder hash** is the SHA-256, in lower-case hex, of the lines `<relative path> \0 <size> \0 <SHA-256 of the file> \n` for every file below `<mod>/anvil/<id>/`, sorted by path.
  - The path is UTF-8, with `/` and ASCII letters in lower case. The size is decimal, and each file's SHA-256 is lower-case hex.
  - Empty folders do not count. The hash is the same on every machine and covers added, removed and renamed files.
  - A link (junction or symbolic link), two paths that differ only in case, more than 4096 files or more than 256 MB is an error, and the plugin is invalid.
- **`anvil/trust.toml`**
  - One `[[plugin]]` table per id:
    ```toml
    [[plugin]]
    id = "hello"
    version = "1.0.0"                # plugin.toml's version when it was approved, for the player
    mod = "mod/ugc_3290499798.mod"   # the descriptor as dlc_load.json lists it: the approval binds the id to this mod
    mod_name = "Hello"
    workshop_id = "3290499798"       # "" for a local mod
    hash = "…64 hex digits…"
    approved = 2026-10-20T18:05:11Z  # UTC
    ```
  - The schema is strict: an unknown or missing key, an id twice, or a hash that is not 64 lower-case hex digits is an error.
  - A file that does not parse approves nothing. Anvil does not overwrite it, and it removes no approved copy until the file is fixed or removed.
  - Anvil writes the file atomically, sorted by id, and reads it back before it replaces the old one.
- **Decisions.** Every `<mod>/anvil/<id>/` of an enabled mod is in one of these states:
  - **shadowed:** a local plugin in `anvil/plugins` has this id (or this folder name, when its `plugin.toml` is broken) and wins over every mod. The log names the mods it shadows.
  - **conflict:** two or more enabled mods have a plugin with this id. The one that `trust.toml` binds the id to is decided as below, and the others are in conflict. With none bound, none loads until the player approves one, and that approval binds the id to that mod.
  - **approved:** the approval binds the id to this mod with this hash. The plugin loads from `anvil/data/anvil/approved/<id>/<hash>/`. A missing copy is made again from the mod folder and checked against the approved hash.
  - **update pending:** the approval binds the id to this mod with another hash, and that hash's approved copy exists. The approved copy loads until the new hash is approved; this is also the case when the new version is broken.
  - **pending:** no approval, or one bound to another mod.
  - **invalid:** the folder name is not a plugin id, or `plugin.toml` does not parse, has another id, or its entry is missing, or the folder cannot be hashed.
- **Loading**
  - Approved plugins go through the resolver with the local ones, from their copies (§7, §10).
  - Right before `LoadLibrary`, the core hashes the copy again with every file open and no write or delete sharing, and keeps the files open until the DLL is mapped. A copy that no longer has the approved hash fails to load with "its approved copy changed".
  - Safe mode skips plugins from mods.
  - At each start, before any plugin loads, the copies that `trust.toml` no longer names are removed: revoked ids, older hashes and unfinished copies.
  - With one mod that has a plugin, discovery took about 2 ms of start-up in the tests and 3 ms in the game. The first start after the files were copied took 29 ms (EU4 engine lab, `docs/anvil-workshop-trust-game-test.md`).
- **Approval and revocation** (`approve_mod_plugin`, `revoke_mod_plugin`)
  - An approval takes the hash the player was shown. It checks that hash against the mod folder, copies the folder into a temporary folder under `approved/<id>/`, checks the copy and moves it into place. Only then does it write `trust.toml`, replacing an earlier approval of the id.
  - Copies of the id other than the new one and the one the running game loaded are removed where possible.
  - A revocation removes the entry and the copies, except the one in use.
  - Both take effect from the next start.
- **In the overlay** (§15)
  - `anvil.control` lists the plugins in mods with what the prompt shows and offers `approve_mod_plugin` and `revoke_mod_plugin`.
  - A non-blocking notice lists the plugins that wait (new, an update, a conflict) and leads to the panel's **Mods** tab. There the player approves, with a second click, or revokes. The game never pauses.
  - Plugins that wait for approval are not repeated in the "not loaded" notice.
- **In the Manager** (§15)
  - A **Mods** tab shows the same states and reasons from `anvil check --game`.
  - The player can approve before launch through a prompt, and can revoke.
  - An approval reads the playset again and goes through only when the same mod still has that plugin with the hash that was shown.
- **`anvil check --game [--user-dir <folder>]`**
  - It lists the plugins in mods with their state, reason, hash and approval, checks each one, and resolves the approved ones together with the local plugins.
  - It writes nothing, so a missing copy is reported, not made.
  - Plugins that wait for the player (pending, update pending, conflict, shadowed) do not fail the check; invalid ones do.
  - JSON adds `mods` and `mod_plugins`, and the `load_order` and `skipped` entries of a plugin from a mod get `mod`.

## 15. Player UI

Decided in [Player UI: in-game overlay and Anvil Manager](https://github.com/happy-dwarf-team/anvil/issues/26).

- **`anvil-overlay`**
  - Drawing: Dear ImGui on D3D9 (`EndScene`/`Reset` via `hook_function_at`, chained with other D3D9 hooks); input by subclassing the game window's WndProc. It turns itself off on the OpenGL backend, and the Manager shows the same information outside the game (§21).
  - Notifications: non-blocking, for skipped plugins, the previous crash, pending approvals/updates and safe mode.
  - **A newer Anvil** ([ADR 0005](adr/0005-release-check-in-the-game.md), decided on 2026-11-09): the release check the Manager has, from the game. When the game is ready, if the player agreed, a thread of its own asks `api.github.com` once for Anvil's latest release; when it is newer than the running Anvil, a notice says so, how to update (run the new `AnvilManager.exe`) and offers "Open its page" and "Copy link". The panel's Status tab names it too.
    - Consent is the setting `release_check` ("Check for new Anvil releases": `ask`, the default, `on` or `off`). `ask` follows the player's answer to the Manager's question in `%LOCALAPPDATA%\Anvil\manager.toml`; without one, a notice asks once in the main menu (Yes or No, which become the setting; closed unanswered, it asks again at the next start). The Manager, when it never asked, takes `on` or `off` from this setting, so one answer counts for both.
    - The notice shows at each start, for the notification time, until the player closes it or opens the page; then not again for that release (its tag in `anvil/data/anvil-overlay/release-notice.txt`). A start of the start-up benchmark never checks, and without the network the log says why and nothing is shown.
  - **Badge in the main menu** (decided by the maintainer on 2026-11-02, [#184](https://github.com/happy-dwarf-team/anvil/pull/184)): while the game shows its main menu, a small "Anvil x.y.z" on the left edge tells the player that Anvil runs and which version; a click on it opens or closes the panel, for players who do not know the key. Its dot is green, amber when a plugin waits for this game version (§6) and red when a plugin failed. Its tooltip gives the plugin counts and the panel key. The setting `menu_badge` (default on) turns it off.
    - Only the main menu: not on the nation selection or the other front-end screens, not over the loading screen of a save loaded from the menu, and never in a game, where the overlay does nothing more than before.
    - Drawn by the overlay with ImGui rather than added to the game's own interface: a widget in the game's GUI would need the GUI tree's layout and the menu's `.gui` files (which mods replace) in the overlay, which has no game knowledge; the overlay only needs to know when the main menu is up, and `anvil-game` tells it (§13.1).
    - Where: 12 px from the left edge, halfway down. EU4 draws its menu at the same size at every resolution, grown only by its own interface scale (`game_ui_scale`, at most the height / 720): the logo keeps to the top-left corner, the bar with the buttons to the bottom centre. Halfway down the left edge lies between them from 1280×720 to 3440×1440 at every interface and overlay scale (unit-tested against the menu's widgets as Tongs reads them); the bottom-left corner would need the game's scale, which the overlay does not know. A mod that rearranges the main menu may put something there.
    - It takes the mouse only while the pointer is over it; clicks beside it reach the game.
  - Panel on `Ctrl+Shift+F12` (or a click on the badge in the main menu):
    - status and reasons;
    - approvals;
    - "disable at the next start";
    - the crash report;
    - the log tail;
    - the settings editor.
  - It never pauses the game. Plugins get no ImGui context.
  - **Windows of plugins** ([ADR 0004](adr/0004-plugins-without-addresses.md), `anvil-overlay.panel` version 1, [`anvil_panel.h`](../include/anvil/anvil_panel.h)): a plugin creates a panel with a title and hands over rows (text, a value beside it, a button, a separator), and the overlay draws each shown panel as a window of its own that fits its rows. It needs no mod and no address, and it keeps the game's checksum.
    - `src/panels.cpp` keeps the rows, copied, under its own lock, so a plugin may call from any thread. The status tab has a switch per panel, so the player can show one again after closing it. A shown panel needs an ImGui frame in each `EndScene`, like a notice.
    - A click comes from `EndScene`, and frames are also drawn inside the game's update, so it is queued with `run_on_main_thread`: the handler runs after an outer frame and may read `anvil-game.world`. Without `anvil-game` no click is delivered.
    - Text the catalogs do not cover: Dear ImGui finds a glyph in its fonts in order, Segoe UI, the language's subset, then the fonts of Windows, which merge only once a panel or a plugin translation (§5.4) holds a CJK character; the Manager merges the player's language's font of Windows when that language is Chinese, Japanese or Korean. Microsoft YaHei, Microsoft JhengHei, Yu Gothic and Malgun Gothic are mapped read-only, not read into memory, with the player's language's first, so that a shared ideograph takes that language's form. A missing file is skipped with a warning, and its characters show as `?`.
  - **Language** ([#158](https://github.com/happy-dwarf-team/anvil/issues/158)): the overlay and the Manager show their texts in the language of EU4's settings, `language` in `settings.txt` of the game's user folder (read only): English, French, German or Spanish; any other language shows English. `ANVIL_LANGUAGE` (`en`, `fr`, `de`, `es`, `zh-Hans`, `zh-Hant`, `ja`, `ko`, `pl`) overrides it, for tests and for a player who prefers another language. Both windows draw from Segoe UI, whose glyphs Dear ImGui rasterizes when a text first needs them.
    - **Chinese, Japanese and Korean** (decided on 2026-11-03; the maintainer's decision of 2026-11-02 that they come with the Unicode Patch port, now [Mei](https://github.com/happy-dwarf-team/mei)). EU4 has no such language of its own: a translation mod (the Simplified Chinese one, Workshop 2976470733, has about 340,000 subscribers) replaces the English localisation, and Mei lets the game show it, so `settings.txt` says `l_english`. The order is therefore: `ANVIL_LANGUAGE`; the overlay's setting `language` (default "Like the game"; the Manager reads the same value from `anvil/data/anvil-overlay/settings.toml`); French, German or Spanish from `settings.txt`; and, in the overlay when Mei (`mei`) is among the plugins that load, in the Manager always (since lab#93), Steam's language (`HKCU\Software\Valve\Steam` `Language`: `schinese`, `tchinese`, `japanese`, `koreana`), then Windows' display language (`zh-CN`/`zh-SG` Simplified, `zh-TW`/`zh-HK`/`zh-MO` Traditional, `ja`, `ko`); then Polish when Steam's language is `polish`; otherwise English. `anvil.log` names the source (`the player's language: zh-Hans (Steam)`).
      - Why Mei and not the translation mod: the mods have no common marker (EU4DLL escapes, plain UTF-8, Korean drawn from Latin-1 pairs), their ids and names would be a list to keep, and one mod may serve several languages; Mei loads exactly when the game shows CJK text. Why Steam before Windows: Steam's language is the one the player chose for games, and a Chinese player on an English Windows usually keeps Steam in Chinese. Without Mei (a Korean mod that needs no DLL, or a player who reads English) nothing changes, and the setting covers the rest. Why the Manager does not wait for Mei: it is the installer, so at its first start neither Anvil nor Mei is in the folder, and a Chinese player read the steps that matter most, installing and replacing EU4DLL, in English. A player whose Steam or Windows is Chinese but who reads English sets `language` in the overlay's settings, which the Manager follows, or `ANVIL_LANGUAGE`.
      - Fonts: `src/i18n/fonts`, one subset of Source Han Sans Regular per language (SC, TC, J and K faces, so each language gets its own forms), with only the characters its catalog uses: 370–498 characters, 38–112 KB, 317 KB together, linked as resources into `anvil_overlay.dll` and `AnvilManager.exe`. They are Modified Versions under the SIL Open Font License, named "Anvil CJK …" without the Reserved Font Name "Source"; the license ships in `anvil/docs/third-party` and the Manager's About tab names it. Only the player's language's font is loaded, merged into the Latin font (`OversampleH` 1, as ImGui advises for CJK) at 1.12 times the Latin size, as Source Han Sans draws its ideographs smaller in the same line height. Since Dear ImGui 1.92 the atlas grows as glyphs are used and keeps its CPU copy, which a device reset uploads again.
      - Translations are drafted by the agent; a native speaker checks them before they count as done (§21).
      - ImGui wraps at spaces and after `.,;!?"`: a run of CJK text without them moves to the next line whole, or is cut anywhere when longer than a line.
    - **Polish** (decided on 2026-11-09 in [eu4-engine-lab#25](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/25); the maintainer, a native speaker, accepted the catalog): EU4 has no Polish, so `settings.txt` says English for Polish players. Steam's language is the player's own choice for their games, so `polish` there chooses Polish, after EU4's own French, German or Spanish and after the Chinese, Japanese or Korean of Steam and Windows (in the overlay only with Mei). Windows' display language does not, as for French, German and Spanish. The overlay's setting and `ANVIL_LANGUAGE` (`pl`) choose it too. Polish needs no font of its own: its letters are in Latin Extended-A, which Segoe UI has.
    - `anvil.log`, `anvil check` and the benchmark reports stay English. So do texts that are not Anvil's: a plugin's name, its description and setting texts without a plugin translation (§5.4), the diagnostics of `anvil check` the Manager lists (a `plugin.toml` error, a symbol that does not verify) and errors of the system, which the translated words around them quote as they are.
    - Why a plugin is skipped or failed is a message: a code with arguments, which may be messages themselves (`src/i18n/message.hpp`). The core writes its English words to the log and keeps the code; the overlay gets it through `anvil.control`, the Manager from `cli::check_game`, and each words it in the player's language.
    - Texts are looked up by their English words in one catalog per language (`src/i18n/catalog_*.cpp`); a text without a translation stays English. `anvil_i18n_tests` fails when a catalog misses a marked text of the overlay or the Manager, keeps one no source uses, changes the placeholders or uses a character the fonts lack. The catalogs are linked into the overlay and the Manager only, not into `version.dll`.
  - **How it is built** (`plugins/anvil-overlay`, `phase = "early"`, `affects_gameplay = false`, an optional dependency on `anvil-game`):
    - The device: at the entry point it hooks `Direct3DCreate9` and `Direct3DCreate9Ex` in `d3d9.dll` with `hook_function_at`. eu4.exe imports both, but its renderer also looks them up with `GetProcAddress`: in the lab (1.37.5) a replaced import was never called, so the exports themselves are hooked. On the object they return it hooks `CreateDevice` (and `CreateDeviceEx`) with `hook_function_at`, and on the device `EndScene`, `Reset` (and `ResetEx`). No symbols, so it runs on every build. On OpenGL none of this is called; at `anvil-game.ready` the log says the overlay is off.
    - Dear ImGui (a pinned release tag, linked statically; the core, the Win32 and the Direct3D 9 backends, without XInput, IME or shell functions) draws only on the back buffer. The only link it opens is a release page of this repository, on the player's click, through `ShellExecuteW` on a thread of its own. With no notice shown, the panel closed and the badge not shown the `EndScene` hook only passes on: no ImGui frame. The context and the fonts are made the first time something is shown.
    - The badge follows `anvil-game.main_menu` (§13.1): its event marks the overlay dirty, and only while the last event said "main menu" does the `EndScene` hook ask `main_menu()` each frame, which is 0 inside a frame drawn from a save load. In a game neither happens. Without `anvil-game` 1.8.0 or later there is no badge.
    - Input: the game window's procedure is subclassed, in the `CreateDevice` hook before the call goes on, so the overlay sits under whatever the device itself puts on the window. DXVK in exclusive fullscreen hooks the window inside `CreateDevice` and, when its swap chain goes, puts the old procedure back only if its own is still on top; an overlay above it would close a cycle of procedures at the next device (a stack overflow at start, lab#123). The panel key (a setting, `Ctrl+Shift+F12`) toggles the panel, Escape closes it. While something is shown, mouse and keyboard messages go to ImGui; those it wants (`WantCaptureMouse`, and `WantCaptureKeyboard` while the panel is open) do not reach the game, `WM_INPUT` included, and the game's own polling (`GetAsyncKeyState`, `GetKeyState`, imports of eu4.exe) reads those keys as up. Releases always reach the game.
    - Keys for plugins ([#248](https://github.com/happy-dwarf-team/anvil/issues/248), `anvil-overlay.input` version 1, [`anvil_input.h`](../include/anvil/anvil_input.h)): a plugin binds a code, the code of a `key` setting (§5.3) or a mouse button's virtual key with the same modifier bits, and its handler runs on the main thread after the next outer frame (`run_on_main_thread`, like a panel's click). The window procedure matches a key or button press against the bindings in `src/input.cpp`, after the panel key and before ImGui and the game, without calling plugin code; a binding with `CONSUME` keeps the press, not the release, from the game. Nothing is delivered while the panel is open, ImGui takes the keyboard, or `anvil-game`'s `keyboard()` says a text field or the shortcut editor has it; mouse buttons only wait while ImGui takes the mouse. Modifiers count from the window's own key messages as well as `GetKeyState`, so posted messages carry them. The first binding of a code keeps it, and the panel key is taken: a second gets `ANVIL_ERROR_CONFLICT` and stays inert. Anvil does not read the game's shortcuts (`CKeyboardShortcuts`, from `shortcuts.txt`), so a code that is also one fires both unless consumed. `held(code)` answers from the same messages, for keys polled every frame.
    - Notices start when the game is ready (`anvil-game.ready`; without `anvil-game`, with the first frame): safe mode, a new previous crash with "disable X at the next start" for the plugins it was in (code or change level), and the plugins that were not loaded (not those the player disabled). They close after `notification_seconds` or on a click. During a session it shows the ironman notice of §13 while a gameplay plugin acts by opt-in, which cannot be closed and names the plugins and modules (ADR 0011) that act by the player's opt-in, and the plugin-set hash in multiplayer, which the player can close for the rest of that session ([#29](https://github.com/happy-dwarf-team/anvil/issues/29) asks only for the hash; the notice sits over the game's top right corner).
    - The panel has tabs for the status (plugins, reasons, warnings, "disable at the next start"), the settings editor (values as the player's file holds them, "applies at the next start" for `restart = true`, reset to the default, keys recorded from a key press), the crash report ("copy report", the `issues` link) and the log tail.
    - ImGui writes no `imgui.ini`; the clipboard is used only on the player's click or Ctrl+C/Ctrl+V in a text field.
    - The release check is `anvil_net` (`src/net`), the Manager's code: one WinHTTP GET, `tag_name` and `html_url` of the reply, the SemVer comparison, and the Manager's answer read from `manager.toml` without a TOML parser. `winhttp.dll`, `shell32.dll` and `ole32.dll` are delay-loaded, so the overlay's static imports stay `KERNEL32` and `USER32` (EU4 1.37.5 loads all three itself: `eu4.exe` imports `shell32` and `ole32`, its EOS SDK `winhttp`); the thread loads `winhttp.dll` from System32 before its first call.
  - **`anvil.control` v1**, the core's private service for the overlay ([`src/control/anvil_control.h`](../src/control/anvil_control.h), a C table like any service). It is not in the SDK and may change in a minor release; the Manager uses `anvil_core` instead. Every call may come from any thread.
    - Status: the Anvil version, the build, safe mode and the count of unfinished starts, each plugin (resolved, then skipped) with its state, the reason it failed or was skipped (in English and, since 1.7, as a message for the player's language), whether it is suspended and whether it is disabled from the next start. Plugin states are reported once every plugin was processed. The warnings and errors the log got (most of them at start-up).
    - The current session from the gameplay policy, the number of gameplay plugins that act in it, and the hash of §13: the start of a SHA-256 over the `id version` lines of those plugins, shown as `XXXX-XXXX-XXXX-XXXX`.
    - Settings: the schema of each resolved plugin, values as text (current, saved, default) and the changes of §11 (`set`, `set_text`, `reset`), with the warnings of the player's file.
    - The previous crash: the newest report written before this start, with its level, summary, involved plugins, `issues` link and text. It is "new" until a start has shown it; the first call in a process records its name in `anvil/data/anvil/crash-shown.txt`.
    - The log tail: the end of `anvil.log`, read from the file.
    - The player's language (since 1.7): `en`, `fr`, `de` or `es`, chosen at start-up as above and written to `anvil.log`.
    - "Disable at the next start": the id goes into `disabled_plugins` of `anvil/anvil.toml`, only on the player's click. The file keeps every other line and comment: the id is inserted into the existing list, or the key is added before the first table. The edit must read back as the same file plus the id; it is written to a temporary file and replaces the old one in one step. A file that does not parse is not touched.
- **`AnvilManager.exe`** (C++ and Dear ImGui, static):
  - finds the game (Steam `libraryfolders.vdf`, GOG's registry key);
  - installs, repairs and uninstalls Anvil from its own package;
  - enables plugins, approves and revokes;
  - shows what will load and why (`anvil check --game --format json`);
  - shows crashes and logs, and launches through `steam://rungameid/236850` or, in a GOG install, through `dowser.exe`, what GOG's own shortcut starts;
  - runs the start-up benchmark on its Benchmark tab ([spec](spec/benchmark-v1.md) §8): a series of starts of `eu4.exe` from the game folder that quit by themselves, its result, and the report the player may copy or send;
  - has an opt-in GitHub release check that only shows a link, the same code and answer as the overlay's ([ADR 0005](adr/0005-release-check-in-the-game.md));
  - lists the team's plugins on its Catalog tab and installs one on the player's click, from a release CI verified ([ADR 0002](adr/0002-manager-plugin-catalog.md)); with the release check on, it reads the list at start and shows which installed plugins have an update ([ADR 0006](adr/0006-catalog-check-at-start.md)).
  - **How it is built** (`tools/manager`, decided in [#59](https://github.com/happy-dwarf-team/anvil/issues/59)):
    - One exe that is also the installer. The build zips `cmake --install --component anvil` of the same build (`version.dll`, `anvil/` with the standard plugins, the default `anvil.toml` and the docs) into the resource `ANVIL_PACKAGE`; `AnvilManager.exe --check-package` reads it back. Updating Anvil means running a newer Manager.
    - `anvil_manager_core` (no window) holds what it does, with `anvil_manager_tests`:
      - the game from the registry's Steam path, `steamapps/libraryfolders.vdf` (both formats) and `appmanifest_236850.acf`, or `path` of `HKLM\SOFTWARE\GOG.com\Games\2057001589` (32-bit view), with a folder picker as fallback; `eu4.exe` is hashed against the known builds (§6). A folder with `goggame-2057001589.info` is a GOG install, also on a build Anvil does not know; a `steam_appid.txt` says nothing, since GOG ships one too. There the Manager starts the game without Steam, offers no Workshop link, and the benchmark neither asks for Steam nor sets `SteamAppId`;
      - install, update and repair as one plan: missing or differing package files, extra files in a standard plugin's folder, `anvil.toml` only when missing, the Manager's own copy at `<EU4>/AnvilManager.exe` when its bytes differ, and `version.dll` last;
      - a `version.dll` whose version resource does not name Anvil belongs to another mod and is replaced only on the player's word;
      - a legacy text patch (EU4DLL or EU4 Unicode Patch, [GLOSSARY.md](../GLOSSARY.md); lab#104) is recognized by its files next to a foreign `version.dll`, not by release hashes: EU4DLL by `plugins/plugin64.dll`, `.dist.v1.json` or a 32-bit `d3d9.dll` (`Machine` 0x014C, which the 64-bit game never loads), the Unicode Patch by `plugins/eu4_unicode_patch.dll`. The Anvil tab names it and "Replace EU4DLL" (or the Unicode Patch) is the one confirmation, with no checkbox: both are the game's `version.dll`, and Anvil does not support running beside them (lab#42). When `anvil/` is there too, the patch was installed again over Anvil (the Simplified Chinese translation mod's FAQ says to), and the tab says that the plugins and settings are still in `anvil/`; the same button brings Anvil back;
      - nothing foreign is overwritten: before Anvil's files, the plan moves the foreign `version.dll` and the patch's files (EU4DLL: `d3d9.dll`, `.dist.v1.json`, its `README.md`, and `plugin64.dll`, `plugin.ini`, `Plugin.dll`, `dllautoupdater.exe` and `autoupdate64.bat` in `plugins/`; the Unicode Patch: `eu4_unicode_patch.dll`, its folder and log in `plugins/`, `gfx/fonts/eu4-unicode/` and `EU4UnicodePatch.*.txt`) to a new `anvil-legacy-backup/<patch>-<date>/` in the game folder, laid out as they were. `version.dll` moves first, so the game is vanilla from the first step. The backup is outside `anvil/`, so "remove everything" keeps it. An empty `plugins/` goes; other files there stay, and the plan names them. An unknown proxy moves to `anvil-legacy-backup/version-dll-<date>/`;
      - uninstall removes `version.dll` first (the game is vanilla at once), then the standard plugins, Anvil's docs and `AnvilManager.exe`. It keeps the player's plugins, `anvil/data`, the crash reports in `anvil/crashes` (an empty folder goes), the logs and `anvil.toml`; "remove everything" removes `anvil/`. Removing folders never follows junctions. When the running Manager is the copy in the folder, a copy of it in the temporary folder removes it after exit, and the next Manager removes that copy. The Start menu shortcut goes too, if it starts this folder's copy. With a backup, "Restore … from the backup" (off by default) moves the newest one's files back after that, `version.dll` last, keeps a file whose place is taken, and removes the folders it emptied; it warns that saves made with Mei may show garbled text in the old patch;
      - the Start menu entry: "Add to the Start menu" on the Anvil tab, once the folder has the Manager's copy, writes `Anvil Manager.lnk` to this Windows user's `Programs` folder, pointing at that copy. Nothing asks for it at install, and one entry serves one game: the button on another game moves it;
      - plugin zips (`<id>/…` as the template releases them, a flat zip or `anvil/plugins/<id>/…`). The shallowest `plugin.toml` names the plugin's folder, and files beside it (a release's `SHA256SUMS.txt`) are not installed. The zip is checked before anything is copied: names that would leave the folder or alias another, more than 4096 entries or 256 MB are refused. It is unpacked to the temporary folder and checked with `anvil check`'s own code (`cli::check_plugin`) against the game's `eu4.exe`, without writing `symbols.lock`. The Manager shows the name, version, authors, `affects_gameplay`, the problems, a native-code warning and whether the loader would skip it. To install, the plugin is unpacked to `anvil/data/anvil/manager/` and swapped into `anvil/plugins/<id>`, replacing the installed copy of that id after confirmation; `anvil/data/<id>` stays. Standard plugins come only from the package;
      - "What will load" is `cli::check_game`, the code of `anvil check --game`: no child process and no `anvil.exe` of another version;
      - the catalog ([ADR 0002](adr/0002-manager-plugin-catalog.md)): `catalog/plugins.json`, linked in as the resource `ANVIL_CATALOG`, or the same file fetched from `main`. It validates only with `schema` 1, plugin ids, versions and Anvil ranges that parse, pages on `anvil.happydwarf.dev`, Workshop links of `steamcommunity.com/sharedfiles/filedetails/?id=<digits>`, and a pinned zip on `github.com/happy-dwarf-team/<repo>/releases/download/` with its size (at most 256 MB) and SHA-256; unknown keys are ignored. Each entry's state comes from the game folder: not installed, installed (the same or another version), from a mod, or not for this build. A downloaded zip whose size or SHA-256 differs from the pin, or whose `plugin.toml` names another id or version, is refused before the zip check above.
    - The window: Dear ImGui (the tag `anvil-overlay` pins) on Direct3D 11 (WARP without a GPU driver), static CRT, no `imgui.ini`. Tabs for Anvil (install, repair, uninstall), Catalog (the team's plugins, "Get the current list", the number of updates on its label, "Install" or "Update" with a confirmation that names the file, its size and its host, then the zip dialog, the plugin's page and "Subscribe on Steam Workshop" for a Steam game), Plugins (load or not through `disabled_plugins`, `allow_ironman`/`allow_multiplayer` for plugins that offer opt-in, a line and a button to the Catalog tab for a plugin with an update, remove, install from a zip by picker or drop), Settings (the plugin's `settings.toml` against `anvil/data/<id>/settings.toml`), Benchmark (setup, running, result and the report's preview), Crashes, Log and About. "Play EU4" opens `steam://rungameid/236850`. While a benchmark series runs, the other tabs, "Play EU4" and "Change..." are locked, and the game folder is read again only once the series ends.
    - It runs `asInvoker`. When Windows refuses access, it offers to start again as administrator with `--game <folder>`. It imports DLLs from System32 only (`/DEPENDENTLOADFLAG:0x800`, `SetDefaultDllDirectories`), so it never loads Anvil's `version.dll` from the game folder it runs in.
    - While an `eu4.exe` of that folder runs (or one whose path cannot be read), nothing that changes the folder is offered; it checks every 1.5 s. The same check sees an `eu4.exe` from another folder ([#253](https://github.com/happy-dwarf-team/anvil/issues/253)): while Anvil is installed in the chosen folder, the game bar says where that game runs from and offers to switch to it, since Steam's Play starts Steam's copy whatever folder the Manager shows. When a game from the chosen folder ends and `anvil/anvil.log` was not written during it (missing, or last written before the process started; Anvil opens it first thing once Windows loads its `version.dll`), the bar says the game ran but Anvil did not load, with a link to the troubleshooting page. A benchmark series does not count.
    - The Log tab's "Copy a problem report" ([#186](https://github.com/happy-dwarf-team/anvil/issues/186), `src/problem_report.cpp` of the Manager) puts on the clipboard, and shows in place of the log, the fields of the bug forms as Markdown: the installed Anvil, the Manager, the build and SHA-256 of `eu4.exe`, `anvil.toml`, the playset's size, the newest crash report, the game folders Steam and GOG list and which one is chosen (as counts), an `eu4.exe` from another folder and a last game without Anvil (#253), every plugin from `check_game` with its version, source and status or skip reason, and the last 150 lines (at most 32 KiB) of `anvil.log`. It stays English. Before it leaves the Manager, the game folder, EU4's user folder and `%USERPROFILE%` become `<game>`, `<eu4 user folder>` and `<user folder>` (any case, either slash), any other `X:\Users\<name>` becomes `<user folder>`, and the Windows user name where it is a whole path component becomes `<user>`. Nothing is uploaded; "Open an Anvil issue" opens the issue chooser.
    - Its own settings (the game folder, the release check) are in `%LOCALAPPDATA%\Anvil\manager.toml`. The release check asks `api.github.com` for the latest release once per start, only after the player opts in, and opens only links to this repository's releases. Its question says it covers the Manager and the game; a player who answered in the game (the overlay's `release_check` is `on` or `off`) is not asked. The catalog goes to the network on a click, "Get the current list" (`raw.githubusercontent.com`) and "Install" (the pinned zip on `github.com` and the host its download redirects to), and, with the release check on, reads the same list once at start and when the player turns the check on ([ADR 0006](adr/0006-catalog-check-at-start.md)). An update is a plugin in `anvil/plugins` older than its pin, for this build and the installed Anvil; a plugin only from a mod updates through Steam and its approval, and is not one. Without the network the Manager keeps its own list and names the error on the Catalog tab. Nothing is downloaded or installed without the player's click and the confirmations. The exe is not signed, so Windows SmartScreen may warn on the first start.

## 16. Tooling for authors

- **`anvil.exe`** (in the SDK archive) is built from the core's own library:
  - `anvil gen [--lang c|cpp|rust]`;
  - `anvil check` with strict schemas (`plugin.toml`, `symbols.toml`, `settings.toml`, `translations/<code>.toml` with `--complete-translations` to require every text in every language, §5.4) and the names `anvil gen` refuses in any language it writes (the generators' own checks, so a name that cannot be generated is reported at its line before the build), which also verifies, with `--exe`, the build, bytes, memory, pointers, exports, values and instruction boundaries; with `--game <folder>`, every installed plugin, the plugins in the enabled mods (`--user-dir` picks EU4's user folder; §14.1) and the conflicts between them;
  - `--format text|json|github`; exit codes 0 OK, 1 problems, 2 cannot check.
  - `anvil port`, which places a plugin's symbols in a new game build.
- **`anvil gen [--lang c|cpp|rust] <symbols.toml> <out-dir>`** writes both headers without `--lang` (`--lang rust` writes only `symbols.gen.rs`, and `settings.gen.rs` for a settings file, §9) and leaves a file alone when its content would not change:
  - `symbols.gen.h`: `SYM_<NAME>` ids, `sym_<name>_fn` pointer types from `signature`, `SYMVAL_<SYMBOL>_<VALUE>` constants;
  - `symbols.gen.hpp`: namespace `sym` with a constant per symbol whose type `sym::Types::<name>` derives from `anvil::Function<signature>`, `Site`, `Slot` or `Data<type>` ([`anvil_symbols.hpp`](../include/anvil/anvil_symbols.hpp)) and holds `symbol_name`, values as `static constexpr` members (`sym::root_and_date.date_offset`), and an `anvil::Data<type>` for each typed `rip_relative` value (`sym::root`);
  - a value is the integer in the declared bytes, or for `rip_relative` an RVA, which gen cannot know for an `export` symbol, nor when the builds of a file with `builds` give different ones (a comment then says to read it when the game loads). The plugin declares the types named in `signature` and `type` before including the header. Names the C++ header cannot hold (keywords, a value called `id`, `value` or `symbol_name`, two `sym::` names alike) are errors.
- **`anvil gen [--lang c|cpp|rust] [--settings <file>] <settings.toml> <out-dir>`** does the same for settings; one call may take `symbols.toml` and `settings.toml` together. A file is a settings file when `--settings` names it, its name ends in `settings.toml` or it has `[setting.<name>]` tables. Nothing is written when an input fails.
  - `settings.gen.h`: `CFG_<NAME>` ids with the type, default, limits and label in a comment, `CFG_<NAME>_<OPTION>` indices of enum options, and with a key setting `ANVIL_KEY_SHIFT`/`CTRL`/`ALT` and `ANVIL_KEY_VK(code)`;
  - `settings.gen.rs`: the same ids and option indices as Rust constants (§9);
  - `settings.gen.hpp` (includes `anvil.hpp`): namespace `cfg` with a constant per setting whose type `cfg::Types::<name>` derives from `anvil::Setting<T>` (`get()`; T is `bool`, `std::int64_t`, `double`, `std::string`, `anvil::Key`, or for an enum the generated `enum class cfg::Types::<name>_value`) and holds `setting_name`, `restart`, `default_value`, `minimum`/`maximum` when declared, and the options as members (`cfg::mode.get() == cfg::mode.on`);
  - names the headers cannot hold are errors: C++ keywords, an option called `id`, `get`, `setting_name`, `restart`, `default_value`, `minimum` or `maximum`, a setting named like another enum's `<name>_value`, and two `CFG_` names alike.
- **Problems carry lines:** the parsers report the line of the key (one error per file, as the loader stops at it); `--exe` reports every failing symbol at its `[symbol.<name>]` line. `--format github` prints `::error file=<path>,line=<n>,title=anvil check::<message>` with the path as passed, so CI runs it from the repository root.
- **Instruction boundaries** (`--exe`): `symbols.toml` does not say how a symbol is used, so the rule covers every function or site in code with at least 5 declared bytes (a `jmp rel32`):
  - decoded from the first byte (Zydis, x86-64; `??` bytes take the image's values), the declared bytes are valid instructions and the last one ends exactly at their end, so a block replacement owns whole instructions and a hook's stolen bytes lie in verified bytes;
  - the instructions that start in the first 5 bytes, which a hook relocates, include no `JRCXZ`, `JECXZ`, `LOOP`, `LOOPE` or `LOOPNE` (§8.3);
  - a block's `resume` symbol is in code.

  Shorter sites (byte patches, which may start inside an instruction), data and pointer slots are not decoded. Hooking a symbol with fewer than 5 declared bytes would steal unverified bytes; the change registry decides whether to refuse it.
- **`anvil check --game <folder> [--format text|json|github]`** is the start-up analysis (§10) without starting the game, for a folder with `eu4.exe` and `anvil/`:
  - It reads `anvil/anvil.toml` (`enabled`, `disabled_plugins`; a file that does not parse is a problem, as the loader ignores it) and every folder of `anvil/plugins`, in the order the loader discovers them. Each plugin is checked as `anvil check <folder> --exe <folder>/eu4.exe` does, except that nothing is written: no `symbols.lock`.
  - The plugins that pass discovery (a readable `plugin.toml`, the entry DLL present) go to the loader's own resolver (`ResolveInput`, shared code in `anvil_core`): API and `anvil` range, build and symbols, duplicates, disabled ids, `conflicts`, dependencies and cycles. The result is what loads, in which order, and what is skipped and why, in the words of the loader's log. Only what the loader acts on skips a plugin: a failing instruction-boundary check is a problem of the plugin, which still loads.
  - **Overlapping ranges (warning).** `symbols.toml` does not say which symbols a plugin will change, so every verified symbol with `memory = "code"` and declared `bytes` counts as a range `[rva, rva + length)`. For each pair of plugins that load together, ranges that overlap are reported as one warning, at the symbol of the plugin that loads first (`file:line: warning: …`; `::warning` with `github`), naming both symbols, and counting further overlaps of the pair. Ranges that start at the same address are the normal case of a chain (§8.2) and are not reported, whatever their lengths: a length says how many bytes a plugin verifies, not how many it changes. Whether two plugins really change the same bytes is decided at load by the change registry, which refuses the second change (§8.3). A warning never changes the exit code, and plugins that are skipped overlap nothing. Symbols of other memory kinds and symbols without `bytes` are not compared.
  - **Exit codes** as in `check`. 0: nothing is wrong. 1: a plugin has problems, a plugin that is not disabled by the player is skipped, or `anvil.toml` does not parse. What the player disabled (`disabled_plugins`, `enabled = false`) is listed but does not count, and neither do its problems (text and `github` print them as warnings). 2: no folder, no `eu4.exe` (or not a readable game image), no `anvil` folder, or a plugin whose files cannot be read. With 2 for the whole folder nothing else is printed.
  - **JSON** is one object, which the Manager (§15) reads. `plugins[]` has one entry per folder of `anvil/plugins`, each the result `anvil check` prints for one plugin (`result`, `exit_code`, `plugin`, `build` (the build checked: the exe's, if the file declares it), `builds` and `symbols` with a `symbols.toml`, `settings` (their number) with a `settings.toml`, `exe`, `problems`) plus `folder` and `status`; the other sections are always present, empty when there is nothing:
    ```json
    {
      "tool": "anvil 1.0.0",
      "result": "problems",              // "ok" | "problems" | "cannot-check"
      "exit_code": 1,
      "cannot_check": "…",               // only with "cannot-check"
      "game": {"folder": "C:/…/Europa Universalis IV",
               "exe": {"name": "eu4.exe", "sha256": "9AD3EFE1…", "build": "eu4-1.37.5-steam-win64"}},   // build: null when unknown
      "anvil_toml": {"enabled": true, "disabled_plugins": ["old-ui"]},
      "problems": [{"file": "…/anvil.toml", "line": 2, "message": "…"}],   // of anvil.toml
      "plugins": [
        {"folder": "hello", "status": "loaded",   // "loaded" | "skipped" | "disabled"
         "result": "ok", "exit_code": 0, "plugin": "hello", "build": "eu4-1.37.5-steam-win64",
         "builds": ["eu4-1.37.5-steam-win64"],
         "symbols": 3, "exe": {"sha256": "…", "build": "…"}, "problems": []}
      ],
      "load_order": [{"id": "hello", "version": "0.1.0", "phase": "early", "folder": "hello"}],   // early plugins first
      "skipped": [{"id": "other", "folder": "other-1.2", "reason": "conflicts with hello 0.1.0; disable one of them", "disabled": false}],
      "warnings": [{"file": "…/hello/symbols.toml", "line": 7, "message": "…", "plugins": ["hello", "tweaks"]}]
    }
    ```
    `id` in `skipped` is the folder name when `plugin.toml` is broken; `disabled` marks what the player turned off. `file` and `line` are as in `check` (the path starts with the folder as passed; `line` 0 is the whole file). Fields may be added in minor versions; consumers ignore the ones they do not know.
  - Text prints each plugin's problems and summary, then the load order, the skipped plugins and the warnings. `github` prints annotations for the problems, for a plugin that is skipped without problems of its own, and for the warnings.
- **`symbols.lock`:** `anvil check --exe` writes the `eu4.exe` SHA-256, a digest of `symbols.toml`, the tool version and the result. CI, which cannot have the game, fails when the lock is stale.
  ```toml
  format = 1
  tool = "anvil 1.0.0"                # informational
  symbols_sha256 = "…"                # symbols.toml without a UTF-8 BOM, CRLF read as LF (git autocrlf)
  exe_sha256 = "9AD3EFE1…771A"
  build = "eu4-1.37.5-steam-win64"    # the exe's build, or "unknown"
  result = "ok"                       # or "problems"
  ```

  It lies next to `plugin.toml` and is committed. `--exe` writes it also when it finds problems, so CI fails until they are fixed, but not when a file does not parse or the exe cannot be read. Without `--exe`, a plugin with `symbols` fails on a missing lock, a different digest, `result = "problems"`, an unknown key or a newer `format`.

  A `symbols.toml` with `builds` (§5.2) gets format 2, with one check per build in the order of `builds`. `--exe` replaces the check of its exe's build and keeps the others while the digest is the same, so the author runs it once with each build's `eu4.exe`; an exe of a build the file does not declare is reported and leaves the lock alone. Without `--exe`, every build needs a check, and each must be `ok`:
  ```toml
  format = 2
  tool = "anvil 1.7.0"
  symbols_sha256 = "…"

  [[check]]
  build = "eu4-1.37.5-steam-win64"
  exe_sha256 = "9AD3EFE1…771A"
  result = "ok"

  [[check]]
  build = "eu4-1.37.5.2-gog-win64"
  exe_sha256 = "D29CF745…5319"
  result = "ok"
  ```
- **SDK:** `anvil-sdk-windows-x64.zip`, an asset of every release next to `anvil-windows-x64.zip`, in `SHA256SUMS.txt` and with the same build attestation. CI builds it with the release's own `anvil.exe` and builds the examples from it before it is published. [`sdk/`](../sdk) holds its CMake files and README.
  ```
  include/anvil/{anvil_plugin.h, anvil.hpp, anvil_symbols.hpp, anvil_game.h}
  bin/anvil.exe
  cmake/AnvilPluginConfig.cmake, AnvilPluginConfigVersion.cmake
  CMakeLists.txt  README.md  LICENSE  version.txt  third-party/  SHA256SUMS.txt
  ```
  - **Use.** A plugin repository takes the package with `FetchContent` of the release URL (the root `CMakeLists.txt` of the archive defines `anvil_add_plugin`), or with `find_package(AnvilPlugin REQUIRED)` on an unpacked archive (`-DAnvilPlugin_DIR=<sdk>/cmake`; `AnvilPluginConfigVersion.cmake` accepts a release of the same major version):
    ```cmake
    cmake_minimum_required(VERSION 3.25)
    project(my_plugin LANGUAGES CXX)

    include(FetchContent)
    FetchContent_Declare(anvil_sdk
        URL https://github.com/happy-dwarf-team/anvil/releases/download/v1.0.0/anvil-sdk-windows-x64.zip
        URL_HASH SHA256=<hash from the release's SHA256SUMS.txt>)
    FetchContent_MakeAvailable(anvil_sdk)

    anvil_add_plugin(my_plugin SOURCES src/plugin.cpp)
    ```
  - **`anvil_add_plugin(<target> SOURCES <files>… [PLUGIN_DIR <dir>])`** reads `plugin.toml` in the calling `CMakeLists.txt`'s folder (`PLUGIN_DIR` names another one): `id`, `entry`, `symbols`, `settings` and `api`, which must be the API the SDK builds for (2), or configuring fails. It runs `anvil gen` on `symbols.toml` at build time (`symbols.gen.h`, `symbols.gen.hpp`, plus the settings headers when `settings` is set), builds `<entry>` as a shared library with the static CRT, C++20 (C11 for C sources) and `/W4`, and lays out `<build>/plugin/<id>/{plugin.toml, symbols.toml, settings.toml?, translations/*.toml?, <entry>.dll}`, one folder for every configuration. Warnings are not errors unless the repository says so (`CMAKE_COMPILE_WARNING_AS_ERROR`).
  - **`<target>-check`** runs `anvil check <plugin folder>`, and with `-DEU4_EXE=<eu4.exe>` `anvil check <plugin folder> --exe <eu4.exe>`, which also rewrites `symbols.lock` for the author to commit. **`<target>-deploy`** exists when `-DEU4_DIR=<game folder>` is set and copies the plugin folder to `<EU4_DIR>/anvil/plugins/<id>`.
  - **CI of a plugin repository:** build, `<target>-check` (the lock check) or `bin/anvil.exe check <folder> --format github`. The tree builds `examples/hello-*` through the same package, and `tests/sdk/verify-sdk.ps1` builds them from the archive both ways (`FetchContent`, `find_package`).
- **Template:** `happy-dwarf-team/anvil-plugin-template`. Its CI runs `anvil check --format github` and the lock check; its release workflow zips the plugin folder, or a mod folder for the Workshop. The README covers a first plugin in 15 minutes.
- **`anvil port <symbols.toml> --from <old eu4.exe> --to <new eu4.exe> --build <new id> [--out <file>] [--from-build <id>] [--unknown-build] [--format text|json|github]`** places a plugin's symbols in a new build of the game, so a game update does not mean moving hundreds of addresses by hand. It reads both files and writes only `--out`, which may be the input; without `--out` it only reports. `--build` must be the id the table of builds (§6) has for `--to`, since a file with any other id never loads there. `--unknown-build` is the way out for a build the table does not have yet, as when trying a port before its Anvil exists: then the id must have the form `eu4-<version>-<store>-win64`. Nothing scans at load time: this is an author's tool, and the result is an ordinary `symbols.toml` that the loader verifies as ever.
  - **Matching.** Every byte comes from the old `eu4.exe`, so a file on digests ports like one on `bytes`. Code is compared with what the linker decides zeroed: `rel32` and `rel8` targets, RIP-relative displacements, jump tables and displacements through a register `lea reg, [__ImageBase]` loaded in the same code; a large displacement through any other register is a struct offset, an operand like the rest. In order:
    1. Functions (from `.pdata`, with the leaf functions between its entries) whose zeroed bytes occur once in each build are pairs. What falls out of the longest run that keeps their order is dropped.
    2. Each pair's references pair up what they point to: functions (even ones that occur twice) and data. Functions of the same shape, which differ only in operands such as a moved struct field, pair up the same way. A vtable counts only as a whole and only if nothing known speaks against it: one slot added in the middle would pair every later method with its neighbour, and those look alike.
    3. A symbol in a paired function is at the same offset; in a paired function that changed, a window around its bytes is searched in that function; anywhere else, in the whole image. The window must occur once in each build (among copies, the one between the same two paired functions counts), keep the order of the pairs and refer to what the pairs say.
    4. A slot takes its place from the table around it: the slots that hold known functions name the place, and the walk out from the slot stops where the table stops agreeing, so a method added far away does not matter. Data takes its place from the instructions that refer to it. An export needs nothing.
  - **Refusing.** A wrong address is worse than none, so anything less than the above stays by hand, with the reason and, where one is known, a place to look at:
    - no match, more than one, or one that is out of order or refers to other functions;
    - code that differs in an operand no `values` entry declares. Such an operand is what the code means, and a plugin that replaces a block may depend on it. The entry of a paired function is still placed, since a hook on it does not care, and the report says to read it (a warning, `review` in JSON);
    - a symbol that does not verify on the old build, or is `absent` there;
    - a symbol that fails what `anvil check --exe` would check on the new build: bytes, memory, pointers, values with `is`, instruction boundaries. `anvil port` runs those checks itself, whether or not this Anvil knows the build. In a file with `bytes`, `is` is a promise a person wrote, so a value that moved stays by hand. In a digest, `is` is also how the converter states every value, field offsets and addresses included, so there a value that moved loses its `is` and the symbol is placed, as with `bytes`: the new digest still pins the code, `anvil gen` reads the value when the game loads, and code that uses it as a constant stops compiling. The report names those values (`values_changed` in JSON).
  - **Writing.** Only what changes changes: comments and layout stay. `builds` gets the new id (a file with `build` becomes one with `builds`), and each symbol a `build."<id>"` entry in the style of its table, with `bytes` or `sha256` as the symbol has them. A shared key that no longer holds moves to every build's table; its comment stays on its line. A symbol left by hand gets a comment that names the reason and no address, so the file does not parse until a person places it or declares it `absent`, and a shared `rva` cannot claim the old place for the new build. A complete file is read back and verified on both builds before it is written.
  - **Output.** Text and `github`: one line per symbol left by hand (an error) or to read (a warning), then a summary. JSON: `symbols` (name to address, only what was placed, the form Mei's `tools/gen-symbols.py` reads), `ported[]` and `manual[]`. No output holds bytes of the game. Exit codes: 0 every symbol placed, 1 some left by hand, 2 cannot port.
  - **What it was tried on** (2026-11-05): the five `symbols.toml` of the team (644 symbols), Steam 1.37.5 to GOG 1.37.5.2 and back: 98.4 % of the 1278 placeable symbols placed, 0 wrong, every placed `bytes` and `points_to` equal to the file's, `anvil check --exe` clean on both builds, and the 5 symbols the GOG build lacks refused. A unique search for the declared bytes alone places 33 %, with one wrong. With the code of 1228 symbols overwritten in a copy of the exe (never run), nothing was placed. These two builds are close: 79,000 of about 102,000 stretches of code pair up as identical. A real update will place fewer, and the checks above are what keeps that from turning into wrong addresses. The procedure for a game update is in the lab, `docs/new-game-version.md`.

## 17. Performance budgets

Decided in [Performance budgets and how the lab measures them](https://github.com/happy-dwarf-team/anvil/issues/20).

| Quantity | Budget |
| --- | --- |
| Core start-up, no plugins (entry point → end of loading) | ≤ 20 ms; no difference in `Running application` detectable within the spread |
| Same with `anvil-game` and `anvil-overlay` | ≤ 50 ms |
| Function hook, call redirect: first / each further link | ≤ 2 ns / ≤ 2 ns |
| Mid hook, block replacement: first / each further link | ≤ 25 ns / ≤ 20 ns |
| Pointer swap, byte patch, import replacement | 0 |
| All changes of the standard plugins in a SIMULATION year | ≤ 0.5% |
| Memory: core / per change | ≤ 2 MB / ≤ 1 KB |
| An ImGui frame with plugin windows shown, per frame | ≤ 200 µs (two windows took 54–62 µs on the lab machine at 3440×1440, 8.6 ms in the frame that first draws CJK text) |

**Method**
- In-game impact is per-call cost × counted calls; the optional counters are enabled with `profile = true`.
- Microbenchmarks run in CI as a ±20% regression check. Binding numbers come from the lab: i9-13900KF, CPU4, median of ≥ 30.
- **Microbenchmarks** (`anvil_bench`, `bench/`; built with the project, not a ctest):
  - each case is a small function in scratch memory, changed through the change registry and called in a dependent loop. The cases are a plain call, a function hook and a call redirect with 1 and 8 links, a mid hook with 1 and 8 empty callbacks, and a block replacement next to the block's own code. The parser cases parse and resolve a typical set (10 `plugin.toml`, a 48-symbol `symbols.toml`);
  - repetitions interleave the cases; each case reports the median in ns per call;
  - a first link = 1 link − the unchanged code; each further link = (8 links − 1 link) / 7.
- **CI gate** (`bench/check-regression.ps1`):
  - Runners differ in CPU model and clock, so the gate never compares nanoseconds. It compares each case's ratio to the plain call of the same run.
  - It compares only against a baseline from the same CPU model: `bench/baseline-ci.json` keeps one entry per runner CPU.
  - Each case takes its lowest ratio of three runs. The baseline keeps the highest ratio seen on that CPU, because a short path can differ by almost 20% between two processes on the same runner (code placement). A case more than 20% slower than its baseline fails the build. A case more than 20% faster prints a notice to lower its baseline.
  - On a CPU model without an entry the gate prints the entry to add and does not fail.
  - A baseline is raised only with a reason in the pull request.
- **Lab:** `bench/run-lab.ps1` runs `anvil_bench` pinned to CPU4 at high priority, with ≥ 30 repetitions and EU4 closed, and checks the budgets above.
- **Memory** (`anvil_bench --memory`; in the lab `bench/run-lab.ps1 -Memory`, which exits 1 over a budget; CI only runs it, as the bytes depend on the Windows build):
  - The budgets count commit charge (`PrivateUsage` of `PROCESS_MEMORY_COUNTERS_EX`). The result also gives the busy heap bytes (`HeapWalk` over every process heap, without the heap's headers) and the committed private executable pages (Anvil's stubs, SafetyHook's trampolines) apart.
  - **Core:** a child process runs the real `anvil::startup()` on a game folder with the shipped `anvil.toml` and no plugins, and compares itself before the call and after the main-phase thread has ended. That covers the runtime, the change registry, the ServiceBus, the log, the settings, safe mode, the gameplay policy, the build table lookup, and crash capture with its helper thread (its committed stack; the 512 KB reservation is reported, not counted) and the DLLs it loads (`dbghelp.dll` and its dependencies, listed with their sizes; their writable pages count). The game may already have some of those DLLs loaded, so the number is an upper bound.
  - `version.dll`'s image is not private memory. Its writable sections (from its headers; mostly the crash writer's static buffers) are added to the core; its code and read-only data are file-backed and shared, so `SizeOfImage` is reported but not budgeted. This is the method of record for the core budget: start-up private bytes plus `version.dll`'s writable sections, without its image.
  - Not in the core number: what the static CRT of `version.dll` allocates before `AnvilEntry`, the transient buffers of the `eu4.exe` SHA-256 check after loading, and a thread-pool worker that loading `dbghelp.dll` sometimes starts (about 40 KB, named in `other_threads_added` when it is in the measurement).
  - **Per change:** for each kind, 1000 changes of one owner at 1000 distinct sites of a synthetic image (functions in executable scratch memory, a PE header with 1000 imports for import replacements), with a snapshot every 100 changes. Bytes per change are the slope of a least-squares line, so a stub arena or a vector's growth counts by its share. The plugin's own memory (its `next` pointers) exists before the first snapshot. Each kind reports the executable bytes per change apart; the budget is the total. Each kind runs in its own child process: in one process, heap commit one kind left behind or a commit step it ended on landed on the next kind's slope (a block replacement read about 1645 B right after the mid hooks, about 1450 B alone).
  - Mid hooks and block replacements fit the same 1 KB since 1.1 (#90). 1.0 used about 1520 B and 1540 B: each code site with them had its own executable context stub (391 B) and about 774 B and 791 B of heap in 12 blocks. Now the context stub is shared (each address adds a 24-byte thunk), a change keeps its label and symbol in one block, a dispatch list is one block, and a site keeps its few original bytes in place (heap about 638 B and 653 B in 9 blocks). About 270 B of a code site's heap is SafetyHook's own: the `InlineHook` object (168 B), its copy of the stolen bytes, and a node of its thread-trap map (96 B) that it keeps for every hooked address.
- Start-up: `anvil.log` times everything from `AnvilEntry`: `resolved … at`, `early phase done at`, and `plugins loaded at`, which is the end of the core start-up; `game ready at` follows `anvil-game.ready`.
- Start-up follows the lab procedure, with ≥ 5 runs per variant. The start-up budgets apply to a usual start, with Anvil and the plugins already installed by an earlier start: the lab installs once, starts the game once without measuring, then measures each further start of the same installation.
- The first start after the DLLs were copied is reported apart and has no budget. Loading a freshly written plugin DLL took about 27–30 ms in the 1.3.0 lab run, against about 0.5 ms on later starts of the same installation (probably an antivirus scan of the new file; not verified) (EU4 engine lab, `docs/anvil-1.3-budgets.md`).
- SIMULATION A/B through Tongs, with ≥ 5 valid trials, guards only against ≥ 2% regressions.

Exceeding a budget blocks the release and any change to stubs, chains or core start-up.

**Call counters** (`profile = true` in `anvil/anvil.toml`, default `false`)
- Every function hook, call redirect, mid hook and block replacement requested while it is on counts its calls in its own atomic 64-bit counter, one per cache line.
- Function hooks and call redirects:
  - The chain enters the link through a 32-byte counting stub: `push rax; mov rax, counter; lock inc qword [rax]; pop rax; jmp detour`.
  - Flags are dead at a function entry and at a call, so the stub only saves `rax`.
  - The link's own `next` is unchanged.
- Mid hooks and block replacements: the shared context stub calls a counting copy of the dispatch loop, which counts each callback it calls. A callback skipped because an earlier mid hook changed `rip` is not counted.
- Off, none of this exists: chains enter detours directly and the context stub calls the plain dispatch loop. The setting is read at start-up.
- Pointer swaps, byte patches and import replacements are not counted: their cost is 0 (see the table above).
- **Report:** when the process exits, `anvil.log` gets one line per counted change under its plugin's id (at info level), e.g. `calls: function hook rva 0x8024D0 [speed_object]: 1234 calls`.
  - Removed changes that saw calls are included.
  - The report is written when the game calls `ExitProcess` (its C runtime does when `main` returns: the menu's Exit, `SDL_QUIT`), through the core's own replacement of that import of `eu4.exe`, before Windows stops the other threads and detaches the DLLs. `DLL_PROCESS_DETACH` is only the fallback: in the lab a DLL that detaches before Anvil ended the process there (a graphics driver behind the Epic overlay's detach, [#164](https://github.com/happy-dwarf-team/anvil/issues/164)). The same exit work puts the safe-mode count back for an exit before readiness (§10).
  - The report never waits for a lock: if another thread holds one, the report is skipped with a warning.
  - A process that is terminated (rather than exiting) writes no report.

## 18. Logging and status

`anvil/anvil.log` is UTF-8 and rotated. It records the Anvil version, the build, each plugin's status and reason, its changes, verification failures with the symbol and byte, timings and the call counters. The same status is published as the `anvil.status` service and event for the overlay and the Manager.

After the build line it names the platform, so a report from Linux says what the game ran on (`src/core/platform.cpp`; read and logged only, nothing behaves differently):

- `Wine: <version> (build <build id>, host <system> <release>)` from `ntdll`'s `wine_get_version`, `wine_get_build_id` and `wine_get_host_version` under Wine or Proton; `Wine: not detected` on Windows.
- `d3d9.dll: …` with the company and file version of its version resource and whether it came from the system folder, the game folder or elsewhere. DXVK's resource copies the version of Microsoft's `d3d9.dll`, so for DXVK the line starts with the version DXVK keeps next to its `DXVK: ` log prefix (`d3d9.dll: DXVK v2.7.1 (version resource: DXVK 10.0.17763.1, system folder)`); Wine's own `d3d9.dll` (wined3d, `PROTON_USE_WINED3D=1`) gives the company `Microsoft Corporation` and the product `Wine <version>` (`include/ntverp.h.in`). Microsoft's `d3d9.dll` is not searched for that string.

## 19. Security

Plugins are native code with the game's permissions:
- local plugins are trusted by placement, and plugins from mods (Workshop or local) need per-version approval;
- Anvil and its standard plugins never download anything. The release check of the Manager and of `anvil-overlay` is opt-in, reads one JSON reply from `api.github.com` and only links to this repository's release pages ([ADR 0005](adr/0005-release-check-in-the-game.md)); `version.dll` and the other standard plugins never use the network. Its catalog reads the list at start only with the release check on ([ADR 0006](adr/0006-catalog-check-at-start.md)), downloads a plugin zip only on the player's click, and installs a zip only when its SHA-256 equals the pin in `catalog/plugins.json`: a pin changes only through a pull request whose CI verified the release's attestation ([ADR 0002](adr/0002-manager-plugin-catalog.md));
- releases are built by GitHub Actions with SHA-256 sums and provenance attestation. They are not signed, decided on 2026-11-02 ([#157](https://github.com/happy-dwarf-team/anvil/issues/157)): players who want to check a download compare it with `SHA256SUMS.txt` or the attestation, and SmartScreen asks once on the first start.

## 20. Build, test, release

- CMake, with MSVC (VS 2022) as the reference toolchain. C++23 inside Anvil, a C ABI at the boundary, and the static CRT.
- **Tests:** unit tests for manifests, symbols, settings, the resolver, chains (orders, removal in any order, conflicts, staging, suspension) and the CLI, plus microbenchmarks, the Rust crates' tests and the two Rust examples.
- **Lab checks:** the vanilla copy, the proxy without `anvil/`, each standard plugin, the budgets, and E1/E2 for the Workshop.
- **Releases:** release-please and Conventional Commits, squash-merged PRs; the release workflow attaches the Anvil zip (`version.dll`, `anvil/` and `AnvilManager.exe`), `AnvilManager.exe` on its own, the SDK archive (with `anvil.exe`) and publishes `anvil-sys` and a new version of `anvil-sdk`. The three assets are in `SHA256SUMS.txt` and have a build attestation.

## 21. Open items and fog

The fog of the map ([#12](https://github.com/happy-dwarf-team/anvil/issues/12)) was reviewed on 2026-10-26. Each item ended as a decision, an issue, or not planned.

**Decided**
- The ABI stability policy for API 2: [ADR 0001](adr/0001-plugin-api-2-compatibility.md), summarized in §9 and in `CONTRIBUTING.md`.
- The safe Rust crate ([#159](https://github.com/happy-dwarf-team/anvil/issues/159)), 2026-11-09: built now as `anvil-sdk` (§9), without waiting for an outside Rust author. Its 1.0 and a Rust variant of `anvil-plugin-template` wait for the first real Rust plugin.
- Code signing ([#157](https://github.com/happy-dwarf-team/anvil/issues/157)), 2026-11-02: not planned. Releases keep SHA-256 sums and the build attestation (§19), and the README says how to check them and how to report an antivirus false positive.

**Issues**
- Localization of player messages in the overlay and the Manager: [#158](https://github.com/happy-dwarf-team/anvil/issues/158). Decided on 2026-11-02: the language follows EU4's own language setting; English, French, German and Spanish come first; Chinese, Japanese and Korean come with the Unicode Patch port ([#104](https://github.com/happy-dwarf-team/anvil/issues/104)), which brings the fonts they need. English, French, German and Spanish are in (§15), and since 1.9 Simplified and Traditional Chinese, Japanese and Korean with Mei (§15); every catalog but English waits for a native speaker's check, and who checks them is still open.
- Other storefront builds ([#36](https://github.com/happy-dwarf-team/anvil/issues/36)): GOG 1.37.5.2 and 1.37.5.0 are known and `anvil-game` declares them ([#160](https://github.com/happy-dwarf-team/anvil/issues/160), [#187](https://github.com/happy-dwarf-team/anvil/issues/187), §5.2, §6); each plugin declares them in its own release.
- Call counts of text hooks (a lighter context): a step of the Unicode Patch port, [#104](https://github.com/happy-dwarf-team/anvil/issues/104#issuecomment-6407336807).

**Not planned**
- Hot reload: nothing is unloaded in API 2 ([ADR 0001](adr/0001-plugin-api-2-compatibility.md)), so a publisher of services never disappears under its consumers; a restart and `anvil check` without the game are the authoring loop.
- A shared address library: the builds Anvil supports no longer change, and every plugin verifies its own bytes, so plugins share knowledge of the game through the services of library plugins (`anvil-game` today) instead of shared addresses. With the GOG build, each plugin's `symbols.toml` lists its own per-build addresses (§5.2); the question comes back if several plugins need the same tables ([#160](https://github.com/happy-dwarf-team/anvil/issues/160)).
- A vcpkg port: the SDK is headers and a host tool that `FetchContent` with a hash already delivers, and a port would build `anvil.exe` from source instead of the attested release binary ([research](https://github.com/happy-dwarf-team/anvil/blob/research/vcpkg-port/docs/research/vcpkg-port.md)). It comes back as an issue when an author asks.
- OpenGL in the overlay: Direct3D 9 is EU4's default renderer on Windows, and everything the overlay shows is also in the Manager.
- Setting profiles per playset or save: nobody has asked, and `restart = false` settings change in the overlay while the game runs.
- Automatic exchange of the gameplay plugin-set hash between multiplayer peers: gameplay plugins are suspended in multiplayer unless the player opts in, the hash compared by hand covers that, and a channel in the game's netcode risks desyncs.
- The Microsoft Store and Epic builds: the Store's `eu4.exe` is encrypted on disk, so its bytes cannot be verified, and nothing is known about Epic's.
- The Paradox forum's Workshop rules (E3): read, and recorded in §14 with the conflict between their rule 4 and open-source license notices.

## 22. Delivery

Decided in [Rewrite or evolve, and the pull request sequence to Anvil 2](https://github.com/happy-dwarf-team/anvil/issues/28).

### 22.1 Evolve, not rewrite

- **Kept:** the proxy and entry-point hook, log, semver, resolver, services and events, build identity, CI and release-please.
- **Rewritten:** the change layer (`PatchRegistry` → a change registry with chains) and the public header.
- **Replaced:** `[[expect]]`, by `symbols.toml` verification.

### 22.2 Versions, repositories, releases

- **Versions:** the first release with plugin API 2 is **Anvil 1.0.0**. The Anvil version and the plugin API version are separate numbers.
- **Repositories:** `plugins/anvil-game`, `plugins/anvil-overlay` and `tools/manager` live in this repository (one CI, one release archive). `anvil-plugin-template`, Bellows and Tongs have their own repositories.
- **Releases:**
  - **1.0:** core with API 2, `anvil.exe`, SDK, template, `anvil-game`, settings, crash capture with safe mode, `anvil-sys`, and the migrated Bellows and Tongs.
  - **1.1:** `anvil-overlay` with the settings panel.
  - **1.2:** the Manager.
  - **1.3:** Workshop discovery and trust.
- **API 1:** no compatibility layer. Anvil 1.0 skips `api = 1` plugins with "plugin API 1: update the plugin". Bellows and Tongs migrate before 1.0 and release with it.

### 22.3 Pull-request sequence

One PR per line, Conventional Commits, squash-merged. Implementation issues and milestones track them.

**1.0**
- **A. Foundations**
  1. Split the core into a library shared by the loader and a CLI target.
  2. Manifest v2.
  3. `symbols.toml` parsing and verification.
  4. `anvil.exe gen` and `check` (`symbols.lock`, `--format json|github`).
- **B. Changes** (after A)
  5. Change registry with chains, staging and handles: function hook, call redirect, byte patch, import replacement, `*_at`.
  6. Mid hooks and block replacement.
  7. Pointer swap.
  8. Suspend and resume of a plugin's changes.
  9. The API 2 header, the C++ wrapper, and hello examples in C and C++.
  10. Microbenchmarks with a CI regression gate.
- **C. Around the plugins** (after B; independent of each other)
  11. Settings.
  12. Crash capture and attribution with safe mode.
  13. `conflicts` in the resolver.
  14. `anvil check --game`.
  15. Call counters.
  16. SDK archive and CMake package.
- **D**
  17. `plugins/anvil-game` with the gameplay policy.
  18. `anvil-sys` with `anvil gen --lang rust`.
- **E. Elsewhere:** the `anvil-plugin-template` repository; the Bellows migration; the Tongs migration.
- **F:** lab measurements against §17, then the release-please PR.

**1.1:** `plugins/anvil-overlay`. **1.2:** `tools/manager`. **1.3:** Workshop discovery, `trust.toml`, approved copies, and the approval UI in the overlay and the Manager.
