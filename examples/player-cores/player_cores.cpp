// player-cores: an example plugin map mode on anvil-map.modes v1, with no
// game addresses of its own.
//
// The mode colours the player's cores green where the player holds them and
// orange where another country does; other land is grey, and sea and
// wasteland keep the game's political colours. The province tooltip says
// whose core it is and who holds it.
//
// The colours are computed only while someone looks at them: when the mode
// is shown, at each new month and when the player's country changes. All of
// it runs in anvil-game's or anvil-map's step on the main thread, where
// anvil-game.world may be read; anvil-map paints the last buffer, also in
// frames the game draws inside a day.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_map.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kHeld = ANVIL_MAP_RGB(46, 160, 67);   // a core the player holds
constexpr std::uint32_t kLost = ANVIL_MAP_RGB(230, 120, 30);  // a core another country holds
constexpr std::uint32_t kOther = ANVIL_MAP_RGB(85, 85, 85);   // other land

const AnvilGameWorldService* g_world = nullptr;
const AnvilMapModesService* g_map = nullptr;
const AnvilMapMode* g_mode = nullptr;

bool same(const AnvilGameTag& a, const AnvilGameTag& b) { return std::memcmp(a.text, b.text, 3) == 0; }

std::string tag_text(const AnvilGameTag& tag) { return std::string(tag.text, strnlen(tag.text, 3)); }

// Whether `tag` has a core on `province`.
bool has_core(std::int32_t province, const AnvilGameTag& tag) {
    AnvilGameTag cores[64];
    std::uint32_t count = 0;
    if (g_world->province_cores(province, cores, 64, &count) != ANVIL_OK) return false;
    for (std::uint32_t i = 0; i < count && i < 64; ++i) {
        if (same(cores[i], tag)) return true;
    }
    return false;
}

// One pass over the map (about a millisecond), so only while the mode is shown.
void paint() {
    AnvilGameTag player{};
    const std::int32_t limit = g_world->province_limit();
    if (limit <= 0 || g_world->player(&player) != ANVIL_OK) return;
    std::vector<std::uint32_t> colors(static_cast<std::size_t>(limit), ANVIL_MAP_NO_COLOR);
    for (std::int32_t id = 1; id < limit; ++id) {
        AnvilGameProvince p{};
        p.size = sizeof(p);
        if (g_world->province(id, &p) != ANVIL_OK) continue;
        if ((p.flags & ANVIL_GAME_PROVINCE_LAND) == 0 || (p.flags & ANVIL_GAME_PROVINCE_WASTELAND) != 0) continue;
        if (!has_core(id, player)) {
            colors[static_cast<std::size_t>(id)] = kOther;
        } else {
            colors[static_cast<std::size_t>(id)] = same(p.owner, player) ? kHeld : kLost;
        }
    }
    const AnvilResult r = g_map->set_colors(g_mode, colors.data(), static_cast<std::uint32_t>(colors.size()));
    if (r != ANVIL_OK) anvil::Host().warn("set_colors: error {}", static_cast<int>(r));
}

void paint_if_shown() {
    if (g_map->shown() == g_mode) paint();
}

// The tooltip's lines for the province under the cursor.
std::size_t describe(void*, const AnvilMapMode*, std::int32_t province, char* text, std::size_t capacity) {
    AnvilGameTag player{};
    AnvilGameProvince p{};
    p.size = sizeof(p);
    if (g_world->player(&player) != ANVIL_OK || g_world->province(province, &p) != ANVIL_OK) return 0;
    if ((p.flags & ANVIL_GAME_PROVINCE_LAND) == 0) return 0;
    std::string line;
    if (!has_core(province, player)) {
        line = "Not a core of yours";
    } else if (same(p.owner, player)) {
        line = "Your core, held by you";
    } else if (p.owner.text[0] == '\0') {
        line = "Your core, held by nobody";
    } else {
        line = std::format("Your core, held by {}", tag_text(p.owner));
    }
    if (line.size() >= capacity) return 0;
    std::memcpy(text, line.c_str(), line.size() + 1);
    return line.size();
}

struct PlayerCores : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_world = host.service<AnvilGameWorldService>(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION);
        g_map = host.service<AnvilMapModesService>(ANVIL_MAP_MODES_SERVICE, ANVIL_MAP_MODES_SERVICE_VERSION);
        if (g_world == nullptr || g_map == nullptr) {
            host.warn("anvil-game.world or anvil-map.modes is not available on this build; no map mode");
            return;
        }
        AnvilMapModeInfo info{};
        info.size = sizeof(info);
        info.tab = ANVIL_MAP_TAB_DEFAULT;
        info.id = "player-cores.cores";
        info.label = "Player's cores";
        info.description = "Your cores: green where you hold them, orange where another country does.";
        info.describe = &describe;
        if (const AnvilResult r = g_map->register_mode(&info, &g_mode); r != ANVIL_OK) {
            host.warn("register_mode: error {}", static_cast<int>(r));
            return;
        }
        host.subscribe(ANVIL_MAP_EVENT_SHOWN, [](const void* payload) {
            if (static_cast<const AnvilMapModeChange*>(payload)->mode == g_mode) paint();
        });
        host.subscribe(ANVIL_GAME_EVENT_MONTH, [] { paint_if_shown(); });
        host.subscribe(ANVIL_GAME_EVENT_PLAYER, [] { paint_if_shown(); });
        host.info("map mode {} registered", g_map->id(g_mode));
    }
};

}  // namespace

ANVIL_PLUGIN(PlayerCores)
