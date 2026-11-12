# The inspector

A window of Anvil's overlay for modders: the flags, variables and timed modifiers of the province
whose view is open, of its owner and of the player's country, and the countries' rulers, heirs and
consorts, as the running game holds them. It is the standard plugin `anvil-inspector` (design §4),
off by default, and it only reads: everything comes from `anvil-game.world`
([`anvil_game.h`](../include/anvil/anvil_game.h)), and it calls no game code and changes nothing, so
`affects_gameplay = false`.

Its shape was decided in [eu4-engine-lab#65](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/65)
and [#155](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/155); the window follows the script
errors panel of `anvil-log-throttle` ([script-errors-panel.md](script-errors-panel.md)).

## Turning it on

- The setting **Inspector window** (`window`, default off, applies at once) of anvil-inspector, in the
  overlay's settings tab or the Manager's.
- On: the window opens, at every start too. Once closed, its switch is in the **Status** tab of the Anvil
  panel, like every plugin's window.
- Off: the window hides. With the setting off from the start the plugin starts no thread, queues no work
  and subscribes to no event: it costs nothing in a frame or a game day.
- It needs `anvil-game` and `anvil-overlay` (required dependencies). On OpenGL nothing is drawn. With an
  `anvil-game` older than 1.21 it shows the player's country without the timed modifiers, and says that
  the province view needs a newer `anvil-game`.

## What it shows

From the top:

1. **Date**, a **Refresh** button and a line that says when the window reads the game.
2. **Province: <name> (<id>)**: the province the game's province view shows, while that window is open
   (`province_view`). Without one, a line asks the player to open a province's view.
3. **Owner: <tag>**: the province's owner, or **Owner and your country** when the player owns it.
4. **Your country: <tag>**: the player's country (the observed one in observe mode), when it is not the
   owner.

Each of them is a heading that folds, with these lists, each a heading of its own:

- **Flags (n)**: each flag that is set and the date it was set. A province's list holds its permanent
  flags too, as `has_province_flag` sees them.
- **Variables (n)**: each variable and its value, written as scripts write it (`2.5`, `-1`).
- **Timed modifiers (n)**: each named modifier an effect gave for a time (`add_country_modifier`,
  `add_province_modifier`, `add_ruler_modifier`), by its key, with **hidden** for `hidden = yes` and
  **ruler modifier** for one that goes with the ruler, and its end date, or **no end date**. These are
  what `has_country_modifier` and `has_province_modifier` look for. Triggered and static modifiers are
  not among them. The list is the game's own, so it also shows the few the game sets up by itself and
  leaves out of the save (in the Russia 1719 save: 15 `world_port` of Russia and the `world_trade_center`
  of Moskva).
- For a country, **Ruler, heir and consort**: name and dynasty with the skills (adm/dip/mil), **none**
  when the country has no such person, and the person's flags below.

A list shows at most 60 items and then **N more**. Everything starts open; a click on a heading folds it
until the game closes. Keys, flag names and variable names are the game's, as the scripts write them.
A list that this game build cannot read says so instead of its items.

## When it reads

The game's world may be read only on its main thread, after a frame, so the window's thread queues one
piece of main-thread work (`run_on_main_thread`) a second while the window is shown. That work asks
for the session, the date, the province view and the player (a few calls), and reads everything only
when one of them changed, when the window was opened, or after **Refresh**. So the window follows a new
date at most once a second, another province's view, a formed nation, observe mode and a loaded save
(a new session is read from scratch). A change that comes without one of those, such as the console's
`set_country_flag` while the game is paused, shows after **Refresh**.

A full read of three objects takes a few hundred calls of the service, a fraction of a millisecond on the
main thread; nothing is kept between reads but the copied text, so a save load leaves no stale pointer.
A hidden window costs a wake-up of the plugin's thread a second and nothing on the main thread.

## What `anvil-game.world` gained for it (1.21.0)

- `country_timed_modifiers`, `province_timed_modifiers` and their `_keys` calls: the game's lists of
  `CTimedModifier` (`CCountry` `+0x1F0`, a linked list; `CProvince` `+0x2F0`, a vector), each with its
  `CStaticModifier`'s key (`+0x158`, what the save writes as `modifier`), the start and end dates (`+0x40`,
  `+0x44`, the unset date for none; a modifier given for good ends at the game's date for never, a global
  that `CCountry::AddTemporaryModifier` reads, which the save writes as `date=-1.1.1`), the ruler flag
  (`+0x48`) and `hidden` (`+0x4B`). The offsets come
  from `CCountry::HasModifier`, `CProvince::HasModifier` and `CTimedModifier::WriteMembers` in the GOG
  1.37.5.2 PDB, each site unique in all three builds.
- `province_view`: the province the province view shows while its window is shown, through the session's
  selection object, its view, the view's province and window, and the window's visible byte: the same
  path Tongs reads.

Design §13 has the details; the Rust wrappers are `World::country_timed_modifiers`,
`province_timed_modifiers` and `province_view`.

## Not in v1, on purpose

A filter field, the choice of any tag or province, triggered modifiers, the values of the modifiers
(the 1.15 calls give a modifier's sum by key; the timed list gives only keys and dates), event targets,
saved scopes and a write of any kind. They wait until modders ask (eu4-engine-lab#65).

## Tests

- `anvil_inspector_tests`: the read through a stand-in of `anvil-game.world` (the three objects, the
  owner who is the player, no view, no session, an `anvil-game` 1.20 table and a NULL world), what a
  read is due on, the rows (folding, values, dates, the panel's limits, long lists) and that every text
  has every language with the same placeholders.
- `anvil_game_plugin_tests`: the new calls of `anvil-game.world` on a fake world laid out as Steam 1.37.5
  (two timed modifiers of a country, one of a province, dates and flags, keys cut at a whole key, a broken
  list, the view shown, closed, pointing at no province, and without a selection object).
- The Rust wrappers against a stand-in service (`cargo test -p anvil-sdk`).
- In the game: see the pull request that added the plugin.

## Translations

The window's words (`plugins/anvil-inspector/src/texts.cpp`) and the plugin's description and setting
(`plugins/anvil-inspector/translations/`) were drafted by the agent in every language; each waits for a
native speaker's check (for Polish, the maintainer's).
