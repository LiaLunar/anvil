# Anvil

Anvil loads native plugins into Europa Universalis IV without modifying the game. This glossary fixes the words used for its parts and for the plugins built on it.

## Language

### Anvil and plugins

**Core**:
The part of Anvil that must work before any plugin loads or needs a view across all plugins at once, such as load order, the registry of changes and plugin approval. Nothing else belongs to it.
_Avoid_: loader (for the whole of Anvil), framework

**Library plugin**:
A plugin that offers services to other plugins.
_Avoid_: shared plugin, dependency plugin, framework plugin

**Standard plugin**:
A library plugin maintained by the Anvil project and released together with Anvil. It loads, fails and can be disabled or replaced like any other plugin.
_Avoid_: built-in plugin, core plugin, official module

**Service**:
A named set of functions with a major version that one plugin publishes under its own id and other plugins look up.
_Avoid_: interface, API (for one plugin's offer), export

**Service version**:
The major version under which a service is published; within it the service only grows, and a breaking change is a new service version that may be offered next to the old one.
_Avoid_: service API version, interface version

**Gameplay plugin**:
A plugin that declares it can change the simulation's state or results. Anvil takes its changes out of ironman and multiplayer games unless both the plugin's author and the player allow it there.
_Avoid_: cheat plugin, unsafe plugin

**Plugin translation**:
A file of a plugin package, `translations/<code>.toml`, with one language's words for the plugin's texts the player reads in the overlay and the Manager: its description and its settings' labels, descriptions, groups and option labels, each found by its English words.
_Avoid_: catalog (Anvil's own texts have catalogs; the Plugin catalog is the list of plugins), locale

**Module**:
A part of one plugin that `plugin.toml` declares with its own `affects_gameplay` and opt-ins, and that the player turns on or off by `<plugin>.<module>`; the plugin acts for it through the module's handle, so Anvil suspends it on its own (ADR 0011).
_Avoid_: feature (the player's word for what a module does), sub-plugin, component

**Start**:
One run of the game's process with Anvil in it, from the game's entry point to its exit.
_Avoid_: launch, run, session (a game session is another thing)

**Handle**:
What a plugin, or one of its modules that is on, acts through: every request it makes of Anvil carries one, and Anvil names, counts and suspends its changes by it.
_Avoid_: self (the parameter's name), owner (the handle's number in the registry of changes), context

**Plugin set**:
The plugins Anvil found at one start, in load order, each with its handles and its outcome.
_Avoid_: plugin list, plugin registry, loaded plugins (some are not)

**Outcome**:
What became of a plugin at one start: loaded, failed, skipped or disabled, with the reason for a failed or a skipped one. A module has no outcome of its own: it is on or off, and shares its plugin's.
_Avoid_: state, status, result

**Plugin catalog**:
The list of the team's plugins that the Manager offers to install, each pinned to one release zip by its size and SHA-256 (`catalog/plugins.json`).
_Avoid_: store, marketplace, plugin registry

**Pin**:
The release a catalog entry names, with the zip's URL, size and SHA-256; the Manager installs a downloaded zip only when it matches the pin.
_Avoid_: lock (taken by `symbols.lock`)

**Legacy text patch**:
EU4DLL or EU4 Unicode Patch: a `version.dll` loader with its files in the game folder, which Anvil replaces. The Manager recognizes one by its files, moves them to `anvil-legacy-backup/` and can put them back when Anvil is uninstalled.
_Avoid_: old loader, foreign DLL (for a recognized one), EU4DLL (for both)

**Game session**:
One loaded save or new game while it runs, from the moment it is playable until the player leaves it or loads another; it is ironman, multiplayer, both or neither.
_Avoid_: game (for this), campaign, match

**Suspension**:
The state of a plugin whose changes are all taken out of the game for a while, keeping their place, and come back unchanged when it is resumed.
_Avoid_: disabling (that lasts until the next start), unloading, pausing

### Changing the game

**Game build**:
One `eu4.exe` that Anvil knows by its PE header and SHA-256, named by a build id with the store and the version, such as `eu4-1.37.5.2-gog-win64`. Symbols are declared for game builds.
_Avoid_: version (two GOG builds share 1.37.5), edition, platform

**Symbol**:
A game address that a plugin declares together with what must be found there, such as the expected bytes, the kind of memory or the value a pointer holds. One declaration may give it a different address and bytes in each game build.
_Avoid_: offset, signature, address (for a declared one)

**Change**:
One modification of the game's code or data by a plugin: a function hook, a mid hook, a block replacement, a call redirect, a byte patch, a pointer swap or an import replacement.
_Avoid_: patch (as the general word; a byte patch is one kind of change), modification, tweak

**Hook**:
A change that runs plugin code when the game reaches a symbol: a function hook at the start of a function, or a mid hook at any instruction.
_Avoid_: detour (for the whole change), interception

**Block replacement**:
A change that runs plugin code instead of a declared block of game instructions, which never run, and then resumes at the block's end or at another declared symbol.
_Avoid_: mid hook (for this; a mid hook lets the original code run), code cave, skip hook

**Chain**:
The ordered changes of several plugins at one address, each passing control or the previous value to the next.
_Avoid_: stack, hook list

### Map modes

**Plugin mode**:
A map mode that a plugin registers with the service `anvil-map.modes`, known by its id `<plugin-id>.<name>`; the plugin gives its colours and tooltip lines, and the game shows its button on the minimap's list.
_Avoid_: custom map mode, user map mode, mapmode (as one word in prose)

**Borrowed number**:
A value of the game's map mode enum that `anvil-map` has lent to one plugin mode for the whole process; the game's own mode under that number is not shown while it runs, not even from the console.
_Avoid_: slot, map mode id (the id is the text), enum value (for a lent one)

**Number pool**:
The 13 values of the game's map mode enum that `anvil-map` may lend: 80–83 first, then the debug modes 50, 57–60, 66, 68, 71 and 84.
_Avoid_: slots, free modes

### Script effects

**Script effect**:
A word of the game's script language that does something when the game runs it, such as `add_treasury = 50` in an event option. A plugin's script effect is one that a gameplay plugin adds through `anvil-script` and a mod writes like the game's own.
_Avoid_: command (the game's commands are what its clients exchange), script hook, custom effect (for the service)

**Effect key**:
The word on the left of a script effect, `chisel_set_terrain` in `chisel_set_terrain = mountain`. A plugin's key starts with its id.
_Avoid_: effect name, keyword (for the game's own lexer words)

### Game commands

**Game command**:
An object the session queues, the host of a multiplayer game sends out and every machine runs at the same place in the queue. The unit panel's buttons and `anvil-game.commands` make them.
_Avoid_: order, action

**Post**:
Handing a game command to the session's queue. A posted command has not run yet: it runs at the session's next turn of the queue, and in multiplayer it is only on its way to the host.
_Avoid_: send, execute

**Intent**:
The state a plugin asks for, such as "drilling", which `anvil-game.commands` turns into a command only for the units whose state differs.
_Avoid_: toggle

**Pending event**:
An event the game waits on a choice for, known by its pending id; one pending event has at most one event window.
_Avoid_: open event, pop-up

### The game's day

**Day end**:
The moment after the game has finished a day (with its month and year) on the main thread, before it compares checksums, when no worker thread runs. A gameplay plugin is called there through `anvil-script.day_end`.
_Avoid_: daily tick hook, after tick, end of tick

### The script profiler

**Script block**:
One `AND` block of a trigger or one block of effects, as the game runs it: the `trigger` of an event, a decision's `potential`, a `limit`, an option's effects. `anvil-script-profiler` sees every one the game runs.
_Avoid_: script, clause, scope (a scope is what a block runs on)

**Root block**:
The outermost script block of its kind running on a thread: the only one the profiler times, with the blocks it runs inside it counted toward it. Triggers and effects have separate roots, so a `limit` inside an effect is a root trigger.
_Avoid_: top-level block, parent

**Owner**:
The event, event option, decision, on_action, casus belli, triggered modifier, mission, church aspect or scripted trigger a root block belongs to, with the block's role there (`trigger`, `potential`, `effect`...). The profiler's rows are owners with their roles; a block whose owner it cannot tell is "no owner found".
_Avoid_: source, parent object, definition

**Profiling span**:
The game time between the start and the end of one measurement of `anvil-script-profiler`: its hooks exist only then. A load, leaving to the menu or quitting interrupts it.
_Avoid_: session (a game session is another thing), recording, run

### When the game crashes

**Crash report**:
What Anvil writes when the game crashes: where it happened and which plugins were involved, on one of four levels (in a plugin's code, in a plugin's change, a plugin on the stack, no plugin involved). It never says which plugin caused the crash.
_Avoid_: crash log, blame, culprit

**Safe mode**:
A start that loads no plugins except the standard ones needed to explain it, because the two starts before it ended before the game was ready.
_Avoid_: recovery mode, failsafe, disabled mode

### Start-up benchmark

**Start-up benchmark**:
A measurement on the player's computer of how long the game takes to start without one plugin and with it.
_Avoid_: benchmark (alone; that names Anvil's microbenchmarks), performance test

**Benchmark series**:
The starts run for one start-up benchmark, alternating between the two arms, which ends complete or stopped and gives one benchmark report.
_Avoid_: run, session, test

**Benchmark start**:
One start of the game in a benchmark series, which quits by itself once the game is ready and the participants are done.
_Avoid_: run, iteration, launch

**Arm**:
One side of a start-up benchmark: without the compared plugin, or with it.
_Avoid_: variant, mode, A/B (for the side itself)

**Compared plugin**:
The plugin a benchmark series leaves out in one arm and loads in the other.
_Avoid_: tested plugin, target

**Start time**:
The time from the creation of the game's process to the moment the game is ready for plugins, the value a benchmark start measures.
_Avoid_: load time, time to menu

**Open cost**:
The time per file that the first opens of the game's files take in one start; it tells a warm start from a cold one.
_Avoid_: disk speed, I/O time

**Warm start**:
A benchmark start whose open cost shows the files were already at hand, as in most starts after the first since boot.
_Avoid_: hot start, cached start

**Cold start**:
A benchmark start whose open cost shows the files had to be reached anew, such as the first start after a boot or after an antivirus definitions update.
_Avoid_: cold boot, first run

**Unclear start**:
A benchmark start whose open cost is neither clearly warm nor clearly cold; it counts in neither group.
_Avoid_: lukewarm start, borderline start

**Discarded start**:
A benchmark start kept in the report with its reason but counted for neither arm.
_Avoid_: failed start, invalid run

**Learning start**:
A benchmark start that a participant declares not representative for itself, such as a plugin's first start on a new mod set; it is a discarded start.
_Avoid_: warm-up start, first run

**Benchmark participant**:
A plugin that joins a start-up benchmark when it loads and promises to say when its work after the game is ready is done.
_Avoid_: benchmark plugin

**Benchmark field**:
A number, option or flag that a plugin adds to each benchmark start's row; never text.
_Avoid_: metric, custom field, stat

**Start record**:
What Anvil writes about one benchmark start for the Manager to read after the game has quit.
_Avoid_: timing file, start log

**Benchmark report**:
The one document a benchmark series gives, which the player sees, keeps and may send or copy, byte for byte the same.
_Avoid_: result file, telemetry

**Benchmark receiver**:
The endpoint of the Anvil project that accepts sent benchmark reports.
_Avoid_: telemetry server, backend, collector

**Day of receipt**:
The UTC date the benchmark receiver stores with a report, its only addition to it.
_Avoid_: timestamp, received at
