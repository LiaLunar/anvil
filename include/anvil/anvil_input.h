/*
 * anvil-overlay.input: keys and mouse buttons for plugins, from the game
 * window's procedure, held back while the player types in the game or uses
 * Anvil's panel. Depend on the overlay in plugin.toml ([[dependencies]] id =
 * "anvil-overlay", optional = true for a plugin that also works without it)
 * and look the service up with get_service. It is NULL with an overlay older
 * than anvil-overlay 1.18.0.
 *
 * Codes. A code is what a settings.toml setting of type "key" holds
 * (anvil::Key in C++): the Windows virtual key in the low byte, with
 * ANVIL_INPUT_SHIFT, ANVIL_INPUT_CTRL and ANVIL_INPUT_ALT above it. Mouse
 * buttons are their virtual keys (VK_LBUTTON 0x01, VK_RBUTTON 0x02, VK_MBUTTON
 * 0x04, VK_XBUTTON1 0x05, VK_XBUTTON2 0x06) with the same modifiers: Ctrl and
 * a right click is 0x202. A press matches a binding when that key goes down
 * with exactly the code's modifiers held. 0 is no binding: it never fires.
 * The modifier keys themselves (Shift, Ctrl, Alt, the Windows keys) are not
 * codes.
 *
 * Keys and settings. Declare the key as a setting of type "key", so the
 * player can choose it in the overlay's panel, bind its value, and call
 * set_code from the setting's callback. Anvil does not read the game's own
 * shortcuts (shortcuts.txt): a code that is also a shortcut of the game fires
 * both, unless the binding consumes the press.
 *
 * Two plugins, one code. The first binding of a code keeps it: bind or
 * set_code with a code that another binding holds answers
 * ANVIL_ERROR_CONFLICT, and the binding stays inert (code 0) until a call
 * gives it a free code. Plugins bind in their load, in load order, so the
 * earlier plugin wins at start. The overlay's panel key comes before every
 * binding; a code equal to it answers ANVIL_ERROR_CONFLICT too.
 *
 * Held back. Nothing is delivered, and held() answers 0, while state() has
 * any of ANVIL_INPUT_HOLD_KEYS (for keys) or ANVIL_INPUT_HOLD_MOUSE (for mouse
 * buttons). A text field of the game holds keys back, not mouse buttons. The
 * text field's state comes from anvil-game: without it, or on a build that
 * lacks its symbols, state() has ANVIL_INPUT_GAME_UNKNOWN and keys are
 * delivered as if no text field had focus.
 *
 * Threads. Every call may come from any thread. The window procedure, on the
 * game's main thread, matches a press against the bindings itself, consumes it
 * if asked, and queues the handler as run_on_main_thread work of anvil-game:
 * the handler runs on the main thread after the next outer frame, so it may
 * read anvil-game.world. Without anvil-game, bind answers
 * ANVIL_ERROR_UNSUPPORTED. A binding lives until the process ends; after
 * unbind its queued presses are not delivered, unless the handler already
 * runs. On the OpenGL renderer the overlay does not see the window: calls work
 * and nothing is delivered.
 *
 * What it does not do. It reads input and changes nothing in the game, so it
 * needs no affects_gameplay = true; what a handler then does is the plugin's
 * own affair. Not in version 1: releases, the wheel and mouse movement.
 *
 * Plain C11, like anvil_game.h. Within version 1 functions are only appended
 * to the end of the service, and events grow behind `size`.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_INPUT_H
#define ANVIL_INPUT_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_INPUT_SERVICE "anvil-overlay.input"
#define ANVIL_INPUT_SERVICE_VERSION 1

/* The modifiers of a code, as in settings of type "key". */
#define ANVIL_INPUT_SHIFT 0x100
#define ANVIL_INPUT_CTRL 0x200
#define ANVIL_INPUT_ALT 0x400

/* state() */
#define ANVIL_INPUT_OVERLAY_PANEL 0x1u    /* Anvil's panel is open, where the player chooses keys */
#define ANVIL_INPUT_OVERLAY_KEYBOARD 0x2u /* the overlay takes the keyboard (a field of its own) */
#define ANVIL_INPUT_OVERLAY_MOUSE 0x4u    /* the mouse is over a window of the overlay */
#define ANVIL_INPUT_GAME_TEXT 0x8u        /* a text field of the game has focus */
#define ANVIL_INPUT_GAME_SHORTCUT 0x10u   /* the game's shortcut editor waits for a key */
#define ANVIL_INPUT_GAME_UNKNOWN 0x20u    /* anvil-game cannot say whether the two above hold */
#define ANVIL_INPUT_HOLD_KEYS                                                                     \
    (ANVIL_INPUT_OVERLAY_PANEL | ANVIL_INPUT_OVERLAY_KEYBOARD | ANVIL_INPUT_GAME_TEXT | \
     ANVIL_INPUT_GAME_SHORTCUT)
#define ANVIL_INPUT_HOLD_MOUSE ANVIL_INPUT_OVERLAY_MOUSE

/* Flags of a binding. */
#define ANVIL_INPUT_CONSUME 0x1u /* the game does not see the press (it sees the release) */
#define ANVIL_INPUT_REPEAT 0x2u  /* also the auto-repeat of a held key */

/* Flags of an event. */
#define ANVIL_INPUT_EVENT_REPEAT 0x1u /* an auto-repeat */

typedef struct AnvilInputEvent {
    uint32_t size;  /* sizeof(AnvilInputEvent) as known by the overlay */
    uint32_t flags; /* ANVIL_INPUT_EVENT_* */
    int64_t code;   /* the binding's code at the press */
    int32_t x, y;   /* the cursor at the press, in the window's client pixels */
    uint64_t frame; /* anvil-game's frame number at the press */
} AnvilInputEvent;

typedef struct AnvilInputBinding AnvilInputBinding;

/* A press of a binding's code; see "Threads" above. `event` is valid for the
 * call. */
typedef void (*AnvilInputHandler)(void *context, const AnvilInputEvent *event);

typedef struct AnvilInputService {
    uint32_t size; /* sizeof(AnvilInputService) as known by the overlay */

    /* ANVIL_INPUT_* flags now. */
    uint32_t (*state)(void);
    /* A binding of `code` (0 for none yet) with ANVIL_INPUT_CONSUME and
     * ANVIL_INPUT_REPEAT. ANVIL_ERROR_INVALID_ARGUMENT for a NULL handler or
     * `out`, an unknown flag or a code that is not one (see "Codes");
     * ANVIL_ERROR_CONFLICT as above, with *out set to the inert binding;
     * ANVIL_ERROR_OUT_OF_MEMORY after 256 bindings in the process. */
    AnvilResult (*bind)(int64_t code, uint32_t flags, AnvilInputHandler handler, void *context,
                        AnvilInputBinding **out);
    /* Another code for the binding, such as after its setting changed; 0
     * suspends it. Errors as for bind; ANVIL_ERROR_NOT_FOUND after unbind. */
    AnvilResult (*set_code)(AnvilInputBinding *binding, int64_t code);
    /* Ends the binding. ANVIL_ERROR_NOT_FOUND when it already ended. */
    AnvilResult (*unbind)(AnvilInputBinding *binding);
    /* 1 while the code's key is down with at least the code's modifiers, as the
     * window's messages say, and state() does not hold it back; else 0. For
     * what is polled every frame, such as a camera key. */
    int (*held)(int64_t code);
} AnvilInputService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_INPUT_HAS(service, field) \
    ((service)->size >= offsetof(AnvilInputService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_INPUT_H */
