// What the overlay tells the player without being asked (design §12, §13, §15):
// pure functions from what anvil.control reports to notices, so the rules are
// tested without a game or a device.
#pragma once

#include "control_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace overlay {

enum class NoticeKind { Info, Warning, Error };

struct NoticeAction {
    enum class Type { OpenPanel, DisablePlugin };
    Type type = Type::OpenPanel;
    std::string plugin;  // DisablePlugin
    std::string label;
};

struct Notice {
    std::string id;  // stable: "safe-mode", "crash", "unknown-build", "not-loaded", "approvals", "ironman", "multiplayer"
    NoticeKind kind = NoticeKind::Info;
    std::string title;
    std::vector<std::string> lines;
    std::vector<NoticeAction> actions;
    // Session notices stay while their session lasts and cannot be closed;
    // the others close after the notification time or on a click.
    bool lasting = false;
    // Stays until the player closes it, whatever the notification time: it comes
    // before the game's menu, when a timer would run out on the loading screen.
    bool until_closed = false;
};

// Once per start, when the game is ready and every plugin was processed.
struct StartFacts {
    bool safe_mode = false;
    uint32_t unfinished_starts = 0;
    std::vector<PluginRow> plugins;
    std::optional<CrashInfo> crash;  // the previous crash, if any
    std::vector<ModRow> mod_plugins;  // plugins in mods (design §14)
};
std::vector<Notice> start_notices(const StartFacts& facts);

// While a game session runs (anvil-game.session), design §13.
struct SessionFacts {
    bool session = false;
    bool ironman = false;
    bool multiplayer = false;
    uint32_t gameplay_active = 0;
    std::string gameplay_hash;
};
std::vector<Notice> session_notices(const SessionFacts& facts);

// The badge in the game's main menu (design §15): where it goes, in the
// window's client pixels, and what its tooltip says.
struct BadgeRect {
    float x = 0, y = 0, w = 0, h = 0;
};
// On the left edge, halfway down: between the game's logo at the top and the
// menu's bottom bar at any resolution and at any interface scale the game
// allows (game_ui_scale), however wide the badge is at any overlay scale.
BadgeRect menu_badge_rect(float screen_height, float badge_width, float badge_height);
// The tooltip's lines: the plugin counts and what a click does. `panel_key`
// is the key as the settings show it, "" when there is none.
std::vector<std::string> menu_badge_tooltip(const AnvilControlStatus& status, bool panel_open,
                                            const std::string& panel_key);

// The panel key: a key code of settings.toml (virtual key in bits 0-7,
// 0x100 Shift, 0x200 Ctrl, 0x400 Alt) against a key press with the
// modifiers held. Exact: Ctrl+F12 does not match Ctrl+Shift+F12.
bool key_matches(int64_t code, unsigned vk, bool shift, bool ctrl, bool alt);
// A key press as a key code; 0 for a modifier key on its own.
int64_t key_code(unsigned vk, bool shift, bool ctrl, bool alt);

}  // namespace overlay
