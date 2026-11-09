# Script effects v1 specification

`anvil-script.effects` version 1 lets a gameplay plugin add effects of its own to EU4's script language: a modder writes `chisel_set_terrain = mountain` in an event option, a decision or a scripted effect, and the game runs the plugin's code where it runs its own effects. The decisions and their reasons are in [ADR 0008](../adr/0008-plugin-script-effects.md); the static evidence is the lab's `docs/research/script-effect-statics.md` and the prototype's run is the lab's `docs/script-effect-game-test.md` (eu4-engine-lab#32, #77). Terms are those of [GLOSSARY.md](../../GLOSSARY.md).

Until the Anvil release that ships `anvil-script`, this specification is a living document: when implementation learns something that changes it, a pull request changes it. After that release it is frozen, and version 2 gets its own file.

## 1. The plugin

| | |
| --- | --- |
| Folder | `plugins/anvil-script`, released in `anvil/plugins/anvil-script/` |
| Manifest | `phase = "early"`, `affects_gameplay = false`, no dependencies |
| Symbols | 11 per build, Steam 1.37.5 and GOG 1.37.5.2 (section 7); `symbols.lock` committed and checked in CI |
| Service | `anvil-script.effects` version 1, registered in its load |
| Header | `include/anvil/anvil_script.h` (C), in the SDK |
| Safe mode | Not kept (design §10): a safe start loads no `anvil-script`, so no plugin effect is parsed |

A consumer declares the dependency and is `early` itself:

```toml
phase = "early"
affects_gameplay = true

[[dependencies]]
id = "anvil-script"
version = "^1.16"   # the first Anvil with the service (working number)
```

## 2. The C interface

```c
#define ANVIL_SCRIPT_EFFECTS_SERVICE "anvil-script.effects"
#define ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION 1

/* Where an effect runs: the scope the game gives it. */
typedef struct AnvilScriptScope {
    uint32_t size;          /* set by the service */
    int32_t province;       /* the scope's province ID; 0 when the scope has none */
    char country[4];        /* the scope's country tag, "RUS"; "" when it has none */
} AnvilScriptScope;

/* One execution of one effect. Valid only during the call. */
typedef struct AnvilScriptCall {
    uint32_t size;          /* set by the service */
    uint32_t reserved;
    const char *key;        /* the registered key, lower case */
    const char *value;      /* the right-hand side as written, without quotes */
    AnvilScriptScope scope;
} AnvilScriptCall;

/* Runs where the game runs its own effects (section 4). */
typedef void (*AnvilScriptExecute)(void *context, const AnvilScriptCall *call);
/* One line for the tooltip, UTF-8, no line break. Writes at most capacity
 * bytes including the terminating zero and answers the whole length; 0 for no
 * line. The scope is the one the tooltip is built for. */
typedef uint32_t (*AnvilScriptDescribe)(void *context, const AnvilScriptCall *call, char *utf8, uint32_t capacity);

typedef struct AnvilScriptEffect {
    uint32_t size;               /* sizeof(AnvilScriptEffect) as known by the caller */
    uint32_t flags;              /* 0 in version 1 */
    const char *key;             /* "<owner>_<name>": section 3 */
    AnvilScriptExecute execute;  /* required */
    AnvilScriptDescribe describe;/* may be NULL: no tooltip line */
    void *context;               /* passed back as is; must live for the process */
} AnvilScriptEffect;

typedef struct AnvilScriptEffectsService {
    uint32_t size;
    /* From an early plugin with affects_gameplay = true, before the game
     * parses its first effect: in practice in its anvil_plugin_load. The
     * service copies the key; the callbacks and the context must live for the
     * process. */
    AnvilResult (*register_effect)(AnvilPlugin *self, const AnvilScriptEffect *effect);
} AnvilScriptEffectsService;
```

C++ (`anvil.hpp` gets nothing new): `host.service<AnvilScriptEffectsService>(ANVIL_SCRIPT_EFFECTS_SERVICE, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION)` and a plain `noexcept` function for each callback. The callbacks are called by the game through the service, not through a change, so the wrapper's adapters do not catch for them: an exception that leaves one ends the process like any exception in game code, and the crash report names the plugin's code (design §12).

Within API 2 (ADR 0001): the structs grow only by appending behind `size`; a changed meaning is version 2 next to version 1.

## 3. Registration

`register_effect` checks, in this order, and answers the first failure:

| Check | Result |
| --- | --- |
| `self` is a plugin the core knows, `effect` and its `key` and `execute` are not NULL, `effect->size` covers `execute` | `ANVIL_ERROR_INVALID_ARGUMENT` |
| The caller's manifest has `affects_gameplay = true` (from `anvil.control`) | `ANVIL_ERROR_WRONG_KIND` |
| The caller's manifest has `phase = "early"` (from `anvil.control`), and the game has not parsed an effect yet (the hook has seen no call) | `ANVIL_ERROR_WRONG_PHASE` |
| The key is 2–63 bytes of `[a-z0-9_]`, starts with the caller's id with `-` as `_`, then `_`, and has a name after it | `ANVIL_ERROR_INVALID_ARGUMENT` |
| No plugin registered the key before | `ANVIL_ERROR_ALREADY_EXISTS` |
| `flags` is 0 | `ANVIL_ERROR_UNSUPPORTED` |
| Room: at most 256 keys in all | `ANVIL_ERROR_OUT_OF_MEMORY` |

Each refusal is logged with the caller and the key. A key is never removed: nothing is unloaded in API 2.

The service cannot ask the game's lexer at registration, because an early plugin loads before the game's C runtime. A key that is a word of the game is refused at parse time instead: when the hook sees a registered key with a token other than the parser's token for a name the lexer does not know (`0xF` on 1.37.5, seen in the game on both builds; the lexer's `FindTok` answers `0xC`, but that is not what reaches `ReadMember`), the key is passed to the game and the service logs once that the key is a word of the game or of its data.

## 4. Life of an effect

**Parse.** The hook on `CEffect::ReadMember(effect, reader, token)` runs for every member of every container effect, on whichever thread parses: seven threads in a start of the base game, one loading thread with almost all calls [live]. It reads the key from `CReader::_Toklhs` and looks it up (a bit per first byte of the registered keys, then a case-insensitive comparison with each key; no lock: the table is complete before the first parse). For a registered key whose right-hand side (`CReader::_Tokrhs`) is not `{`, it:

1. allocates its object from its own heap: the game's `CEffect` (0x38 bytes) followed by the key's index and a copy of the value (up to 255 bytes; a longer value is refused, as below);
2. runs the game's constructor `CEffect::CEffect()`, which sets the token to −1 and registers the effect in the game's effect database under its mutex;
3. puts its vtable over the game's: one copy of `CEffect`'s 19 slots, made once, with the scalar deleting destructor (0), `Assign` (9), `GetDesc` (13) and `Execute` (15) replaced; the rest stay the game's;
4. appends the object to the parent's children through the game's `CArray::Append`, which grows the vector with the game's heap;
5. calls its own `Assign` through the vtable with `_Tokrhs`, as the game does for its simple effects, which copies the value;
6. stores the token in `_Token` (`CEffect::SetToken` is that one store).

A block on a registered key, and a value over 255 bytes, are passed to the game, which logs `Unknown effect type. Key: <key>` and skips them; the service adds one line to `anvil.log` per key with the reason. Every other member goes to the game unchanged.

**Execute.** The game calls slot 15 (`Execute(CEventScope&, int)`) where it runs its own effects: an event option or a decision inside the command that every client runs, an on-action in the tick, a scripted effect wherever it is used [live: on the main thread, inside the command of an event option and of an AI decision; the `int` was 0]. The service:

1. returns at once when the owner is suspended (design §13), asked through `anvil.control` at each call;
2. fills `AnvilScriptScope` from the scope's own fields, as the game's simple effects read them: the province at `CEventScope+0x28` (as `CProvinceCultureEffect::Execute`), the country's tag at `+0x44` when `+0x4B` is set and at `+0x2C` otherwise (as `CTreasuryEffect::Execute`), `""` when that tag is not valid. `CEffect::GetScopeProvinceId` and `GetScopeCountryTag` are not used: they answer only for an effect's scope bits (`ROOT`, `FROM`, event targets), which the service's effects never have, and gave 0 and `---` in the game [live]. In a province scope the country is `""` [live];
3. calls the plugin's `execute`.

`anvil-game.world` answers `ANVIL_ERROR_WRONG_PHASE` inside `execute`: the game is in the middle of a command or a day. The plugin reads what it needs itself or from the call.

**Describe.** When the game builds a tooltip it calls slot 13 (`GetDesc(CEventScope&, CString prefix, int, bool)`). The service calls the game's base `GetDesc`, which frees the prefix the caller passed by value and gives an empty string. Without `describe`, or when it answers 0, that empty string is the answer, as for a bare `CEffect`: no line in the tooltip [live]. Otherwise the line (at most 511 bytes) is converted by ADR 0007's rule (UTF-8 kept when Mei reports UTF-8 text, Windows-1252 with `?` otherwise, `§` and `$` escaped) and ended with a line break, as the game's own lines are (the parent indents them; without the break the next line joins ours [live]), and built as the game's string with the game's `CString::CString(const char*)`, so the game frees it with its own heap.

**Destroy.** The game deletes a parent's children through slot 0 when it drops the parent: a reload of the scripts, or never, at exit [pdb]. The service's destructor runs the game's `~CEffect()` (which unregisters it), then frees the object to its own heap when the flag asks for it. Nothing else of the game frees these objects [pdb]; a reload was not tried in the game.

**Saves and hot-join.** Nothing of an effect is written to a save [pdb]. A loaded save, a hot-joining player and a new game parse nothing; the effects parsed at start-up stay.

## 5. Errors and logging

- Every refusal of section 3 and every passed-on member of section 4 writes one line to `anvil.log` per key and reason, never one per member, so a mod that uses a key thousands of times costs one line.
- At `anvil-game.ready`, when `anvil-game` is loaded, one line counts the parsed effects per key; without registered keys it counts the hook's calls.
- A fault in a callback is the plugin's crash (section 2); the service does not wrap the game's calls in guards.

## 6. Threads and cost

- The parse runs on the game's parsing threads, possibly several at once. The key table is read-only after registration, and each object belongs to the thread that parses its parent, so the hook takes no lock of its own; the game's constructor takes the game's mutex.
- `execute` and `describe` run on the thread the game uses for them: the main thread in every call seen [live]. `describe` runs often (the game builds a decision's description in the background), so it must be cheap. A plugin that keeps state across calls guards it itself.
- The empty cost is a test of the key's first byte per call of `ReadMember`, and a comparison with the registered keys that start with it: 862 474 calls in a start of the base game with a small mod [live]. The release measures the start-up with the service and no registered key against Anvil without it (design §17).

## 7. Symbols

Every address is a declared symbol; every offset is a value taken from verified bytes. RVAs are Steam 1.37.5 / GOG 1.37.5.2 (`eu4-1.37.5-steam-win64`, `eu4-1.37.5.2-gog-win64`); the bytes are the same on both builds unless the row says otherwise.

| Symbol | Steam | GOG | What | Values |
| --- | --- | --- | --- | --- |
| `effect_read_member` | `0x5DEC50` | `0x5DE720` | `CEffect::ReadMember(CReader&, int)`, the function hook: `48 89 5C 24 08 48 89 74 24 10 55 57 41 54 41 56 41 57` | |
| `effect_lhs_site` | `0x5E4B04` | `0x5E45D4` | `lea rbx,[rsi+30h]` in its default branch: `48 8D 5E 30 48 8B CB` | `lhs` (`CReader::_Toklhs`, 0x30) |
| `effect_assign_site` | `0x5E0F43` | `0x5E0A13` | `lea rcx,[rdi+10h]` … `lea rdx,[rsi+440h]; call [rax+48h]` in a simple effect's case: `48 8D 4F 10 48 8D 54 24 20 E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 8B 01 48 8D 96 40 04 00 00 FF 50 48` | `children` (0x10), `rhs` (`_Tokrhs`, 0x440), `assign` (slot 0x48) |
| `effect_set_token` | `0x5EF140` | `0x5EEC10` | `CEffect::SetToken`: `89 51 28 C3` | `token` (0x28) |
| `effect_ctor` | `0x5DE770` | `0x5DE240` | `CEffect::CEffect()`: `48 89 5C 24 20 48 89 4C 24 08 57 48 83 EC 40 48 8B D9 33 C0` | |
| `effect_vtable_site` | `0x5DE797` | `0x5DE267` | `lea rax,[CEffect::vftable]; mov [rcx],rax` in it; bytes per build (`48 8D 05 AA FD 6B 01 48 89 01` / `48 8D 05 5A 90 6A 01 48 89 01`) | `vtable` (rip-relative: Steam `0x1C9E548`, GOG `0x1C872C8`) |
| `effect_dtor` | `0x5DE8D0` | `0x5DE3A0` | `CEffect::~CEffect()`: `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 4C 8B F1` | |
| `array_append` | `0xC9F10` | `0xC9F40` | `CArray<T*>::Append`, a `std::vector<void*>` push: `4C 8B C2 48 8B 51 08 48 3B 51 10 74 0C 49 8B 00 48 89 02 48 83 41 08 08 C3` | |
| `scope_province_site` | `0x62367F` | `0x62314F` | `CProvinceCultureEffect::Execute+0x1F`, `cmp dword [rdx+28h],0`: `48 8B DA 48 8B F9 83 7A 28 00` | `province` (0x28) |
| `scope_country_site` | `0x5EFC55` | `0x5EF725` | `CTreasuryEffect::Execute+0x25`, the override's flag and the two tags: `44 0F B6 42 4B BA 44 00 00 00 8B C2 41 B9 2C 00 00 00 45 84 C0 41 0F 44 C1` | `override_valid` (0x4B), `override` (0x44), `tag` (0x2C) |
| `cstring_from_text` | `0x1703550` | `0x16ED320` | `CString::CString(const char*)`: `40 53 48 83 EC 20 33 C0 48 C7 41 18 0F 00 00 00 48 89 01 48 8B D9 88 01` | |

The vtable's slot count (19) and the slots replaced (0, 9, 13, 15) come from the PDB and are the same in both builds' vtables [pdb]; the implementation checks at load that slot 4 of the copied vtable is `effect_read_member` and slot 15 lies inside `eu4.exe`, and registers no service when either fails. The GOG build 1.37.5.0 comes as any third build does (design §5.2).

An absent symbol (§5.2) on a future build: without any of these the service is not registered and `get_service` gives NULL, so a consumer that requires it is skipped with the reason.

## 8. Tests

- Unit tests (`tests/anvil_script_plugin_tests.cpp`) with a stand-in game: a fake `ReadMember`, a fake vtable of 19 slots and a reader with `_Toklhs` and `_Tokrhs`, for the key rule, registration's order of checks, the phase rule, a block and a long value passed on, suspension making `execute` a no-op, `describe` truncation and the destructor's free.
- The game test (Steam and GOG, under the lab's lock): a test plugin registers two keys, a test mod uses them in an event option, a decision, a scripted effect with a parameter and an AI decision; it checks the tooltip line, `has_terrain`-style results right after the effect, a block passed on, the plugin missing (`Unknown effect type` and only that key skipped), a `main` consumer refused, an ironman-style suspension (effect does nothing), and the start-up's empty cost.

## 9. Not in version 1

Triggers; block values and nested keys; a key from `plugin.toml` for `main` plugins; removing a key; effects in a safe start; a rule file for mod validators.
