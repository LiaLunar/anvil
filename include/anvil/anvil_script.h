/*
 * anvil_script.h - the C interface of anvil-script, the standard plugin that
 * lets a gameplay plugin add effects of its own to EU4's script language
 * (docs/adr/0008-plugin-script-effects.md, docs/spec/script-effects-v1.md).
 *
 * A modder writes `chisel_set_terrain = mountain` in an event option, a
 * decision or a scripted effect; the game parses it as one of its own effects
 * and, when it runs the effect, calls the plugin's `execute` with the value
 * and the scope.
 *
 *   - Who: a plugin with affects_gameplay = true and phase = "early", which
 *     depends on anvil-script. Anything else is refused.
 *   - When: register_effect before the game parses its first effect, which
 *     means in anvil_plugin_load.
 *   - Keys: "<owner>_<name>", the owner's id with '-' as '_', lower case
 *     [a-z0-9_], 2-63 bytes. A mod's use is case-insensitive, as the game's
 *     own keys are.
 *   - Values: `key = value` only; a block (`key = { ... }`) is left to the
 *     game, which reports it as an unknown effect.
 *   - Threads: execute and describe run where the game runs its own effects
 *     and tooltips (the main thread, inside a command or the day). There
 *     anvil-game.world answers ANVIL_ERROR_WRONG_PHASE: the game is in the
 *     middle of a command. A callback must not throw.
 *   - Suspension: while Anvil suspends the owner (ironman, multiplayer
 *     without the player's opt-in), its effects do nothing.
 *   - Multiplayer: every client runs its own copy of the effect, so every
 *     client needs the same plugin, and execute must be deterministic.
 *   - Nothing of an effect is saved: what it leaves must be the game's state
 *     (a flag, a modifier) or the plugin's own.
 *
 * Within API 2 (ADR 0001): fields are only appended behind `size`.
 */
#ifndef ANVIL_SCRIPT_H
#define ANVIL_SCRIPT_H

#include <anvil/anvil_plugin.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_SCRIPT_EFFECTS_SERVICE "anvil-script.effects"
#define ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION 1

/* Where an effect runs: the scope the game gives it. */
typedef struct AnvilScriptScope {
    uint32_t size;    /* set by the service */
    int32_t province; /* the scope's province ID; 0 when the scope has none */
    char country[4];  /* the scope's country tag, "RUS"; "" when it has none */
} AnvilScriptScope;

/* One execution or description of one effect. Valid only during the call. */
typedef struct AnvilScriptCall {
    uint32_t size; /* set by the service */
    uint32_t reserved;
    const char *key;   /* the registered key */
    const char *value; /* the right-hand side as the file has it, without quotes */
    AnvilScriptScope scope;
} AnvilScriptCall;

/* Runs where the game runs its own effects. */
typedef void (*AnvilScriptExecute)(void *context, const AnvilScriptCall *call);
/* One tooltip line, UTF-8, no line break: writes at most `capacity` bytes
 * including the terminating zero and returns the whole length; 0 for no line.
 * The scope is the one the tooltip is built for. */
typedef uint32_t (*AnvilScriptDescribe)(void *context, const AnvilScriptCall *call, char *utf8, uint32_t capacity);

typedef struct AnvilScriptEffect {
    uint32_t size;  /* sizeof(AnvilScriptEffect) as known by the caller */
    uint32_t flags; /* 0 in version 1 */
    const char *key;
    AnvilScriptExecute execute;   /* required */
    AnvilScriptDescribe describe; /* NULL: no tooltip line */
    void *context;                /* given back as is; must live for the process */
} AnvilScriptEffect;

typedef struct AnvilScriptEffectsService {
    uint32_t size; /* sizeof(AnvilScriptEffectsService) as known by anvil-script */
    /* The key is copied; the callbacks and the context must live for the
     * process. Results, in the order they are checked:
     *   INVALID_ARGUMENT  NULL arguments, or a size that does not cover execute
     *   WRONG_KIND        the caller has affects_gameplay = false
     *   WRONG_PHASE       the caller is not early, or the game already parsed an effect
     *   INVALID_ARGUMENT  the key is not "<owner>_<name>"
     *   ALREADY_EXISTS    another registration has the key
     *   UNSUPPORTED       flags other than 0
     *   OUT_OF_MEMORY     256 keys are registered already */
    AnvilResult (*register_effect)(AnvilPlugin *self, const AnvilScriptEffect *effect);
} AnvilScriptEffectsService;

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_SCRIPT_H */
