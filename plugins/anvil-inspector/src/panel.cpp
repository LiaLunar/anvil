// The window's thread and its reads.
//
// One thread of the plugin, started when the setting is first on, creates and
// shows the panel (a setting's callback runs inside the overlay's own drawing,
// where it must not call the overlay back). While the window is shown it wakes
// once a second and queues one piece of main-thread work (anvil-game's
// run_on_main_thread), the only place anvil-game.world may be read. That work
// asks for the date, the session, the province view and the player, a few
// calls, and reads everything again only when one of them changed, the window
// was opened, or the player clicked Refresh. The thread then hands the overlay
// the rows. A hidden window costs a wake-up a second; with the setting off
// there is no thread and nothing runs on the main thread or in any frame.
#include "panel.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "rows.hpp"
#include "snapshot.hpp"
#include "texts.hpp"

namespace anvil_inspector::panel {

namespace {

const AnvilPanelService* g_panels = nullptr;
const AnvilGameService* g_game = nullptr;
const AnvilGameWorldService* g_world = nullptr;  // NULL on a build without it
AnvilPanel* g_panel = nullptr;                   // the thread's

std::atomic<bool> g_on{false};
std::atomic<bool> g_show{false};     // show it at the next wake-up
std::atomic<bool> g_started{false};
std::atomic<bool> g_pending{false};  // main-thread work is queued
std::atomic<bool> g_force{true};     // the next work reads everything

std::mutex g_wake_mutex;
std::condition_variable g_wake_cv;
bool g_wake = false;

// The last read and the view, for the thread, the work and the clicks.
std::mutex g_mutex;
Snapshot g_snapshot;
Key g_key;
bool g_read = false;
View g_view;
bool g_dirty = false;

Texts texts() { return Texts{lang_of(g_panels->language != nullptr ? g_panels->language() : "en")}; }

void wake() {
    {
        std::lock_guard lock(g_wake_mutex);
        g_wake = true;
    }
    g_wake_cv.notify_one();
}

// On the main thread, after an outer frame.
void read_work(void*) {
    try {
        g_pending.store(false, std::memory_order_release);
        const Key key = key_of(*g_game, g_world);
        const bool force = g_force.exchange(false);
        {
            std::lock_guard lock(g_mutex);
            if (!force && g_read && key == g_key) return;
        }
        Snapshot snapshot = read_snapshot(*g_game, g_world);
        {
            std::lock_guard lock(g_mutex);
            g_snapshot = std::move(snapshot);
            g_key = key;
            g_read = true;
            g_dirty = true;
        }
        wake();
    } catch (...) {
        // A failed allocation: the next wake-up tries again.
        g_force.store(true);
    }
}

void publish() {
    const Texts t = texts();
    std::vector<Row> rows_now;
    {
        std::lock_guard lock(g_mutex);
        if (!g_dirty) return;
        g_dirty = false;
        rows_now = rows(g_snapshot, g_view, t);
    }
    std::vector<AnvilPanelRow> out(rows_now.size());
    for (std::size_t i = 0; i < rows_now.size(); ++i) {
        out[i].kind = rows_now[i].kind;
        out[i].label = rows_now[i].kind == kSeparator ? nullptr : rows_now[i].label.c_str();
        out[i].value = rows_now[i].has_value ? rows_now[i].value.c_str() : nullptr;
        out[i].id = rows_now[i].id;
    }
    const AnvilResult r =
        g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow));
    if (r != ANVIL_OK) anvil::Host{}.warn("the inspector: set_rows failed ({}) for {} rows", static_cast<int>(r), out.size());
}

// On the main thread: a fold only changes the view, Refresh asks for a read.
void on_click(void*, std::uint64_t id) {
    if (action_of(id) == Action::refresh) {
        g_force.store(true);
    } else {
        std::lock_guard lock(g_mutex);
        if (apply_click(g_view, id)) g_dirty = true;
    }
    wake();
}

void run() {
    bool was_shown = false;
    for (;;) {
        {
            std::unique_lock lock(g_wake_mutex);
            g_wake_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_wake; });
            g_wake = false;
        }
        try {
            if (!g_on.load(std::memory_order_acquire)) {
                if (g_panel != nullptr && g_panels->shown(g_panel)) g_panels->show(g_panel, 0);
                was_shown = false;
                continue;
            }
            if (g_panel == nullptr) {
                if (g_panels->create(texts()("Inspector"), &g_panel) != ANVIL_OK) {
                    anvil::Host{}.warn("the inspector: the overlay has no room for another panel");
                    g_on.store(false);
                    continue;
                }
                g_panels->on_click(g_panel, &on_click, nullptr);
            }
            if (g_show.exchange(false)) g_panels->show(g_panel, 1);
            const bool shown = g_panels->shown(g_panel) != 0;
            if (shown && !was_shown) g_force.store(true);  // opened, also from the Status tab
            was_shown = shown;
            if (!shown) continue;
            if (!g_pending.exchange(true) && g_game->run_on_main_thread(&read_work, nullptr) != ANVIL_OK) {
                g_pending.store(false);
            }
            publish();
        } catch (...) {
            // A failed allocation: the next wake-up tries again.
        }
    }
}

void start_thread() {
    if (g_started.exchange(true)) return;
    std::thread(run).detach();
}

}  // namespace

void load(const anvil::Host& host, bool on) {
    g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
    g_game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
    g_world = host.service<AnvilGameWorldService>(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION);
    if (g_panels == nullptr || g_panels->size < sizeof(AnvilPanelService) || g_game == nullptr ||
        !ANVIL_GAME_HAS(g_game, date)) {
        g_panels = nullptr;
        host.info("no anvil-overlay.panel or no anvil-game with dates: the inspector is not available");
        return;
    }
    if (g_world == nullptr) host.info("no anvil-game.world on this build: the inspector shows only the date");
    if (!on) return;
    g_on.store(true);
    g_show.store(true);
    start_thread();
    host.info("the inspector is on");
}

void set_on(bool on) {
    if (g_panels == nullptr) return;
    g_on.store(on, std::memory_order_release);
    if (on) {
        g_show.store(true);
        start_thread();
    }
    wake();
}

}  // namespace anvil_inspector::panel
