// anvil-game: the standard plugin that hooks the game's frame function and
// turns it into readiness, a frame event, main-thread work and session events
// (docs/design.md §4, §13; the C interface is include/anvil/anvil_game.h).
//
// Everything runs in the frame hook on the game's main thread, after an outer
// frame of the main loop (outer = 1) while the game is not quitting:
//   1. readiness: the byte the game sets at the end of its start-up;
//   2. the session: read from the game root, published when it changes (a
//      hook of the root's session setter counts the sessions the game puts
//      in, since a save loaded from the game menu brings the same object back);
//   3. the main menu: outside a session, whether the front end is in its
//      main-menu state, published when it changes;
//   4. the work queued with run_on_main_thread;
//   5. the date's, the player's and, on a new day, the wars' events, by comparing
//      them with the last frame;
//   6. the frame event;
//   7. a restart the service was asked for (restart.hpp, ADR 0012).
// Frames drawn inside a game update or a save load (outer = 0) and frames on
// any other thread are passed on untouched; anvil-game only counts the
// nested ones that run, so the main menu is not reported over a loading
// screen.
//
// The service anvil-game.world reads the world in those same moments: on the
// main thread, inside this step, while a session runs (world.hpp), and
// anvil-game.commands posts the unit panel's commands then (commands.hpp).
//
// In a start-up benchmark start it also quits the game when the core says so
// (ANVIL_BENCHMARK_EVENT_QUIT): SDL_QUIT pushed on the main thread, which is
// what the menu's Quit does (docs/spec/benchmark-v1.md §2, anvil#134).
#include <anvil/anvil.hpp>
#include <anvil/anvil_benchmark.hpp>
#include <anvil/anvil_game.h>

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "symbols.gen.hpp"

#include "anvil_control.h"
#include "commands.hpp"
#include "restart.hpp"
#include "session.hpp"
#include "work_queue.hpp"
#include "world.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using anvil_game::DateEvents;
using anvil_game::DateTracker;
using anvil_game::SessionChanges;
using anvil_game::SessionTracker;
using anvil_game::OosTracker;
using anvil_game::PendingKey;
using anvil_game::PendingTracker;
using anvil_game::SessionView;
using anvil_game::WarSeen;
using anvil_game::WarTracker;
using anvil_game::WorkQueue;
using anvil_game::WorldLayout;
using anvil_game::kUnsetDate;

// What the hook reads, all from verified symbols and values (symbols.toml).
struct Layout {
    std::uint32_t frame_counter = 0;  // app+: qword frame counter
    std::uint32_t quit = 0;           // app+: byte, the main loop stops
    std::uint32_t quit_request = 0;   // app+: byte, the main loop stops
    const volatile std::uint8_t* ready_flag = nullptr;
    const std::uintptr_t* root = nullptr;  // the game root object's global
    std::uint32_t date = 0;                // root+: int32 date
    std::uint32_t session = 0;             // root+: the session object
    std::uint32_t speed = 0;               // session+: the game-speed object
    std::uint32_t ironman = 0;             // root+: byte, ironman
    std::uintptr_t session_vtable = 0;     // the game session's class
    sym::Types::is_multiplayer::Pointer is_multiplayer = nullptr;
    std::uintptr_t frontend_vtable = 0;  // the front end's class (the root's session outside a game)
    std::uint32_t frontend_state = 0;    // frontend+: uint32 state, 0 = the main menu
    bool oos = false;                    // false when these symbols are absent on this build
    std::uint32_t oos_handler = 0;       // session+: the out-of-sync handler, may be null
    std::uint32_t oos_flag = 0;          // handler+: byte, a checksum mismatch
    std::uint32_t oos_paused = 0;        // speed object+: byte, paused because of that
    const std::uintptr_t* gui = nullptr;  // the GUI's global; null when these symbols are absent
    std::uint32_t gui_text_receiver = 0;  // gui+: the focused text field's receiver, may be null
    std::uint32_t gui_shortcut_on = 0;    // gui+: byte, the shortcut editor is on
    std::uint32_t gui_shortcut_listeners = 0;  // gui+: qword, the size of its set of listeners
    bool restart = false;                 // false when these symbols are absent on this build
    std::uint32_t restart_flags = 0;      // app+: word, the exit and restart flags (0x0101 sets both)
    std::uint32_t resign = 0;             // session+: byte, the in-game idler's _bResignGame
};

Layout g_layout;
anvil::HookOf<sym::frame> g_frame;
anvil::HookOf<sym::set_session> g_set_session;
// Sessions the game has put into the root so far, from any thread (the first
// load at start-up runs on another one).
std::atomic<std::uint64_t> g_installs{0};
std::atomic<std::uint32_t> g_main_thread{0};
std::atomic<bool> g_ready{false};
std::atomic<std::uint64_t> g_frame_number{0};
// The main menu as of the last frame of the main loop, and the frames drawn
// inside a game update or a save load that are running now.
std::atomic<bool> g_main_menu{false};
std::atomic<int> g_nested_frames{0};
WorkQueue g_work;
SessionTracker g_tracker;  // main thread only
DateTracker g_dates;       // main thread only
WarTracker g_wars;         // main thread only
PendingTracker g_pending;  // main thread only
OosTracker g_oos;          // main thread only
// The flags as the service reports them to any thread.
std::atomic<std::uint32_t> g_oos_flags{0};
anvil_game::RestartRequests g_restart;

// The world: false when a symbol of it is absent on this build.
WorldLayout g_world;
bool g_world_available = false;
// Inside after_frame(): the moment a world read is allowed.
std::atomic<bool> g_after_frame{false};
// The player's country of this session, to compare frame by frame. Main thread only.
AnvilGameTag g_player{};
std::uint64_t g_player_session = 0;

// The session and its date as the service reports them to any thread.
std::mutex g_session_mutex;
bool g_in_session = false;
AnvilGameSession g_session{};
AnvilGameDate g_date{};

void publish(const char* event, const void* payload) noexcept {
    const anvil::Host host;
    host.c_api().publish(host.self(), event, payload);
}

// No objects with destructors: the body runs under __try. The reads race
// with nothing (main thread), but a half-built root while a save loads may
// hold anything.
void read_session_unguarded(SessionView& view) {
    const std::uintptr_t root = *g_layout.root;
    if (root == 0) return;
    const std::uintptr_t session = *reinterpret_cast<const std::uintptr_t*>(root + g_layout.session);
    if (session == 0 || *reinterpret_cast<const std::uintptr_t*>(session) != g_layout.session_vtable) return;
    const std::uintptr_t speed = *reinterpret_cast<const std::uintptr_t*>(session + g_layout.speed);
    if (speed == 0) return;
    const std::int32_t date = *reinterpret_cast<const std::int32_t*>(root + g_layout.date);
    if (date == kUnsetDate) return;
    view.session = session;
    view.date = date;
    view.ironman = *reinterpret_cast<const std::uint8_t*>(root + g_layout.ironman) != 0;
    view.multiplayer = g_layout.is_multiplayer(reinterpret_cast<void*>(session));
    if (g_layout.oos) {
        const std::uintptr_t handler = *reinterpret_cast<const std::uintptr_t*>(session + g_layout.oos_handler);
        if (handler != 0 && *reinterpret_cast<const std::uint8_t*>(handler + g_layout.oos_flag) != 0) {
            view.oos |= ANVIL_GAME_OOS_DETECTED;
        }
        if (*reinterpret_cast<const std::uint8_t*>(speed + g_layout.oos_paused) != 0) view.oos |= ANVIL_GAME_OOS_PAUSED;
    }
    view.running = true;
}

SessionView read_session() {
    SessionView view;
    view.installs = g_installs.load(std::memory_order_acquire);
    __try {
        read_session_unguarded(view);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        view = SessionView{};
        view.readable = false;
    }
    return view;
}

// The front end's state, or -1 when the root's session is not the front end.
// No objects with destructors: the body runs under __try.
std::int64_t read_front_end_unguarded() {
    const std::uintptr_t root = *g_layout.root;
    if (root == 0) return -1;
    const std::uintptr_t frontend = *reinterpret_cast<const std::uintptr_t*>(root + g_layout.session);
    if (frontend == 0 || *reinterpret_cast<const std::uintptr_t*>(frontend) != g_layout.frontend_vtable) return -1;
    return *reinterpret_cast<const std::uint32_t*>(frontend + g_layout.frontend_state);
}

std::int64_t read_front_end() {
    __try {
        return read_front_end_unguarded();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

const char* mode_text(const AnvilGameSession& s) {
    if (s.ironman && s.multiplayer) return "ironman, multiplayer";
    if (s.ironman) return "ironman";
    if (s.multiplayer) return "multiplayer";
    return "neither ironman nor multiplayer";
}

bool same_tag(const AnvilGameTag& a, const AnvilGameTag& b) { return std::memcmp(a.text, b.text, sizeof(a.text)) == 0; }

void publish_date(const DateEvents& events) {
    if (events.day) publish(ANVIL_GAME_EVENT_DAY, &events.change);
    if (events.month) publish(ANVIL_GAME_EVENT_MONTH, &events.change);
    if (events.year) publish(ANVIL_GAME_EVENT_YEAR, &events.change);
}

void publish_player(std::uint64_t session) {
    AnvilGameTag tag{};
    if (anvil_game::read_player(g_world, tag) != ANVIL_OK) return;
    const AnvilGameTag previous = g_player;
    const bool same_session = session == g_player_session;
    g_player = tag;
    g_player_session = session;
    if (!same_session || same_tag(tag, previous)) return;
    AnvilGamePlayerChange change{};
    change.size = sizeof(change);
    change.tag = tag;
    change.previous = previous;
    change.session = session;
    publish(ANVIL_GAME_EVENT_PLAYER, &change);
}

// The running wars of this day; those already seen keep what was read when they
// began. A read that fails skips the day, so nothing looks ended.
void publish_wars(std::uint64_t session) {
    std::vector<std::uint64_t> ids;
    if (anvil_game::read_war_ids(g_world, ids) != ANVIL_OK) return;
    std::vector<WarSeen> seen;
    seen.reserve(ids.size());
    for (const std::uint64_t id : ids) {
        if (const WarSeen* known = g_wars.find(session, id)) {
            seen.push_back(*known);
            continue;
        }
        AnvilGameWar war{};
        WarSeen entry;
        if (anvil_game::read_war(g_world, id, war) != ANVIL_OK ||
            anvil_game::read_war_name(g_world, id, entry.name) != ANVIL_OK) {
            return;
        }
        entry.id = id;
        entry.attacker = war.attacker;
        entry.defender = war.defender;
        seen.push_back(std::move(entry));
    }
    const WarTracker::Changes changes = g_wars.update(session, std::move(seen));
    const auto tell = [session](const WarSeen& w, std::uint32_t change) {
        AnvilGameWarChange event{};
        event.size = sizeof(event);
        event.change = change;
        event.war = w.id;
        event.attacker = w.attacker;
        event.defender = w.defender;
        event.name = w.name.c_str();
        event.session = session;
        publish(ANVIL_GAME_EVENT_WAR, &event);
    };
    for (const WarSeen& w : changes.ended) tell(w, ANVIL_GAME_WAR_ENDED);
    for (const WarSeen& w : changes.started) tell(w, ANVIL_GAME_WAR_STARTED);
}

// The pending list of this outer frame, before anything is published in it,
// so every handler of the frame sees which entries count. A read that fails
// skips the frame.
void see_pending(bool running) {
    if (!running || g_world.pending_list == 0) {
        g_pending.forget();
        return;
    }
    std::vector<PendingKey> keys;
    if (anvil_game::read_pending_keys(g_world, keys) == ANVIL_OK) g_pending.see(g_tracker.current().number, std::move(keys));
}

// The counted entries of the player's own country, compared with the last frame.
void publish_pending(std::uint64_t session) {
    if (g_world.pending_list == 0) return;
    AnvilGameTag own{};
    const AnvilResult r = anvil_game::read_own_country(g_world, own);
    if (r != ANVIL_OK && r != ANVIL_ERROR_NOT_FOUND) return;  // observe mode: no own country, no entries
    std::vector<PendingKey> keys, mine;
    if (anvil_game::read_pending_keys(g_world, keys) != ANVIL_OK) return;
    for (const PendingKey& key : keys) {
        AnvilGamePendingEvent e{};
        if (!g_pending.counts(key)) continue;
        if (anvil_game::read_pending(g_world, key, e) != ANVIL_OK) return;
        if (own.text[0] != '\0' && same_tag(e.country, own)) mine.push_back(key);
    }
    const PendingTracker::Changes changes = g_pending.player(session, own, std::move(mine));
    const auto tell = [&](const PendingKey& key, std::uint32_t change) {
        AnvilGamePendingChange event{};
        event.size = sizeof(event);
        event.change = change;
        event.pending_id = key.id;
        event.country = own;
        event.session = session;
        publish(ANVIL_GAME_EVENT_PENDING, &event);
    };
    for (const PendingKey& key : changes.removed) tell(key, ANVIL_GAME_PENDING_REMOVED);
    for (const PendingKey& key : changes.added) tell(key, ANVIL_GAME_PENDING_ADDED);
}

void act_on_restart(std::uint8_t* app, const SessionView& view, std::int64_t front_end);

void after_frame_body(std::uint8_t* app, std::uint64_t number) {
    const anvil::Host host;
    if (!g_ready.load(std::memory_order_relaxed) && *g_layout.ready_flag == 1) {
        g_ready.store(true, std::memory_order_release);
        host.info("game ready (frame {})", number);
        publish(ANVIL_GAME_EVENT_READY, nullptr);
    }
    const SessionView view = read_session();
    const SessionChanges changes = g_tracker.update(view);
    // A view that faulted says nothing, so the pending baseline stays.
    if (g_world_available && view.readable) see_pending(view.running && g_tracker.active());
    // The date changes with the session it belongs to, under one lock; a view
    // that faulted keeps the last one, as it keeps the session.
    AnvilGameDate date{};
    if (view.running) {
        date = anvil_game::decode_date(view.date);
        date.session = g_tracker.current().number;
        if (changes.count == 0) {
            std::lock_guard lock(g_session_mutex);
            g_date = date;
        }
    }
    for (int i = 0; i < changes.count; ++i) {
        const AnvilGameSession& s = changes.items[i];
        {
            std::lock_guard lock(g_session_mutex);
            g_in_session = s.change == ANVIL_GAME_SESSION_STARTED;
            g_session = s;
            if (g_in_session) g_date = date;  // STARTED comes only from a running view
        }
        host.info("session {} {} ({})", s.number, s.change == ANVIL_GAME_SESSION_STARTED ? "started" : "ended",
                  mode_text(s));
        publish(ANVIL_GAME_EVENT_SESSION, &s);
    }
    // In a session the root's session is the game's, so the front end is not
    // read at all.
    const std::int64_t front_end = g_ready.load(std::memory_order_relaxed) && !view.running ? read_front_end() : -1;
    const bool main_menu = front_end == 0;
    if (main_menu != g_main_menu.load(std::memory_order_relaxed)) {
        g_main_menu.store(main_menu, std::memory_order_release);
        if (main_menu) {
            host.info("main menu shown");
        } else {
            host.info("main menu left ({})",
                      front_end >= 0 ? "front end state " + std::to_string(front_end) : std::string("no front end"));
        }
        publish(ANVIL_GAME_EVENT_MAIN_MENU, nullptr);
    }
    g_work.run();
    g_frame_number.store(number, std::memory_order_release);
    const bool running = view.running && g_tracker.active();
    const DateEvents dates = g_dates.update(running, g_tracker.current().number, view.date);
    publish_date(dates);
    if (running && g_world_available) {
        publish_player(g_tracker.current().number);
        // The first frame of a session has no day yet: that is the wars' baseline.
        if (dates.day || !g_wars.known(g_tracker.current().number)) publish_wars(g_tracker.current().number);
        publish_pending(g_tracker.current().number);
    }
    // A view that faulted says nothing, so the flags stay.
    if (view.readable) {
        const OosTracker::Change oos = g_oos.update(running, g_tracker.current().number, view.oos);
        g_oos_flags.store(g_oos.flags(), std::memory_order_release);
        if (oos.changed) {
            host.info("out of sync: detected {}, paused {} (session {})", (oos.event.flags & ANVIL_GAME_OOS_DETECTED) != 0,
                      (oos.event.flags & ANVIL_GAME_OOS_PAUSED) != 0, oos.event.session);
            publish(ANVIL_GAME_EVENT_OOS, &oos.event);
        }
    }
    AnvilGameFrame frame{};
    frame.size = sizeof(frame);
    frame.number = number;
    publish(ANVIL_GAME_EVENT_FRAME, &frame);
    // Last, so that the whole frame was published before the game quits.
    if (view.readable) act_on_restart(app, view, front_end);
}

void after_frame(std::uint8_t* app, std::uint64_t number) {
    struct Open {
        Open() { g_after_frame.store(true, std::memory_order_release); }
        ~Open() { g_after_frame.store(false, std::memory_order_release); }
    } open;
    after_frame_body(app, number);
}

// A save loaded from the game menu puts the same session object back into
// the root, inside one frame of the main loop: the frames alone never see the
// change, the setter does (5 calls per load; none while a game runs).
void on_set_session(void* root, void* session) noexcept {
    g_set_session.next(root, session);
    g_installs.fetch_add(1, std::memory_order_release);
}

std::uintptr_t on_frame(void* app, std::uint8_t outer) noexcept {
    if (outer == 0) {
        g_nested_frames.fetch_add(1, std::memory_order_acq_rel);
        const std::uintptr_t result = g_frame.next(app, outer);
        g_nested_frames.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }
    const std::uintptr_t result = g_frame.next(app, outer);
    // The first outer frame comes from the main loop; frames on any other
    // thread are not ours to run work on.
    const std::uint32_t thread = GetCurrentThreadId();
    std::uint32_t expected = 0;
    if (!g_main_thread.compare_exchange_strong(expected, thread) && expected != thread) return result;
    auto* bytes = static_cast<std::uint8_t*>(app);
    if (bytes[g_layout.quit] != 0 || bytes[g_layout.quit_request] != 0) return result;
    std::uint64_t number = 0;
    std::memcpy(&number, bytes + g_layout.frame_counter, sizeof(number));
    try {
        after_frame(bytes, number);
    } catch (...) {
        // Only a failed allocation can throw here; the next frame tries again.
    }
    return result;
}

// ---- the game's own restart (ADR 0012) --------------------------------------------

// The state a restart is checked against, as of this frame.
anvil_game::RestartState restart_state(const SessionView& view, std::int64_t front_end) {
    anvil_game::RestartState s;
    s.ready = g_ready.load(std::memory_order_relaxed);
    s.session = view.running && g_tracker.active();
    s.multiplayer = s.session && view.multiplayer;
    s.number = g_tracker.current().number;
    s.front_end = front_end >= 0;
    s.main_menu = front_end == 0;
    return s;
}

// No objects with destructors: the bodies run under __try.
bool write_restart_flags(std::uint8_t* app) {
    __try {
        const std::uint16_t both = 0x0101;
        std::memcpy(app + g_layout.restart_flags, &both, sizeof(both));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The in-game idler, which the session view verified by its vtable.
bool write_resign(std::uintptr_t session, std::uint8_t value) {
    __try {
        if (*reinterpret_cast<const std::uintptr_t*>(session) != g_layout.session_vtable) return false;
        *reinterpret_cast<volatile std::uint8_t*>(session + g_layout.resign) = value;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void act_on_restart(std::uint8_t* app, const SessionView& view, std::int64_t front_end) {
    if (!g_layout.restart || !g_restart.busy()) return;
    const anvil_game::RestartStep step = g_restart.step(restart_state(view, front_end), GetTickCount64());
    const anvil::Host host;
    switch (step.action) {
    case anvil_game::RestartAction::None:
        return;
    case anvil_game::RestartAction::ApplicationFlags:
        if (write_restart_flags(app)) {
            host.info("restart asked by {}: application flags set ({}); the game quits and starts again", step.caller,
                      step.why);
        } else {
            host.warn("restart asked by {}: the application could not be written; no restart", step.caller);
        }
        return;
    case anvil_game::RestartAction::Resign:
        if (write_resign(view.session, 1)) {
            host.info("restart asked by {}: {} (session {})", step.caller, step.why, g_tracker.current().number);
        } else {
            g_restart.cancel();
            host.warn("restart asked by {}: the in-game idler could not be written; no restart", step.caller);
        }
        return;
    case anvil_game::RestartAction::ClearResign:
        write_resign(view.session, 0);
        host.warn("restart asked by {}: {}; request dropped", step.caller, step.why);
        return;
    case anvil_game::RestartAction::Dropped:
        host.warn("restart asked by {}: dropped, {}", step.caller, step.why);
        return;
    }
}

// ---- quitting a benchmark start ---------------------------------------------------

constexpr std::uint32_t kSdlQuit = 0x100;  // SDL_QUIT

// An SDL_Event of SDL 2.0.4 (a 56-byte union); its first field is the type.
struct alignas(8) SdlEvent {
    unsigned char bytes[56];
};

// On the main thread, after a frame.
void push_quit(void*) {
    const anvil::Host host;
    const std::uintptr_t base = host.game().image_base;
    // SDL's own calls go through its dynamic API table; it still points at
    // the SDL_PushEvent we call, or something else has taken it over.
    // RVAs that differ between the game builds, so read when the game loads.
    std::uintptr_t slot = 0;
    std::memcpy(&slot, reinterpret_cast<const void*>(base + host.value(sym::sdl_dynapi_init, "push_event_slot")),
                sizeof(slot));
    const auto push_event = host.function(sym::sdl_push_event);
    if (slot != base + host.value(sym::sdl_dynapi_init, "push_event") ||
        slot != reinterpret_cast<std::uintptr_t>(push_event)) {
        host.warn("benchmark start: SDL's SDL_PushEvent slot points elsewhere; not quitting");
        return;
    }
    SdlEvent event{};
    std::memcpy(event.bytes, &kSdlQuit, sizeof(kSdlQuit));
    const int result = push_event(&event);
    host.info("benchmark start: SDL_QUIT pushed (frame {}, result {})", g_frame_number.load(), result);
}

// ---- the service ------------------------------------------------------------

int service_ready() { return g_ready.load(std::memory_order_acquire) ? 1 : 0; }
std::uint32_t service_main_thread_id() { return g_main_thread.load(std::memory_order_acquire); }
std::uint64_t service_frame_number() { return g_frame_number.load(std::memory_order_acquire); }

AnvilResult service_run_on_main_thread(AnvilGameWork work, void* context) {
    if (work == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    try {
        g_work.push(work, context);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
    return ANVIL_OK;
}

int service_session(AnvilGameSession* out) {
    std::lock_guard lock(g_session_mutex);
    if (out != nullptr && g_in_session) *out = g_session;
    return g_in_session ? 1 : 0;
}

int service_date(AnvilGameDate* out) {
    std::lock_guard lock(g_session_mutex);
    if (out != nullptr && g_in_session) *out = g_date;
    return g_in_session ? 1 : 0;
}

int service_main_menu() {
    return g_main_menu.load(std::memory_order_acquire) && g_nested_frames.load(std::memory_order_acquire) == 0 ? 1 : 0;
}

std::uint32_t service_out_of_sync() { return g_oos_flags.load(std::memory_order_acquire); }

// No objects with destructors: the body runs under __try.
std::uint32_t read_keyboard_unguarded() {
    const std::uintptr_t gui = *g_layout.gui;
    if (gui == 0) return 0;
    std::uint32_t flags = 0;
    if (*reinterpret_cast<const std::uintptr_t*>(gui + g_layout.gui_text_receiver) != 0) flags |= ANVIL_GAME_KEYBOARD_TEXT;
    if (*reinterpret_cast<const std::uint8_t*>(gui + g_layout.gui_shortcut_on) != 0 &&
        *reinterpret_cast<const std::uint64_t*>(gui + g_layout.gui_shortcut_listeners) != 0) {
        flags |= ANVIL_GAME_KEYBOARD_SHORTCUT;
    }
    return flags;
}

int service_keyboard(std::uint32_t* out) {
    if (g_layout.gui == nullptr) return 0;
    std::uint32_t flags = 0;
    __try {
        flags = read_keyboard_unguarded();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        flags = 0;
    }
    if (out != nullptr) *out = flags;
    return 1;
}

std::string caller_id(const AnvilPlugin* self);

AnvilResult service_restart(AnvilPlugin* self, std::uint32_t flags) {
    if (!g_layout.restart) return ANVIL_ERROR_UNSUPPORTED;
    anvil_game::RestartState last;
    last.ready = g_ready.load(std::memory_order_acquire);
    {
        std::lock_guard lock(g_session_mutex);
        last.session = g_in_session;
        last.multiplayer = g_in_session && g_session.multiplayer != 0;
    }
    const AnvilResult r = anvil_game::check_restart(last, flags);
    const anvil::Host host;
    try {
        std::string caller = caller_id(self);
        if (r == ANVIL_ERROR_WRONG_PHASE) {
            host.info("restart asked by {}: refused, {}", caller,
                      !last.ready         ? "the game is not ready"
                      : last.multiplayer ? "a multiplayer session runs"
                                         : "a campaign runs, and closing it was not asked for");
        }
        if (r != ANVIL_OK) return r;
        host.info("restart asked by {}{}", caller,
                  (flags & ANVIL_GAME_RESTART_CLOSE_CAMPAIGN) != 0 ? ", closing a campaign if one runs" : "");
        g_restart.ask(flags, std::move(caller));
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
    return ANVIL_OK;
}

constexpr AnvilGameService kService = {
    sizeof(AnvilGameService), service_ready, service_main_thread_id, service_frame_number, service_run_on_main_thread,
    service_session,          service_date,  service_main_menu,    service_out_of_sync,  service_keyboard,
    service_restart,
};

// ---- the world service ------------------------------------------------------

bool world_open() {
    return GetCurrentThreadId() == g_main_thread.load(std::memory_order_acquire) &&
           g_after_frame.load(std::memory_order_acquire) && g_tracker.active();
}

// The caller's struct takes only what its `size` covers, and keeps that size.
template <class T>
AnvilResult give(T* out, T& value) {
    const std::uint32_t size = out->size;
    value.size = size;
    std::memcpy(out, &value, std::min<std::size_t>(size, sizeof(T)));
    return ANVIL_OK;
}

AnvilResult world_player(AnvilGameTag* out) {
    if (out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return world_open() ? anvil_game::read_player(g_world, *out) : ANVIL_ERROR_WRONG_PHASE;
}

std::int32_t world_province_limit() { return world_open() ? anvil_game::read_province_limit(g_world) : 0; }

AnvilResult world_province(std::int32_t id, AnvilGameProvince* out) {
    if (out == nullptr || out->size < sizeof(out->size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    AnvilGameProvince province{};
    const AnvilResult r = anvil_game::read_province(g_world, id, province);
    return r == ANVIL_OK ? give(out, province) : r;
}

// A list's answer: as much as fits, and the whole count.
template <class T>
void give_list(const std::vector<T>& list, T* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0) std::memcpy(out, list.data(), std::min<std::size_t>(capacity, list.size()) * sizeof(T));
    if (count != nullptr) *count = static_cast<std::uint32_t>(list.size());
}

AnvilResult world_province_name(std::int32_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && utf8 == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string name;
        const AnvilResult r = anvil_game::read_province_name(g_world, id, name);
        if (r != ANVIL_OK) return r;
        if (capacity != 0) {
            const std::size_t fits = anvil_game::fit_utf8(name, capacity);
            std::memcpy(utf8, name.data(), fits);
            utf8[fits] = '\0';
        }
        if (length != nullptr) *length = static_cast<std::uint32_t>(name.size());
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_province_cores(std::int32_t id, AnvilGameTag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<AnvilGameTag> cores;
        const AnvilResult r = anvil_game::read_province_cores(g_world, id, cores);
        if (r == ANVIL_OK) give_list(cores, out, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country(const char* tag, AnvilGameCountry* out) {
    if (tag == nullptr || out == nullptr || out->size < sizeof(out->size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    AnvilGameCountry country{};
    const AnvilResult r = anvil_game::read_country(g_world, tag, country);
    return r == ANVIL_OK ? give(out, country) : r;
}

AnvilResult world_country_provinces(const char* tag, std::int32_t* ids, std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr || (capacity != 0 && ids == nullptr)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<std::int32_t> provinces;
        const AnvilResult r = anvil_game::read_country_provinces(g_world, tag, provinces);
        if (r == ANVIL_OK) give_list(provinces, ids, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_war_ids(std::uint64_t* ids, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && ids == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<std::uint64_t> list;
        const AnvilResult r = anvil_game::read_war_ids(g_world, list);
        if (r == ANVIL_OK) give_list(list, ids, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_war(std::uint64_t id, AnvilGameWar* out) {
    if (out == nullptr || out->size < sizeof(out->size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        AnvilGameWar war{};
        const AnvilResult r = anvil_game::read_war(g_world, id, war);
        return r == ANVIL_OK ? give(out, war) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_war_name(std::uint64_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && utf8 == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string name;
        const AnvilResult r = anvil_game::read_war_name(g_world, id, name);
        if (r != ANVIL_OK) return r;
        if (capacity != 0) {
            const std::size_t fits = anvil_game::fit_utf8(name, capacity);
            std::memcpy(utf8, name.data(), fits);
            utf8[fits] = '\0';
        }
        if (length != nullptr) *length = static_cast<std::uint32_t>(name.size());
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_war_side(std::uint64_t id, std::uint32_t side, AnvilGameTag* out, std::uint32_t capacity,
                           std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<AnvilGameTag> tags;
        const AnvilResult r = anvil_game::read_war_side(g_world, id, side, tags);
        if (r == ANVIL_OK) give_list(tags, out, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_modifier_id(const char* key, std::int32_t* id) {
    if (key == nullptr || key[0] == '\0' || id == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        return anvil_game::read_modifier_id(g_world, key, *id);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country_modifier(const char* tag, std::int32_t id, std::int32_t* value) {
    if (tag == nullptr || value == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    return anvil_game::read_country_modifier(g_world, tag, id, *value);
}

AnvilResult world_province_modifier(std::int32_t province, std::int32_t id, std::int32_t* value) {
    if (value == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    return anvil_game::read_province_modifier(g_world, province, id, *value);
}

// A name into the caller's buffer, cut at a whole character; *length gets its whole size.
AnvilResult give_text(const std::string& text, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0) {
        const std::size_t fits = anvil_game::fit_utf8(text, capacity);
        std::memcpy(utf8, text.data(), fits);
        utf8[fits] = '\0';
    }
    if (length != nullptr) *length = static_cast<std::uint32_t>(text.size());
    return ANVIL_OK;
}

AnvilResult world_country_units(const char* tag, std::uint64_t* ids, std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr || (capacity != 0 && ids == nullptr)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<std::uint64_t> list;
        const AnvilResult r = anvil_game::read_country_units(g_world, tag, list);
        if (r == ANVIL_OK) give_list(list, ids, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_unit(const char* tag, std::uint64_t id, AnvilGameUnit* out) {
    if (tag == nullptr || out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        AnvilGameUnit unit{};
        const AnvilResult r = anvil_game::read_unit(g_world, tag, id, unit);
        return r == ANVIL_OK ? give(out, unit) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_unit_name(const char* tag, std::uint64_t id, char* utf8, std::uint32_t capacity,
                            std::uint32_t* length) {
    if (tag == nullptr || (capacity != 0 && utf8 == nullptr)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string name;
        const AnvilResult r = anvil_game::read_unit_name(g_world, tag, id, name);
        return r == ANVIL_OK ? give_text(name, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_unit_leader_name(const char* tag, std::uint64_t id, char* utf8, std::uint32_t capacity,
                                   std::uint32_t* length) {
    if (tag == nullptr || (capacity != 0 && utf8 == nullptr)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string name;
        const AnvilResult r = anvil_game::read_unit_leader_name(g_world, tag, id, name);
        return r == ANVIL_OK ? give_text(name, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_selected_units(AnvilGameUnitRef* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<AnvilGameUnitRef> list;
        const AnvilResult r = anvil_game::read_selected_units(g_world, list);
        if (r == ANVIL_OK) give_list(list, out, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

// ---- rulers, flags and variables (1.20.0) ----

// The current session's number, for the dates these answers carry.
std::uint64_t current_session_number() {
    AnvilGameSession s{};
    return service_session(&s) != 0 ? s.number : 0;
}

bool known_monarch(std::uint32_t kind) {
    return kind == ANVIL_GAME_MONARCH_RULER || kind == ANVIL_GAME_MONARCH_HEIR || kind == ANVIL_GAME_MONARCH_CONSORT;
}

AnvilResult world_monarch(const char* tag, std::uint32_t kind, AnvilGameMonarch* out) {
    if (tag == nullptr || out == nullptr || out->size < sizeof(out->size) || !known_monarch(kind)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    AnvilGameMonarch monarch{};
    const AnvilResult r = anvil_game::read_monarch(g_world, tag, kind, monarch);
    if (r != ANVIL_OK) return r;
    monarch.birth.session = current_session_number();
    return give(out, monarch);
}

using MonarchText = AnvilResult (*)(const WorldLayout&, std::string_view, std::uint32_t, std::string&);

AnvilResult monarch_text(MonarchText read, const char* tag, std::uint32_t kind, char* utf8, std::uint32_t capacity,
                         std::uint32_t* length) {
    if (tag == nullptr || (capacity != 0 && utf8 == nullptr) || !known_monarch(kind)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string text;
        const AnvilResult r = read(g_world, tag, kind, text);
        return r == ANVIL_OK ? give_text(text, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_monarch_name(const char* tag, std::uint32_t kind, char* utf8, std::uint32_t capacity,
                               std::uint32_t* length) {
    return monarch_text(anvil_game::read_monarch_name, tag, kind, utf8, capacity, length);
}

AnvilResult world_monarch_dynasty(const char* tag, std::uint32_t kind, char* utf8, std::uint32_t capacity,
                                  std::uint32_t* length) {
    return monarch_text(anvil_game::read_monarch_dynasty, tag, kind, utf8, capacity, length);
}

AnvilResult world_flag_id(const char* name, std::int32_t* id) {
    if (name == nullptr || name[0] == '\0' || id == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        return anvil_game::read_flag_id(g_world, name, *id);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_flag_name(std::int32_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && utf8 == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string name;
        const AnvilResult r = anvil_game::read_flag_name(g_world, id, name);
        return r == ANVIL_OK ? give_text(name, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

// The flags' answer, with the dates decoded for the current session.
template <class Read>
AnvilResult give_flags(Read read, AnvilGameFlag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::FlagEntry> entries;
        const AnvilResult r = read(entries);
        if (r != ANVIL_OK) return r;
        const std::uint64_t session = current_session_number();
        std::vector<AnvilGameFlag> flags;
        flags.reserve(entries.size());
        for (const auto& e : entries) {
            AnvilGameFlag f{};
            f.id = e.id;
            f.date = anvil_game::decode_date(e.hours);
            f.date.session = session;
            flags.push_back(f);
        }
        give_list(flags, out, capacity, count);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country_flags(const char* tag, AnvilGameFlag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_flags([&](auto& e) { return anvil_game::read_country_flags(g_world, tag, e); }, out, capacity, count);
}

AnvilResult world_province_flags(std::int32_t province, AnvilGameFlag* out, std::uint32_t capacity,
                                 std::uint32_t* count) {
    return give_flags([&](auto& e) { return anvil_game::read_province_flags(g_world, province, e); }, out, capacity,
                      count);
}

AnvilResult world_monarch_flags(const char* tag, std::uint32_t kind, AnvilGameFlag* out, std::uint32_t capacity,
                                std::uint32_t* count) {
    if (tag == nullptr || !known_monarch(kind)) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_flags([&](auto& e) { return anvil_game::read_monarch_flags(g_world, tag, kind, e); }, out, capacity,
                      count);
}

AnvilResult world_global_flags(AnvilGameFlag* out, std::uint32_t capacity, std::uint32_t* count) {
    return give_flags([&](auto& e) { return anvil_game::read_global_flags(g_world, e); }, out, capacity, count);
}

// One variable by its name, from the scope's list.
template <class Read>
AnvilResult give_variable(Read read, const char* name, std::int32_t* value) {
    if (name == nullptr || name[0] == '\0' || value == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::VariableEntry> variables;
        const AnvilResult r = read(variables);
        if (r != ANVIL_OK) return r;
        for (const auto& v : variables) {
            if (v.name == name) {
                *value = v.value;
                return ANVIL_OK;
            }
        }
        return ANVIL_ERROR_NOT_FOUND;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

// The names, each with its NUL, whole names as far as they fit.
template <class Read>
AnvilResult give_variable_names(Read read, char* names, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && names == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::VariableEntry> variables;
        const AnvilResult r = read(variables);
        if (r != ANVIL_OK) return r;
        std::size_t whole = 0, written = 0;
        bool fits = true;
        for (const auto& v : variables) {
            const std::string name = anvil_game::game_text_to_utf8(v.name);
            whole += name.size() + 1;
            if (fits && written + name.size() + 1 <= capacity) {
                std::memcpy(names + written, name.c_str(), name.size() + 1);
                written += name.size() + 1;
            } else {
                fits = false;
            }
        }
        if (length != nullptr) *length = static_cast<std::uint32_t>(whole);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country_variable(const char* tag, const char* name, std::int32_t* value) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_variable([&](auto& v) { return anvil_game::read_country_variables(g_world, tag, v); }, name, value);
}

AnvilResult world_province_variable(std::int32_t province, const char* name, std::int32_t* value) {
    return give_variable([&](auto& v) { return anvil_game::read_province_variables(g_world, province, v); }, name,
                         value);
}

AnvilResult world_country_variable_names(const char* tag, char* names, std::uint32_t capacity, std::uint32_t* length) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_variable_names([&](auto& v) { return anvil_game::read_country_variables(g_world, tag, v); }, names,
                               capacity, length);
}

AnvilResult world_province_variable_names(std::int32_t province, char* names, std::uint32_t capacity,
                                          std::uint32_t* length) {
    return give_variable_names([&](auto& v) { return anvil_game::read_province_variables(g_world, province, v); },
                               names, capacity, length);
}

// ---- pending events and statistics (1.21.0) ----

// The counted entries now, in the game's order.
AnvilResult counted_pending(std::vector<PendingKey>& out) {
    std::vector<PendingKey> keys;
    const AnvilResult r = anvil_game::read_pending_keys(g_world, keys);
    if (r != ANVIL_OK) return r;
    out.clear();
    for (const PendingKey& key : keys) {
        if (g_pending.counts(key)) out.push_back(key);
    }
    return ANVIL_OK;
}

// The counted entry with `id`: NOT_FOUND when none waits.
AnvilResult counted_pending_key(std::int32_t id, PendingKey& out) {
    std::vector<PendingKey> keys;
    const AnvilResult r = counted_pending(keys);
    if (r != ANVIL_OK) return r;
    for (const PendingKey& key : keys) {
        if (key.id == id) {
            out = key;
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

AnvilResult world_pending_events(const char* tag, std::int32_t* ids, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && ids == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    if (g_world.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    try {
        AnvilGameTag wanted{};
        if (tag == nullptr) {
            if (const AnvilResult r = anvil_game::read_own_country(g_world, wanted); r != ANVIL_OK) return r;
        } else {
            AnvilGameCountry c{};
            if (const AnvilResult r = anvil_game::read_country(g_world, tag, c); r != ANVIL_OK) return r;
            wanted = c.tag;
        }
        std::vector<PendingKey> keys;
        if (const AnvilResult r = counted_pending(keys); r != ANVIL_OK) return r;
        std::vector<std::int32_t> list;
        for (const PendingKey& key : keys) {
            AnvilGamePendingEvent e{};
            if (const AnvilResult r = anvil_game::read_pending(g_world, key, e); r != ANVIL_OK) return r;
            if (same_tag(e.country, wanted)) list.push_back(key.id);
        }
        give_list(list, ids, capacity, count);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_pending_event(std::int32_t id, AnvilGamePendingEvent* out) {
    if (out == nullptr || out->size < sizeof(out->size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    if (g_world.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    try {
        PendingKey key;
        if (const AnvilResult r = counted_pending_key(id, key); r != ANVIL_OK) return r;
        AnvilGamePendingEvent e{};
        if (const AnvilResult r = anvil_game::read_pending(g_world, key, e); r != ANVIL_OK) return r;
        e.auto_select.session = current_session_number();
        return give(out, e);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult pending_text(std::int32_t id, bool title, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && utf8 == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    if (g_world.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    try {
        PendingKey key;
        if (const AnvilResult r = counted_pending_key(id, key); r != ANVIL_OK) return r;
        std::string text;
        const AnvilResult r = anvil_game::read_pending_text(g_world, key, title, text);
        return r == ANVIL_OK ? give_text(text, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_pending_event_key(std::int32_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    return pending_text(id, false, utf8, capacity, length);
}

AnvilResult world_pending_event_title(std::int32_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length) {
    return pending_text(id, true, utf8, capacity, length);
}

AnvilResult world_country_statistics(const char* tag, std::uint32_t kind, AnvilGameStatisticPoint* out,
                                     std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr || (capacity != 0 && out == nullptr) || kind > ANVIL_GAME_STATISTIC_INFLATION) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<AnvilGameStatisticPoint> points;
        const AnvilResult r = anvil_game::read_statistics(g_world, tag, kind, points);
        if (r == ANVIL_OK) give_list(points, out, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

// ---- timed modifiers and the province view (1.21.0) ----

template <class Read>
AnvilResult give_timed(Read read, AnvilGameTimedModifier* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::TimedEntry> entries;
        const AnvilResult r = read(entries);
        if (r != ANVIL_OK) return r;
        const std::uint64_t session = current_session_number();
        std::vector<AnvilGameTimedModifier> list;
        list.reserve(entries.size());
        for (const auto& e : entries) {
            AnvilGameTimedModifier m{};
            m.flags = e.flags;
            if ((e.flags & ANVIL_GAME_TIMED_NO_START) == 0) {
                m.start = anvil_game::decode_date(e.start);
                m.start.session = session;
            }
            if ((e.flags & ANVIL_GAME_TIMED_NO_END) == 0) {
                m.end = anvil_game::decode_date(e.end);
                m.end.session = session;
            }
            list.push_back(m);
        }
        give_list(list, out, capacity, count);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

// The keys, each with its NUL, whole keys as far as they fit.
template <class Read>
AnvilResult give_timed_keys(Read read, char* keys, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0 && keys == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::TimedEntry> entries;
        const AnvilResult r = read(entries);
        if (r != ANVIL_OK) return r;
        std::size_t whole = 0, written = 0;
        bool fits = true;
        for (const auto& e : entries) {
            const std::string key = anvil_game::game_text_to_utf8(e.key);
            whole += key.size() + 1;
            if (fits && written + key.size() + 1 <= capacity) {
                std::memcpy(keys + written, key.c_str(), key.size() + 1);
                written += key.size() + 1;
            } else {
                fits = false;
            }
        }
        if (length != nullptr) *length = static_cast<std::uint32_t>(whole);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country_timed_modifiers(const char* tag, AnvilGameTimedModifier* out, std::uint32_t capacity,
                                          std::uint32_t* count) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_timed([&](auto& e) { return anvil_game::read_country_timed(g_world, tag, e); }, out, capacity, count);
}

AnvilResult world_country_timed_modifier_keys(const char* tag, char* keys, std::uint32_t capacity,
                                              std::uint32_t* length) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_timed_keys([&](auto& e) { return anvil_game::read_country_timed(g_world, tag, e); }, keys, capacity,
                           length);
}

AnvilResult world_province_timed_modifiers(std::int32_t province, AnvilGameTimedModifier* out, std::uint32_t capacity,
                                           std::uint32_t* count) {
    return give_timed([&](auto& e) { return anvil_game::read_province_timed(g_world, province, e); }, out, capacity,
                      count);
}

AnvilResult world_province_timed_modifier_keys(std::int32_t province, char* keys, std::uint32_t capacity,
                                               std::uint32_t* length) {
    return give_timed_keys([&](auto& e) { return anvil_game::read_province_timed(g_world, province, e); }, keys,
                           capacity, length);
}

AnvilResult world_province_view(std::int32_t* province) {
    if (province == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    return anvil_game::read_province_view(g_world, *province);
}

// ---- diplomacy (1.22.0) ----

AnvilResult world_country_relations(const char* tag, std::uint32_t kind, AnvilGameRelation* out, std::uint32_t capacity,
                                    std::uint32_t* count) {
    if (tag == nullptr || (capacity != 0 && out == nullptr) || kind > ANVIL_GAME_RELATION_WARNING) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<anvil_game::RelationEntry> entries;
        const AnvilResult r = anvil_game::read_relations(g_world, tag, kind, entries);
        if (r != ANVIL_OK) return r;
        const std::uint64_t session = current_session_number();
        std::vector<AnvilGameRelation> list;
        list.reserve(entries.size());
        for (const auto& e : entries) {
            AnvilGameRelation relation{};
            relation.first = e.first;
            relation.second = e.second;
            if (e.has_start) {
                relation.start = anvil_game::decode_date(e.start);
                relation.start.session = session;
            }
            if (e.has_end) {
                relation.end = anvil_game::decode_date(e.end);
                relation.end.session = session;
            }
            list.push_back(relation);
        }
        give_list(list, out, capacity, count);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_subject_type(const char* overlord, const char* subject, char* utf8, std::uint32_t capacity,
                               std::uint32_t* length) {
    if (overlord == nullptr || subject == nullptr || (capacity != 0 && utf8 == nullptr)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::string key;
        const AnvilResult r = anvil_game::read_subject_type(g_world, overlord, subject, key);
        return r == ANVIL_OK ? give_text(key, utf8, capacity, length) : r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_diplomacy_status(const char* from, const char* to, AnvilGameDiplomacyStatus* out) {
    if (from == nullptr || to == nullptr || out == nullptr || out->size < sizeof(out->size)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    AnvilGameDiplomacyStatus status{};
    const AnvilResult r = anvil_game::read_diplomacy_status(g_world, from, to, status);
    return r == ANVIL_OK ? give(out, status) : r;
}

template <class Read>
AnvilResult give_tags(Read read, AnvilGameTag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (capacity != 0 && out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    try {
        std::vector<AnvilGameTag> tags;
        const AnvilResult r = read(tags);
        if (r == ANVIL_OK) give_list(tags, out, capacity, count);
        return r;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult world_country_rivals(const char* tag, AnvilGameTag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_tags([&](auto& t) { return anvil_game::read_rivals(g_world, tag, t); }, out, capacity, count);
}

AnvilResult world_country_overlord(const char* tag, AnvilGameTag* out) {
    if (tag == nullptr || out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!world_open()) return ANVIL_ERROR_WRONG_PHASE;
    return anvil_game::read_overlord(g_world, tag, *out);
}

AnvilResult world_country_subjects(const char* tag, AnvilGameTag* out, std::uint32_t capacity, std::uint32_t* count) {
    if (tag == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return give_tags([&](auto& t) { return anvil_game::read_subjects(g_world, tag, t); }, out, capacity, count);
}

constexpr AnvilGameWorldService kWorldService = {
    sizeof(AnvilGameWorldService), world_player,         world_province_limit, world_province,
    world_province_name,           world_province_cores, world_country,        world_country_provinces,
    world_war_ids,                 world_war,            world_war_name,       world_war_side,
    world_modifier_id,             world_country_modifier, world_province_modifier,
    world_country_units,           world_unit,             world_unit_name,      world_unit_leader_name,
    world_selected_units,          world_monarch,          world_monarch_name,   world_monarch_dynasty,
    world_flag_id,                 world_flag_name,        world_country_flags,  world_province_flags,
    world_monarch_flags,           world_global_flags,     world_country_variable, world_province_variable,
    world_country_variable_names,  world_province_variable_names,
    world_pending_events,          world_pending_event,    world_pending_event_key, world_pending_event_title,
    world_country_statistics,
    world_country_timed_modifiers, world_country_timed_modifier_keys,
    world_province_timed_modifiers, world_province_timed_modifier_keys,
    world_province_view,
    world_country_relations,       world_subject_type,     world_diplomacy_status,
    world_country_rivals,          world_country_overlord, world_country_subjects,
};

// A port may declare these symbols absent on a new build (ADR 0004): the game
// then runs with the session and without the world, or without a country value.
void load_world(anvil::Host& host) {
    const bool present = host.present(sym::province_count) && host.present(sym::province_is_sea) &&
                         host.present(sym::province_is_wasteland) && host.present(sym::province_base_tax) &&
                         host.present(sym::province_base_production) && host.present(sym::province_base_manpower) &&
                         host.present(sym::province_controller) && host.present(sym::province_cores) &&
                         host.present(sym::province_name) && host.present(sym::player_country) &&
                         host.present(sym::tag_buckets) && host.present(sym::country_exists) &&
                         host.present(sym::country_provinces);
    if (!present) {
        host.info("the world's symbols are absent on this build: no service {}", ANVIL_GAME_WORLD_SERVICE);
        return;
    }
    const std::uintptr_t base = host.game().image_base;
    const std::uint64_t root = host.value(sym::root_and_date, "root");
    WorldLayout l;
    l.image_begin = base;
    l.image_end = base + host.game().image_size;
    if (host.value(sym::province_count, "root") != root || host.value(sym::province_is_sea, "root") != root ||
        host.value(sym::province_is_wasteland, "root") != root ||
        host.value(sym::player_country, "countries_db") != host.value(sym::province_base_tax, "countries_db") ||
        host.value(sym::country_exists, "countries_db") != host.value(sym::province_base_tax, "countries_db")) {
        host.warn("the world's symbols disagree on the game root: no service {}", ANVIL_GAME_WORLD_SERVICE);
        return;
    }
    l.root = base + root;
    l.countries_db = base + host.value(sym::province_base_tax, "countries_db");
    l.provinces = sym::province_count.provinces;
    l.provinces_end = sym::province_count.provinces_end;
    l.pointer_shift = sym::province_count.pointer_shift;
    l.province_array = sym::province_is_sea.province_array;
    l.province_size = sym::province_is_sea.province_size;
    l.province_id = sym::province_base_tax.province_id;
    l.owner = sym::province_base_tax.owner;
    l.controller = sym::province_controller.controller;
    l.cores = sym::province_cores.cores;
    l.cores_end = sym::province_cores.cores_end;
    l.base_tax = sym::province_base_tax.base_tax;
    l.base_production = sym::province_base_production.base_production;
    l.base_manpower = sym::province_base_manpower.base_manpower;
    l.static_part = sym::province_is_sea.static_part;
    l.name = sym::province_name.name;
    l.land_flags = sym::province_is_sea.land_flags;
    l.land_bit = sym::province_is_sea.land_bit;
    l.wasteland_flags = sym::province_is_wasteland.wasteland_flags;
    l.wasteland_bit = sym::province_is_wasteland.wasteland_bit;
    l.country_index_shift = sym::province_base_tax.country_index_shift;
    l.tag_kind = sym::player_country.player_kind - sym::player_country.player;
    l.tag_valid = sym::player_country.observed_valid - sym::player_country.observed;
    l.observer_kind = sym::player_country.observer_kind;
    l.countries = sym::province_base_tax.countries;
    l.buckets = sym::tag_buckets.buckets;
    l.bucket_count = sym::tag_buckets.bucket_count;
    l.bucket_mask = sym::tag_buckets.bucket_mask;
    l.bucket_stride = 0x18;  // the lea pair of tag_buckets: lea rcx,[rax+rax*2]; lea r15,[r10+rcx*8]
    l.compared = sym::tag_buckets.compared;
    l.country_tag = sym::country_exists.kind - l.tag_kind;
    l.capital = sym::province_base_tax.capital;
    l.owned = sym::country_provinces.owned;
    l.owned_count = sym::country_provinces.owned_count;
    l.player = sym::player_country.player;
    l.observed = sym::player_country.observed;
    if (host.present(sym::country_treasury)) l.treasury = sym::country_treasury.treasury;
    if (host.present(sym::country_stability)) l.stability = sym::country_stability.stability;
    if (host.present(sym::country_prestige)) l.prestige = sym::country_prestige.prestige;
    if (host.present(sym::country_manpower)) l.manpower = sym::country_manpower.manpower;
    if (host.present(sym::country_monthly_income)) l.monthly_income = sym::country_monthly_income.monthly_income;
    if (host.present(sym::war_list) && host.present(sym::war_attacker) && host.present(sym::war_defender) &&
        host.present(sym::war_attackers) && host.present(sym::war_defenders)) {
        l.wars = sym::war_list.wars;
        l.war_count = sym::war_list.war_count;
        l.war_name = sym::war_list.name;
        l.war_attacker = sym::war_attacker.attacker;
        l.war_defender = sym::war_defender.defender;
        l.war_attackers = sym::war_attackers.begin;
        l.war_attackers_end = sym::war_attackers.end;
        l.war_defenders = sym::war_defenders.begin;
        l.war_defenders_end = sym::war_defenders.end;
    }
    if (host.present(sym::modifier_value) && host.present(sym::modifier_definitions) &&
        host.present(sym::country_modifiers) && host.present(sym::country_modifier_total) &&
        host.present(sym::province_modifier)) {
        l.modifier_value = reinterpret_cast<std::uintptr_t>(host.address(sym::modifier_value));
        l.definitions = base + host.value(sym::modifier_definitions, "definitions");
        l.definitions_end = base + host.value(sym::modifier_definitions, "definitions_end");
        l.definition_size = sym::modifier_definitions.definition_size;
        l.country_modifiers = sym::country_modifier_total.cached + sym::country_modifiers.modifiers;
        l.calculating = sym::country_modifiers.calculating;
        l.country_total = sym::country_modifier_total.total;
        l.province_modifier = sym::province_modifier.modifier;
    }
    if (host.present(sym::unit_list) && host.present(sym::unit_country) && host.present(sym::unit_leader) &&
        host.present(sym::unit_location) && host.present(sym::unit_mission) && host.present(sym::unit_name) &&
        host.present(sym::unit_id) && host.present(sym::unit_strength) && host.present(sym::unit_mission_type) &&
        host.present(sym::selection) && host.present(sym::selection_walk) && host.present(sym::leader_id) &&
        host.present(sym::leader_name)) {
        // The walk over the selection reads the same kind of list from the
        // map input as a country holds its units in: head, count, next.
        l.list_head = 0;
        l.list_count = sym::selection_walk.count - sym::selection_walk.head;
        l.node_object = 0;
        l.node_next = sym::selection_walk.next;
        l.kind_slot = sym::selection_walk.kind_slot;
        l.units = sym::unit_list.units;
        l.unit_id = sym::unit_id.offset;
        l.unit_country = sym::unit_country.country;
        l.unit_leader = sym::unit_leader.leader;
        l.unit_location = sym::unit_location.location;
        l.unit_mission = sym::unit_mission.mission;
        l.unit_name = sym::unit_name.name;
        l.subunits = sym::unit_strength.begin;
        l.subunits_end = sym::unit_strength.end;
        l.subunit_strength = sym::unit_strength.strength;
        l.mission_slot = sym::unit_mission_type.slot;
        l.mission_limit = sym::unit_mission_type.limit;
        l.leader_id = sym::leader_id.offset;
        l.leader_name = sym::leader_name.name;
        l.session = sym::session_of_root.session;
        l.map_input = static_cast<std::uint32_t>(host.value(sym::selection, "input"));
        l.selection = sym::selection.list;
        if (sym::selection_walk.head != l.selection || sym::unit_mission_type.slot == 0) {
            host.warn("the unit symbols disagree on the selection: no unit reads");
            l.units = 0;
        }
    }
    if (host.present(sym::monarch_of_type) && host.present(sym::null_monarch) && host.present(sym::null_heir) &&
        host.present(sym::null_consort) && host.present(sym::leader_id) && host.present(sym::monarch_name) &&
        host.present(sym::monarch_skills) && host.present(sym::monarch_birth) && host.present(sym::monarch_female) &&
        host.present(sym::monarch_leader) && host.present(sym::monarch_regency) && host.present(sym::monarch_dynasty) &&
        host.present(sym::dynasty_name) && host.present(sym::heir_claim)) {
        l.ruler = sym::monarch_of_type.ruler;
        l.heir = sym::monarch_of_type.heir;
        l.consort = sym::monarch_of_type.consort;
        l.null_monarch = base + host.value(sym::null_monarch, "instance");
        l.null_heir = base + host.value(sym::null_heir, "instance");
        l.null_consort = base + host.value(sym::null_consort, "instance");
        l.object_id = sym::leader_id.offset;
        l.monarch_name = sym::monarch_name.name;
        l.skills = sym::monarch_skills.skills;
        l.birth = sym::monarch_birth.birth;
        l.female = sym::monarch_female.female;
        l.monarch_leader = sym::monarch_leader.leader;
        l.leader_type = sym::monarch_leader.leader_type;
        l.regency = sym::monarch_regency.regency;
        l.queen_regent_slot = sym::monarch_regency.queen_regent_slot;
        l.dynasty = sym::monarch_dynasty.dynasty;
        l.dynasty_name = sym::dynasty_name.name;
        l.claim = sym::heir_claim.claim;
    }
    if (host.present(sym::flag_dates) && host.present(sym::flag_names) && host.present(sym::country_flags) &&
        host.present(sym::province_flags) && host.present(sym::global_flags) && host.present(sym::monarch_flags)) {
        constexpr std::uint32_t begin = sym::flag_dates.begin;
        // Each place reads a CIntegerFlags' index vector: its begin and end, 8 bytes apart.
        static_assert(sym::flag_dates.end == begin + 8 && sym::province_flags.flags_end == sym::province_flags.flags_begin + 8 &&
                      sym::province_flags.permanent_end == sym::province_flags.permanent_begin + 8 &&
                      sym::global_flags.end == sym::global_flags.begin + 8 && sym::province_flags.flags_begin >= begin &&
                      sym::province_flags.permanent_begin >= begin && sym::global_flags.begin >= begin,
                      "the flag symbols disagree on CIntegerFlags");
        if (host.value(sym::global_flags, "root") == root) {
            l.flag_names = base + host.value(sym::flag_names, "names");
            l.flags_begin = begin;
            l.flags_end = sym::flag_dates.end;
            l.flag_dates = sym::flag_dates.dates;
            l.country_flags = sym::country_flags.flags;
            l.province_flags = sym::province_flags.flags_begin - begin;
            l.permanent_flags = sym::province_flags.permanent_begin - begin;
            l.global_flags = sym::global_flags.begin - begin;
            l.monarch_flags = sym::monarch_flags.flags;
        } else {
            host.warn("the flag symbols disagree on the game root: no flag reads");
        }
    }
    if (host.present(sym::country_variables) && host.present(sym::province_variables) &&
        host.present(sym::variable_list)) {
        static_assert(sym::country_variables.countries == sym::province_base_tax.countries &&
                          sym::province_variables.province_array == sym::province_is_sea.province_array &&
                          sym::variable_list.fixed_point >= 0x20,
                      "the variable symbols disagree on the game's layout");
        l.country_variables = sym::country_variables.variables;
        l.province_variables = sym::province_variables.variables;
        l.variable_head = sym::variable_list.head;
        l.variable_value = sym::variable_list.fixed_point;
    }
    if (host.present(sym::country_timed_modifiers) && host.present(sym::province_timed_modifiers) &&
        host.present(sym::timed_modifier_key) && host.present(sym::timed_modifier_dates) &&
        host.present(sym::timed_modifier_ruler) && host.present(sym::timed_modifier_hidden) &&
        host.present(sym::timed_modifier_never)) {
        static_assert(sym::country_timed_modifiers.modifier == sym::province_timed_modifiers.modifier &&
                          sym::country_timed_modifiers.modifier == sym::timed_modifier_key.modifier &&
                          sym::province_timed_modifiers.end == sym::province_timed_modifiers.begin + 8 &&
                          sym::province_timed_modifiers.pointer_shift == 3,
                      "the timed modifier symbols disagree on the game's layout");
        l.country_timed = sym::country_timed_modifiers.list;
        l.timed_node_next = sym::country_timed_modifiers.next;
        l.province_timed = sym::province_timed_modifiers.begin;
        l.province_timed_end = sym::province_timed_modifiers.end;
        l.timed_modifier = sym::timed_modifier_key.modifier;
        l.timed_key = sym::timed_modifier_key.key;
        l.timed_start = sym::timed_modifier_dates.start;
        l.timed_end = sym::timed_modifier_dates.end;
        l.timed_ruler = sym::timed_modifier_ruler.ruler;
        l.timed_hidden = sym::timed_modifier_hidden.hidden;
        l.unset_hours = static_cast<std::int32_t>(sym::timed_modifier_dates.unset);
        l.timed_never = base + host.value(sym::timed_modifier_never, "never");
    }
    if (host.present(sym::selection_holder) && host.present(sym::selection_view) && host.present(sym::view_window) &&
        host.present(sym::province_view_province) && host.present(sym::dialog_window_part) &&
        host.present(sym::widget_shown)) {
        static_assert(sym::selection_view.view == sym::view_window.view, "the province view symbols disagree");
        l.session = sym::session_of_root.session;
        l.view_holder = static_cast<std::uint32_t>(host.value(sym::selection_holder, "holder"));
        l.view = sym::selection_view.view;
        l.view_province = sym::province_view_province.shown;
        l.view_window = sym::view_window.window;
        l.window_part = static_cast<std::uint32_t>(host.value(sym::dialog_window_part, "window_part"));
        l.widget_shown = sym::widget_shown.shown;
    }
    if (host.present(sym::pending_events) && host.present(sym::pending_entry) && host.present(sym::pending_event) &&
        host.present(sym::event_scope_country) && host.present(sym::event_options) &&
        host.present(sym::event_hidden) && host.present(sym::event_title) && host.present(sym::event_key)) {
        static_assert(sym::pending_events.entry_id == sym::pending_entry.entry_id &&
                          sym::event_scope_country.province_array == sym::province_is_sea.province_array &&
                          sym::event_scope_country.province_size == sym::province_is_sea.province_size &&
                          sym::event_scope_country.owner == sym::province_base_tax.owner &&
                          sym::event_options.options_end == sym::event_options.options + 8,
                      "the pending event symbols disagree on the game's layout");
        // The game tests the override's valid byte and, for the tag it takes,
        // byte 7: the valid byte of every tag the service reads.
        if (sym::event_scope_country.override_valid - sym::event_scope_country.override == l.tag_valid &&
            l.tag_valid == 7) {
            l.pending_list = sym::pending_events.entries;
            l.pending_count = sym::pending_events.count;
            l.pending_size = sym::pending_events.entry_size;
            l.pending_id = sym::pending_entry.entry_id;
            l.pending_event = sym::pending_event.event;
            l.pending_scope = sym::pending_entry.scope;
            l.auto_select = sym::pending_entry.auto_select;
            l.scope_province = sym::event_scope_country.province;
            l.scope_country = sym::event_scope_country.country;
            l.scope_override = sym::event_scope_country.override;
            l.scope_override_valid = sym::event_scope_country.override_valid;
            l.event_key = sym::event_key.key;
            l.event_title = sym::event_title.title;
            l.event_options = sym::event_options.options;
            l.event_options_end = sym::event_options.options_end;
            l.event_hidden = sym::event_hidden.hidden;
        } else {
            host.warn("the pending event symbols disagree on the tags' valid byte: no pending event reads");
        }
    }
    if (host.present(sym::statistics_series) && host.present(sym::statistics_item) &&
        host.present(sym::statistics_income) && host.present(sym::statistics_score) &&
        host.present(sym::statistics_nation_size) && host.present(sym::statistics_inflation)) {
        // A node is the list's two links, then the pair: the key, a std::string, and the item.
        static_assert(sym::statistics_item.item == 0x10 + 0x20 &&
                          sym::statistics_series.values_end == sym::statistics_series.values + 8 &&
                          sym::statistics_series.values > sym::statistics_item.item &&
                          sym::statistics_series.years > sym::statistics_series.values,
                      "the statistics symbols disagree on the game's layout");
        l.statistics[ANVIL_GAME_STATISTIC_INCOME] = sym::statistics_income.series;
        l.statistics[ANVIL_GAME_STATISTIC_SCORE] = sym::statistics_score.series;
        l.statistics[ANVIL_GAME_STATISTIC_NATION_SIZE] = sym::statistics_nation_size.series;
        l.statistics[ANVIL_GAME_STATISTIC_INFLATION] = sym::statistics_inflation.series;
        l.statistics_head = sym::statistics_series.head;
        l.statistics_key = sym::statistics_item.item - 0x20;
        l.statistics_values = sym::statistics_series.values;
        l.statistics_years = sym::statistics_series.years;
    }
    if (host.present(sym::opinion_cache) && host.present(sym::country_diplomacy) &&
        host.present(sym::diplomacy_statuses) && host.present(sym::relation_start) && host.present(sym::relation_end) &&
        host.present(sym::status_cached_sum) && host.present(sym::status_trust) && host.present(sym::status_favors) &&
        host.present(sym::status_truce) && host.present(sym::relations_alliance) &&
        host.present(sym::relations_guarantee) && host.present(sym::relations_subsidies) &&
        host.present(sym::relations_dependency) && host.present(sym::relations_warning) &&
        host.present(sym::relations_marriage) && host.present(sym::relations_military_access) &&
        host.present(sym::relations_giving_military_access) && host.present(sym::subject_type) &&
        host.present(sym::country_rivals) && host.present(sym::country_overlord) && host.present(sym::country_subjects)) {
        // Every getter reads a 16-byte entry (shl by 4) of a CPdxArray: its data, then its int32 count.
        static_assert(sym::relations_alliance.entry_shift == 4 && sym::relations_guarantee.entry_shift == 4 &&
                          sym::relations_subsidies.entry_shift == 4 && sym::relations_dependency.entry_shift == 4 &&
                          sym::relations_warning.entry_shift == 4 && sym::relations_marriage.entry_shift == 4 &&
                          sym::relations_military_access.entry_shift == 4 &&
                          sym::diplomacy_statuses.countries == sym::province_base_tax.countries &&
                          sym::relation_start.start + 4 == sym::relation_end.end &&
                          sym::opinion_cache.cache_end == sym::opinion_cache.cache + 8 &&
                          sym::relation_end.unset == sym::timed_modifier_dates.unset,
                      "the diplomacy symbols disagree on the game's layout");
        l.diplomacy = sym::country_diplomacy.diplomacy;
        l.relations[ANVIL_GAME_RELATION_ALLIANCE] = sym::relations_alliance.data;
        l.relation_counts[ANVIL_GAME_RELATION_ALLIANCE] = sym::relations_alliance.size;
        l.relations[ANVIL_GAME_RELATION_ROYAL_MARRIAGE] = sym::relations_marriage.data;
        l.relation_counts[ANVIL_GAME_RELATION_ROYAL_MARRIAGE] = sym::relations_marriage.size;
        l.relations[ANVIL_GAME_RELATION_GUARANTEE] = sym::relations_guarantee.data;
        l.relation_counts[ANVIL_GAME_RELATION_GUARANTEE] = sym::relations_guarantee.size;
        l.relations[ANVIL_GAME_RELATION_MILITARY_ACCESS] = sym::relations_military_access.data;
        l.relation_counts[ANVIL_GAME_RELATION_MILITARY_ACCESS] = sym::relations_military_access.size;
        l.relations[ANVIL_GAME_RELATION_GIVING_MILITARY_ACCESS] = sym::relations_giving_military_access.data;
        l.relation_counts[ANVIL_GAME_RELATION_GIVING_MILITARY_ACCESS] = sym::relations_giving_military_access.size;
        l.relations[ANVIL_GAME_RELATION_SUBSIDIES] = sym::relations_subsidies.data;
        l.relation_counts[ANVIL_GAME_RELATION_SUBSIDIES] = sym::relations_subsidies.size;
        l.relations[ANVIL_GAME_RELATION_DEPENDENCY] = sym::relations_dependency.data;
        l.relation_counts[ANVIL_GAME_RELATION_DEPENDENCY] = sym::relations_dependency.size;
        l.relations[ANVIL_GAME_RELATION_WARNING] = sym::relations_warning.data;
        l.relation_counts[ANVIL_GAME_RELATION_WARNING] = sym::relations_warning.size;
        l.relation_first = sym::diplomacy_statuses.first;
        l.relation_second = sym::diplomacy_statuses.second;
        l.relation_start = sym::relation_start.start;
        l.relation_end = sym::relation_end.end;
        l.relation_unset = static_cast<std::int32_t>(sym::relation_end.unset);
        l.subject_type = sym::subject_type.type;
        l.subject_type_key = sym::subject_type.key;
        l.statuses = sym::diplomacy_statuses.statuses;
        l.status_size = 0x48;  // the lea pair of diplomacy_statuses: lea rdx,[rax+rax*8]; lea rcx,[rax+rdx*8]
        l.status_target = sym::relations_giving_military_access.target;
        l.status_cached_sum = sym::status_cached_sum.cached_sum;
        l.status_trust = sym::status_trust.trust;
        l.status_favors = sym::status_favors.favors;
        l.status_truce = sym::status_truce.truce;
        l.truce_bit = sym::status_truce.bit;
        l.opinion_cache = sym::opinion_cache.cache;
        l.opinion_cache_end = sym::opinion_cache.cache_end;
        l.rivals = sym::country_rivals.rivals;
        l.rival_tag = sym::country_rivals.tag;
        l.rival_next = sym::country_rivals.next;
        l.overlord = sym::country_overlord.overlord;
        l.subjects = sym::country_subjects.subjects;
    }
    g_world = l;
    g_world_available = true;
    host.register_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION, kWorldService);
    host.info("service {} v{}; events {}, {}, {}, {}, {}{}{}", ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION,
              ANVIL_GAME_EVENT_DAY, ANVIL_GAME_EVENT_MONTH, ANVIL_GAME_EVENT_YEAR, ANVIL_GAME_EVENT_PLAYER,
              ANVIL_GAME_EVENT_WAR, l.pending_list != 0 ? ", " : "", l.pending_list != 0 ? ANVIL_GAME_EVENT_PENDING : "");
}

// ---- the commands service ---------------------------------------------------

// What the buttons of the unit panel use, from verified symbols (symbols.toml,
// "game commands"). A function of 0 is absent on this build.
struct CommandsLayout {
    sym::Types::command_post::Pointer post = nullptr;
    sym::Types::command_new::Pointer allocate = nullptr;
    std::uint32_t game = 0;     // the idler (the root's session): its game, CEU4Application
    std::uint32_t session = 0;  // the game: its CSession
    std::uint32_t hotjoin = 0;  // CSession: byte, a hot-join in progress
    sym::Types::drill_command::Pointer drill = nullptr;
    std::uint32_t drill_size = 0;
    std::uint8_t drill_reason = 0;
    sym::Types::split_command::Pointer split = nullptr;
    std::uint32_t split_size = 0;
    sym::Types::merge_command::Pointer merge = nullptr;
    std::uint32_t merge_size = 0;
    // The post function's conditions, for EVENT_OPTION's pre-check.
    std::uint32_t server = 0;   // CSession: pointer, the session's server
    std::uint32_t dummy = 0;    // CSession: byte, a single-player session
    std::uint32_t allowed = 0;  // CSession: byte, posting allowed now
    // EVENT_OPTION: `option_selected` 0 when absent on this build.
    sym::Types::option_selected::Pointer option_selected = nullptr;
    std::uintptr_t window_vtable = 0;  // CEventWindow's
    std::uint32_t popups = 0;          // idler: the pop-up list's head
    std::uint32_t node_next = 0;       // a pop-up node: the next one
    std::uint32_t pending = 0;         // window: int32, its pending event's id
    std::uint32_t keep_alive = 0;      // window: byte, no option chosen yet
    std::uint32_t buttons = 0;         // window: the option buttons' list head
    std::uint32_t button_next = 0;     // a button node: the next one
    std::uint32_t button_name = 0;     // button: std::string "<event id>_<option index>"
    std::uint32_t button_state = 0;    // button: int32, 3 = disabled
    // SPEED (version 3): `speed_up` 0 when absent on this build.
    sym::Types::speed_up_command::Pointer speed_up = nullptr;
    sym::Types::speed_down_command::Pointer speed_down = nullptr;
    sym::Types::speed_admin::Pointer speed_admin = nullptr;
    std::uint32_t speed_up_size = 0;
    std::uint32_t speed_down_size = 0;
    std::uint32_t speed = 0;       // root: int32, the game speed, 0 = the bar's 1
    std::uint32_t menu = 0;        // idler: the in-game menu window
    std::uint32_t menu_shown = 0;  // that window: byte, shown
    // EXPLORE and MOVE (version 3): 0 when absent on this build.
    sym::Types::explore_command::Pointer explore = nullptr;
    std::uint32_t explore_size = 0;
    std::uint8_t explore_reason = 0;
    sym::Types::move_command::Pointer move = nullptr;
    std::uint32_t move_size = 0;
    std::uint32_t move_is_valid = 0;  // the command's vtable: byte offset of IsValid
    // SIEGE_AREA and SUPPRESS_AREA (version 3): 0 when absent on this build.
    sym::Types::siege_toggle_area::Pointer siege_area = nullptr;
    sym::Types::suppress_toggle_area::Pointer suppress_area = nullptr;
    std::uint32_t map_part = 0;  // province: its map part
    std::uint32_t map_area = 0;  // map part: its CMapArea
};

CommandsLayout g_commands;
const AnvilControlService* g_control = nullptr;  // asked on first use: anvil.control may register after us
anvil_game::CommandPoster g_poster;              // main thread only

// The game's list of units as CMergeCommand's constructor reads it: head,
// tail, count; a node holds the unit, the previous and the next node.
struct GameUnitNode {
    std::uintptr_t unit;
    GameUnitNode* previous;
    GameUnitNode* next;
    std::uint64_t locked;
};
struct GameUnitList {
    GameUnitNode* head;
    GameUnitNode* tail;
    std::int32_t count;
    std::uint32_t locked;
};

// No objects with destructors: the body runs under __try.
std::uintptr_t read_command_session_unguarded() {
    const std::uintptr_t root = *g_layout.root;
    if (root == 0) return 0;
    const std::uintptr_t idler = *reinterpret_cast<const std::uintptr_t*>(root + g_layout.session);
    if (idler == 0 || *reinterpret_cast<const std::uintptr_t*>(idler) != g_layout.session_vtable) return 0;
    const std::uintptr_t game = *reinterpret_cast<const std::uintptr_t*>(idler + g_commands.game);
    if (game == 0) return 0;
    return *reinterpret_cast<const std::uintptr_t*>(game + g_commands.session);
}

// The session the buttons post to; 0 when there is none to read.
std::uintptr_t command_session() {
    __try {
        return read_command_session_unguarded();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

std::uint8_t read_hotjoin(std::uintptr_t session) {
    __try {
        return *reinterpret_cast<const volatile std::uint8_t*>(session + g_commands.hotjoin);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return 1;  // unreadable: post nothing
    }
}

// MSVC's std::string: the text in place below 16 characters, else a pointer;
// the size at +0x10 and the capacity at +0x18.
struct GameString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;
};

// The option index after the last '_' of a button's name, as CEventWindow::Select
// reads it; -1 for a name that has none. No objects with destructors: it runs under __try.
std::int32_t button_option_unguarded(std::uintptr_t button) {
    const auto* s = reinterpret_cast<const GameString*>(button + g_commands.button_name);
    if (s->size == 0 || s->size > 64 || s->size > s->capacity) return -1;
    const char* text = s->capacity >= 16 ? s->pointer : s->buffer;
    std::int64_t underscore = -1;
    for (std::uint64_t i = 0; i < s->size; ++i) {
        if (text[i] == '_') underscore = static_cast<std::int64_t>(i);
    }
    if (underscore < 0 || static_cast<std::uint64_t>(underscore) + 1 >= s->size) return -1;
    std::int32_t value = 0;
    for (std::uint64_t i = static_cast<std::uint64_t>(underscore) + 1; i < s->size; ++i) {
        if (text[i] < '0' || text[i] > '9' || value > 100000) return -1;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

constexpr int kMaxPopups = 1000;
constexpr int kMaxButtons = 256;

// The window of `pending` in the idler's pop-ups, and the button of `option`.
// Returns false when it found none. No objects with destructors: under __try.
bool find_event_window_unguarded(std::int32_t pending, std::int32_t option, anvil_game::EventWindow* out) {
    const std::uintptr_t root = *g_layout.root;
    if (root == 0) return false;
    const std::uintptr_t idler = *reinterpret_cast<const std::uintptr_t*>(root + g_layout.session);
    if (idler == 0 || *reinterpret_cast<const std::uintptr_t*>(idler) != g_layout.session_vtable) return false;
    std::uintptr_t node = *reinterpret_cast<const std::uintptr_t*>(idler + g_commands.popups);
    for (int i = 0; node != 0 && i < kMaxPopups; ++i) {
        const std::uintptr_t popup = *reinterpret_cast<const std::uintptr_t*>(node);
        node = *reinterpret_cast<const std::uintptr_t*>(node + g_commands.node_next);
        if (popup == 0 || *reinterpret_cast<const std::uintptr_t*>(popup) != g_commands.window_vtable) continue;
        if (*reinterpret_cast<const std::int32_t*>(popup + g_commands.pending) != pending) continue;
        out->object = popup;
        out->keep_alive = *reinterpret_cast<const std::uint8_t*>(popup + g_commands.keep_alive) != 0;
        std::uintptr_t b = *reinterpret_cast<const std::uintptr_t*>(popup + g_commands.buttons);
        for (int j = 0; b != 0 && j < kMaxButtons; ++j) {
            const std::uintptr_t button = *reinterpret_cast<const std::uintptr_t*>(b);
            b = *reinterpret_cast<const std::uintptr_t*>(b + g_commands.button_next);
            if (button == 0 || button_option_unguarded(button) != option) continue;
            out->has_button = true;
            out->enabled = *reinterpret_cast<const std::int32_t*>(button + g_commands.button_state) != 3;
            break;
        }
        return true;
    }
    return false;
}

// ANVIL_OK, ANVIL_ERROR_NOT_FOUND, or ANVIL_ERROR_INTERNAL when memory faulted.
AnvilResult find_event_window(std::int32_t pending, std::int32_t option, anvil_game::EventWindow& out) {
    out = anvil_game::EventWindow{};
    anvil_game::EventWindow* const where = &out;
    __try {
        return find_event_window_unguarded(pending, option, where) ? ANVIL_OK : ANVIL_ERROR_NOT_FOUND;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return ANVIL_ERROR_INTERNAL;
    }
}

// The game speed (1 .. 5), the idler and whether the in-game menu is shown;
// no objects with destructors: the body runs under __try.
bool read_speed_state(std::int32_t& speed, std::uintptr_t& idler, bool& menu_shown) {
    __try {
        const std::uintptr_t root = *g_layout.root;
        if (root == 0) return false;
        idler = *reinterpret_cast<const std::uintptr_t*>(root + g_layout.session);
        if (idler == 0 || *reinterpret_cast<const std::uintptr_t*>(idler) != g_layout.session_vtable) return false;
        speed = *reinterpret_cast<const volatile std::int32_t*>(root + g_commands.speed) + 1;
        const std::uintptr_t menu = *reinterpret_cast<const std::uintptr_t*>(idler + g_commands.menu);
        menu_shown = menu != 0 && *reinterpret_cast<const volatile std::uint8_t*>(menu + g_commands.menu_shown) != 0;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The post function's conditions: a server, and a dummy session or posting allowed.
bool post_allowed_on(std::uintptr_t session) {
    __try {
        const auto* s = reinterpret_cast<const volatile std::uint8_t*>(session);
        if (*reinterpret_cast<const volatile std::uintptr_t*>(session + g_commands.server) == 0) return false;
        return s[g_commands.dummy] != 0 || s[g_commands.allowed] != 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// anvil.control with plugin_of, or null.
const AnvilControlService* control() {
    if (g_control == nullptr) {
        const anvil::Host host;
        const auto* found =
            static_cast<const AnvilControlService*>(host.c_api().get_service(host.self(), ANVIL_CONTROL_SERVICE,
                                                                             ANVIL_CONTROL_VERSION));
        if (found == nullptr || found->size < offsetof(AnvilControlService, plugin_of) + sizeof(found->plugin_of)) {
            return nullptr;
        }
        g_control = found;
    }
    return g_control;
}

std::string caller_id(const AnvilPlugin* self) {
    AnvilControlPlugin info{};
    info.size = sizeof(info);
    if (self == nullptr || control() == nullptr || g_control->plugin_of(self, &info) == 0 || info.id == nullptr) {
        return "an unknown caller";
    }
    return info.id;
}

class GameCommands final : public anvil_game::CommandBackend {
public:
    bool caller(const AnvilPlugin* self, bool& affects_gameplay, bool& suspended) override {
        AnvilControlPlugin info{};
        info.size = sizeof(info);
        if (self == nullptr || control() == nullptr || g_control->plugin_of(self, &info) == 0) return false;
        affects_gameplay = info.affects_gameplay != 0;
        suspended = info.suspended != 0;
        return true;
    }
    bool declared(std::uint32_t type) override {
        switch (type) {
        case ANVIL_GAME_COMMAND_DRILL: return g_commands.drill != nullptr;
        case ANVIL_GAME_COMMAND_SPLIT: return g_commands.split != nullptr;
        case ANVIL_GAME_COMMAND_MERGE: return g_commands.merge != nullptr;
        case ANVIL_GAME_COMMAND_EVENT_OPTION: return g_commands.option_selected != nullptr;
        case ANVIL_GAME_COMMAND_SPEED: return g_commands.speed_up != nullptr;
        case ANVIL_GAME_COMMAND_EXPLORE: return g_commands.explore != nullptr;
        case ANVIL_GAME_COMMAND_MOVE: return g_commands.move != nullptr;
        case ANVIL_GAME_COMMAND_SIEGE_AREA: return g_commands.siege_area != nullptr;
        case ANVIL_GAME_COMMAND_SUPPRESS_AREA: return g_commands.suppress_area != nullptr;
        }
        return false;
    }
    bool open() override {
        session_ = world_open() ? command_session() : 0;
        return session_ != 0;
    }
    bool hot_join() override { return read_hotjoin(session_) != 0; }
    std::uint64_t frame() override { return g_frame_number.load(std::memory_order_acquire); }
    AnvilResult find(const AnvilGameUnitRef& ref, anvil_game::CommandUnit& out) override {
        AnvilGameTag own{};
        if (const AnvilResult r = anvil_game::read_own_country(g_world, own); r != ANVIL_OK) return r;
        // A tag ignores case, as the world's calls take it.
        for (int i = 0; i < 3; ++i) {
            const char a = ref.country.text[i], b = own.text[i];
            if ((a >= 'a' && a <= 'z' ? a - 32 : a) != b) return ANVIL_ERROR_NOT_FOUND;
        }
        AnvilGameUnit unit{};
        std::uintptr_t object = 0;
        if (const AnvilResult r = anvil_game::read_unit_object(g_world, own.text, ref.id, object, unit); r != ANVIL_OK) {
            return r;
        }
        out.object = object;
        out.id = ref.id;
        out.kind = unit.kind;
        out.location = unit.location;
        out.drilling = unit.mission == ANVIL_GAME_MISSION_DRILL;
        out.subunits = unit.subunits;
        out.mission = unit.mission;
        return ANVIL_OK;
    }
    void* make_drill(const anvil_game::CommandUnit& unit) override {
        void* command = g_commands.allocate(g_commands.drill_size);
        if (command == nullptr) return nullptr;
        return g_commands.drill(command, reinterpret_cast<const void*>(unit.object), g_commands.drill_reason);
    }
    void* make_split(const anvil_game::CommandUnit& unit, std::int32_t count) override {
        void* command = g_commands.allocate(g_commands.split_size);
        if (command == nullptr) return nullptr;
        return g_commands.split(command, reinterpret_cast<const void*>(unit.object), count);
    }
    void* make_merge(const std::vector<anvil_game::CommandUnit>& units) override {
        // The constructor copies the list into nodes of its own.
        std::vector<GameUnitNode> nodes(units.size());
        for (std::size_t i = 0; i < units.size(); ++i) {
            nodes[i] = {units[i].object, i > 0 ? &nodes[i - 1] : nullptr, i + 1 < units.size() ? &nodes[i + 1] : nullptr, 0};
        }
        GameUnitList list{nodes.data(), &nodes.back(), static_cast<std::int32_t>(nodes.size()), 0};
        void* command = g_commands.allocate(g_commands.merge_size);
        if (command == nullptr) return nullptr;
        return g_commands.merge(command, &list);
    }
    bool submit(void* command) override {
        return g_commands.post(reinterpret_cast<void*>(session_), command);
    }
    void destroy(void* command) override {
        // Slot 0 of a command's vtable is its deleting destructor: flag 1 frees it.
        using DeletingDestructor = void* (*)(void*, unsigned);
        (*static_cast<DeletingDestructor* const*>(command))[0](command, 1);
    }
    AnvilResult find_event_window(std::int32_t pending, std::int32_t option, anvil_game::EventWindow& out) override {
        return ::find_event_window(pending, option, out);
    }
    bool post_allowed() override { return post_allowed_on(session_); }
    void choose_option(const anvil_game::EventWindow& window, std::int32_t option) override {
        g_commands.option_selected(reinterpret_cast<void*>(window.object), option);
    }
    bool game_speed(std::int32_t& speed, bool& buttons_act) override {
        std::uintptr_t idler = 0;
        bool menu_shown = true;
        if (!read_speed_state(speed, idler, menu_shown)) return false;
        // The buttons' own order: the menu first, then the admin rights.
        buttons_act = !menu_shown && g_commands.speed_admin(reinterpret_cast<void*>(idler));
        return true;
    }
    void* make_explore(const anvil_game::CommandUnit& unit, std::int32_t mission, std::int32_t target) override {
        void* command = g_commands.allocate(g_commands.explore_size);
        if (command == nullptr) return nullptr;
        return g_commands.explore(command, reinterpret_cast<const void*>(unit.object), g_commands.explore_reason, target,
                                  mission);
    }
    void* make_move(const anvil_game::CommandUnit& unit, std::int32_t province, bool clear_previous) override {
        void* command = g_commands.allocate(g_commands.move_size);
        if (command == nullptr) return nullptr;
        // A right-click's plain move: no safe move, the automatic transport cancelled.
        return g_commands.move(command, reinterpret_cast<const void*>(unit.object), province, clear_previous, false,
                               true, false);
    }
    AnvilResult area_of(std::int32_t province, std::uintptr_t& area) override {
        return anvil_game::read_province_area(g_world, province, g_commands.map_part, g_commands.map_area, area);
    }
    bool toggle_area(std::uint32_t type, const anvil_game::CommandUnit& unit, std::uintptr_t area) override {
        const auto* u = reinterpret_cast<const void*>(unit.object);
        const auto* a = reinterpret_cast<const void*>(area);
        return type == ANVIL_GAME_COMMAND_SIEGE_AREA ? g_commands.siege_area(u, a)
                                                     : g_commands.suppress_area(u, a, nullptr, true);
    }
    bool valid(void* command) override {
        using IsValid = bool (*)(void*);
        const auto* vtable = *static_cast<const IsValid* const*>(command);
        return vtable[g_commands.move_is_valid / sizeof(void*)](command);
    }
    void* make_speed_step(bool up) override {
        void* command = g_commands.allocate(up ? g_commands.speed_up_size : g_commands.speed_down_size);
        if (command == nullptr) return nullptr;
        return up ? g_commands.speed_up(command) : g_commands.speed_down(command);
    }

private:
    std::uintptr_t session_ = 0;  // as found by open() in this call
};

AnvilResult commands_post(AnvilPlugin* self, const AnvilGameCommand* command, AnvilGameCommandResult* out) {
    GameCommands backend;
    try {
        return g_poster.post(backend, self, command, out);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

constexpr AnvilGameCommandsService kCommandsService = {sizeof(AnvilGameCommandsService), commands_post};

// The post function, the allocator, the session's path and the hot-join flag
// are the service; each command type has its own symbols, which a port may
// declare absent on a new build.
void load_commands(anvil::Host& host) {
    if (!g_world_available || g_world.units == 0 || !host.present(sym::command_post) ||
        !host.present(sym::command_new) || !host.present(sym::hotjoin_flag) || !host.present(sym::drill_button)) {
        host.info("the commands' symbols are absent on this build: no service {}", ANVIL_GAME_COMMANDS_SERVICE);
        return;
    }
    CommandsLayout c;
    c.post = host.function(sym::command_post);
    c.allocate = host.function(sym::command_new);
    c.game = static_cast<std::uint32_t>(host.value(sym::drill_button, "game"));
    c.session = static_cast<std::uint32_t>(host.value(sym::drill_button, "session"));
    c.hotjoin = static_cast<std::uint32_t>(host.value(sym::hotjoin_flag, "hotjoin"));
    if (host.present(sym::drill_command) && host.present(sym::drill_reason)) {
        c.drill = host.function(sym::drill_command);
        c.drill_size = static_cast<std::uint32_t>(host.value(sym::drill_button, "size"));
        c.drill_reason = static_cast<std::uint8_t>(host.value(sym::drill_reason, "reason"));
    }
    if (host.present(sym::split_command) && host.present(sym::split_button)) {
        // The button counts the same regiments the world reads.
        if (host.value(sym::split_button, "subunits") == g_world.subunits &&
            host.value(sym::split_button, "subunits_end") == g_world.subunits_end) {
            c.split = host.function(sym::split_command);
            c.split_size = static_cast<std::uint32_t>(host.value(sym::split_button, "size"));
        } else {
            host.warn("the split button reads other regiments than anvil-game.world: no SPLIT");
        }
    }
    if (host.present(sym::merge_command) && host.present(sym::merge_button)) {
        c.merge = host.function(sym::merge_command);
        c.merge_size = static_cast<std::uint32_t>(host.value(sym::merge_button, "size"));
    }
    c.server = static_cast<std::uint32_t>(host.value(sym::command_post, "server"));
    c.dummy = static_cast<std::uint32_t>(host.value(sym::command_post, "dummy"));
    c.allowed = static_cast<std::uint32_t>(host.value(sym::command_post, "allowed"));
    if (host.present(sym::option_selected) && host.present(sym::option_window) && host.present(sym::option_click) &&
        host.present(sym::button_state) && host.present(sym::event_window) && host.present(sym::popup_list_tail) &&
        host.present(sym::popup_list_append)) {
        // OptionSelected takes the session from the window's idler, as the buttons
        // of the unit panel do: the same path as the other types.
        c.option_selected = host.function(sym::option_selected);
        c.window_vtable = host.game().image_base + host.value(sym::event_window, "vtable");
        c.popups = static_cast<std::uint32_t>(host.value(sym::popup_list_tail, "tail")) - 8;  // head, tail
        c.node_next = static_cast<std::uint32_t>(host.value(sym::popup_list_append, "node_next"));
        c.pending = static_cast<std::uint32_t>(host.value(sym::option_window, "pending"));
        c.keep_alive = static_cast<std::uint32_t>(host.value(sym::option_click, "keep_alive"));
        c.buttons = static_cast<std::uint32_t>(host.value(sym::option_click, "buttons"));
        c.button_next = static_cast<std::uint32_t>(host.value(sym::option_click, "next"));
        c.button_name = static_cast<std::uint32_t>(host.value(sym::option_click, "name"));
        c.button_state = static_cast<std::uint32_t>(host.value(sym::button_state, "state"));
    }
    if (host.present(sym::speed_up_command) && host.present(sym::speed_down_command) &&
        host.present(sym::speed_value) && host.present(sym::speed_admin) && host.present(sym::speed_button) &&
        host.present(sym::speed_down_button)) {
        // The buttons post through the same path as the unit panel's.
        if (host.value(sym::speed_button, "game") == c.game) {
            c.speed_up = host.function(sym::speed_up_command);
            c.speed_down = host.function(sym::speed_down_command);
            c.speed_admin = host.function(sym::speed_admin);
            c.speed_up_size = static_cast<std::uint32_t>(host.value(sym::speed_button, "size"));
            c.speed_down_size = static_cast<std::uint32_t>(host.value(sym::speed_down_button, "size"));
            c.speed = static_cast<std::uint32_t>(host.value(sym::speed_value, "speed"));
            c.menu = static_cast<std::uint32_t>(host.value(sym::speed_button, "menu"));
            c.menu_shown = static_cast<std::uint32_t>(host.value(sym::speed_button, "menu_shown"));
        } else {
            host.warn("the speed buttons reach the session another way than the unit panel: no SPEED");
        }
    }
    if (host.present(sym::explore_command) && host.present(sym::explore_land_reason) &&
        host.present(sym::explore_button)) {
        if (host.value(sym::explore_button, "game") == c.game) {
            c.explore = host.function(sym::explore_command);
            c.explore_size = static_cast<std::uint32_t>(host.value(sym::explore_button, "size"));
            c.explore_reason = static_cast<std::uint8_t>(host.value(sym::explore_land_reason, "reason"));
        } else {
            host.warn("the explore button reaches the session another way than the unit panel: no EXPLORE");
        }
    }
    if (host.present(sym::siege_toggle_area) && host.present(sym::suppress_toggle_area) &&
        host.present(sym::province_map_area)) {
        c.siege_area = host.function(sym::siege_toggle_area);
        c.suppress_area = host.function(sym::suppress_toggle_area);
        c.map_part = static_cast<std::uint32_t>(host.value(sym::province_map_area, "static_part"));
        c.map_area = static_cast<std::uint32_t>(host.value(sym::province_map_area, "area"));
    }
    if (host.present(sym::move_command) && host.present(sym::move_site)) {
        c.move = host.function(sym::move_command);
        c.move_size = static_cast<std::uint32_t>(host.value(sym::move_site, "size"));
        c.move_is_valid = static_cast<std::uint32_t>(host.value(sym::move_site, "is_valid"));
    }
    g_commands = c;
    // Each version only appends types and fields, so older callers get the same table.
    host.register_service(ANVIL_GAME_COMMANDS_SERVICE, 1, kCommandsService);
    host.register_service(ANVIL_GAME_COMMANDS_SERVICE, 2, kCommandsService);
    host.register_service(ANVIL_GAME_COMMANDS_SERVICE, ANVIL_GAME_COMMANDS_SERVICE_VERSION, kCommandsService);
    host.info("service {} v1 to v{}: drill {}, merge {}, split {}, event option {}, speed {}, explore {}, move {}, areas {}",
              ANVIL_GAME_COMMANDS_SERVICE, ANVIL_GAME_COMMANDS_SERVICE_VERSION, c.drill != nullptr ? "yes" : "absent",
              c.merge != nullptr ? "yes" : "absent", c.split != nullptr ? "yes" : "absent",
              c.option_selected != nullptr ? "yes" : "absent", c.speed_up != nullptr ? "yes" : "absent",
              c.explore != nullptr ? "yes" : "absent", c.move != nullptr ? "yes" : "absent",
              c.siege_area != nullptr ? "yes" : "absent");
}

struct AnvilGame : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_layout.frame_counter = sym::frame_counter.counter;
        g_layout.quit = sym::main_loop_frame.quit;
        g_layout.quit_request = sym::main_loop_quit.quit_request;
        g_layout.ready_flag = host.data(sym::ready_flag);
        g_layout.root = host.data(sym::root);
        g_layout.date = sym::root_and_date.date;
        g_layout.session = sym::session_of_root.session;
        // The session is 0x10 bytes shorter before the speed object in the GOG build.
        g_layout.speed = static_cast<std::uint32_t>(host.value(sym::speed_object, "speed"));
        g_layout.ironman = sym::ironman_gate.ironman;
        // The slot is verified to hold speed_object, at the offset the pause
        // command calls; the class's vtable starts that far before it.
        g_layout.session_vtable =
            reinterpret_cast<std::uintptr_t>(host.address(sym::session_speed_slot)) - sym::session_of_root.speed_slot;
        g_layout.is_multiplayer = host.function(sym::is_multiplayer);
        g_layout.frontend_vtable = reinterpret_cast<std::uintptr_t>(host.address(sym::frontend_vtable));
        // The front end is 0x10 bytes shorter before its state in the GOG build.
        g_layout.frontend_state = static_cast<std::uint32_t>(host.value(sym::frontend_state, "state"));
        // A port may declare these absent on a new build: out_of_sync() then stays 0.
        if (host.present(sym::oos_handler) && host.present(sym::oos_flag) && host.present(sym::oos_pause)) {
            // The session is 0x10 bytes shorter before the handler in the GOG build.
            g_layout.oos_handler = static_cast<std::uint32_t>(host.value(sym::oos_handler, "handler"));
            g_layout.oos_flag = sym::oos_flag.flag;
            g_layout.oos_paused = sym::oos_pause.paused;
            g_layout.oos = true;
        } else {
            host.info("the out-of-sync symbols are absent on this build: out_of_sync() stays 0");
        }
        // Likewise for the GUI's keyboard: keyboard() then answers 0.
        if (host.present(sym::gui_text_receiver) && host.present(sym::gui_shortcut_pick)) {
            g_layout.gui = reinterpret_cast<const std::uintptr_t*>(host.game().image_base + host.value(sym::gui_text_receiver, "gui"));
            g_layout.gui_text_receiver = static_cast<std::uint32_t>(host.value(sym::gui_text_receiver, "receiver"));
            g_layout.gui_shortcut_on = static_cast<std::uint32_t>(host.value(sym::gui_shortcut_pick, "enabled"));
            g_layout.gui_shortcut_listeners =
                static_cast<std::uint32_t>(host.value(sym::gui_shortcut_pick, "listeners"));
        } else {
            host.info("the GUI's keyboard symbols are absent on this build: keyboard() answers 0");
        }
        // And for the restart: restart() then answers ANVIL_ERROR_UNSUPPORTED.
        if (host.present(sym::restart_store) && host.present(sym::resign_check)) {
            g_layout.restart_flags = sym::restart_store.flags;
            // The idler is 0x10 bytes shorter before the flag in the GOG build.
            g_layout.resign = static_cast<std::uint32_t>(host.value(sym::resign_check, "resign"));
            g_layout.restart = true;
        } else {
            host.info("the restart symbols are absent on this build: restart() answers unsupported");
        }

        host.register_service(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, kService);
        load_world(host);
        load_commands(host);
        host.hook(g_frame, sym::frame, on_frame);
        host.hook(g_set_session, sym::set_session, on_set_session);
        host.info("frame hook staged; events {}, {}, {}, {}; service {} v{}", ANVIL_GAME_EVENT_READY,
                  ANVIL_GAME_EVENT_FRAME, ANVIL_GAME_EVENT_SESSION, ANVIL_GAME_EVENT_MAIN_MENU, ANVIL_GAME_SERVICE,
                  ANVIL_GAME_SERVICE_VERSION);
        // The core decides when (every participant done, or the limit); the
        // quit itself needs the main thread.
        if (anvil::Benchmark(host).active()) {
            host.subscribe(ANVIL_BENCHMARK_EVENT_QUIT, [] { g_work.push(&push_quit, nullptr); });
            host.info("benchmark start: the game quits after ready once every participant is done");
        }
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilGame)
