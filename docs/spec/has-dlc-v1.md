# has_dlc for plugin names, v1

A mod that uses a plugin's script effect (Chisel's `chisel_set_terrain`, for example) needs a way to take another path when the plugin is not there or does not act. `anvil-script` answers the game's own `has_dlc` trigger for a reserved name, `anvil:<plugin id>`, without registering anything as a DLC ([anvil#316](https://github.com/happy-dwarf-team/anvil/issues/316)). Terms are those of [GLOSSARY.md](../../GLOSSARY.md).

Until the Anvil release that ships it, this specification is a living document: when implementation or a game test learns something that changes it, a pull request changes it. After that release it is frozen.

## 1. For modders

```
if = {
    limit = { has_dlc = "anvil:chisel" }
    chisel_set_terrain = forest
}
else = {
    add_province_modifier = { name = my_mod_drought duration = -1 }
}
```

- The name is `anvil:` and the plugin's id as its `plugin.toml` writes it, in quotes (the colon is not a word character of the game's lexer). Anything else, `anvil:Chisel` included, is a name the game does not know.
- **True** only while that plugin is loaded and not suspended, in a single-player session: also in ironman, for a gameplay plugin the player let act there (design §13), and for any plugin with `affects_gameplay = false`.
- **False** in multiplayer for every plugin, so a host with Anvil and a client without it take the same branch; outside a session (the main menu, a save that is still loading); for a plugin that is not installed, is disabled, failed or is suspended; and, without Anvil, because the game knows no DLC of that name.
- `NOT = { has_dlc = "anvil:chisel" }` is the plugin's absence, as with a DLC.
- The tooltip is the game's own for a DLC it does not know: an empty name with "enabled" or "disabled".

## 2. What the game does with an unknown name

[pdb] `CHasDLCTrigger` (0x70 bytes) keeps one bit per DLC the game knows, 31 of them, in a bit field at `+0x68`. `CHasDLCTrigger::Assign` copies the token's text, compares it with each DLC's name (`strcmp`) and sets that DLC's bit; for any other name it sets nothing, logs nothing and returns true, and the text is freed. `Evaluate` tests the bits in order, and without any it answers false. So without Anvil, `has_dlc = "anvil:chisel"` parses silently and is false; `make_unique<CHasDLCTrigger>` clears bits 0 to 30 and leaves bit 31 as the heap had it, so no unused bit can carry a mark, and `+0x6C` to the end of the object is padding.

## 3. How it is built

- A function hook on the start of `CHasDLCTrigger::Assign` (Steam `0xCC5BA0`, GOG `0xCC4350`) reads the token's text (`+4`). A name of section 1 is interned in a table of at most 256 plugin ids, under a lock, on whichever thread parses; the trigger keeps the game's cleared bits, gets the id's index in its padding (`+0x6C`) and a copy of the game's vtable (16 slots, from `make_unique`'s `lea`) with `Evaluate` (slot 6) replaced. Every other name goes on to the game. The copy is checked at load: every slot inside `eu4.exe`, slot 7 the hooked `Assign`, the bits and the index inside the object's 0x70 bytes; otherwise has_dlc stays the game's and the log says so.
- The replaced `Evaluate` is one relaxed atomic load of the index's answer. The game evaluates triggers inside its `ParallelFor` passes (ADR 0008), and the table is only written while it parses and when a session starts or ends.
- The answers are set on `anvil-game.session`: Anvil suspends plugins before any plugin hears of a session, so `anvil.control`'s entries (`plugin`, state `loaded`, `suspended`) say who acts in it. `STARTED` without multiplayer gives each id its plugin's answer, `STARTED` in multiplayer and `ENDED` give false to all. An id first seen during a session gets the answer of the last update. Each update logs the answers (`has_dlc for this session: chisel yes`), and the line at `anvil-game.ready` counts the names parsed.
- Symbols: `has_dlc_assign` and `has_dlc_make`, 15 per build in all, Steam and both GOG builds. A build without them keeps has_dlc as the game's; the effects and the Day end do not depend on them.
- A trigger object the game makes by copying one of these (no such copy is known [hypothesis]) would get the game's vtable back and answer false.

## 4. Checksums and multiplayer

- **The game's checksum** (the one the lobby compares and ironman records) is made from the game's and the mods' files. The gate changes no file and no data the game loads: the mod's text is the same with or without Anvil, and no DLC is added to the game's DLC list, so the lobby, the checksum and the DLC list a host sends are unchanged [pdb: the gate never touches `CDLCManager`].
- **Synchronisation.** In multiplayer every machine answers false from the session's start, which is what a machine without Anvil answers, so every machine takes the same branch and the game's state stays the same everywhere. Before the session event, while a game or a save loads, the answer is also false on every machine. A single-player save played with the plugin keeps what the branches did; loading it into multiplayer only changes which branch later evaluations take, for everyone alike.
- **Ironman.** The answer follows suspension, so a gameplay plugin the player did not let act in ironman is "not there" in its scripts too.
