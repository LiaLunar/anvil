# Plugin map modes live in a separate standard plugin, `anvil-map`, on borrowed numbers

Status: **accepted** (2026-11-09). Nothing here is built yet; the service is specified in [docs/spec/map-modes-v1.md](../spec/map-modes-v1.md).

A plugin author can read the world (`anvil-game.world`, [ADR 0004](0004-plugins-without-addresses.md)) but has no way to show what they computed on the map. The map mode is how EU4 shows a value per province: a button on the minimap's list, a colour for each province and a few lines in the province's tooltip. This ADR decides where a service for plugin map modes lives, how a mode gets a place in the game's closed list of map modes, and how its text reaches the game. Two of these choices are hard to undo once plugins use them: the plugin that carries the service and the numbers its modes borrow.

How to read the evidence. **[code]**: read in this repository, a team plugin or the game's files. **[pdb]**: static, from the GOG build's `eu4.pdb`, the bytes of both builds and their disassembly. **[live]**: seen in the game in a lab run on 9 November 2026 (Steam 1.37.5 and GOG 1.37.5.2, a probe plugin that only counted calls). **[hypothesis]**: not checked. No game was started for this ADR.

## Decision

1. **A new standard plugin, `anvil-map`**, publishes the service `anvil-map.modes` version 1. It depends on `anvil-game`, has `affects_gameplay = false` and its own `symbols.toml` (about 14 symbols per build, Steam and GOG). It ships and is released with Anvil, like `anvil-log-throttle`.
2. **A plugin mode borrows a number** of the game's map mode enum for the whole process. The pool has 13 numbers: first 80–83, then the nine debug modes 50, 57–60, 66, 68, 71 and 84, lent in the order modes are registered. A fourteenth mode is refused.
3. **The borrowed number belongs to the plugin entirely**, including when the player turns the original mode on from the console (`mapmode <name>`). The header and the documentation say so; there is no console hook.
4. **The plugin gives the colours as a buffer**, not a callback at paint time; the service copies it and the paint hook reads only the last full buffer.
5. **The API takes UTF-8 only.** The service passes it to the game unchanged when Mei reports UTF-8 text (`mei.text` version 1, an optional dependency), and otherwise in Windows-1252 with `?` for what does not fit.

## Why a separate plugin

- **Not in `anvil-game`.** `anvil-game` is what suspension and safe mode stand on (design §10, §13): every gameplay plugin depends on it, and a port to a new build may declare its world symbols absent so that it never holds the port up. A map mode hooks six functions that draw the map, build tooltips and write `settings.txt`. A fault in one of those hooks, or a port that has not found them yet, must not take suspension with it. A separate plugin fails, is disabled or is skipped on its own, and its dependents are skipped with the reason (§4).
- **Not in `anvil-overlay`.** The overlay draws Anvil's own ImGui window and knows nothing about the game; map modes are the game's own widgets and memory.
- **Not in Tongs.** Tongs has `affects_gameplay = true` and a control pipe; a plugin that only paints the map should not bring either to players. This is the reason ADR 0004 gave for the world reads.
- **The rule of design §4 holds:** the core does not know the game beyond its build; knowledge only some plugins need stays in a library plugin and its service. A standard plugin is that, maintained and released with Anvil, so authors can depend on it with a version range.

## Why borrowed numbers

The enum is closed [pdb]: the three switches on the map mode (colour, name, province tooltip) compare with `0x54` and jump through tables of 85 entries. A mode above 84 would need byte patches in three jump tables of both builds. A borrowed number needs none: the service hooks the functions that answer for a number (colour, name, icon, button, tooltip) and answers for its own numbers, passing every other number to the game.

Which numbers [pdb, live]:

- **80–83** (`EXPANSION`, `CONTENT`, `UNIT`, `MUSIC` in the PDB) have no console name, no button and no paint of their own (one shared default case). In the lab run, no console name reached them. They go first.
- **50, 57–60, 66, 68, 71, 84** are debug modes with a fixed label and no button. All nine are reachable from the console (`continent`, `armyeval`, `threateval`, `buildingeval`, `aihre`, `pathfind`, `airegion`, `technologygroup`, `aidev`). The game itself marks 57, 58, 59, 66 and 68 dirty, which only repaints them when they are shown, so it repaints the plugin's colours.
- **Not lent:** 13 (the siege panel switches to it), 40 (the front end; it shares the political colour and the game marks it dirty), 41–43 (fog of war, the player's combined mode, nudge colours), and 44, 45, 47 and 65, which have console names and paint of their own.

Because every number in the pool is reachable from the console, a player can always type the original's name. Deciding that the number belongs to the plugin is simpler and more honest than a console hook: the console is a debug tool, and the header names the console names of each number so that an author is not surprised. The id of a mode, `<plugin-id>.<name>`, is what plugins and tools use; the API never promises a number, and a mode may get another one at the next start when plugins register in another order.

## Why a buffer

The game paints the map and builds the province tooltip also in frames drawn inside a game update [live]: at speed 5, 8 of 11 paints and 9 of 41 tooltips on Steam, 2 of 14 paints and 8 of 27 tooltips on GOG, ran with `outer = 0`. `anvil-game.world` refuses those frames because they see a day half applied (design §13.1). A callback at paint time could not read the world, so the plugin computes in its own step (an `anvil-game` event or queued work) and hands the service a finished buffer. The tooltip follows the same rule: the service asks the plugin for a province's text in its step after a frame of the main loop and shows the last answer, at worst one frame late.

## Why UTF-8 and `mei.text`

`anvil-game.world` already answers UTF-8. With Mei's `utf8_localization` (on by default) the game keeps and draws text in UTF-8; without Mei, in Windows-1252 [code: Mei]. The service must know which, and reading Mei's `settings.toml` would tie it to a file Mei may change. A small service of Mei, `mei.text` version 1 (`game_text_utf8()`), answers it; without that service (no Mei, or an older one) the service writes Windows-1252. Text is literal in v1: the service escapes the game's colour and key characters (`§`, `$`); colour can come later as a field appended after `size`.

## Considered options

- *The service in `anvil-game`, as a family next to `world`.* Rejected for the reasons above: a fault or a missing port of map hooks would cost suspension and safe mode.
- *New numbers above 84.* Rejected: three byte patches of jump tables per build, which no other change of Anvil needs, for no gain over the 13 numbers the pool has.
- *A pool of 17 with 44, 45, 47 and 65.* Rejected after the lab run: they have console names and their own paint.
- *A pool of 15 with 13 and 40–42*, as another loader uses. Rejected: the game uses 13 and 40 itself.
- *A console hook that keeps `mapmode <original>` for the game's mode.* Rejected: one more hook in a debug path, and a console mode that paints a plugin's colours under another name would still confuse.
- *A colour callback at paint time.* Rejected: it would run in nested frames, where the world may not be read.
- *A flag in Mei's settings read by the service.* Rejected: a file format of another plugin becomes a contract it never offered.
- *Map modes from files for modders without C++, checked by Etch.* Not in v1; it can be built on this service later.

## Consequences

- Anvil gains a fourth standard plugin, `anvil-map`, released with Anvil from 1.17 (working number); `anvil/plugins/anvil-map/` in the release zip, `symbols.lock` committed and checked in CI.
- Design §4 lists it; `include/anvil/anvil_map.h` is its C interface.
- Without plugin modes the service changes nothing the player sees: no button is added, every hook passes the game's numbers on. Its hooks still run on every paint and tooltip, so their empty cost is measured in frame time before the release.
- Etch's corpus runs without plugin modes and sees the game's buttons unchanged. Tongs reports `map_mode_id` next to the raw number from the service's `mode_id(number)`.
- A loader that hooks the same functions and uses the same pool (`eu4agent.dll` of europagiga) cannot run next to `anvil-map`: the plugin finds that DLL, logs it and installs no hooks.
- Hidden information is not filtered (ADR 0004): a mode that shows in multiplayer what the player cannot see does not enter the catalog.
