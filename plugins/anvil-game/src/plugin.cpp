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
//   6. the frame event.
// Frames drawn inside a game update or a save load (outer = 0) and frames on
// any other thread are passed on untouched; anvil-game only counts the
// nested ones that run, so the main menu is not reported over a loading
// screen.
//
// The service anvil-game.world reads the world in those same moments: on the
// main thread, inside this step, while a session runs (world.hpp).
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
OosTracker g_oos;          // main thread only
// The flags as the service reports them to any thread.
std::atomic<std::uint32_t> g_oos_flags{0};

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

void after_frame_body(std::uint64_t number) {
    const anvil::Host host;
    if (!g_ready.load(std::memory_order_relaxed) && *g_layout.ready_flag == 1) {
        g_ready.store(true, std::memory_order_release);
        host.info("game ready (frame {})", number);
        publish(ANVIL_GAME_EVENT_READY, nullptr);
    }
    const SessionView view = read_session();
    const SessionChanges changes = g_tracker.update(view);
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
}

void after_frame(std::uint64_t number) {
    struct Open {
        Open() { g_after_frame.store(true, std::memory_order_release); }
        ~Open() { g_after_frame.store(false, std::memory_order_release); }
    } open;
    after_frame_body(number);
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
    const auto* bytes = static_cast<const std::uint8_t*>(app);
    if (bytes[g_layout.quit] != 0 || bytes[g_layout.quit_request] != 0) return result;
    std::uint64_t number = 0;
    std::memcpy(&number, bytes + g_layout.frame_counter, sizeof(number));
    try {
        after_frame(number);
    } catch (...) {
        // Only a failed allocation can throw here; the next frame tries again.
    }
    return result;
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

constexpr AnvilGameService kService = {
    sizeof(AnvilGameService), service_ready, service_main_thread_id, service_frame_number, service_run_on_main_thread,
    service_session,          service_date,  service_main_menu,    service_out_of_sync,
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

constexpr AnvilGameWorldService kWorldService = {
    sizeof(AnvilGameWorldService), world_player,         world_province_limit, world_province,
    world_province_name,           world_province_cores, world_country,        world_country_provinces,
    world_war_ids,                 world_war,            world_war_name,       world_war_side,
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
    if (host.value(sym::province_count, "root") != root || host.value(sym::province_is_sea, "root") != root ||
        host.value(sym::province_is_wasteland, "root") != root ||
        host.value(sym::player_country, "countries_db") != host.value(sym::province_base_tax, "countries_db") ||
        host.value(sym::country_exists, "countries_db") != host.value(sym::province_base_tax, "countries_db")) {
        host.warn("the world's symbols disagree on the game root: no service {}", ANVIL_GAME_WORLD_SERVICE);
        return;
    }
    WorldLayout l;
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
    g_world = l;
    g_world_available = true;
    host.register_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION, kWorldService);
    host.info("service {} v{}; events {}, {}, {}, {}, {}", ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION,
              ANVIL_GAME_EVENT_DAY, ANVIL_GAME_EVENT_MONTH, ANVIL_GAME_EVENT_YEAR, ANVIL_GAME_EVENT_PLAYER,
              ANVIL_GAME_EVENT_WAR);
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

        host.register_service(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, kService);
        load_world(host);
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
