# Plugin script effects live in a separate standard plugin, `anvil-script`, for gameplay plugins only

Status: **accepted** (2026-11-09). Nothing here is built yet; the service is specified in [docs/spec/script-effects-v1.md](../spec/script-effects-v1.md).

A mod can call what a plugin does only through the game's own words: a modder writes `add_treasury = 50` in an event, a decision or a mission, and the game runs it. Chisel, a plugin that changes a province's terrain during a campaign (lab decision in eu4-engine-lab#32), needs the opposite direction: a word of its own, `chisel_set_terrain = mountain`, that a mod writes where it writes any other effect. EU4 has no way to add one. This ADR decides how a plugin gets a script effect of its own, where the service lives, which plugins may use it, when they must register, and what v1 leaves out: triggers, threads other than the game's own, and multiplayer beyond what every client does by itself. Two choices are hard to undo once mods use the service: the plugin that carries it and the rule for the keys.

How to read the evidence. **[code]**: read in this repository, a team plugin or the game's files. **[pdb]**: static, from the GOG build's `eu4.pdb`, the bytes of both builds and their disassembly (lab `docs/research/script-effect-statics.md`). **[live]**: seen in the game on 9 November 2026: the lab's prototype (Steam 1.37.5, the lab plugin `lab-script-effect` and a test mod, eu4-engine-lab#77) and the service from the implementation's branch (Steam and GOG 1.37.5.2, [#282](https://github.com/happy-dwarf-team/anvil/pull/282)); lab `docs/script-effect-game-test.md`. **[hypothesis]**: not checked. The ADR's decisions were taken after those runs.

## Decision

1. **A new standard plugin, `anvil-script`**, publishes the service `anvil-script.effects` version 1. It has `phase = "early"`, `affects_gameplay = false`, no dependency, and its own `symbols.toml` (11 symbols per build, Steam and GOG 1.37.5.2). It ships and is released with Anvil, like `anvil-log-throttle` and `anvil-map`.
2. **Only plugins with `affects_gameplay = true` may register an effect.** The service asks the core through `anvil.control` and refuses any other plugin with `ANVIL_ERROR_WRONG_KIND`. While the core suspends the owner (ironman, multiplayer, design §13), its effects do nothing.
3. **Only `early` plugins register, before the game parses a script file**: in practice in their `anvil_plugin_load`. A `main` plugin, and any call after the game has parsed its first effect, gets `ANVIL_ERROR_WRONG_PHASE`.
4. **A key starts with the owner's id** (`-` becomes `_`) and an underscore: Chisel's keys are `chisel_*`. A key the game knows, and a key another plugin has, are refused.
5. **v1 is effects with a single value** (`key = value`), executed by the game on its own thread, with an optional tooltip line. Triggers, block values (`key = { ... }`) and effects in multiplayer beyond each client running its own copy are not in v1.

## How the game reads an effect

[pdb] EU4 has no table of effect names. `CEffect::ReadMember(CReader&, int token)` is a 31.5 kB switch over the lexer's compile-time token ids, and every container effect (scopes, `if`, `limit`, options, `random_list`, scripted effects with parameters) parses its members through it. Each case allocates the effect's class, appends it to the parent's children, gives it the right-hand side (`Assign` for a value, `Read` for a block) and sets its token. A key that is no built-in token falls through a list of data lookups (regions, areas, event targets, scripted effects, …) to `Unknown effect type`, which skips only that key and its value.

So the service hooks the start of that one function, compares the key of the member being read (`CReader::_Toklhs`) with the registered keys, and for one of them builds its own `CEffect`: the game's constructor, a copy of the game's vtable with five slots replaced, the parent's `Append`, its token. Anything else goes on to the game. Nothing in the 31.5 kB switch is patched.

[live] The prototype did exactly this, on Steam, for `lab_set_terrain = <terrain>` in event options, a scripted effect with a parameter and an AI decision: the game's own `has_terrain` in the same block saw the new terrain at once, and a block on the key went back to the game, which logged it and skipped only that key. Two things the static read had wrong: the token of a plugin's key is `0xF`, not the `0xC` that `CLexer::FindTok` answers for an unknown word, and `CEffect::GetScopeProvinceId` and `GetScopeCountryTag` answer only for an effect's scope bits (`ROOT`, `FROM`, event targets) and give 0 and `---` for the plain scope, so the service reads the scope's fields, as the game's simple effects do (`CEventScope+0x28` for the province, `+0x2C` or the override at `+0x44` for the country).

## Why a separate plugin

- **Not in `anvil-game`.** `anvil-game` is what suspension and safe mode stand on (design §10, §13), and [ADR 0007](0007-plugin-map-modes.md) keeps the map's hooks out of it for that reason. The script hook is riskier than those: it runs for every member of every effect in every script file, on the threads that parse them, seconds before the game is ready. A mistake there crashes the start, which counts toward safe mode; safe mode keeps `anvil-game` and `anvil-overlay`, and it must not keep a hook that crashes parsing. `ReadMember` is also the hardest address of the set to port (the map between builds found it only on its third pass [pdb]). A separate plugin fails, is disabled or is skipped on its own, and only its dependents go with it.
- **Not in the core.** The rule of design §4 holds: the core knows the game only by its build. Effects are game knowledge that only some plugins need.
- **Not in each plugin.** Chisel could hook `ReadMember` itself, and lab decision #32 chose a service instead. Two plugins with their own detours on one function chain in load order, and both would copy the game's vtable and its lifetime rules; a mistake in either breaks the parse of every mod. One service gets the ABI right once, keeps the key rule, and gives one log line format for modders.
- **Not `anvil-game.script`.** The working name of #32 put the service in `anvil-game`. The first point is the reason it moved.

## Why only gameplay plugins

ADR 0004 (stage 3, point 5) made `affects_gameplay` the gate for any service that changes the game, and said the core should enforce it, because a service cannot see who calls it. An effect changes the campaign by definition: it runs inside the game's commands and the tick.

- **How it is enforced.** `register_effect` takes the caller's `AnvilPlugin *self`. `anvil-script`, a standard plugin released with the core, asks the core's private service `anvil.control` for that plugin's entry (a lookup by handle appended to `anvil.control` v1) and refuses a plugin with `affects_gameplay = false`. No host field is added to API 2, and a plugin cannot claim another's flags: the core answers from the manifest it resolved.
- **Suspension.** The core suspends a gameplay plugin's changes in ironman and multiplayer unless the player opted in (design §13). The effects are not changes: once the game has parsed them they are objects of the game, reached through their vtable, and suspending the owner's hooks would not reach them. So at each execution the service asks whether the owner is suspended, and a suspended owner's effect does nothing, as if the plugin were missing but without an `error.log` line. In a multiplayer game this makes every client agree: a client with the plugin suspended skips the effect, and a client without the plugin dropped it at parse time.
- **What it does not stop.** A native plugin can hook `ReadMember` itself. The gate is for the service's promise, not a sandbox.

## Why early, and refused afterwards

The game parses effects once per file, while it loads its databases, and again only on a reload. A key registered after its file was parsed is never seen there: the file already holds `Unknown effect type` for it.

[live] On the lab machine the first call of `ReadMember` came 11–17 s after an early plugin loaded (4 starts). A `main` copy of the prototype loaded 85 ms after the entry point, 11.7 s before the first call, and saw every one of the 862 469 calls an early copy saw. So `main` wins by a wide margin there; it is still a race against a slower disk, a faster start or a plugin ahead of it that blocks the main phase.

A `main` plugin's load races the game's start-up; lab start-ups put `plugins loaded at` 50–220 ms after the entry point and the databases seconds later [live, lab measurements], so a `main` registration would work on most machines and fail on a slow or fast one. The service turns that race into a rule that answers the same on every machine: it refuses a `main` plugin (its phase comes from `anvil.control`, like its `affects_gameplay`), and it records when the hook first sees a member and refuses every registration from then on. A plugin learns it at its first start instead of from a player's bug report. An `early` plugin may depend on `anvil-script` because `anvil-script` is early itself.

## Why prefixed keys

The hook runs before the game's switch, so a registered key wins over everything the game would make of it: a built-in effect, a scripted effect of a mod, a region name. A key `<owner>_<name>` cannot be a built-in effect of 1.37.5 (none starts with a plugin's id) and makes the owner plain to a modder who reads it. The service still refuses a key the lexer knows as a built-in token, and a key another plugin registered first. A mod's scripted effect with the same name as a registered key is shadowed; the service logs it once when it parses one [hypothesis: the scripted effect database is filled before events, as `GetScriptedEffect` is a lookup].

## What v1 covers

| Question | v1 | Why |
| --- | --- | --- |
| Effects with a value (`key = value`) | Yes | Chisel's two keys need nothing else; the value is the token's text, copied at parse time. |
| Block values (`key = { ... }`) | No | A block needs a reader API for plugins: nested keys, scopes, the game's lexer. A block on a registered key goes to the game, which reports it as unknown. |
| Triggers | No | Same shape (`CTrigger::CreateAndReadTrigger`, 5 more symbols [pdb]), but `Evaluate` runs inside `ParallelFor` in AI and alert passes, so a plugin's trigger is called on worker threads and must be pure. Chisel needs none in v1: a mod tests the result with the game's own `has_terrain`. A trigger service is version 2 or a second service. |
| Threads | The game's: parse on the parsing threads, execute where the game executes | [live] Seven threads called `ReadMember` during a start: one loading thread (not the main one) with all but about 900 of the 862 469 calls and every hit of a plugin's key, five more with 77–260 calls and the main thread with 150. `Execute` (event options, an AI decision) and `GetDesc` ran on the main thread. The plugin's `execute` is called where the game's effects run, inside a command or the tick; `anvil-game.world` refuses that point (design §13.1), so the plugin uses what the call gives it. |
| Tooltips | Optional `describe`: one line, UTF-8 | [live] The base `GetDesc` adds nothing to an option's tooltip; an own line shows under the scope's header. The game's lines end with a line break and the parent indents them, so the service adds the break; without it the next line joined ours. The game also builds a decision's description in the background (about 2 000 calls in 4 months of play for one decision), so `describe` must be cheap. Without `describe` the effect has no line in the tooltip, as a bare `CEffect`. Text goes to the game by ADR 0007's rule (point 5). |
| Multiplayer | The service does nothing special | Each client runs its own parsed copy [pdb: the command carries only the event and option, or the decision's name]. Whether a plugin may run in multiplayer stays its own `allow_multiplayer`, and ADR 0004 (stage 3) says what that takes: the same plugin and version everywhere, `execute` deterministic, no extra draw from the game's RNG. |
| Saves | Nothing is saved | Effects come from script files, never from a save [pdb]. State an effect leaves must be the game's own (a flag, a modifier) or the plugin's; Chisel keeps it in hidden province flags. |

## Considered options

- *The service in `anvil-game` (`anvil-game.script`).* Rejected: a fault in the hook, or a port that has not found `ReadMember`, would cost suspension and safe mode.
- *Each plugin hooks `ReadMember` itself.* Rejected in #32: the ABI and the lifetime rules would be copied into every plugin, and their detours would chain on one function that every mod's parse goes through.
- *Registering keys in `plugin.toml`*, so a `main` plugin could have its keys known before it loads. Rejected for v1: the callbacks are code, which the service cannot call before the plugin loads, and an `early` load costs a plugin nothing it needs.
- *Patching the switch, or a dynamic lexer token per key (`CLexer::AddDynamicToken`).* Rejected: the switch's cases are compiled in, a new token reaches its default branch anyway, and patching 31.5 kB of code per build is what a detour on its prologue avoids.
- *A host field for the caller's manifest (`affects_gameplay`, suspended).* Not needed: `anvil.control` already holds both for the overlay, and only standard plugins use it. If a plugin outside Anvil ever needs the gate, the host field is the next step.
- *Block values and triggers in v1.* Rejected: nothing needs them yet, and each is a contract we would have to keep within version 1.

## Consequences

- Anvil gains a standard plugin, `anvil-script`, released with Anvil from 1.16 (working number); `anvil/plugins/anvil-script/` in the release zip, `symbols.lock` committed and checked in CI. Design §4 lists it; `include/anvil/anvil_script.h` is its C interface.
- `anvil.control` v1 gets one appended call, the plugin entry for a handle.
- Without registered effects the hook passes every member to the game. It still runs for every member of every parsed effect, 862 474 calls in a start of the base game with a small mod [live], so its empty cost is measured on the start-up before the release.
- Chisel depends on `anvil-script` and is `early`.
- A mod that uses a plugin's keys without the plugin gets one `Unknown effect type` line per use in `error.log` and loses only those effects. The plugin's documentation tells modders so, and how to detect the plugin in script (an effect, then the game's own trigger on its result).
- Mod validators (CWTools) flag the keys; a rule file for them is the plugin's business.

## Limits

- The prototype ran on Steam only; the service from the implementation's branch ran on Steam and GOG 1.37.5.2, with the same token, scope and tooltip [live]. GOG 1.37.5.0 comes as a third build.
- Not seen in the game: a decision the player takes (the decisions view did not open through Tongs), an event option the AI chooses, suspension in ironman or multiplayer (unit tests only), a reload of the scripts, and the start-up's empty cost in time.
