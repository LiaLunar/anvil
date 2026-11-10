# Day end v1 specification

`anvil-script.day_end` version 1 calls a gameplay plugin back at the **Day end**: once a day, after the game has finished the day (with its monthly and yearly updates) on the main thread, when no worker thread of the day runs, before the game compares checksums. Chisel uses it to have the game recompute movement costs on a day with a terrain change. The decisions and their reasons are in [ADR 0009](../adr/0009-day-end.md); the static evidence is the lab's `docs/research/chisel-path-recompute.md` (eu4-engine-lab#131, #132). Terms are those of [GLOSSARY.md](../../GLOSSARY.md).

Until the Anvil release that ships the service, this specification is a living document: when implementation learns something that changes it, a pull request changes it. After that release it is frozen, and version 2 gets its own file.

## 1. The service

| | |
| --- | --- |
| Plugin | `anvil-script` ([ADR 0008](../adr/0008-plugin-script-effects.md), [script-effects-v1.md](script-effects-v1.md) section 1), unchanged manifest |
| Service | `anvil-script.day_end` version 1, registered in its load next to `anvil-script.effects` |
| Symbols | 2 more per build (section 7), 13 in all; GOG 1.37.5.0 added as a third build of `anvil-script` |
| Header | `include/anvil/anvil_script.h` (C), appended |
| Rust | `anvil::script` in `anvil-sdk` (section 2), `anvil_script.h` in `sdk/rust/bindings.h` |
| Safe mode | Not kept (design §10): a safe start calls no Day end |

A consumer declares the dependency; its phase is its own (an effect needs `early`, the Day end does not):

```toml
affects_gameplay = true

[[dependencies]]
id = "anvil-script"
version = "^1.18"   # the first Anvil with the service (working number)
```

## 2. The interface

```c
#define ANVIL_SCRIPT_DAY_END_SERVICE "anvil-script.day_end"
#define ANVIL_SCRIPT_DAY_END_SERVICE_VERSION 1

/* AnvilScriptDay.flags */
#define ANVIL_SCRIPT_DAY_NEW_MONTH 0x1u /* the day is the 1st: the game ran its monthly update in it */
#define ANVIL_SCRIPT_DAY_NEW_YEAR  0x2u /* the day is 1 January: also its yearly update */

/* The day that just ended. Valid only during the call. */
typedef struct AnvilScriptDay {
    uint32_t size;  /* set by the service */
    uint32_t flags; /* ANVIL_SCRIPT_DAY_* */
    int32_t year;   /* the date the game moved to at the start of the day: the top bar's next date */
    int32_t month;  /* 1-12 */
    int32_t day;    /* 1-31 */
} AnvilScriptDay;

/* Runs on the main thread at the Day end (section 4). Must not throw. */
typedef void (*AnvilScriptDayEnd)(void *context, const AnvilScriptDay *day);

typedef struct AnvilScriptDayEndService {
    uint32_t size;
    /* From a plugin with affects_gameplay = true, before the first Day end of
     * the process: in practice in its anvil_plugin_load. The callback and the
     * context must live for the process. */
    AnvilResult (*subscribe)(AnvilPlugin *self, AnvilScriptDayEnd callback, void *context);
} AnvilScriptDayEndService;
```

The date is that of `anvil-game`'s `AnvilGameDate` (hours since 1 January −5000 in 365-day years, no leap days): the game adds 24 hours at the start of `HandleDailyTick`, so the Day end carries the date the day was computed for, the date `anvil-game.day` reports after the frame.

C++: `host.service<AnvilScriptDayEndService>(ANVIL_SCRIPT_DAY_END_SERVICE, ANVIL_SCRIPT_DAY_END_SERVICE_VERSION)` and a plain `noexcept` function. As for effects, the callback is called by the game through the service, not through a change, so an exception that leaves it ends the process and the crash report names the plugin's code (design §12).

Rust, in `anvil-sdk` (`anvil::script`):

```rust
pub struct Day { pub year: i32, pub month: i32, pub day: i32, pub new_month: bool, pub new_year: bool }

pub struct DayEnd { /* the service table */ }
impl DayEnd {
    /// The service, when anvil-script publishes it.
    pub fn get(host: &Host) -> Option<Self>;
    /// Subscribes `handler` to the Day end for this plugin; call it in the plugin's load.
    pub fn subscribe<F: Fn(&Day) + Send + Sync + 'static>(&self, host: &Host, handler: F) -> Result<()>;
}
```

The handler is kept for the process, as `anvil::map`'s tooltips are; a panic in it is caught at the boundary (as in every `anvil-sdk` callback) and logged, and the rest of that Day end goes on.

Within API 2 (ADR 0001): the structs grow only by appending behind `size`; a changed meaning is version 2 next to version 1.

## 3. Subscription

`subscribe` checks, in this order, and answers the first failure:

| Check | Result |
| --- | --- |
| `self` is a plugin the core knows and `callback` is not NULL | `ANVIL_ERROR_INVALID_ARGUMENT` |
| The caller's manifest has `affects_gameplay = true` (from `anvil.control`) | `ANVIL_ERROR_WRONG_KIND` |
| No Day end has run in this process yet | `ANVIL_ERROR_WRONG_PHASE` |
| Room: at most 64 subscriptions in all | `ANVIL_ERROR_OUT_OF_MEMORY` |

Each refusal is logged with the caller. A plugin may subscribe more than once (each callback is called, in order); a subscription is never removed: nothing is unloaded in API 2.

## 4. The Day end

**Where.** A function hook on `CGameState::HandleDailyTick(this)`. Its one caller is `CDailyTickCommand::Execute`, the day's command, which after the return only logs and calls `COOSHandler::MakeSyncCheck` [pdb]. The detour:

1. calls the game's function;
2. returns at once when nobody subscribed;
3. reads the date at `this + date` (section 7) and decodes it;
4. calls each subscriber in the order of subscription, skipping one whose owner is suspended (design §13), asked through `anvil.control` at each Day end.

The list is closed when the detour is first entered, before the game's day runs, so a subscription from inside that first day is refused too.

**Thread.** The main thread, inside the day's command: every `ParallelFor` of the day has waited for its tasks, the monthly and yearly updates are done, and the last frame drawn inside the day is over [pdb]. The service notes the thread of the first Day end and logs once if a later one differs; it calls the callbacks either way.

**What a callback may do.** It runs inside a command, like an effect: `anvil-game.world` answers `ANVIL_ERROR_WRONG_PHASE` there (design §13.1). It may call game functions it declares itself (Chisel calls `CalculateNeighborPaths` and `CalcNavalDistances`) and change game state that every client changes alike. In multiplayer every client runs the same Day ends in the same order; a callback must be deterministic, draw nothing from the game's RNG, and leave what the checksum compares the same on every client (ADR 0004, stage 3).

**When not.** No Day end in the menu, while the game is paused, while a save loads, and in a session until its first day has passed. A loaded save, a new game and a hot-join start with no Day end; a plugin that needs work before the first day uses `anvil-game.session`.

## 5. Errors and logging

- Every refusal of section 3 writes one line to `anvil.log` with the caller.
- At `anvil-game.ready`, when `anvil-game` is loaded, the existing line of `anvil-script` names the subscribers of the Day end (`day end: <id>, <id>` or `day end: no subscriber`).
- A fault in a callback is the plugin's crash (section 2); the service does not wrap the call in a guard.

## 6. Cost

With no subscriber the detour costs a call of the game's function through the hook and one test, once a day. With subscribers it adds, per subscriber, one call of `anvil.control`'s `plugin_of`, and the date's decode once. The release measures a year at speed 5 with `anvil-script` with and without the hook (design §17).

## 7. Symbols

RVAs are Steam 1.37.5 / GOG 1.37.5.2 and 1.37.5.0 (`eu4-1.37.5-steam-win64`, `eu4-1.37.5.2-gog-win64`, `eu4-1.37.5.0-gog-win64`); the bytes are the same on all three builds, and both GOG builds have the same addresses [pdb].

| Symbol | Steam | GOG | What | Values |
| --- | --- | --- | --- | --- |
| `daily_tick` | `0x758430` | `0x7575F0` | `CGameState::HandleDailyTick()`, the function hook: `48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8 FE FF FF 48 81 EC F8 01 00 00` | |
| `daily_tick_date_site` | `0x7584C1` | `0x757681` | `HandleDailyTick+0x91` to `+0xCC`: `lea r15,[rsi+1DD0h]` … `add dword [r15],18h`, the day's 24 hours added to the date: `4C 8D BE D0 1D 00 00 49 8B CF E8 ?? ?? ?? ?? 4C 8D 25 ?? ?? ?? ?? 83 E8 1F 78 0F 49 8B CC 90 41 FF C5 48 8D 49 04 2B 01 79 F5 49 8B CF E8 ?? ?? ?? ?? 89 85 40 01 00 00 41 83 07 18` | `date` (`CGameState+0x1DD0`, int32 hours) |

The 11 symbols of the effects (script-effects-v1.md section 7) get the GOG 1.37.5.0 build with the addresses of 1.37.5.2 through `anvil port`, checked with `anvil check --exe` on that build.

An absent symbol (§5.2) on a future build: without either of these two the Day end service is not registered and `get_service` gives NULL, while the effects go on; without an effect symbol the effects service is not registered and the Day end goes on.

## 8. Tests

- Unit tests (`tests/anvil_script_plugin_tests.cpp`) with a stand-in game: a fake `HandleDailyTick` that advances a date, for the order of checks of section 3, the list closed at the first Day end, the order of callbacks, a suspended owner skipped, the date's decode and the flags on 1 January, 1 February and an ordinary day, and no callback with no subscriber.
- The game test (Steam and GOG 1.37.5.2, under the lab's lock) with a lab plugin that subscribes and logs each Day end: one callback per day over a run that crosses a month and a year, on the main thread (`anvil-game`'s `main_thread_id`), with the date `anvil-game.day` then reports and the flags on the 1st and on 1 January; nothing before the first day of a session and nothing in the menu or on a pause; a plugin with `affects_gameplay = false` refused. The empty hook's cost: a year at speed 5 with `anvil-script` of this release and with that of the release before, two runs each.

## 9. Not in version 1

A day start (before the day); a callback in the middle of the day; subscribers with `affects_gameplay = false`; an order other than subscription; unsubscribing; a Day end in a safe start.
