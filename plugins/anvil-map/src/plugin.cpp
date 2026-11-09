// anvil-map: the standard plugin that lets plugins add map modes
// (docs/spec/map-modes-v1.md, ADR 0007; the C interface is
// include/anvil/anvil_map.h).
//
// A plugin mode borrows a number of the game's map mode enum, and the hooks
// answer for that number in place of the game:
//   - map_color paints the plugin's last colour buffer (over the political
//     map mode where it has no colour); it runs in any frame, nested ones
//     included, so it reads nothing but that buffer and the game's count;
//   - map_mode_label and map_mode_sprite give the button's name, tooltip and
//     icon; the call in CSettings::WriteMembers gets the sprite the writer
//     skips, so settings.txt never keeps a plugin mode under a debug mode's name;
//   - add_map_modes appends the tab's plugin modes to the array the game
//     builds the tab's buttons from;
//   - mapmode_tooltip writes the plugin's kept lines in place of the borrowed
//     debug mode's and notes which province was asked for.
// Every other number goes to the game unchanged, so without plugin modes
// nothing the player sees changes.
//
// The step runs after each frame of the main loop (anvil-game.frame) while a
// session runs: shown/hidden when the current map mode changes, the shown
// mode marked dirty after a new buffer, and describe() for the province the
// tooltip asked for. anvil-game.world may be read there.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_map.h>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "symbols.gen.hpp"

#include "modes.hpp"
#include "text.hpp"

namespace {

using anvil_map::Registry;

// The political map mode, whose colours a province without one keeps.
constexpr std::int32_t kPolitical = 1;
// The sprite of a plugin mode that names none, and the one settings.txt skips.
constexpr const char* kGenericSprite = "GFX_mapmode_notselected";
constexpr const char* kSkippedSprite = "GFX_mapmode_notselected";
// Room for a tab's array and every plugin mode.
constexpr std::size_t kTabCapacity = 128;
constexpr std::size_t kDescribeCapacity = 2048;

// Mei's text service (mei.text version 1): whether the game's text is UTF-8.
constexpr const char* kMeiTextService = "mei.text";
struct MeiTextService {
    std::uint32_t size;
    int (*game_text_utf8)(void);
};

struct Layout {
    std::uintptr_t* root = nullptr;       // the global that holds the game root
    std::uint32_t session = 0;            // root+: the session
    std::uint32_t map_mode = 0;           // session+: int32, the current map mode
    std::uintptr_t* map = nullptr;        // the global that holds the map's provinces
    std::uint32_t provinces = 0;          // map+: the province vector {begin, end}
    std::uint32_t shift = 0;
    std::uint64_t magic = 0;
    std::uint32_t province_id = 0;        // province+: int32, its own ID
    std::array<const std::int32_t*, anvil_map::kTabs> tabs{};
};

Layout g_layout;
Registry& g_registry = *new Registry;  // kept for the process
bool g_utf8_game = false;
// Another loader of map modes is in the process: every hook passes on.
std::atomic<bool> g_off{false};

anvil::HookOf<sym::map_color> g_color;
anvil::HookOf<sym::map_mode_label> g_label;
anvil::HookOf<sym::map_mode_sprite> g_sprite;
anvil::HookOf<sym::map_mode_sprite> g_settings_sprite;  // the redirected call in CSettings::WriteMembers
anvil::HookOf<sym::add_map_modes> g_add;
anvil::HookOf<sym::mapmode_tooltip> g_tooltip;
sym::Types::cstring_from_text::Pointer g_cstring = nullptr;
sym::Types::text_append::Pointer g_append = nullptr;
sym::Types::set_map_mode_dirty::Pointer g_dirty = nullptr;

const AnvilGameWorldService* g_world = nullptr;

std::atomic<bool> g_in_session{false};
std::atomic<std::int32_t> g_province_limit{0};
std::atomic<AnvilMapMode*> g_shown{nullptr};  // as of the last step
// What the tooltip last asked for; the step answers it.
std::atomic<AnvilMapMode*> g_asked_mode{nullptr};
std::atomic<std::int32_t> g_asked_province{0};

std::atomic<std::uint64_t> g_paints{0};
std::atomic<std::uint64_t> g_tooltips{0};

void publish(const char* event, AnvilMapMode* mode) noexcept {
    AnvilMapModeChange change{};
    change.size = sizeof(change);
    change.mode = mode;
    change.id = mode->id.c_str();
    const anvil::Host host;
    host.c_api().publish(host.self(), event, &change);
}

// ---- reads under a guard -------------------------------------------------------

// No objects with destructors here: the body runs under __try.
bool read_raw(std::uintptr_t address, void* out, std::size_t size) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> bool read(std::uintptr_t address, T& out) noexcept { return read_raw(address, &out, sizeof(T)); }

// The session object and its current map mode.
bool current_mode(std::uintptr_t& session, std::int32_t& mode) noexcept {
    std::uintptr_t root = 0;
    return read(reinterpret_cast<std::uintptr_t>(g_layout.root), root) && root != 0 &&
           read(root + g_layout.session, session) && session != 0 && read(session + g_layout.map_mode, mode);
}

// How many provinces the paint fills, as the game counts them.
std::uint64_t paint_count() noexcept {
    std::uintptr_t map = 0;
    std::uintptr_t vector[2] = {};
    if (!read(reinterpret_cast<std::uintptr_t>(g_layout.map), map) || map == 0 ||
        !read_raw(map + g_layout.provinces, vector, sizeof(vector)) || vector[1] < vector[0]) {
        return 0;
    }
    const std::uint64_t count = ((vector[1] - vector[0]) >> g_layout.shift) * g_layout.magic;
    return count > 100000 ? 0 : count;
}

// ---- the hooks -----------------------------------------------------------------

AnvilMapMode* plugin_mode(std::int32_t number) noexcept {
    return g_off.load(std::memory_order_relaxed) ? nullptr : g_registry.by_number(number);
}

void on_color(std::uint8_t* colors, std::uint32_t kind, std::int32_t mode) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) {
        g_color.next(colors, kind, mode);
        return;
    }
    g_paints.fetch_add(1, std::memory_order_relaxed);
    const std::shared_ptr<const anvil_map::Colors> buffer = m->snapshot();
    const std::uint64_t count = paint_count();
    const std::size_t given = buffer ? buffer->values.size() : 0;
    if (buffer == nullptr || !buffer->complete || given < count) g_color.next(colors, kind, kPolitical);
    if (buffer == nullptr || colors == nullptr) return;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, given));
    for (std::size_t i = 1; i < n; ++i) {
        const std::uint32_t c = buffer->values[i];
        if ((c >> 24) != 0) continue;
        std::uint8_t* p = colors + i * 4;
        p[0] = static_cast<std::uint8_t>(c);        // B
        p[1] = static_cast<std::uint8_t>(c >> 8);   // G
        p[2] = static_cast<std::uint8_t>(c >> 16);  // R
        p[3] = 0xFF;
    }
}

void* on_label(void* out, std::int32_t mode, std::uint8_t description) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) return g_label.next(out, mode, description);
    return g_cstring(out, description != 0 ? m->description.c_str() : m->label.c_str());
}

void* on_sprite(void* out, std::int32_t mode) noexcept {
    AnvilMapMode* m = plugin_mode(mode);
    if (m == nullptr) return g_sprite.next(out, mode);
    return g_cstring(out, m->sprite.empty() ? kGenericSprite : m->sprite.c_str());
}

void* on_settings_sprite(void* out, std::int32_t mode) noexcept {
    if (plugin_mode(mode) == nullptr) return g_settings_sprite.next(out, mode);
    return g_cstring(out, kSkippedSprite);
}

void on_add(void* panel, const std::int32_t* modes, std::int32_t count, std::uint8_t flag) noexcept {
    std::uint32_t tab = 0;
    for (std::uint32_t t = 0; t < anvil_map::kTabs; ++t) {
        if (modes == g_layout.tabs[t]) tab = t + 1;
    }
    if (tab == 0 || g_off.load(std::memory_order_relaxed) || count < 0 ||
        static_cast<std::size_t>(count) + anvil_map::kPool.size() > kTabCapacity) {
        g_add.next(panel, modes, count, flag);
        return;
    }
    // One array per tab, kept: the game reads it while it builds the buttons.
    static std::array<std::array<std::int32_t, kTabCapacity>, anvil_map::kTabs> arrays{};
    std::array<std::int32_t, kTabCapacity>& array = arrays[tab - 1];
    std::size_t added = 0;
    try {
        added = g_registry.tab_numbers(tab, array.data() + count, anvil_map::kPool.size());
    } catch (...) {
        added = 0;
    }
    if (added == 0) {
        g_add.next(panel, modes, count, flag);
        return;
    }
    std::memcpy(array.data(), modes, static_cast<std::size_t>(count) * sizeof(std::int32_t));
    g_add.next(panel, array.data(), count + static_cast<std::int32_t>(added), flag);
}

void on_tooltip(void* province, std::uint8_t flag, const void* other, void* tooltip) noexcept {
    std::uintptr_t session = 0;
    std::int32_t mode = -1;
    AnvilMapMode* m = nullptr;
    if (g_in_session.load(std::memory_order_acquire) && current_mode(session, mode)) m = plugin_mode(mode);
    if (m == nullptr) {
        g_tooltip.next(province, flag, other, tooltip);
        return;
    }
    // The plugin's lines replace the borrowed debug mode's.
    g_tooltips.fetch_add(1, std::memory_order_relaxed);
    std::int32_t id = 0;
    if (province == nullptr || !read(reinterpret_cast<std::uintptr_t>(province) + g_layout.province_id, id)) return;
    g_asked_province.store(id, std::memory_order_relaxed);
    g_asked_mode.store(m, std::memory_order_release);
    try {
        const std::string text = anvil_map::kept_text(*m, id);
        if (!text.empty() && tooltip != nullptr) g_append(tooltip, text.data(), text.size());
    } catch (...) {
        // Only a failed allocation: the tooltip goes without the lines.
    }
}

// ---- the step ------------------------------------------------------------------

void describe_asked(AnvilMapMode* shown) {
    AnvilMapMode* asked = g_asked_mode.exchange(nullptr, std::memory_order_acq_rel);
    if (asked == nullptr || asked != shown || asked->describe == nullptr) return;
    const std::int32_t province = g_asked_province.load(std::memory_order_relaxed);
    std::array<char, kDescribeCapacity> buffer{};
    std::size_t n = 0;
    try {
        n = asked->describe(asked->context, asked, province, buffer.data(), buffer.size());
    } catch (...) {
        n = 0;
    }
    n = std::min(n, buffer.size() - 1);
    anvil_map::keep_text(*asked, province,
                         anvil_map::game_text(anvil_map::utf8_prefix(std::string_view(buffer.data(), n)), g_utf8_game));
}

void step() {
    if (!g_in_session.load(std::memory_order_acquire) || g_off.load(std::memory_order_relaxed)) return;
    if (g_world != nullptr && g_province_limit.load(std::memory_order_relaxed) == 0) {
        g_province_limit.store(g_world->province_limit(), std::memory_order_relaxed);
    }
    std::uintptr_t session = 0;
    std::int32_t number = -1;
    AnvilMapMode* shown = current_mode(session, number) ? plugin_mode(number) : nullptr;
    AnvilMapMode* last = g_shown.load(std::memory_order_relaxed);
    if (shown != last) {
        g_shown.store(shown, std::memory_order_release);
        if (last != nullptr) publish(ANVIL_MAP_EVENT_HIDDEN, last);
        if (shown != nullptr) publish(ANVIL_MAP_EVENT_SHOWN, shown);
    }
    if (shown != nullptr) {
        // A new buffer (maybe from the shown handler just now) repaints the map.
        const std::uint64_t generation = shown->generation.load(std::memory_order_acquire);
        if (generation != shown->dirtied) {
            shown->dirtied = generation;
            g_dirty(reinterpret_cast<void*>(session), number);
        }
    }
    describe_asked(shown);
}

void on_session(const void* payload) {
    const auto* s = static_cast<const AnvilGameSession*>(payload);
    if (s == nullptr) return;
    if (s->change == ANVIL_GAME_SESSION_STARTED) {
        g_province_limit.store(g_world != nullptr ? g_world->province_limit() : 0, std::memory_order_relaxed);
        g_in_session.store(true, std::memory_order_release);
        return;
    }
    g_in_session.store(false, std::memory_order_release);
    g_province_limit.store(0, std::memory_order_relaxed);
    const std::uint64_t paints = g_paints.exchange(0), tooltips = g_tooltips.exchange(0);
    if (paints != 0 || tooltips != 0) {
        const anvil::Host host;
        host.info("session {} ended: {} paints and {} tooltips of plugin modes", s->number, paints, tooltips);
    }
    g_asked_mode.store(nullptr, std::memory_order_relaxed);
    g_registry.clear_session();
    if (AnvilMapMode* last = g_shown.exchange(nullptr, std::memory_order_acq_rel)) publish(ANVIL_MAP_EVENT_HIDDEN, last);
}

// ---- the service ---------------------------------------------------------------

AnvilResult svc_register(const AnvilMapModeInfo* info, const AnvilMapMode** out) {
    const anvil::Host host;
    if (g_off.load(std::memory_order_relaxed)) {
        host.warn("register_mode {}: refused, another loader of map modes runs (eu4agent.dll)",
                  info != nullptr && info->id != nullptr ? info->id : "(null)");
        return ANVIL_ERROR_UNSUPPORTED;
    }
    try {
        std::string why;
        const AnvilResult r = g_registry.add(info, g_utf8_game, out, why);
        if (r == ANVIL_OK) {
            host.info("mode {} borrows map mode {} (tab {})", (*out)->id, (*out)->number, (*out)->tab);
        } else {
            host.warn("register_mode {}: {}", info != nullptr && info->id != nullptr ? info->id : "(null)", why);
        }
        return r;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult svc_set_colors(const AnvilMapMode* mode, const std::uint32_t* colors, std::uint32_t count) {
    if (mode == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!g_in_session.load(std::memory_order_acquire)) return ANVIL_ERROR_WRONG_PHASE;
    try {
        auto buffer = std::make_shared<anvil_map::Colors>();
        const AnvilResult r =
            anvil_map::make_colors(colors, count, g_province_limit.load(std::memory_order_relaxed), *buffer);
        if (r != ANVIL_OK) return r;
        anvil_map::set_colors(*const_cast<AnvilMapMode*>(mode), std::move(buffer));
        return ANVIL_OK;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

const char* svc_mode_id(std::int32_t number) {
    const AnvilMapMode* m = g_registry.by_number(number);
    return m != nullptr ? m->id.c_str() : nullptr;
}

const AnvilMapMode* svc_shown() { return g_shown.load(std::memory_order_acquire); }

const char* svc_id(const AnvilMapMode* mode) { return mode != nullptr ? mode->id.c_str() : nullptr; }

const AnvilMapModesService kService = {
    sizeof(AnvilMapModesService), &svc_register, &svc_set_colors, &svc_mode_id, &svc_shown, &svc_id,
};

bool other_loader() { return GetModuleHandleW(L"eu4agent.dll") != nullptr; }

struct AnvilMap : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_layout.root = host.data(sym::root);
        g_layout.session = sym::session_of_root.session;
        g_layout.map_mode = static_cast<std::uint32_t>(host.value(sym::set_map_mode_dirty, "map_mode"));
        g_layout.map = host.data(sym::map_global);
        g_layout.provinces = sym::map_color_count.provinces;
        g_layout.shift = sym::map_color_count.shift;
        g_layout.magic = sym::map_color_count.magic;
        g_layout.province_id = sym::province_own_id.own_id;
        g_layout.tabs = {static_cast<const std::int32_t*>(host.address(sym::tab_1)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_2)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_3)),
                         static_cast<const std::int32_t*>(host.address(sym::tab_4))};
        g_cstring = host.function(sym::cstring_from_text);
        g_append = host.function(sym::text_append);
        g_dirty = host.function(sym::set_map_mode_dirty);

        if (const auto* mei = host.service<MeiTextService>(kMeiTextService, 1);
            mei != nullptr && mei->size >= sizeof(MeiTextService) && mei->game_text_utf8 != nullptr) {
            g_utf8_game = mei->game_text_utf8() != 0;
        }
        g_world = host.service<AnvilGameWorldService>(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION);
        if (g_world == nullptr) host.info("anvil-game.world is not available: set_colors is limited by the map only");

        host.register_service(ANVIL_MAP_MODES_SERVICE, ANVIL_MAP_MODES_SERVICE_VERSION, kService);
        if (other_loader()) {
            g_off.store(true);
            host.warn("eu4agent.dll is loaded: it hooks the same map mode functions and numbers, so anvil-map "
                      "installs no hooks and refuses plugin modes");
            return;
        }
        // The load is one group: all six are enabled together, or none.
        host.hook(g_color, sym::map_color, on_color);
        host.hook(g_label, sym::map_mode_label, on_label);
        host.hook(g_sprite, sym::map_mode_sprite, on_sprite);
        host.redirect(g_settings_sprite, sym::settings_sprite_call, on_settings_sprite);
        host.hook(g_add, sym::add_map_modes, on_add);
        host.hook(g_tooltip, sym::mapmode_tooltip, on_tooltip);
        host.subscribe(ANVIL_GAME_EVENT_SESSION, [](const void* payload) { on_session(payload); });
        host.subscribe(ANVIL_GAME_EVENT_FRAME, [] {
            try {
                step();
            } catch (...) {
                // Only a failed allocation; the next frame tries again.
            }
        });
        host.subscribe(ANVIL_GAME_EVENT_READY, [] {
            const anvil::Host h;
            if (other_loader() && !g_off.exchange(true)) {
                h.warn("eu4agent.dll is loaded: it hooks the same map mode functions and numbers, so anvil-map's "
                       "hooks pass everything on and plugin modes are refused");
            }
        });
        host.info("service {} v{}: {} numbers to lend, text in {}; events {}, {}", ANVIL_MAP_MODES_SERVICE,
                  ANVIL_MAP_MODES_SERVICE_VERSION, anvil_map::kPool.size(), g_utf8_game ? "UTF-8" : "Windows-1252",
                  ANVIL_MAP_EVENT_SHOWN, ANVIL_MAP_EVENT_HIDDEN);
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilMap)
