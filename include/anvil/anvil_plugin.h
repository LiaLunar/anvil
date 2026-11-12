/*
 * Anvil plugin API 2: the C ABI between Anvil and its plugins (docs/design.md §9).
 *
 * Plugins are DLLs with a C ABI; any compiler that produces x64 Windows DLLs
 * works. Copy this header into the plugin repository, or use the C++ wrapper
 * anvil.hpp on top of it. Symbols (AnvilSymbol) and settings (AnvilSetting)
 * are indices that `anvil gen` writes from the plugin's symbols.toml and
 * settings.toml; Anvil reads the same shipped files and verifies every symbol
 * before the plugin DLL is loaded.
 *
 * Compatibility within API 2: functions and fields are only appended to the
 * end of a struct; check ANVIL_HOST_HAS(host, field) before using one added
 * after API 2.0. Removing or changing anything here increments
 * ANVIL_API_VERSION.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_PLUGIN_H
#define ANVIL_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_API_VERSION 2

#if defined(_WIN32)
#define ANVIL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ANVIL_PLUGIN_EXPORT
#endif

typedef struct AnvilPlugin AnvilPlugin; /* the calling plugin (opaque) */
typedef struct AnvilChange AnvilChange; /* one change (opaque); remove it with host->remove */
typedef uint32_t AnvilSymbol;           /* declaration index in symbols.toml */
typedef uint32_t AnvilSetting;          /* declaration index in settings.toml */

typedef enum AnvilResult {
    ANVIL_OK = 0,
    ANVIL_ERROR_INVALID_ARGUMENT = 1,
    ANVIL_ERROR_NOT_DECLARED = 2,   /* unknown symbol/setting, or a raw pointer into eu4.exe */
    ANVIL_ERROR_WRONG_KIND = 3,     /* e.g. swap_pointer on a function symbol */
    ANVIL_ERROR_CONFLICT = 4,       /* overlaps another change at a different address */
    ANVIL_ERROR_UNSAFE_SITE = 5,    /* stolen bytes contain JRCXZ / LOOP*, not whole instructions, < 5 declared bytes */
    ANVIL_ERROR_BYTES_MISMATCH = 6, /* the expected bytes or pointer differ from the original ones */
    ANVIL_ERROR_OUT_OF_MEMORY = 7,
    ANVIL_ERROR_WRONG_PHASE = 8,    /* e.g. begin_group inside anvil_plugin_load */
    ANVIL_ERROR_UNSUPPORTED = 9,
    ANVIL_ERROR_INTERNAL = 10,      /* also: anvil_plugin_load crashed or threw */
    ANVIL_ERROR_NOT_FOUND = 11,     /* a change that is already removed or belongs to another plugin */
    ANVIL_ERROR_ALREADY_EXISTS = 12, /* a service name and version registered twice */
    /* Since Anvil 1.17.0 */
    ANVIL_ERROR_SUSPENDED = 13,      /* the caller is suspended in this session (ironman, multiplayer; design §13) */
    ANVIL_ERROR_LIMIT = 14,          /* more than a service takes at once; nothing of the call was done */
    /* Since Anvil 1.21.0 (working number; ADR 0011) */
    ANVIL_ERROR_DISABLED = 15        /* the player turned this module off (anvil.toml) */
} AnvilResult;

typedef enum AnvilLogLevel {
    ANVIL_LOG_DEBUG = 0,
    ANVIL_LOG_INFO = 1,
    ANVIL_LOG_WARNING = 2,
    ANVIL_LOG_ERROR = 3
} AnvilLogLevel;

/* The running game executable. */
typedef struct AnvilGameInfo {
    uint32_t size;         /* sizeof(AnvilGameInfo) as known by the host */
    const char *build_id;  /* "eu4-1.37.5-steam-win64"; NULL when unknown */
    uintptr_t image_base;  /* where eu4.exe is in memory */
    uint32_t image_size;
    const char *sha256;    /* of eu4.exe, upper-case hex; NULL until verified off the critical path */
} AnvilGameInfo;

/* --- mid hooks and block replacements --------------------------------------
 * Exactly SafetyHook's Context64 (408 bytes), pinned by static_asserts in Anvil.
 * Not saved: YMM upper halves, x87, MXCSR. Valid only during the callback. */
typedef union AnvilXmm {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} AnvilXmm;

typedef struct AnvilCpuContext {
    AnvilXmm xmm[16];
    uintptr_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx, rax, rbp;
    uintptr_t rsp;      /* read-only: the stack pointer at the site */
    uintptr_t reserved; /* do not touch */
    uintptr_t rip;      /* writable; in a mid hook a change ends the chain at this address */
} AnvilCpuContext;

/* Runs on the game's thread at the site. It must not unwind out (no C++
 * exception, no longjmp): Anvil's stubs have no unwind information. */
typedef void (*AnvilCpuCallback)(AnvilCpuContext *cpu, void *user);

/* --- events and settings ------------------------------------------------- */
typedef void (*AnvilEventCallback)(const char *event, const void *payload, void *context);
typedef void (*AnvilSettingCallback)(AnvilSetting setting, void *context);

#define ANVIL_EVENT_PLUGINS_LOADED "anvil.plugins_loaded" /* every plugin processed; payload NULL */
/* To the plugin itself, payload NULL (docs/design.md §8.4, §13): its changes
 * are out of the game (ironman, multiplayer) or back in it. */
#define ANVIL_EVENT_SUSPENDED "anvil.suspended"
#define ANVIL_EVENT_RESUMED "anvil.resumed"

typedef struct AnvilHost {
    uint32_t size;             /* sizeof(AnvilHost) as known by the host */
    uint32_t api_version;      /* ANVIL_API_VERSION of the host */
    const char *anvil_version; /* SemVer */
    const AnvilGameInfo *game;

    /* --- environment --- */
    void (*log)(AnvilPlugin *self, AnvilLogLevel level, const char *message); /* UTF-8 */
    const wchar_t *(*plugin_dir)(AnvilPlugin *self); /* read-only plugin folder */
    const wchar_t *(*data_dir)(AnvilPlugin *self);   /* writable anvil/data/<id>/, created on first call */

    /* --- changes (docs/design.md §8) ---
     * Every call on eu4.exe takes a declared symbol: the expected bytes,
     * memory kind and pointer targets come from symbols.toml and were verified
     * before load. Changes requested during anvil_plugin_load are staged and
     * enabled together after it returns ANVIL_OK; later ones are enabled one
     * by one, or together inside begin_group/commit_group. Changes at one
     * address chain in load order (the earlier plugin is outermost); a change
     * overlapping another one at a different address is refused.
     *
     * `next` receives the rest of the chain before the change is enabled, and
     * never changes afterwards; call through it to pass on. `out` (may be
     * NULL) receives the handle for remove. Nothing a change uses (detours,
     * callbacks, `user`) may go away while it lives; Anvil never frees its
     * own stubs, so a thread still inside a removed change finishes safely. */
    AnvilResult (*hook_function)(AnvilPlugin *self, AnvilSymbol fn, void *detour, void **next, AnvilChange **out);
    /* callback(cpu, user) runs when the game reaches the site (a function or
     * site symbol with >= 5 declared bytes), before the code there. */
    AnvilResult (*mid_hook)(AnvilPlugin *self, AnvilSymbol site, AnvilCpuCallback callback, void *user,
                            AnvilChange **out);
    /* The symbol's declared bytes never run: callback runs instead and the
     * game resumes at the symbol's `resume` or at the block's end (unless the
     * callback writes cpu->rip). */
    AnvilResult (*replace_block)(AnvilPlugin *self, AnvilSymbol block, AnvilCpuCallback callback, void *user,
                                 AnvilChange **out);
    /* A `call rel32` site; `next` is the previous target in the chain. */
    AnvilResult (*redirect_call)(AnvilPlugin *self, AnvilSymbol call_site, void *target, void **next,
                                 AnvilChange **out);
    /* `size` bytes at the symbol, within its declared bytes. */
    AnvilResult (*patch_bytes)(AnvilPlugin *self, AnvilSymbol site, const void *bytes, size_t size,
                               AnvilChange **out);
    /* A slot symbol (`points_to`) holds `value`, stored atomically. *previous
     * receives the rest of the chain and is rewritten by Anvil whenever the
     * chain changes, until the change is removed: keep it in a lasting,
     * 8-byte aligned place and read it on each use (may be NULL). */
    AnvilResult (*swap_pointer)(AnvilPlugin *self, AnvilSymbol slot, void *value, void **previous,
                                AnvilChange **out);
    /* An import of eu4.exe; module case-insensitive, e.g. "KERNEL32.dll". */
    AnvilResult (*replace_import)(AnvilPlugin *self, const char *module, const char *function, void *replacement,
                                  void **next, AnvilChange **out);
    /* Raw addresses, only outside the eu4.exe image (e.g. Direct3D 9 vtables;
     * inside it: ANVIL_ERROR_NOT_DECLARED). The expected bytes may be NULL. */
    AnvilResult (*hook_function_at)(AnvilPlugin *self, void *address, const void *expected_prologue,
                                    size_t prologue_size, void *detour, void **next, AnvilChange **out);
    AnvilResult (*mid_hook_at)(AnvilPlugin *self, void *address, const void *expected, size_t expected_size,
                               AnvilCpuCallback callback, void *user, AnvilChange **out);
    /* Removes a change, staged or enabled; ANVIL_ERROR_NOT_FOUND when it is
     * already gone. Does nothing (ANVIL_OK) while the process exits. */
    AnvilResult (*remove)(AnvilPlugin *self, AnvilChange *change);
    /* After load: the changes between begin_group and commit_group are
     * enabled together or not at all; abort_group drops them. Inside
     * anvil_plugin_load the whole load is a group: ANVIL_ERROR_WRONG_PHASE. */
    AnvilResult (*begin_group)(AnvilPlugin *self);
    AnvilResult (*commit_group)(AnvilPlugin *self);
    void (*abort_group)(AnvilPlugin *self);

    /* The verified address of a symbol, to call (function) or read (data);
     * NULL for an unknown symbol, or one symbols.toml declares `absent` on the
     * running game build (Anvil 1.9 and later): check it before using such a
     * symbol. Changes on an absent symbol give ANVIL_ERROR_NOT_DECLARED. */
    void *(*address_of)(AnvilPlugin *self, AnvilSymbol symbol);
    /* A value named in symbols.toml `values`, e.g. for a C plugin or for an
     * `export` symbol whose rip_relative values gen cannot know. A
     * rip_relative value is an RVA. ANVIL_ERROR_NOT_DECLARED when unknown. */
    AnvilResult (*symbol_value)(AnvilPlugin *self, AnvilSymbol symbol, const char *name, uint64_t *out);

    /* --- settings (docs/design.md §11); values are validated by Anvil ---
     * An unknown setting, or a getter that does not fit its type, reads as
     * 0, 0.0 or "". A key code is the virtual-key code in bits 0-7 with
     * 0x100 Shift, 0x200 Ctrl and 0x400 Alt (0: no binding). */
    int (*setting_bool)(AnvilPlugin *self, AnvilSetting setting);
    int64_t (*setting_int)(AnvilPlugin *self, AnvilSetting setting); /* int, bool, enum index, key code */
    double (*setting_float)(AnvilPlugin *self, AnvilSetting setting); /* float, int */
    /* UTF-8: a string, an enum option's name or a key ("Ctrl+F12"); valid until the next change */
    const char *(*setting_string)(AnvilPlugin *self, AnvilSetting setting);
    AnvilResult (*subscribe_setting)(AnvilPlugin *self, AnvilSetting setting, AnvilSettingCallback callback,
                                     void *context); /* restart = false settings only */

    /* --- services between plugins ---
     * A service is a struct of function pointers whose first field is
     * `uint32_t size`. Names are "<plugin-id>.<service>"; version is the
     * service's major version. A service lives as long as the process. */
    AnvilResult (*register_service)(AnvilPlugin *self, const char *name, uint32_t version, const void *service);
    const void *(*get_service)(AnvilPlugin *self, const char *name, uint32_t version);

    /* --- events: synchronous, on the publishing thread ---
     * A plugin publishes only events named "<plugin-id>.<name>" (lower-case
     * letters, digits, '-', '_', '.'); "anvil." names are Anvil's. It may
     * subscribe to any name. */
    AnvilResult (*subscribe)(AnvilPlugin *self, const char *event, AnvilEventCallback callback, void *context);
    AnvilResult (*publish)(AnvilPlugin *self, const char *event, const void *payload);

    /* --- end of API 2.0: fields added later go below, checked with ANVIL_HOST_HAS --- */

    /* --- modules (appended in 1.21.0, working number; ADR 0011) ---
     * The handle of a module the plugin declares in [[modules]] of plugin.toml.
     * It is an AnvilPlugin * like `self` and every host call takes it: changes
     * requested through it belong to the module, which the core suspends by
     * the module's own affects_gameplay / allow_ironman / allow_multiplayer
     * (design §13), and anvil.suspended / anvil.resumed reach the
     * subscriptions made through it. Settings, symbols, folders, services and
     * the names of events are the plugin's. The same handle for every call,
     * valid for the process. NOT_DECLARED for a name plugin.toml does not
     * declare (or a module handle as `self`), DISABLED for a module the player
     * turned off: then *out is NULL and the module's code should not run. */
    AnvilResult (*module)(AnvilPlugin *self, const char *module, AnvilPlugin **out);
} AnvilHost;

/* Nonzero when the host provides `field`. */
#define ANVIL_HOST_HAS(host, field) \
    ((host)->size >= offsetof(AnvilHost, field) + sizeof((host)->field))

/*
 * Exported by every plugin. Called once, in the phase plugin.toml names:
 *   "early": at the game's entry point, before the C runtime of eu4.exe
 *            initializes (only the main thread runs game code);
 *   "main":  on an Anvil thread, concurrently with the game's start-up.
 * Return ANVIL_OK to stay loaded; anything else (or a crash) rolls back every
 * change the plugin requested, and the plugin stays inactive.
 */
ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost *host, AnvilPlugin *self);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_PLUGIN_H */
