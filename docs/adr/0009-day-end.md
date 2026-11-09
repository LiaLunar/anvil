# Day end: a callback after the game's day, in `anvil-script`

Status: **accepted** (2026-11-09). Nothing here is built yet; the service is specified in [docs/spec/day-end-v1.md](../spec/day-end-v1.md).

Chisel, the plugin that changes a province's terrain during a campaign, has to recompute the game's movement costs after a change (lab decision eu4-engine-lab#132). The game's own `CalculateNeighborPaths` does it, but AI threads read those costs during the day, so the call is safe only at a point of the day where no worker runs, and in multiplayer it must happen at the same point of the same day on every client. No service of Anvil gives such a point: `anvil-game.day` comes after the frame in which the date moved, which is a frame later than the day and, on a client that runs two queued days in one frame, a day late. This ADR decides where a plugin gets that point, which plugins may use it and what it carries. Two choices are hard to undo once plugins use the service: the plugin that carries it and the moment it runs.

How to read the evidence. **[code]**: read in this repository or a team plugin. **[pdb]**: static, from the GOG build's `eu4.pdb`, the bytes of the three builds and their disassembly (lab `docs/research/chisel-path-recompute.md`, eu4-engine-lab#131). **[live]**: seen in the game. **[hypothesis]**: not checked. No game was started for this ADR; the game test of the implementation is in the spec's section 8.

## Decision

1. **A second service of `anvil-script`, `anvil-script.day_end` version 1.** It calls the plugins' callbacks at the **Day end**: after `CGameState::HandleDailyTick` returns, on the main thread, after the day's monthly and yearly updates and every `ParallelFor` of the day, before `COOSHandler::MakeSyncCheck`. A function hook on `HandleDailyTick` (Steam `0x758430`, GOG `0x7575F0`) calls the game's function and then the callbacks. `anvil-script` gets 2 symbols per build: the function and the site that gives the date's offset.
2. **Only plugins with `affects_gameplay = true` subscribe**, with the gate of [ADR 0008](0008-plugin-script-effects.md) (point 2): the service asks the core through `anvil.control` and refuses any other plugin with `ANVIL_ERROR_WRONG_KIND`. While the core suspends the owner (ironman, multiplayer, design §13), its callback is not called.
3. **The order is fixed: the order of subscription**, which is the load order, since a plugin subscribes in its `anvil_plugin_load`. A subscription after the first Day end of the process gets `ANVIL_ERROR_WRONG_PHASE`, so the set and its order are the same for every day of the process and on every client with the same plugins.
4. **The callback gets the date** the day ended on (the date the top bar shows next) and two flags: the day started a month, the day started a year. Nothing else: no world reads (`anvil-game.world` refuses this point, design §13.1), no handle to the game state.
5. **One callback per day the game runs.** No callback while no day passes: in the menu, on a pause, while a save loads, and in a session in which no day has passed yet.

## Where the day ends

[pdb] `HandleDailyTick` has one caller, `CDailyTickCommand::Execute` (Steam `0xB79520`, GOG `0xB77CC0`), the command the game puts in its queue once a day and every multiplayer client runs in the same order. That command calls `HandleDailyTick` (`+0xBA`) and after its return only logs and calls `MakeSyncCheck` (`+0x12B`). `HandleDailyTick` adds 24 hours to the date at `CGameState+0x1DD0` near its start (`+0xC9`), then runs the day: the daily, monthly and yearly updates of countries, provinces and the AI, every one of the 14 `ParallelFor` of the tick (each waits for its tasks before it returns), `CUnitManager::Update` and the last frame drawn inside the day. Its one `ret` is at `+0xAE8`, after the flag `_bIsInHandleDailyUpdate` is cleared (`+0xABE`). The offsets inside the function are the same in Steam and both GOG builds.

So a hook that calls the game's function and then the callbacks runs on the main thread, with no worker of the day still running, before the checksum of the day. Movement costs are not in the checksums [pdb, lab note on #76, §7], so a plugin that changes them there changes nothing the check compares; one that changes what the check does compare must do it on every client alike, which its `allow_multiplayer` already promises (ADR 0004, stage 3).

A mid hook at `+0xABE` was the other choice (#131): it runs with the day's flag still set. Rejected, because nothing needs the flag and a function hook does not depend on an offset inside the function, which a port has to find again.

## Why in `anvil-script`

- **Not a private detour of each plugin.** Chisel could hook `HandleDailyTick` itself. A second plugin that needs the same point would make a second detour of the same function, the chain order would decide who runs first, and each would copy the rules of this ADR. One service keeps them once, as ADR 0008 argued for `ReadMember`.
- **Not in `anvil-game`.** `anvil-game` reads and compares after the frame and puts nothing in the game's tick: "compare, do not hook" ([ADR 0004](0004-plugins-without-addresses.md)). It is also what suspension and safe mode stand on (design §10, §13); code in the tick there would put a crash of the day in the plugin safe mode keeps.
- **Not `anvil-game.day`.** It is published after the outer frame in which the date moved, at most once per frame with a count of days. A frame can hold several days (a multiplayer client catching up, high speed), so a plugin would do its work once for several days, and later than the host did: a client that runs two days in one frame recomputes after the second day, the host after each. That breaks the assumption of lab decision #32 that multiplayer needs no rebuild later.
- **`anvil-script`, because it is the plugin for code that runs inside the game's script and commands for gameplay plugins.** It already has the gate (`anvil.control`'s `plugin_of`) and the suspension rule, it is not kept in safe mode, and Chisel depends on it for its effects anyway. A plugin that uses only the Day end depends on `anvil-script` like one that uses only effects.

## Why gameplay plugins only

The Day end runs inside a command that every client runs and right before the day's checksum. Code there changes the campaign or can, and a plugin that only reads has `anvil-game.day`, which is enough for it and runs outside the tick. So the gate of ADR 0008 holds as it is: a plugin with `affects_gameplay = false` gets `ANVIL_ERROR_WRONG_KIND`, and a suspended owner is skipped at each Day end, asked through `anvil.control` each time. A client with the plugin suspended skips the callback, as one without the plugin does.

## Why a fixed order and a closed list

Two plugins at the Day end may depend on each other's work (one changes terrain, the other reads costs). Load order is already the order the player and the dependencies decide (design §10), it is the same on every client with the same plugins, and it is what a chain of hooks would give anyway. Closing the list at the first Day end keeps it so: a plugin cannot join in the middle of a campaign on one client and not on another. A plugin subscribes in its load, and nothing is unloaded in API 2.

## Considered options

- *A private detour of `HandleDailyTick` in Chisel.* Rejected (#132): a second consumer would add a second detour of the same function.
- *The service in `anvil-game`.* Rejected: it breaks "compare, do not hook" of ADR 0004 and puts code of the tick in the plugin safe mode keeps.
- *`anvil-game.day`.* Rejected: it depends on frames, and in multiplayer a client with two days in one frame would work later than the host.
- *A mid hook at `HandleDailyTick+0xABE`.* Rejected: nothing needs the day's flag, and the offset is one more thing a port has to find.
- *A day start too (before the day).* Not in v1: nothing needs it, and a callback before the day's AI would make a plugin's state part of what the day computes.
- *Subscribers of any kind.* Rejected: a plugin that only reads has `anvil-game.day` outside the tick.
- *An order of the plugin's choosing (priorities).* Rejected: load order is already decided by dependencies, and priorities would be one more contract to keep.

## Consequences

- `anvil-script` gets the service, its header grows (`include/anvil/anvil_script.h`), and `anvil-sdk` gets its Rust wrapper (CONTRIBUTING, Rust parity), with `anvil_script.h` added to `sdk/rust/bindings.h`.
- `anvil-script` declares 13 symbols per build and adds GOG 1.37.5.0 as a third build: the code of 1.37.5.0 is laid out as 1.37.5.2's (design §6), so `anvil port` gives the same addresses.
- With no subscriber the hook calls the game's function and returns: a few instructions once a day. Its cost is measured on a year at speed 5 before the release.
- Chisel's Path recompute subscribes and does nothing on a day with an empty queue.
- A fault in a callback crashes the game in the middle of a command: the crash report names the plugin's code (design §12), as for an effect.

## Limits

- No game was started for this ADR. The spec's game test (section 8) checks one callback per day, the main thread, the date and the flags on days that start a month and a year, suspension, and the empty hook's cost.
- Multiplayer: the order of commands is the same on every client [pdb for the code, hypothesis for the practice]; not tried with two computers.
