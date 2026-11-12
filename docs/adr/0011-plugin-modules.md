# A plugin may declare modules, each with its own gameplay flags

Status: **accepted** (2026-11-12, proposed 2026-11-11). The maintainer chose variant C (handles) and the points under "Release" below. A minimal implementation of the core and of `anvil-sdk` is in the same pull request; the Manager and the overlay are not.

`affects_gameplay`, `allow_ironman` and `allow_multiplayer` belong to a whole plugin (design §13). A plugin that bundles features of different kinds therefore takes the strictest kind for all of them: one feature that posts a command makes the plugin a gameplay plugin, and the core suspends every feature of it in ironman, including the ones that only draw the interface. The lab's port of europagiga (16 features by one author, MIT; eu4-engine-lab `docs/europagiga-port-plan.md`) was split into five plugins for this reason alone: Whetstone and Sieve draw, Muster posts commands, the automata and Anbennar's lifespan fix change the rules. Each would be its own repository, Workshop item, catalog entry and approval. The maintainer asked for one plugin with a switch per feature instead. This ADR decides how a plugin declares such modules, how the core keeps applying §13 to each of them, and what the player, the multiplayer hash and older plugins see. One choice is hard to undo once plugins use it: a module is a handle the plugin acts through, not a tag on its changes.

How to read the evidence. **[code]**: read in this repository or a team plugin. **[test]**: Anvil's unit tests on this branch. **[hypothesis]**: not checked. No game was started for this ADR.

## Decision

1. **`plugin.toml` may list modules** in `[[modules]]`: `id` (`[a-z][a-z0-9_]*`, at most 32 characters, unique in the plugin), `name`, an optional `description`, `affects_gameplay` (required, as for the plugin), `allow_ironman` and `allow_multiplayer` (`false` or `"opt-in"`, default `false`) and `default` (whether the module is on until the player chooses, default `true`). At most 32 modules. A module that does not affect gameplay must not offer an opt-in.
2. **The plugin acts through a module's handle.** A new host function, the first after the "end of API 2.0" line, `module(self, "drill_all", &out)` gives an `AnvilPlugin *` for that module. Every host call takes it where it takes `self`. Changes requested through it belong to the module; settings, symbols, folders and the plugin's services stay the plugin's. A module the player turned off answers `ANVIL_ERROR_DISABLED` (a new result code) and gives no handle, so its code never runs; an undeclared one answers `ANVIL_ERROR_NOT_DECLARED`.
3. **Each module is an owner of the change registry** (design §8), right after its plugin in load order. The gameplay policy (§13) holds an entry for the plugin's own handle and one per module, and suspends each by its own flags. `anvil.suspended` and `anvil.resumed` go to the subscriptions made through that handle. Services that check their caller (`anvil-game.commands`, `anvil-script`) ask `anvil.control` about the handle they got, so they see the module's flags and whether it is suspended.
4. **The player names a module `<plugin>.<module>`** in `anvil.toml`: `allow_ironman = ["smithy.drill_all"]`, `allow_multiplayer = […]`, and two new lists, `enabled_modules` (turns on a module whose `default` is `false`) and `disabled_modules` (turns one off). A choice applies at the next start.
5. **The multiplayer hash** of §13 has a line `<plugin>.<module> <version>` for every module that affects gameplay and acts in the session, next to the line `<plugin> <version>` of a plugin whose own handle does, so two players with the same plugin and different gameplay modules see different hashes.
6. **The plugin's own `affects_gameplay` stays required** and covers its own handle: the shared code that subscribes to events, reads the world and registers services. A modular plugin usually has `affects_gameplay = false` there and keeps everything that changes the game in modules. The plugin depends on `anvil-game` implicitly (§10) when it or any module affects gameplay.

## Release

- **The core, not the plugin, turns a module on and off**, by the two `anvil.toml` lists of point 4. A plugin's own bool setting per feature was the alternative; only the core's lists keep the multiplayer hash and the suspension true, and a module off at the start runs no code at all, which also helps when a module crashes.
- **A choice applies at the next start.** Turning a module off while the game runs is left for later (see "Considered options").
- **Modules ship whole, in the release after 1.21** (working number 1.22.0), not split across releases. Before that release the player gets, each in its own pull request: the module switches and opt-ins in the Manager, the modules' state in the overlay, the module list in the approval prompt of a plugin from a mod, and the modules in `anvil check` output. The catalog's "some modules" can follow, since no modular plugin is in the catalog before then.
- **"Module" is one word across the team's plugins.** A plugin that already calls its setting-switched feature groups modules (Bellows) means the same thing and can move them to `[[modules]]` once this ships.

## Why handles

The core can apply §13 only to what it can tell apart. A plugin's acts reach the core in three ways: changes in the registry, calls of services that check the caller, and events it receives. A handle carries the module's identity into all three with no new rule: the registry already suspends by owner, the services already ask `anvil.control` about the `self` they get (ADR 0008), and the bus already delivers `anvil.suspended` to one owner. The core needs no new notion of "a feature", only one more owner per module.

What the plugin's author writes is the shape every plugin already has, once per module:

```cpp
AnvilPlugin *drill = nullptr;
if (host->module(self, "drill_all", &drill) == ANVIL_OK) {
    host->subscribe(drill, ANVIL_EVENT_SUSPENDED, on_drill_suspended, nullptr);
    commands->post(drill, &command, &result);   // refused while the module is suspended
}
```

## What the player sees

| Where | What |
| --- | --- |
| Approval of a plugin from a mod (overlay Mods tab, Manager) | The folder hash, as today, covers the whole plugin and every module. The prompt lists the modules: name, `affects_gameplay`, the opt-ins each offers and whether it is on by default. One approval for the plugin. |
| Manager, Plugins tab | Under the plugin, a line per module with a switch (`enabled_modules` / `disabled_modules`) and, for a module that offers them, the ironman and multiplayer opt-ins. "Applies at the next start". |
| Overlay, Plugins tab | The plugin's modules with their state: on, off (by the player), suspended in this session and why. |
| Overlay, ironman notice | As today while a gameplay owner acts by opt-in; it names the modules. |
| Overlay, multiplayer hash | Covers the plugin's line and each acting gameplay module's line. |
| Catalog | `affects_gameplay` of a modular plugin shows as "some modules". |

## Checksum, achievements and the multiplayer hash

Anvil never changes the game's files: a mod with only `descriptor.mod` and `anvil/**` keeps the 1.37.5 checksum (design §14, E1), and plugins are code in memory. So the game's checksum and its own achievement rules (ironman and the checksum) do not depend on modules at all. What depends on them is what Anvil adds:

| Module | Normal game | Ironman | Multiplayer |
| --- | --- | --- | --- |
| `affects_gameplay = false`, on | acts | acts, no notice | acts, not in the hash |
| `affects_gameplay = true`, on, no opt-in | acts | suspended | suspended, not in the hash |
| `affects_gameplay = true`, on, opted in | acts | acts, ironman notice (achievements stay on, Paradox's rules may not allow it) | acts, a line in the hash |
| any, off (`disabled_modules`, or `default = false` and not enabled) | its code does not run | same | same, not in the hash |

A setting's value is not in the hash, for a module as for a plugin today. A gameplay module whose result depends on a setting can still disagree between two players with the same hash; that is §21 (comparing plugins between players), not this ADR.

## Compatibility

- **Old `plugin.toml`.** A manifest without `[[modules]]` means what it meant: the plugin is one owner and its flags are the plugin's. Existing `allow_ironman = ["tongs"]` lines keep working.
- **New `plugin.toml` on an older Anvil.** Unknown manifest keys are errors, so an older Anvil skips the plugin with `unknown key 'modules'`. The plugin declares `anvil = ">=<the release with modules>"`, as for any added key (CONTRIBUTING).
- **API 2.** The host table grows after the "end of API 2.0" line (`ANVIL_HOST_HAS(host, module)`), and `AnvilResult` gets `ANVIL_ERROR_DISABLED = 15`; nothing is changed or removed, so this is a `feat(api)`, not a new API. A plugin that declares modules never runs on a host without the field, because that host refuses the manifest.
- **`anvil.control`** gets appended fields: the module count and a call for a module's entry. It is private (ADR 0001) and may change in a minor release.
- **`anvil.toml`.** `enabled_modules` and `disabled_modules` are new keys that an older Anvil ignores [code: `discovery.cpp` of 1.20.0 reads only the keys it knows and reports no unknown ones].
- **Rust.** `anvil-sdk` 0.1.9 gets `Host::module(name) -> Result<Host>` and `ErrorCode::Disabled` (`ErrorCode` is `#[non_exhaustive]`, so the variant breaks nothing); `anvil-sys` gets the field and the constant in its bindings.

## Considered options

- *Keep one plugin per kind (the 7.11 split).* Needs no change in Anvil. Rejected by the maintainer's request: five repositories, Workshop items and approvals for one author's features, and a player who wants three of them installs three plugins.
- *One gameplay plugin that switches its features itself*, with a bool setting per feature and its own reading of the session. Rejected: the core cannot enforce it, which ADR 0004 (stage 3, point 5) made the rule for anything that changes the game. In ironman the whole plugin is suspended unless the player opts in to all of it, interface features included; with the opt-in every feature is allowed and only the plugin's own code stands between a rule-changing feature and an ironman game. The hash would show the plugin whatever its features do.
- *A module tag on changes* (`begin_group(self, "drill_all")`). Covers the registry only. Services and events would still see the plugin, so `anvil-game.commands` would accept a command from a suspended module.
- *Several manifests in one package* (`anvil/plugins/smithy/drill_all/plugin.toml`, one DLL with an entry per manifest). Every module would be a full plugin: its own version, approval, catalog line and dependency checks, and the folder layout of discovery (§10) would change. That is the five plugins again, in one folder.
- *Turning a module on and off while the game runs.* Possible with the same owner: off is a suspension that does not end with the session. Left for a later version, because a module turned off at the start should not run its setup at all (it may be off because it crashes), and the two paths need one clear rule for the overlay.

## Consequences

- A plugin like the lab's working "Smithy" can ship every europagiga feature in one repository and one Workshop item, with the interface modules acting in ironman and multiplayer and each command module asking for its own opt-in.
- The core gains, per module: one registry owner, one policy entry, one bus owner (`<plugin>.<module>`) and one `anvil.control` entry. No new code runs in the game's frames or tick.
- Crash attribution (§12) names the module for a crash in its change (the owner); a crash in the plugin's DLL names the plugin, since the modules share it. Safe mode and "disable X at the next start" stay per plugin.
- An update of one module changes the plugin's folder hash, so a plugin from a mod waits for one new approval, as today.
- design §5.1 and §13 are updated with the release that ships it.

## Limits

- The implementation in this pull request covers the manifest, the host function, the registry owners, the policy, the hash, `plugin_of`, the two `anvil.toml` lists and `Host::module` in `anvil-sdk`, with unit tests [test]. The Manager, the overlay, the approval prompt, `anvil check` output and the catalog are described above and not built: until they are, a player edits `anvil.toml` by hand to choose modules and opt-ins.
- `bindings.rs` was edited by hand to what bindgen gives for the two additions (one constant, one field and its offset check), because the machine of this change has no libclang; CI's `regen-bindings.ps1 -Check` decides.
- Tried in the game (lab run, 2026-11-12, Steam 1.37.5, single player; eu4-engine-lab `docs/anvil-modules-game-test.md`): `disabled_modules` and `enabled_modules`, `ANVIL_ERROR_DISABLED` and `NOT_DECLARED` from `module()`, per-module change counts, a module's DRILL posted in a normal game, the same module suspended in ironman without its opt-in (`ANVIL_ERROR_SUSPENDED` from `anvil-game.commands`, `anvil.suspended` to that module only, interface modules untouched) and acting with it. Not seen in the game: multiplayer and its hash lines (unit tests only), GOG.
