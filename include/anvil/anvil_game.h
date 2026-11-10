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

    /* Since anvil-game 1.14.0; check with ANVIL_GAME_HAS(service, out_of_sync). */

    /* The session's ANVIL_GAME_OOS_* flags as of the last frame of the main
     * loop; 0 in sync, with no session, or on a game build without these
     * symbols. Callable from any thread. */
    uint32_t (*out_of_sync)(void);

    /* Since anvil-game 1.18.0; check with ANVIL_GAME_HAS(service, keyboard). */

    /* Whether the game's GUI takes the keyboard for itself: nonzero and *flags
     * set to ANVIL_GAME_KEYBOARD_* (0 when it does not), or 0 when this build
     * declares the symbols absent. Read-only and live, not as of a frame: the
     * GUI belongs to the main thread, where the game window's procedure also
     * runs, so ask there; another thread may see a state a moment old.
     * `flags` may be NULL. anvil-overlay.input uses it to hold plugin keys
     * back while the player types. */
    int (*keyboard)(uint32_t *flags);
} AnvilGameService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_GAME_HAS(service, field) \
    ((service)->size >= offsetof(AnvilGameService, field) + sizeof((service)->field))

/* --- Since anvil-game 1.18.0: the GUI's keyboard ----------------------------
 *
 * TEXT: a text field of the game has focus (chat, a save's name, the console,
 * the province finder) and gets the keys.
 * SHORTCUT: the game's shortcut editor waits for the key of a shortcut and gets
 * every key. */
#define ANVIL_GAME_KEYBOARD_TEXT 0x1u
#define ANVIL_GAME_KEYBOARD_SHORTCUT 0x2u

/* --- Since anvil-game 1.14.0: out of sync ----------------------------------
 *
 * A multiplayer game whose machines no longer agree. Read-only.
 *
 * DETECTED: this game's checksums differed from the host's. Only a client
 * compares them, so the host never has this flag.
 * PAUSED: the game is paused because a machine went out of sync.
 *
 * ANVIL_GAME_EVENT_OOS comes after the outer frame in which the flags changed,
 * while a session runs. A session starts from 0, and its end publishes nothing:
 * ANVIL_GAME_EVENT_SESSION says that. Payload AnvilGameOosChange. */
#define ANVIL_GAME_OOS_DETECTED 0x1u
#define ANVIL_GAME_OOS_PAUSED 0x2u

#define ANVIL_GAME_EVENT_OOS "anvil-game.oos"

typedef struct AnvilGameOosChange {
    uint32_t size;     /* sizeof(AnvilGameOosChange) as known by anvil-game */
    uint32_t flags;    /* ANVIL_GAME_OOS_* now */
    uint32_t previous; /* ANVIL_GAME_OOS_* before */
    uint32_t reserved;
    uint64_t session;  /* AnvilGameSession.number */
} AnvilGameOosChange;

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

/* A war started or ended, after the outer frame of the day it happened. The
 * wars are compared once a game day, so a war that starts and ends within one
 * day is not reported. Not for the wars a session starts with, and no ENDED
 * for those still running when it ends: at a session's start ask war_ids.
 * Payload const AnvilGameWarChange*, with its name, valid for the handler. */
#define ANVIL_GAME_EVENT_WAR "anvil-game.war"

typedef enum AnvilGameWarChangeKind {
    ANVIL_GAME_WAR_STARTED = 1,
    ANVIL_GAME_WAR_ENDED = 2
} AnvilGameWarChangeKind;

typedef struct AnvilGameWarChange {
    uint32_t size;   /* sizeof(AnvilGameWarChange) as known by anvil-game */
    uint32_t change; /* AnvilGameWarChangeKind */
    uint64_t war;    /* the id of AnvilGameWar: no longer valid for ENDED */
    AnvilGameTag attacker;
    AnvilGameTag defender;
    const char *name; /* UTF-8, as the game names the war */
    uint64_t session; /* AnvilGameSession.number */
} AnvilGameWarChange;

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

typedef struct AnvilGameWar {
    uint32_t size;
    uint32_t reserved;
    uint64_t id;           /* stays the same while the war runs; the game's address, so a later war may reuse it */
    AnvilGameTag attacker; /* who started it */
    AnvilGameTag defender; /* whom it was declared on */
} AnvilGameWar;

/* AnvilGameWorldService.war_side */
#define ANVIL_GAME_WAR_ATTACKERS 0u
#define ANVIL_GAME_WAR_DEFENDERS 1u

/* --- Since anvil-game 1.15.0: units ---------------------------------------- */

/* AnvilGameUnit.kind, AnvilGameUnitRef.kind */
#define ANVIL_GAME_UNIT_ARMY 0u
#define ANVIL_GAME_UNIT_NAVY 1u

/* AnvilGameUnit.mission: what the unit does on its own, by the game's numbers. */
#define ANVIL_GAME_MISSION_UNKNOWN (-1) /* not one the service could read */
#define ANVIL_GAME_MISSION_NONE 0       /* no mission: the player moves it */
#define ANVIL_GAME_MISSION_PROTECT_TRADE 1
#define ANVIL_GAME_MISSION_PRIVATEER 2
#define ANVIL_GAME_MISSION_HUNT_PIRATES 3
#define ANVIL_GAME_MISSION_PATROL 4
#define ANVIL_GAME_MISSION_EXPLORE 5
#define ANVIL_GAME_MISSION_EVADE 6
#define ANVIL_GAME_MISSION_HUNT_FLEETS 7
#define ANVIL_GAME_MISSION_BLOCKADE 8
#define ANVIL_GAME_MISSION_INTERCEPT 9
#define ANVIL_GAME_MISSION_REPAIR 10
#define ANVIL_GAME_MISSION_CIRCUMNAVIGATE 11
#define ANVIL_GAME_MISSION_EXPLORE_LAND 12
#define ANVIL_GAME_MISSION_HUNT_REBELS 13
#define ANVIL_GAME_MISSION_SIEGE 14     /* besiege the enemy's provinces on its own */
#define ANVIL_GAME_MISSION_DRILL 15

typedef struct AnvilGameUnit {
    uint32_t size;
    uint32_t kind;        /* ANVIL_GAME_UNIT_* */
    uint64_t id;          /* the game's id: stays the same while the unit exists */
    AnvilGameTag country; /* the country it belongs to */
    int32_t location;     /* the province it is in or at; 0: none */
    int32_t mission;      /* ANVIL_GAME_MISSION_* */
    uint32_t subunits;    /* regiments or ships */
    int32_t strength;     /* the sum of the regiments' or ships' strength, x 1000: men for an army */
    uint32_t reserved;
    uint64_t leader;      /* the leader's id; 0: no leader */
} AnvilGameUnit;

/* One unit of the player's selection. It comes in arrays and has no `size`,
 * so it never grows within version 1: more about a unit comes from unit(). */
typedef struct AnvilGameUnitRef {
    uint64_t id;
    AnvilGameTag country;
    uint32_t kind; /* ANVIL_GAME_UNIT_* */
} AnvilGameUnitRef;

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
 * Modifiers are the same: a rival's modifiers in multiplayer are more than the
 * player sees, and a read changes nothing, so it cannot cause a desync.
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
    /* The ids of the running wars. ANVIL_ERROR_NOT_DECLARED for all four war
     * calls when a port declared the war symbols absent. */
    AnvilResult (*war_ids)(uint64_t *ids, uint32_t capacity, uint32_t *count);
    /* ANVIL_ERROR_NOT_FOUND for an id that is not a running war. */
    AnvilResult (*war)(uint64_t id, AnvilGameWar *out);
    AnvilResult (*war_name)(uint64_t id, char *utf8, uint32_t capacity, uint32_t *length);
    /* The countries on one side now, ANVIL_GAME_WAR_ATTACKERS or _DEFENDERS. */
    AnvilResult (*war_side)(uint64_t id, uint32_t side, AnvilGameTag *out, uint32_t capacity, uint32_t *count);
    /* Since anvil-game 1.15.0, for the three calls below. The id of a modifier
     * from its script key, as written in the game's files ("land_morale",
     * "local_tax_modifier"), case included. An id holds as long as the game
     * runs, so ask once: this call walks every definition.
     * ANVIL_ERROR_NOT_FOUND for an unknown key; ANVIL_ERROR_NOT_DECLARED for
     * all three when a port declared the modifier symbols absent. */
    AnvilResult (*modifier_id)(const char *key, int32_t *id);
    /* The value of a modifier as the game uses it, x 1000: the sum of
     * everything that adds to it, after the game's own adjustment for a DLC
     * that is off, which mostly makes it 0. A yes or no modifier is 1000 or 0.
     * ANVIL_ERROR_NOT_FOUND for an unknown tag or id. */
    AnvilResult (*country_modifier)(const char *tag, int32_t id, int32_t *value);
    /* The same for a province: only what the province has itself, not its
     * owner's values. */
    AnvilResult (*province_modifier)(int32_t province, int32_t id, int32_t *value);
    /* Since anvil-game 1.15.0, for the five calls below. The ids of a
     * country's armies and navies, in the game's order. A unit is read by its
     * country and its id: ANVIL_ERROR_NOT_FOUND when the country has no unit
     * with that id (it was disbanded, merged or destroyed). All five answer
     * ANVIL_ERROR_NOT_DECLARED when a port declared the unit symbols absent.
     * A read costs the country's list: a few hundred ns for a large one. */
    AnvilResult (*country_units)(const char *tag, uint64_t *ids, uint32_t capacity, uint32_t *count);
    AnvilResult (*unit)(const char *tag, uint64_t id, AnvilGameUnit *out);
    /* The name the game shows ("1st Army"), in UTF-8 with a NUL. */
    AnvilResult (*unit_name)(const char *tag, uint64_t id, char *utf8, uint32_t capacity, uint32_t *length);
    /* Its leader's name; ANVIL_ERROR_NOT_FOUND also for a unit without one. */
    AnvilResult (*unit_leader_name)(const char *tag, uint64_t id, char *utf8, uint32_t capacity, uint32_t *length);
    /* The armies and navies the player has selected, in the game's order;
     * provinces and other things on the map are left out. Units of other
     * countries can be in it: the game lets a player select them to look. */
    AnvilResult (*selected_units)(AnvilGameUnitRef *out, uint32_t capacity, uint32_t *count);
} AnvilGameWorldService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_GAME_WORLD_HAS(service, field) \
    ((service)->size >= offsetof(AnvilGameWorldService, field) + sizeof((service)->field))

/* --- Since anvil-game 1.17.0: game commands ---------------------------------
 *
 * A game command is an object the session queues, the host of a multiplayer
 * game sends out and every machine runs at the same place in the queue. The
 * service posts the commands a player's click on the unit panel would post,
 * built by the game's own constructors and handed to the session as the
 * buttons do, so a plugin needs no game address for them (Anvil's ADR 0004,
 * stage 3; anvil#256).
 *
 * Who. Only a plugin with affects_gameplay = true, and only while Anvil has not
 * suspended it (ironman or multiplayer without the player's opt-in). `post`
 * takes the caller's own `self`: ANVIL_ERROR_UNSUPPORTED for a plugin with
 * affects_gameplay = false or a pointer that is no plugin's, and
 * ANVIL_ERROR_SUSPENDED for a suspended one; nothing then reaches the game.
 * This keeps honest plugins from mistakes; native code can pass any `self`.
 *
 * What. Only the player's own units, given as AnvilGameUnitRef from
 * anvil-game.world, and only through the game's commands: no pointer leaves the
 * service, and nothing is written outside a command. Each unit is looked up in
 * its country's list. ANVIL_ERROR_NOT_FOUND for a unit that is gone or belongs
 * to another country (also every unit in observe mode); ANVIL_ERROR_INVALID_ARGUMENT
 * for a unit of the wrong kind, one given twice, or a bad combination. Either
 * way nothing of the call is posted.
 *
 * Intents, not toggles. The game's drill command toggles, so two in flight
 * would undo each other. DRILL with ANVIL_GAME_COMMAND_START posts only for the
 * armies that do not drill now, and without it only for those that do; the
 * others count as `skipped`.
 *
 * Thread and moment. Call only on the game's main thread, after an outer frame,
 * in a session: from a handler of an anvil-game event, from run_on_main_thread
 * work, or from an anvil-overlay.input handler. Anywhere else, and while this
 * machine takes part in a hot-join (the game would drop the command on every
 * machine), ANVIL_ERROR_WRONG_PHASE and nothing is posted; try again later. The
 * service draws nothing from the game's random numbers.
 *
 * Bounded. At most ANVIL_GAME_COMMAND_MAX_UNITS units per call and
 * ANVIL_GAME_COMMAND_MAX_PER_FRAME commands per frame for all callers together:
 * above that ANVIL_ERROR_LIMIT, and nothing of that call is posted.
 *
 * Results. `posted` is not "happened": a posted command runs at the session's
 * next turn of the queue on every machine, also while the game is paused, and
 * the game checks it again then. In multiplayer a command is only on its way to
 * the host. `refused` counts the commands the session did not take at its own
 * check (a session without a server, or an observer's game, which allows
 * posting only inside its input handling). The service frees a refused
 * command. A plugin that needs the outcome reads
 * anvil-game.world a few frames later.
 *
 * Game builds. A build whose symbols for a command type are declared absent
 * answers ANVIL_ERROR_NOT_DECLARED for that type; the others keep working.
 * NULL from get_service with an older anvil-game, or when the post function's
 * own symbols are absent.
 *
 * Version 2 (also anvil-game 1.17.0) adds ANVIL_GAME_COMMAND_EVENT_OPTION and
 * the two fields it reads; anvil-game registers the same table as version 1
 * too, so a plugin built against version 1 keeps working. Ask for version 2
 * first and fall back to version 1 for an anvil-game that has only that.
 *
 * Choosing an event option. The type does what the option's button does: it
 * finds the event window of the local player's pending event by its id and
 * calls the game's CEventWindow::OptionSelected, which posts the command and
 * closes the window. A command posted beside the window would leave it open,
 * and a click in it would run the option's effects a second time, so there is
 * no other path. Before the call the service checks the conditions the game's
 * post function checks (a session with a server; a dummy session, or one that
 * allows posting): when they fail it answers `refused` and leaves the window
 * alone, since the game would close the window and drop the command.
 *   - Only an option whose button is in the window and enabled now: that
 *     covers the option's trigger and the half second (real time, also while
 *     paused) the game keeps the buttons disabled after the window opens. The
 *     service does not wait: ANVIL_ERROR_WRONG_PHASE, try a frame later.
 *   - `count` is 0 and `units` NULL; `size` covers `option_index`.
 *   - Results: `posted` 1 when OptionSelected was called; `skipped` 1 when the
 *     window is there but already chosen (the same frame); ANVIL_ERROR_NOT_FOUND
 *     without a window for the id (chosen already, an AI's or another
 *     country's event, or the game's event windows turned off);
 *     ANVIL_ERROR_INVALID_ARGUMENT for an option without a button, or units;
 *     ANVIL_ERROR_WRONG_PHASE for a disabled button, a hot-join, or outside the
 *     main thread between frames; `refused` 1 when the post's conditions fail.
 *   - Known limit: a pending event without a window (the console's `msg`, an
 *     observer's save, a player whose country was annexed) has no choice
 *     through the service, and the game's own auto-choice does not run either.
 *
 * Within a version types and fields are only appended; check `size`. */

/* AnvilGameCommand.type */
#define ANVIL_GAME_COMMAND_DRILL 1u /* armies: start or stop drilling, an intent (ANVIL_GAME_COMMAND_START) */
#define ANVIL_GAME_COMMAND_MERGE 2u /* two or more armies, or navies, in one province: into the first */
#define ANVIL_GAME_COMMAND_SPLIT 3u /* one army or navy: in half, rounded up, as the button does */
#define ANVIL_GAME_COMMAND_EVENT_OPTION 4u /* version 2: the local player's pending event, choose an option */

/* AnvilGameCommand.flags */
#define ANVIL_GAME_COMMAND_START 0x1u /* DRILL: start; without it, stop */

#define ANVIL_GAME_COMMAND_MAX_UNITS 64u
#define ANVIL_GAME_COMMAND_MAX_PER_FRAME 256u

typedef struct AnvilGameCommand {
    uint32_t size;                 /* sizeof(AnvilGameCommand) as the caller knows it */
    uint32_t type;                 /* ANVIL_GAME_COMMAND_* */
    uint32_t flags;                /* ANVIL_GAME_COMMAND_START; other bits: ANVIL_ERROR_INVALID_ARGUMENT */
    uint32_t count;                /* units, 1 .. ANVIL_GAME_COMMAND_MAX_UNITS */
    const AnvilGameUnitRef *units; /* from anvil-game.world */
    /* Version 2, read only for EVENT_OPTION. */
    int32_t pending_event_id;      /* the pending event's id, as its event window holds it */
    int32_t option_index;          /* the option in the event file's order, from 0 */
} AnvilGameCommand;

typedef struct AnvilGameCommandResult {
    uint32_t size;    /* set by the caller: only that much is written */
    uint32_t posted;  /* commands the session took (in multiplayer: sent to the host) */
    uint32_t skipped; /* units already in the asked state: nothing posted for them */
    uint32_t refused; /* commands the session's own check refused at posting */
} AnvilGameCommandResult;

#define ANVIL_GAME_COMMANDS_SERVICE "anvil-game.commands"
#define ANVIL_GAME_COMMANDS_SERVICE_VERSION 2

typedef struct AnvilGameCommandsService {
    uint32_t size; /* sizeof(AnvilGameCommandsService) as known by anvil-game */

    /* Posts the commands a player's click would post for `command`, through
     * the session. ANVIL_OK when the call was done, whatever the counts in
     * *out say (`out` may be NULL); any other result posted nothing, but
     * ANVIL_ERROR_OUT_OF_MEMORY, after which *out tells what was posted. */
    AnvilResult (*post)(AnvilPlugin *self, const AnvilGameCommand *command, AnvilGameCommandResult *out);
} AnvilGameCommandsService;

/* Nonzero when `service` provides `field`. */
#define ANVIL_GAME_COMMANDS_HAS(service, field) \
    ((service)->size >= offsetof(AnvilGameCommandsService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_GAME_H */
