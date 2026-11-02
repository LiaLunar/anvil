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
//   5. the frame event.
// Frames drawn inside a game update or a save load (outer = 0) and frames on
// any other thread are passed on untouched; anvil-game only counts the
// nested ones that run, so the main menu is not reported over a loading
// screen.
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

#include <atomic>
#include <mutex>
#include <new>
#include <string>

namespace {

using anvil_game::SessionChanges;
using anvil_game::SessionTracker;
using anvil_game::SessionView;
using anvil_game::WorkQueue;
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
    if (*reinterpret_cast<const std::uintptr_t*>(session + g_layout.speed) == 0) return;
    const std::int32_t date = *reinterpret_cast<const std::int32_t*>(root + g_layout.date);
    if (date == kUnsetDate) return;
    view.session = session;
    view.date = date;
    view.ironman = *reinterpret_cast<const std::uint8_t*>(root + g_layout.ironman) != 0;
    view.multiplayer = g_layout.is_multiplayer(reinterpret_cast<void*>(session));
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

void after_frame(std::uint64_t number) {
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
    AnvilGameFrame frame{};
    frame.size = sizeof(frame);
    frame.number = number;
    publish(ANVIL_GAME_EVENT_FRAME, &frame);
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

constexpr AnvilGameService kService = {
    sizeof(AnvilGameService), service_ready, service_main_thread_id, service_frame_number, service_run_on_main_thread,
    service_session,          service_date,  service_main_menu,
};

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

        host.register_service(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, kService);
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
