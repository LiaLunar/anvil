# Plugin map modes v1 specification (`anvil-map.modes`)

A plugin registers a map mode: a button on the minimap's list of map modes, a colour for each province and a few lines in the province's tooltip. The standard plugin `anvil-map` carries the service; why it is a plugin of its own and why modes borrow numbers of the game's enum is in [ADR 0007](../adr/0007-plugin-map-modes.md). Terms are those of [GLOSSARY.md](../../GLOSSARY.md): plugin mode, borrowed number, number pool.

Until the Anvil release that ships `anvil-map`, this specification is a living document: when the implementation or its test in the game learns something that changes it, a pull request changes it. After that release it is frozen, and a version 2 gets its own file. Statements marked **[to check]** are what the implementation's test in the game must confirm (§9).

## 1. Scope

In v1:

- registering up to 13 modes per process, each under an id `<plugin-id>.<name>`;
- a label, a description, a tab of the list and an optional sprite for its button;
- province colours from a buffer the plugin hands over (a colour or "none" per province ID);
- the mode's part of the province tooltip, asked for in the service's step;
- the events `anvil-map.shown` and `anvil-map.hidden`;
- for tools such as Tongs, the id of the mode that has a number (`mode_id`).

Not in v1 (each can be appended after `size` or come as a service of its own):

- a tooltip for anything other than a province in a plugin mode (the general tooltip needs a decision on where knowledge of the GUI lives);
- map modes from files for modders without C++, and an Etch rule for them;
- stripes (the secondary colour), a legend, coloured text in the tooltip;
- a quick bar that remembers plugin modes between starts;
- the front end (the nation selection's map), the tutorial.

## 2. The plugin

- `plugins/anvil-map`, released in `anvil/plugins/anvil-map/` with Anvil, version equal to Anvil's.
- `plugin.toml`: `phase = "main"`, `affects_gameplay = false`, a dependency on `anvil-game` (`^1`) and an optional one on `mei` for the service `mei.text` version 1. No settings in v1.
- `symbols.toml` declares the symbols of §8 for every build `anvil-game` knows; `symbols.lock` is committed and checked in CI.
- It installs its hooks in `anvil_plugin_load`, as one group. With no plugin mode registered every hook passes the call on unchanged, so the player sees the game's list, colours and tooltips.
- **europagiga.** When `eu4agent.dll` is loaded in the process at `anvil_plugin_load` or at `anvil-game.ready`, `anvil-map` writes one line to its log, installs no hooks (found at load) or lets every hook pass the call on (found at `anvil-game.ready`), and `register_mode` answers `ANVIL_ERROR_UNSUPPORTED`. That loader hooks the same functions and lends the same numbers.

## 3. C interface

[`include/anvil/anvil_map.h`](../../include/anvil/anvil_map.h), plain C11 like `anvil_plugin.h`; the header is the contract and this section only names its parts. Within service version 1, functions and fields are only appended; check `size` (`ANVIL_MAP_HAS`).

- `ANVIL_MAP_MODES_SERVICE` `"anvil-map.modes"`, version 1: `register_mode(info, &out)`, `set_colors(mode, colors, count)`, `mode_id(number)`, `shown()` and `id(mode)`.
- `AnvilMapModeInfo`: `size`, `tab` (`ANVIL_MAP_TAB_DEFAULT` is the last tab, or `ANVIL_MAP_TAB_1` to `_4`), `id` (`<plugin-id>.<name>`, a name of `[a-z0-9_-]{1,32}`), `label` (UTF-8, 1 to 64 bytes), `description` (UTF-8, at most 512 bytes, may be NULL), `sprite` (a GFX name of `[A-Za-z0-9_]`, may be NULL), `describe` and `context`.
- `AnvilMapDescribe(context, mode, province, text, capacity)`: the tooltip's lines in UTF-8, lines separated by `\n`; the return value is the text's length.
- Colours: `ANVIL_MAP_RGB(r, g, b)` is `0x00RRGGBB`; a top byte of `0xFF` (`ANVIL_MAP_NO_COLOR`) leaves the province its political colour; any other top byte is an error. Province 0 never has a colour.
- The events `ANVIL_MAP_EVENT_SHOWN` (`"anvil-map.shown"`) and `ANVIL_MAP_EVENT_HIDDEN`, with `AnvilMapModeChange` (`size`, `mode`, `id`).

The id's prefix cannot be checked by the service (a service does not know its caller); a plugin uses its own id, as it does for its events. Every pointer the service gives stays valid until the process ends (CONTRIBUTING, "Nothing is unloaded in API 2").

## 4. Phases and threads

| What | Where it runs | Reads the world |
| --- | --- | --- |
| `register_mode`, `set_colors`, `mode_id`, `shown`, `id` | any thread | — |
| `anvil-map.shown`, `anvil-map.hidden` | main thread, the step | yes |
| `describe` | main thread, the step | yes |
| the paint hook (`UpdateMapColorProvinceBuffer`) | main thread, any frame, nested ones included | no: copies the last buffer |
| the tooltip hook (`SetupMapmodeTooltip`) | main thread, any frame, nested ones included | no: writes the last text |

**The step** is `anvil-map`'s handler of `anvil-game.frame`: after a frame of the main loop (`outer = 1`), on the main thread, while a session runs. In it, in this order:

1. The current map mode (the session's field, §8) is compared with the last step's. A change from or to a borrowed number publishes `hidden` for the old plugin mode, then `shown` for the new one.
2. A shown plugin mode whose buffer changed since its last paint is marked dirty (`SetMapModeDirty(number)`), so the game repaints it in a coming frame.
3. When the tooltip hook asked for a province of the shown plugin mode since the last step, `describe` is called for the last one it asked for, and its text replaces the kept one.

**Session.** At `anvil-game.session` `ENDED` the buffers and kept texts are dropped, and a shown plugin mode gets `hidden`. Modes and their numbers stay registered for the whole process. `set_colors` outside a session is `WRONG_PHASE`; the province limit comes from `anvil-game.world` (`province_limit`) at the session's first step.

**First frames.** A plugin mode that the player picks is painted at once with the buffer the service has (all "none" when the plugin has given none yet). A plugin that computes in its `shown` handler gets its colours on the map one step later. A tooltip asked for the first time shows the province's name and, a frame later, the plugin's lines.

## 5. The button, the label and the sprite

- The button is added to the tab's list every time the game builds the list. `CMinimapPanel::ShowMapModesList` calls `AddMapModes` for the quick bar and then only for the tab shown (bits 1, 2, 4 and 8 of its argument), so the list is built anew whenever the player opens a tab, in each session, and when a plugin such as Hamweard rebuilds the minimap. The service recognises a tab by the address of the game's array for it and hands the game a copy with its modes appended, in the order they registered. The quick bar's call is passed on unchanged.
- The label and the description come from the hook of `GetMapModeNameOrDesc`; the string is built with the game's own `CString` constructor. The original labels of borrowed numbers are not used.
- The sprite comes from the hook of `GetMapModeSpriteName`. A plugin's own sprite lives in its mod's GFX files (`<mod>/anvil/<plugin-id>/` is the plugin, design §14; the sprite is defined by the mod's `interface/*.gfx`). Anvil ships no graphics; without a sprite the button shows `GFX_mapmode_notselected`, the game's empty button frame, so the label alone tells the modes apart (lab run, GOG 1.37.5.2). A sprite of the game, such as `GFX_mapmode_religion`, works as well.
- **`settings.txt`.** `CSettings::WriteMembers` saves the quick bar under sprite names. Its call of `GetMapModeSpriteName` is redirected: for a borrowed number it answers `GFX_mapmode_notselected`, which the writer skips, so `settings.txt` never stores a plugin mode under the name of the debug mode it borrowed. A plugin mode on the quick bar is therefore not kept across starts in v1.

## 6. Colours

- The plugin's buffer is copied; the paint hook takes the last full copy (a shared pointer swapped under a lock). A copy costs `count` × 4 bytes.
- The paint hook, for a borrowed number of a registered mode: unless the buffer gives a colour to every province up to the province limit, it first lets the game paint the political map mode (1) into the game's buffer; then it writes the plugin's colours as BGRA with alpha 0xFF, indexed by province ID, for at most as many provinces as the game's own function paints (`map_color_count`). For any other number it passes the call on.
- A mode that is not shown costs nothing at paint. The game itself marks some borrowed numbers dirty (57, 58, 59, 66, 68); that only repaints them when they are shown.

## 7. Text

- The API takes UTF-8 only: `label`, `description` and the text of `describe`.
- **To the game.** When `mei.text` version 1 answers `game_text_utf8()` nonzero, the bytes go to the game unchanged; Mei draws them. Otherwise (no Mei, an older Mei, or `utf8_localization` off) the text is written in Windows-1252, and a character it lacks becomes `?`. Text that is not valid UTF-8 is cut at the first bad byte. The service never reads Mei's `settings.toml`.
- **Literal.** Text is shown as written. `$` (the game's keys) needs nothing: a label or a tooltip line with `$PROV$` shows `$PROV$`. `§` (the game's colour codes) has no escape in the game, so the service writes it as `?` (lab run, GOG 1.37.5.2). Colour may come later as a field appended after `size`.
- **The tooltip.** In a plugin mode, the game's `CProvince::GetTooltip` writes the province's name and owner and a separator, `Oka (Russia) - `; `SetupMapmodeTooltip`, the part that depends on the mode, is not called for a borrowed number, and the plugin's kept lines are appended instead, the first on the same line as the name. The plugin's text replaces the borrowed debug mode's lines. A mode with no text for the province (no `describe`, or it returned 0) leaves the separator at the end of the line.
- **No game function builds text.** The service calls no game function that computes values for a tooltip, so it never sends a command into the session (such as the opinion cache update the diplomacy tooltip posts). It works in multiplayer and ironman without suspension. A plugin that needs opinion reads it from `anvil-game.world` when the diplomacy reads arrive there, never by asking the game for a description.

## 8. Symbols

One `symbols.toml`, the builds `anvil-game` knows (Steam 1.37.5, GOG 1.37.5.2 and GOG 1.37.5.0; the last has the same addresses and bytes as 1.37.5.2 here, placed by `anvil port`). Addresses are RVAs from the lab's reading of the PDB and both builds; every symbol has its `bytes`.

| Symbol | What | Steam | GOG | Change |
| --- | --- | --- | --- | --- |
| `map_color` | `UpdateMapColorProvinceBuffer(uchar*, uint, EPdxMapColorUpdateMode)` | `0x12F0B40` | `0x12EACA0` | function hook |
| `map_mode_label` | `GetMapModeNameOrDesc(EPdxMapColorUpdateMode, bool)`, a `CString` through a hidden pointer | `0x822580` | `0x8218E0` | function hook |
| `map_mode_sprite` | `GetMapModeSpriteName(EPdxMapColorUpdateMode)` | `0x1247340` | `0x1241690` | function hook |
| `add_map_modes` | `CMinimapPanel::AddMapModes(const EPdxMapColorUpdateMode*, int, bool)` | `0x124B1F0` | `0x1245540` | function hook |
| `mapmode_tooltip` | `CProvince::SetupMapmodeTooltip(const bool, const CProvince*, CToolTip&)` | `0x9E3210` | `0x9E21F0` | function hook |
| `settings_sprite_call` | the call of `GetMapModeSpriteName` in `CSettings::WriteMembers` | `0xAD4BFE` | `0xAD378E` | call redirect |
| `map_color_count` | inside `map_color`: the provinces it paints, `((map+0x6F0).end - begin) >> 4` times the inverse of 737 | `0x12F0BCF` | `0x12EAD2F` | values from bytes |
| `tab_1` … `tab_4` | the game's arrays of the four tabs, which `ShowMapModesList` adds for the bits 1, 2, 4 and 8 of its argument: 14 modes from political (1), 13 from terrain (0), 16 from trade (4), 11 from diplomacy (5) | `0x1FD4E88`, `0x1FD50C8`, `0x1FD7A50`, `0x1FD79D8` | `0x1FB8E88`, `0x1FB90C8`, `0x1FBBA50`, `0x1FBB9D8` | data in `.data`, compared by address only; tabs 1 to 4 are the buttons with a building, a globe, a sack and a dove (lab run, GOG 1.37.5.2) |
| `cstring_from_text` | `CString::CString(const char*)` | `0x1703550` | `0x16ED320` | called |
| `text_append` | `CString::append(const char*, size_t)` on `CToolTip::_Instant` | `0x932F0` | `0x93320` | called |
| `set_map_mode_dirty` | `CEU3Idler::SetMapModeDirty(EPdxMapColorUpdateMode)`; its bytes give the current mode's offset in the session (`+0x2C`, GOG `+0x24`) | `0x6EA0B0` | `0x6E9A90` | called; value from bytes |
| `session_of_root` | the root object and its session (`root+0x1E00`), as `anvil-game` reads them | as `anvil-game` | as `anvil-game` | value from bytes |
| `province_own_id` | the province's own ID in `CProvince` (`+0x20`), as `anvil-game` reads it | as `anvil-game` | as `anvil-game` | value from bytes |

That is 6 changes, 4 data symbols, 3 calls and 3 symbols that only give values, 16 per build. None of them is a byte patch or a write to `.rdata`; the only writes to the game's image are the trampolines of Anvil's hooks.

The pool, with the console names that reach each number (lab run, both builds):

| Number | PDB name | Console | Lent |
| --- | --- | --- | --- |
| 80, 81, 82, 83 | `EXPANSION`, `CONTENT`, `UNIT`, `MUSIC` | none | 1st–4th |
| 50 | `CONTINENT` | `continent` | 5th |
| 57 | `AI_ARMY_EVAL` | `armyeval`, `aieval` | 6th |
| 58 | `AI_THREAT_EVAL` | `threateval`, `aithreateval` | 7th |
| 59 | `AI_BUILDING_EVAL` | `buildingeval` | 8th |
| 60 | `AI_JOIN_HRE` | `aihre` | 9th |
| 66 | `AI_PATHFIND` | `pathfind`, `path` | 10th |
| 68 | `AI_REGION` | `airegion` | 11th |
| 71 | `TECHNOLOGY_GROUP` | `technologygroup` | 12th |
| 84 | `AI_DEV` | `aidev` | 13th |

A borrowed number belongs to its plugin mode for the whole process, the console included: `mapmode continent` shows the plugin mode that has 50. The fourteenth `register_mode` answers `UNSUPPORTED` and logs one line; the overlay shows no notice.

## 9. Test in the game before the release

Under the lab's game lock, on Steam and GOG, with the example "player's cores" (`examples/`, on `anvil-game.world` v1 `province_cores` only):

1. **Paint from the buffer in a nested frame.** At speed 5 with the plugin mode shown, the paint hook runs in frames with `outer = 0` and paints the last buffer; no `anvil-game.world` call happens outside the step.
2. **Tooltip.** The plugin's lines appear under the province's name, the borrowed debug mode's lines do not; the first hover shows them a frame late at most.
3. **`mapmode` on a borrowed number.** `mapmode continent` (or the console name of whichever number the example borrowed) shows the plugin mode, and `anvil-map.shown` comes.
4. **The empty hook's cost.** Frame times (Bellows' frame pacing series) with `anvil-map` and no plugin mode do not differ from those without `anvil-map`.
5. **Etch's corpus without plugin modes** has no extra buttons on the list.

Also: the session ends and a new one starts (buttons again, buffers dropped); `settings.txt` after quitting with the plugin mode on the quick bar; the console names of the pool; the generic sprite; the tabs' order; how `§` and `$` are shown. The results and what was not checked go into the pull request and into design §4.
