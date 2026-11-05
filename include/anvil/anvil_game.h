/*
 * anvil-game: the standard plugin that knows the game's main loop
 * (docs/design.md §4, §13). It is a plugin like any other: depend on it in
 * plugin.toml ([[dependencies]] id = "anvil-game", version = "^1") and look up
 * its service, or subscribe to its events. Every event below is published on
 * the game's main thread, from anvil-game's hook of the frame function.
 *
 * Plain C11, like anvil_plugin.h. Within service version 1 fields are only
 * appended to the end of a struct; check `size` before using a later one
 * (ANVIL_GAME_HAS for the service).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ANVIL_GAME_H
#define ANVIL_GAME_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_GAME_ID "anvil-game"

/* Once, when the game has finished initializing (the main menu is up).
 * Payload NULL. A plugin that subscribes later asks the service (ready). */
#define ANVIL_GAME_EVENT_READY "anvil-game.ready"

/* After every frame of the main loop while the game is not quitting.
 * Payload const AnvilGameFrame*. Frames drawn from inside a game update or a
 * save load are not reported. Keep the handler short: it runs every frame. */
#define ANVIL_GAME_EVENT_FRAME "anvil-game.frame"

/* A game session started or ended: a save or a new game is loaded and
 * running, or the player left it (to the menu, or by loading another save,
 * which ends the old session and starts the new one). Payload
 * const AnvilGameSession*. Anvil itself listens to it to suspend the changes
 * of plugins with affects_gameplay = true in ironman and multiplayer (§13);
 * it does so before any plugin is told. Quitting the game from a session
 * publishes no ENDED: frames stop first. */
#define ANVIL_GAME_EVENT_SESSION "anvil-game.session"

/* Since anvil-game 1.8.0: the game's main menu came up or went away (the
 * player opened the nation selection or another front-end screen, or loaded
 * a game). Payload NULL; ask the service (main_menu). Published after the
 * frame that saw the change. */
#define ANVIL_GAME_EVENT_MAIN_MENU "anvil-game.main_menu"

typedef struct AnvilGameFrame {
    uint32_t size;   /* sizeof(AnvilGameFrame) as known by anvil-game */
    uint32_t reserved;
    uint64_t number; /* the game's own frame counter after this frame */
} AnvilGameFrame;

typedef enum AnvilGameSessionChange {
    ANVIL_GAME_SESSION_STARTED = 1,
    ANVIL_GAME_SESSION_ENDED = 2
} AnvilGameSessionChange;

typedef struct AnvilGameSession {
    uint32_t size;        /* sizeof(AnvilGameSession) as known by anvil-game */
    uint32_t change;      /* AnvilGameSessionChange; ENDED carries the flags the session had */
    uint32_t ironman;     /* nonzero: an ironman game */
    uint32_t multiplayer; /* nonzero: a multiplayer game */
    uint64_t number;      /* 1 for the first session since the game started, then 2, ... */
} AnvilGameSession;

/* The in-game date of a running session, as the game's top bar shows it.
 * Years have 365 days: the game has no leap days. */
typedef struct AnvilGameDate {
    uint32_t size;    /* sizeof(AnvilGameDate) as known by anvil-game */
    int32_t year;     /* e.g. 1444 */
    uint32_t month;   /* 1..12 */
    uint32_t day;     /* 1..31 */
    uint64_t session; /* AnvilGameSession.number of the session it belongs to */
} AnvilGameDate;

/* Work for the main thread; `context` is the caller's. */
typedef void (*AnvilGameWork)(void *context);

#define ANVIL_GAME_SERVICE "anvil-game.game"
#define ANVIL_GAME_SERVICE_VERSION 1

typedef struct AnvilGameService {
    uint32_t size; /* sizeof(AnvilGameService) as known by anvil-game */

    /* Nonzero once ANVIL_GAME_EVENT_READY was published. */
    int (*ready)(void);
    /* The game's main thread (the thread of the main loop); 0 before its
     * first frame. */
    uint32_t (*main_thread_id)(void);
    /* The frame number of the last ANVIL_GAME_EVENT_FRAME; 0 before it. */
    uint64_t (*frame_number)(void);
    /* Queues `work` to run on the main thread after the next frame of the
     * main loop, in the order queued, before that frame's
     * ANVIL_GAME_EVENT_FRAME. Callable from any thread, also from the main
     * thread (it then runs a frame later). Work still queued when the game
     * quits never runs, and frames stop while a save loads, so never wait for
     * it without a limit. ANVIL_ERROR_INVALID_ARGUMENT for a NULL `work`. */
    AnvilResult (*run_on_main_thread)(AnvilGameWork work, void *context);
    /* The current session: nonzero and *out filled (change =
     * ANVIL_GAME_SESSION_STARTED) while one runs, 0 otherwise. `out` may be
     * NULL. Its view is as of the last frame of the main loop. */
    int (*session)(AnvilGameSession *out);

    /* Since anvil-game 1.6.0; check with ANVIL_GAME_HAS(service, date). */

    /* The current date: nonzero and *out filled while a session runs, 0 when
     * no game is loaded (the main menu, a save loading from it). Callable
     * from any thread; `out` may be NULL. Like session(), its view is as of
     * the last frame of the main loop, and out->session is the number
     * session() reports: while a save loads from a running game no frame
     * runs, so the old session and its last date stay until the load ends. */
    int (*date)(AnvilGameDate *out);

    /* Since anvil-game 1.8.0; check with ANVIL_GAME_HAS(service, main_menu). */

    /* Nonzero while the game shows its main menu: the game is ready, no
     * session runs and the front end is in its main-menu state, as of the
     * last frame of the main loop; 0 on the nation selection and the other
     * front-end screens, in a game, and while a frame is drawn inside a game
     * update or a save load (the loading screen). Callable from any thread. */
    int (*main_menu)(void);
} AnvilGameService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_GAME_HAS(service, field) \
    ((service)->size >= offsetof(AnvilGameService, field) + sizeof((service)->field))

/* --- Since anvil-game 1.11.0: the campaign ---------------------------------
 *
 * Reads and events for a plugin with no game addresses. All of it only reads:
 * it calls no game code and changes nothing, so it needs no
 * affects_gameplay = true. What a plugin then does with the answers is its own
 * affair: frames differ between the machines of a multiplayer game, so these
 * events are local observations, not a clock for anything that must match
 * there. Anvil's ADR 0004 has the reasons. */

/* A country's tag: three characters and a NUL, "" for no country. */
typedef struct AnvilGameTag {
    char text[4];
} AnvilGameTag;

/* After the outer frame in which the date moved, while a session runs. Each is
 * published at most once per frame: `days` says how many days it covers, 1 in
 * a single-player game, perhaps more on a multiplayer client. So .month and
 * .year come after .day of the same frame, and by then the game has run its
 * monthly and yearly updates. Nothing is published for the first date of a
 * session, across a load, or for a date that goes back. Payload
 * const AnvilGameDateChange*, valid for the handler. */
#define ANVIL_GAME_EVENT_DAY "anvil-game.day"
#define ANVIL_GAME_EVENT_MONTH "anvil-game.month"
#define ANVIL_GAME_EVENT_YEAR "anvil-game.year"

typedef struct AnvilGameDateChange {
    uint32_t size; /* sizeof(AnvilGameDateChange) as known by anvil-game */
    uint32_t days;
    AnvilGameDate date;
    AnvilGameDate previous;
} AnvilGameDateChange;

/* After the outer frame in which the player's country changed within a
 * session: a formed nation, observe mode, the console's tag. Not for the first
 * country of a session. Payload const AnvilGamePlayerChange*. */
#define ANVIL_GAME_EVENT_PLAYER "anvil-game.player"

typedef struct AnvilGamePlayerChange {
    uint32_t size; /* sizeof(AnvilGamePlayerChange) as known by anvil-game */
    AnvilGameTag tag;
    AnvilGameTag previous;
    uint64_t session; /* AnvilGameSession.number */
} AnvilGamePlayerChange;

/* AnvilGameProvince.flags */
#define ANVIL_GAME_PROVINCE_LAND 0x1u      /* not a sea or a lake */
#define ANVIL_GAME_PROVINCE_WASTELAND 0x2u

/* AnvilGameCountry.flags */
#define ANVIL_GAME_COUNTRY_OBSERVER 0x1u   /* the observer's pseudo-country */

/* AnvilGameCountry.values: the fields below this game build has. */
#define ANVIL_GAME_VALUE_TREASURY 0x1u
#define ANVIL_GAME_VALUE_STABILITY 0x2u
#define ANVIL_GAME_VALUE_PRESTIGE 0x4u
#define ANVIL_GAME_VALUE_MANPOWER 0x8u
#define ANVIL_GAME_VALUE_MONTHLY_INCOME 0x10u

/* Set `size` to sizeof(the struct) before a call: only that much is written,
 * so a plugin built against an older header stays safe. Values marked x 1000
 * are as the game stores them: 4500 is 4.5. */
typedef struct AnvilGameProvince {
    uint32_t size;
    int32_t id;
    AnvilGameTag owner;
    AnvilGameTag controller;
    int32_t base_tax;        /* x 1000, without the capital's bonus */
    int32_t base_production; /* x 1000 */
    int32_t base_manpower;   /* x 1000 */
    uint32_t flags;          /* ANVIL_GAME_PROVINCE_* */
} AnvilGameProvince;

typedef struct AnvilGameCountry {
    uint32_t size;
    AnvilGameTag tag;
    int32_t capital;        /* a province ID; 0: none */
    int32_t province_count; /* > 0: the country exists on the map */
    uint32_t flags;         /* ANVIL_GAME_COUNTRY_* */
    uint32_t values;        /* ANVIL_GAME_VALUE_*; a field without its bit is 0 */
    int32_t treasury;       /* x 1000, ducats */
    int32_t stability;      /* x 1000 */
    int32_t prestige;       /* x 1000 */
    int32_t manpower;       /* x 1000 of the game's thousands: 25000 is 25 000 men */
    int32_t monthly_income; /* x 1000: the income total of the economy view, before expenses, as of the last
                             * monthly update */
} AnvilGameCountry;

#define ANVIL_GAME_WORLD_SERVICE "anvil-game.world"
#define ANVIL_GAME_WORLD_SERVICE_VERSION 1

/* The world of the running session, read straight from the game. NULL from
 * get_service with an older anvil-game, or on a game build whose symbols for
 * it are declared absent.
 *
 * Thread. Call only on the game's main thread, after an outer frame: from a
 * handler of an anvil-game event or from run_on_main_thread work. Anywhere
 * else, and outside a session, a call answers ANVIL_ERROR_WRONG_PHASE and reads
 * nothing: the game is in the middle of a day on any other thread and in any
 * frame drawn inside its update.
 *
 * Results. ANVIL_ERROR_NOT_FOUND for an unknown province ID or tag,
 * ANVIL_ERROR_INVALID_ARGUMENT for a bad argument or a tag that is not three
 * characters, ANVIL_ERROR_INTERNAL when the game's memory is not what it should
 * be, which costs that answer only.
 *
 * Buffers are the caller's. A list or a name is filled as far as it fits and
 * *count or *length gets the whole size, so a call with capacity 0 asks for it.
 *
 * Cost. A province read takes about 100 ns, and nothing is kept between calls.
 * A pass over the whole map takes about 0.6 ms: fine once a month, not for a
 * plugin that does it every day of a 35 ms game day without a reason.
 *
 * What it shows. Everything: provinces under terra incognita and any country,
 * also in multiplayer, where that is more than the player sees. Anvil cannot
 * hide what a native plugin can read for itself, so it does not try. A plugin
 * that shows such values in multiplayer does not enter Anvil's catalog.
 *
 * Within version 1 fields are only appended to the structs and functions to
 * the end of the service; check with ANVIL_GAME_WORLD_HAS. */
typedef struct AnvilGameWorldService {
    uint32_t size; /* sizeof(AnvilGameWorldService) as known by anvil-game */

    /* The country the game treats as the player's, the observed one in
     * observe mode. */
    AnvilResult (*player)(AnvilGameTag *out);
    /* Valid province IDs are 1 .. limit - 1; 0 where a call is not allowed. */
    int32_t (*province_limit)(void);
    AnvilResult (*province)(int32_t id, AnvilGameProvince *out);
    /* The name the game displays, which follows the owner's culture, in UTF-8
     * with a NUL, cut at a whole character. */
    AnvilResult (*province_name)(int32_t id, char *utf8, uint32_t capacity, uint32_t *length);
    AnvilResult (*province_cores)(int32_t id, AnvilGameTag *out, uint32_t capacity, uint32_t *count);
    /* `tag` ignores case. */
    AnvilResult (*country)(const char *tag, AnvilGameCountry *out);
    /* The IDs of the provinces it owns, in the game's order. */
    AnvilResult (*country_provinces)(const char *tag, int32_t *ids, uint32_t capacity, uint32_t *count);
} AnvilGameWorldService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_GAME_WORLD_HAS(service, field) \
    ((service)->size >= offsetof(AnvilGameWorldService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_GAME_H */
