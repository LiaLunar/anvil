/*
 * anvil-overlay.panel: a window of a plugin in Anvil's overlay, for a plugin
 * with no game addresses (Anvil's ADR 0004). The plugin hands over rows of
 * text and buttons, and the overlay draws them; it never gets an ImGui context.
 * Depend on the overlay in plugin.toml ([[dependencies]] id = "anvil-overlay",
 * optional = true, for a plugin that also works without a window) and look the
 * service up with get_service. It is NULL with an older overlay.
 *
 * What it does not do. It changes nothing in the game and sends nothing to it,
 * so a panel needs no affects_gameplay = true. It needs no mod and no address,
 * and it keeps the game's checksum. The overlay runs on Direct3D 9: on the
 * OpenGL renderer the calls work and nothing is drawn.
 *
 * Text is UTF-8. Anvil translates none of it: ask language() and choose your
 * own words. Latin, Greek and Cyrillic draw from Segoe UI, Chinese, Japanese
 * and Korean from the fonts Windows ships; a Windows without those shows `?`
 * for them, and so does any other script Segoe UI lacks, such as Thai or
 * Devanagari. A row wraps at about 40 characters' width. "##" in a title or a
 * button shows as "# #", since ImGui reads it as an id.
 *
 * Threads. Every call may come from any thread, and rows are copied. A panel
 * lives until the process ends. Clicks run on the game's main thread after an
 * outer frame, as run_on_main_thread work of anvil-game, so a click handler may
 * read anvil-game.world. Without anvil-game no click is delivered.
 *
 * Plain C11, like anvil_game.h. Within version 1 functions are only appended
 * to the end of the service, and rows grow behind row_size.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_PANEL_H
#define ANVIL_PANEL_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_OVERLAY_ID "anvil-overlay"
#define ANVIL_PANEL_SERVICE "anvil-overlay.panel"
#define ANVIL_PANEL_SERVICE_VERSION 1

typedef struct AnvilPanel AnvilPanel;

typedef enum AnvilPanelRowKind {
    ANVIL_PANEL_TEXT = 1,     /* `label`, and `value` beside it when not NULL */
    ANVIL_PANEL_BUTTON = 2,   /* a button with `label`; a click gives back `id` */
    ANVIL_PANEL_SEPARATOR = 3 /* a line */
} AnvilPanelRowKind;

typedef struct AnvilPanelRow {
    uint32_t kind; /* AnvilPanelRowKind */
    uint32_t reserved;
    const char *label; /* UTF-8; NULL for a separator */
    const char *value; /* UTF-8; NULL for none */
    uint64_t id;       /* given back on a click */
} AnvilPanelRow;

/* A click on a button; see "Threads" above. */
typedef void (*AnvilPanelClick)(void *context, uint64_t id);

typedef struct AnvilPanelService {
    uint32_t size; /* sizeof(AnvilPanelService) as known by the overlay */

    /* A hidden panel with a title, which also names it in the overlay's status
     * tab, where the player can show and hide it. At most 64 panels in a
     * process: ANVIL_ERROR_OUT_OF_MEMORY after that. */
    AnvilResult (*create)(const char *title, AnvilPanel **out);
    /* Replaces the rows; at most 1024, each text at most 4096 bytes, else
     * ANVIL_ERROR_INVALID_ARGUMENT. Pass row_size = sizeof(AnvilPanelRow). */
    AnvilResult (*set_rows)(AnvilPanel *panel, const AnvilPanelRow *rows, uint32_t count, uint32_t row_size);
    /* The handler of the panel's buttons, replacing the last one. */
    AnvilResult (*on_click)(AnvilPanel *panel, AnvilPanelClick click, void *context);
    /* Shows or hides it. The player may close it at any time: ask shown(). */
    AnvilResult (*show)(AnvilPanel *panel, int shown);
    int (*shown)(const AnvilPanel *panel);
    /* The player's language: "en", "fr", "de", "es", "zh-Hans", "zh-Hant",
     * "ja" or "ko". Valid until the process ends. */
    const char *(*language)(void);
} AnvilPanelService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_PANEL_HAS(service, field) \
    ((service)->size >= offsetof(AnvilPanelService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_PANEL_H */
