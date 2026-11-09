/*
 * anvil-map: the standard plugin that lets plugins add map modes to the game
 * (docs/design.md §4, docs/spec/map-modes-v1.md, ADR 0007). Depend on it in
 * plugin.toml ([[dependencies]] id = "anvil-map", version = "^1") and look up
 * its service.
 *
 * A plugin mode is a button on the minimap's list of map modes, a colour for
 * each province and the mode's lines in the province's tooltip. The game's
 * list of map modes is closed, so each plugin mode borrows a number of the
 * game's own enum for the whole process, from a pool of 13: 80, 81, 82, 83,
 * then the debug modes 50, 57, 58, 59, 60, 66, 68, 71 and 84, in the order
 * modes are registered. The borrowed number belongs to the plugin mode
 * entirely: the game's debug mode under that number is not shown while the
 * game runs, not even from the console (`mapmode continent` shows the plugin
 * mode that has 50; the console names are in docs/spec/map-modes-v1.md §8).
 * The API never promises a number: a mode is known by its id, and it may get
 * another number at the next start.
 *
 * Colours come from a buffer the plugin hands over, because the game paints
 * the map and builds tooltips also in frames drawn inside a game update,
 * where anvil-game.world may not be read. The plugin computes in its own step
 * (an anvil-game event, queued work, or the events below) and calls
 * set_colors; the paint copies the last buffer.
 *
 * Text is UTF-8. anvil-map writes it to the game unchanged when Mei says the
 * game's text is UTF-8 (the service mei.text version 1), and otherwise in
 * Windows-1252, where a character it lacks becomes '?'. Text is shown as
 * written: the game's colour codes are not interpreted.
 *
 * What it shows. Like anvil-game.world, the service does not filter what the
 * player may know: a mode that shows in multiplayer more than the player
 * sees does not enter Anvil's catalog.
 *
 * Plain C11, like anvil_plugin.h. Within service version 1 functions and
 * fields are only appended; check with ANVIL_MAP_HAS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_MAP_H
#define ANVIL_MAP_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_MAP_ID "anvil-map"

#define ANVIL_MAP_MODES_SERVICE "anvil-map.modes"
#define ANVIL_MAP_MODES_SERVICE_VERSION 1

/* A plugin mode became the map mode shown (the player clicked its button, or
 * typed its number's console name), or stopped being it (the player picked
 * another mode, the session ended). Payload const AnvilMapModeChange*.
 * Published on the main thread in anvil-map's step after a frame of the main
 * loop, where anvil-game.world may be read: hidden for the old mode first,
 * then shown for the new one. A plugin that computes its colours only while
 * someone looks at them starts in `shown`; they reach the map one frame
 * later. */
#define ANVIL_MAP_EVENT_SHOWN "anvil-map.shown"
#define ANVIL_MAP_EVENT_HIDDEN "anvil-map.hidden"

/* A colour is 0x00RRGGBB. ANVIL_MAP_NO_COLOR leaves the province the colour
 * the game gives it in the political map mode, so sea, wasteland and empty
 * land look as players know them. Any other value with a nonzero top byte is
 * an error. */
#define ANVIL_MAP_RGB(r, g, b) \
    ((uint32_t)((((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu)))
#define ANVIL_MAP_NO_COLOR 0xFF000000u

/* The four tabs of the minimap's list of map modes, named by the game's map
 * modes they start with: 1 political, 2 terrain, 3 trade, 4 diplomacy. Their
 * buttons stand two by two beside the shortcuts: 1 and 4 above, 3 and 2
 * below. ANVIL_MAP_TAB_DEFAULT is tab 4. */
typedef enum AnvilMapTab {
    ANVIL_MAP_TAB_DEFAULT = 0,
    ANVIL_MAP_TAB_1 = 1,
    ANVIL_MAP_TAB_2 = 2,
    ANVIL_MAP_TAB_3 = 3,
    ANVIL_MAP_TAB_4 = 4
} AnvilMapTab;

/* A registered mode; valid until the process ends. */
typedef struct AnvilMapMode AnvilMapMode;

/* The mode's lines of the tooltip of `province` (a province ID), as UTF-8 in
 * `text`, which has `capacity` bytes (room for a terminating zero included).
 * Return the length of the text without the zero; anvil-map takes at most
 * capacity - 1 bytes and cuts them at a whole character. 0: no lines.
 * Lines are separated by '\n'.
 *
 * Called on the main thread in anvil-map's step after a frame of the main
 * loop, where anvil-game.world may be read, for the province under the cursor
 * while this mode is shown. The game shows the province's name and owner
 * above the text; the borrowed debug mode's lines are not shown. A tooltip the
 * game builds before the step has the last text this function gave for that
 * province, so the first one for a province shows the plugin's lines a frame
 * later. Keep it short: it runs once per frame while the player hovers. */
typedef size_t (*AnvilMapDescribe)(void *context, const AnvilMapMode *mode, int32_t province, char *text,
                                   size_t capacity);

typedef struct AnvilMapModeInfo {
    uint32_t size;             /* sizeof(AnvilMapModeInfo) as known by the caller */
    uint32_t tab;              /* AnvilMapTab */
    const char *id;            /* "<plugin-id>.<name>": the caller's own plugin id, '.', [a-z0-9_-]{1,32} */
    const char *label;         /* UTF-8, the button's name; 1..64 bytes */
    const char *description;   /* UTF-8, the button's tooltip; at most 512 bytes; may be NULL */
    const char *sprite;        /* a sprite (GFX_...) of the game or of the plugin's mod; NULL: a generic one */
    AnvilMapDescribe describe; /* may be NULL: the tooltip shows the province's name and owner only */
    void *context;             /* passed to describe */
} AnvilMapModeInfo;

typedef struct AnvilMapModeChange {
    uint32_t size;             /* sizeof(AnvilMapModeChange) as known by anvil-map */
    uint32_t reserved;
    const AnvilMapMode *mode;
    const char *id;            /* the mode's id */
} AnvilMapModeChange;

typedef struct AnvilMapModesService {
    uint32_t size; /* sizeof(AnvilMapModesService) as known by anvil-map */

    /* Registers a mode and lends it a number from the pool. The strings are
     * copied. Callable from any thread at any time; its button appears the
     * next time the game builds the tab's list (opening the tab, or a new
     * session). *out is the mode.
     * ANVIL_ERROR_INVALID_ARGUMENT: NULL, a small `size`, a bad id, label,
     * description or tab. ANVIL_ERROR_ALREADY_EXISTS: the id is registered.
     * ANVIL_ERROR_UNSUPPORTED: the pool is empty (a fourteenth mode), or
     * anvil-map runs without its hooks (another loader of map modes). */
    AnvilResult (*register_mode)(const AnvilMapModeInfo *info, const AnvilMapMode **out);

    /* Hands over the colours of `mode`: colors[i] for the province with ID i,
     * `count` entries; provinces from `count` on have ANVIL_MAP_NO_COLOR, and
     * so does province 0. The buffer is copied; the map shows it from the
     * next step on, while the mode is shown. Callable from any thread while a
     * session runs; at the end of a session every buffer is dropped.
     * ANVIL_ERROR_INVALID_ARGUMENT: NULL, `count` above the session's province
     * limit (anvil-game.world's province_limit), a colour with a top byte
     * other than 0 or 0xFF. ANVIL_ERROR_WRONG_PHASE: no session runs. */
    AnvilResult (*set_colors)(const AnvilMapMode *mode, const uint32_t *colors, uint32_t count);

    /* The id of the plugin mode that borrowed `number`, a value of the game's
     * map mode enum; NULL for the game's own modes and numbers no plugin mode
     * has. For tools that see the game's number, such as Tongs. Callable from
     * any thread. */
    const char *(*mode_id)(int32_t number);

    /* The plugin mode shown on the map as of the last step, or NULL. Callable
     * from any thread. */
    const AnvilMapMode *(*shown)(void);

    /* The id of `mode`; NULL for NULL. */
    const char *(*id)(const AnvilMapMode *mode);
} AnvilMapModesService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_MAP_HAS(service, field) \
    ((service)->size >= offsetof(AnvilMapModesService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_MAP_H */
