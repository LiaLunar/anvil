// anvil-overlay (plugins/anvil-overlay) without a device: which notices it
// shows (design §12, §13, §15), its release check (ADR 0005), the panel key,
// and its client of anvil.control against the core's service on fake plugins.
#include "core/control.hpp"
#include "core/host.hpp"
#include "core/mod_plugins.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-overlay/src/control_client.hpp"
#include "../plugins/anvil-overlay/src/notices.hpp"
#include "../plugins/anvil-overlay/src/panels.hpp"
#include "../plugins/anvil-overlay/src/release.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace overlay;
namespace fs = std::filesystem;

PluginRow row(const std::string& id, const std::string& state, const std::string& reason = "", bool disabled = false) {
    PluginRow p;
    p.id = id;
    p.state = state;
    p.reason = reason;
    p.disabled = disabled;
    return p;
}

const Notice* find(const std::vector<Notice>& notices, const std::string& id) {
    for (const Notice& n : notices) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

size_t disable_actions(const Notice& n) {
    size_t count = 0;
    for (const auto& a : n.actions) count += a.type == NoticeAction::Type::DisablePlugin ? 1 : 0;
    return count;
}

}  // namespace

int main() {
    run("overlay: plugins' panels keep what they are given", [] {
        PanelStore store;
        AnvilPanel* panel = nullptr;
        CHECK(store.create(nullptr, &panel) == ANVIL_ERROR_INVALID_ARGUMENT && store.create("", &panel) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.create("Stats", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.create("Stats", &panel) == ANVIL_OK && panel != nullptr && panel->title == "Stats");
        CHECK(!store.shown(panel) && !store.any_shown() && !store.wants_cjk());

        std::string label = "Treasury", value = "100";
        const AnvilPanelRow rows[] = {{ANVIL_PANEL_TEXT, 0, label.c_str(), value.c_str(), 0},
                                      {ANVIL_PANEL_SEPARATOR, 0, nullptr, nullptr, 0},
                                      {ANVIL_PANEL_TEXT, 0, "plain", nullptr, 0},
                                      {ANVIL_PANEL_BUTTON, 0, "Save", nullptr, 7}};
        CHECK(store.set_rows(panel, rows, 4, sizeof(AnvilPanelRow)) == ANVIL_OK);
        label = "changed";  // the rows were copied
        store.each([](AnvilPanel& p) {
            CHECK(p.rows.size() == 4 && p.rows[0].label == "Treasury" && p.rows[0].has_value && p.rows[0].value == "100");
            CHECK(p.rows[1].kind == ANVIL_PANEL_SEPARATOR && !p.rows[2].has_value && p.rows[3].id == 7);
        });
        CHECK(store.set_rows(panel, rows, 0, sizeof(AnvilPanelRow)) == ANVIL_OK);
        store.each([](AnvilPanel& p) { CHECK(p.rows.empty()); });

        // ImGui would end a label at "##".
        PanelStore labels;
        AnvilPanel* hashes = nullptr;
        CHECK(labels.create("a##b###c", &hashes) == ANVIL_OK && hashes->title == "a# #b# # #c");
        const AnvilPanelRow button[] = {{ANVIL_PANEL_BUTTON, 0, "Go##1", nullptr, 0}, {ANVIL_PANEL_TEXT, 0, "a##b", nullptr, 0}};
        CHECK(labels.set_rows(hashes, button, 2, sizeof(AnvilPanelRow)) == ANVIL_OK);
        CHECK(hashes->rows[0].label == "Go# #1" && hashes->rows[1].label == "a##b");

        // Bad rows change nothing.
        CHECK(store.set_rows(panel, rows, 4, sizeof(AnvilPanelRow)) == ANVIL_OK);
        const AnvilPanelRow bad_kind[] = {{9, 0, "x", nullptr, 0}};
        const AnvilPanelRow no_label[] = {{ANVIL_PANEL_TEXT, 0, nullptr, nullptr, 0}};
        const std::string long_text(kMaxText + 1, 'x');
        const AnvilPanelRow too_long[] = {{ANVIL_PANEL_TEXT, 0, long_text.c_str(), nullptr, 0}};
        CHECK(store.set_rows(panel, bad_kind, 1, sizeof(AnvilPanelRow)) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set_rows(panel, no_label, 1, sizeof(AnvilPanelRow)) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set_rows(panel, too_long, 1, sizeof(AnvilPanelRow)) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set_rows(panel, rows, kMaxRows + 1, sizeof(AnvilPanelRow)) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set_rows(panel, rows, 4, 8) == ANVIL_ERROR_INVALID_ARGUMENT);
        store.each([](AnvilPanel& p) { CHECK(p.rows.size() == 4); });
        AnvilPanel stranger;
        CHECK(store.set_rows(&stranger, rows, 4, sizeof(AnvilPanelRow)) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.show(&stranger, true) == ANVIL_ERROR_INVALID_ARGUMENT && !store.shown(&stranger));

        // A newer row: a longer stride reads the fields it knows.
        struct Newer {
            AnvilPanelRow row;
            uint64_t extra;
        } newer[2] = {{{ANVIL_PANEL_TEXT, 0, "a", nullptr, 0}, 1}, {{ANVIL_PANEL_TEXT, 0, "b", nullptr, 0}, 2}};
        CHECK(store.set_rows(panel, &newer[0].row, 2, sizeof(Newer)) == ANVIL_OK);
        store.each([](AnvilPanel& p) { CHECK(p.rows.size() == 2 && p.rows[1].label == "b"); });

        CHECK(store.show(panel, true) == ANVIL_OK && store.shown(panel) && store.any_shown());
        CHECK(store.show(panel, true) == ANVIL_OK && store.any_shown());
        store.each([&](AnvilPanel& p) { store.set_shown_locked(p, false); });  // the player closed it
        CHECK(!store.shown(panel) && !store.any_shown());

        static uint64_t clicked = 0;
        static void* clicked_context = nullptr;
        int context = 0;
        store.deliver(panel, 5);  // no handler
        CHECK(store.on_click(panel, [](void* c, uint64_t id) {
            clicked = id;
            clicked_context = c;
        }, &context) == ANVIL_OK);
        store.deliver(panel, 5);
        CHECK(clicked == 5 && clicked_context == &context);
        store.deliver(&stranger, 6);
        CHECK(clicked == 5);

        // Texts that need the fonts of Windows.
        CHECK(!has_cjk("Moskva") && !has_cjk("S\xC3\xA3o Tom\xC3\xA9") && !has_cjk("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0"));
        CHECK(has_cjk("\xE5\x8C\x97\xE4\xBA\xAC") && has_cjk("a\xE3\x81\x82") && has_cjk("\xEC\x84\x9C\xEC\x9A\xB8"));
        CHECK(!has_cjk("\xE5\x8C"));  // cut short
        const AnvilPanelRow cjk[] = {{ANVIL_PANEL_TEXT, 0, "city", "\xE5\x8C\x97\xE4\xBA\xAC", 0}};
        CHECK(store.set_rows(panel, cjk, 1, sizeof(AnvilPanelRow)) == ANVIL_OK && store.wants_cjk());
        PanelStore titled;
        AnvilPanel* other = nullptr;
        CHECK(titled.create("\xE5\x8C\x97", &other) == ANVIL_OK && titled.wants_cjk());
        for (uint32_t i = 1; i < kMaxPanels; ++i) CHECK(titled.create("x", &other) == ANVIL_OK);
        CHECK(titled.create("x", &other) == ANVIL_ERROR_OUT_OF_MEMORY);
    });

    run("notices: a clean start shows none", [] {
        StartFacts facts;
        facts.plugins = {row("anvil-game", "loaded"), row("anvil-overlay", "loaded"), row("off", "skipped", "disabled", true)};
        CHECK(start_notices(facts).empty());
        CrashInfo old;
        old.is_new = false;
        old.level = "in_plugin_code";
        facts.crash = old;
        CHECK(start_notices(facts).empty());  // a crash already shown
    });

    run("notices: skipped and failed plugins", [] {
        StartFacts facts;
        facts.plugins = {row("a", "skipped", "requires plugin 'b', which is not loaded"), row("c", "failed", "boom"),
                         row("d", "skipped", "disabled in anvil.toml", true), row("e", "loaded")};
        const auto notices = start_notices(facts);
        const Notice* n = find(notices, "not-loaded");
        CHECK(notices.size() == 1 && n != nullptr && n->title == "2 plugins were not loaded" && !n->lasting);
        CHECK(n->lines.size() == 2 && n->lines[0].find("a: requires") == 0 && n->lines[1] == "c: failed: boom");
        for (int i = 0; i < 10; ++i) facts.plugins.push_back(row("x" + std::to_string(i), "skipped", "why"));
        const auto more = start_notices(facts);
        const Notice* many = find(more, "not-loaded");
        CHECK(many != nullptr);
        if (many != nullptr) CHECK(many->lines.size() == 7 && many->lines.back() == "and 6 more");
    });

    run("notices: an unknown game build gives one notice that stays", [] {
        StartFacts facts;
        for (const char* id : {"anvil-game", "bellows", "tongs"}) {
            facts.plugins.push_back(row(id, "skipped", "game build unknown (EU4 updated?)"));
            facts.plugins.back().why = anvil::i18n::Message("build_unknown", {});
        }
        facts.plugins.push_back(row("no-addresses", "loaded"));
        facts.plugins.push_back(row("other", "skipped", "requires plugin 'b', which is not loaded"));
        const auto notices = start_notices(facts);
        CHECK(notices.size() == 2);
        const Notice* n = find(notices, "unknown-build");
        CHECK(n != nullptr);
        if (n == nullptr) return;
        CHECK(n->title == "3 plugins were not loaded");
        CHECK(n->until_closed && !n->lasting && n->kind == NoticeKind::Warning);
        CHECK(n->lines.size() == 2);
        CHECK(n->lines[0].find("eu4.exe is not a build Anvil knows (EU4 updated?)") == 0);
        CHECK(n->lines[1] == "anvil-game, bellows, tongs");
        const Notice* rest = find(notices, "not-loaded");  // another reason keeps its own notice
        CHECK(rest != nullptr && rest->lines.size() == 1 && rest->lines[0].find("other: requires") == 0 && !rest->until_closed);

        // A plugin held back by one of them counts with them, however long the chain.
        StartFacts chain;
        chain.plugins = {row("anvil-game", "skipped", "game build unknown (EU4 updated?)"),
                         row("no-addresses", "skipped", "requires plugin 'anvil-game' (^1.11.0), which is not loaded"),
                         row("on-top", "skipped", "requires plugin 'no-addresses' (*), which is not loaded"),
                         row("gameplay", "skipped", "affects gameplay, so it needs plugin 'anvil-game'"),
                         row("elsewhere", "skipped", "requires plugin 'nothing' (*), which is not loaded")};
        chain.plugins[0].why = anvil::i18n::Message("build_unknown", {});
        chain.plugins[1].why = anvil::i18n::Message("requires_missing", {"anvil-game", "^1.11.0"});
        chain.plugins[2].why = anvil::i18n::Message("requires_missing", {"no-addresses", "*"});
        chain.plugins[3].why = anvil::i18n::Message("needs_anvil_game", {"anvil-game"});
        chain.plugins[4].why = anvil::i18n::Message("requires_missing", {"nothing", "*"});
        const auto chained = start_notices(chain);
        const Notice* c = find(chained, "unknown-build");
        CHECK(chained.size() == 2 && c != nullptr);
        if (c != nullptr) {
            CHECK(c->title == "4 plugins were not loaded");
            CHECK(c->lines[1] == "anvil-game, no-addresses, on-top, gameplay");
        }
        const Notice* other = find(chained, "not-loaded");
        CHECK(other != nullptr && other->lines.size() == 1 && other->lines[0].find("elsewhere: ") == 0);

        // On GOG the reason says what to do.
        StartFacts gog;
        gog.plugins = {row("anvil-game", "skipped", "this GOG build of the game is unknown")};
        gog.plugins[0].why = anvil::i18n::Message("build_unknown_gog", {"1.37.5.2"});
        const auto gog_notices = start_notices(gog);
        const Notice* g = find(gog_notices, "unknown-build");
        CHECK(gog_notices.size() == 1 && g != nullptr && g->title == "1 plugin was not loaded");
        if (g != nullptr) {
            CHECK(g->lines[0].find("Anvil runs on 1.37.5.2") != std::string::npos);
            CHECK(g->lines[0].find("GOG Galaxy") != std::string::npos && g->lines[1] == "anvil-game");
        }
    });

    run("notices: plugins not ported to a known build wait in one notice that stays", [] {
        StartFacts facts;
        facts.plugins = {row("anvil-game", "loaded"),
                         row("bellows", "skipped", "symbols.toml is for builds a, b, the game is c"),
                         row("tongs", "skipped", "symbols.toml is for build a, the game is c"),
                         row("on-tongs", "skipped", "requires plugin 'tongs' (*), which is not loaded"),
                         row("newer", "skipped", "requires Anvil >=99.0.0, this is Anvil 1.11.0")};
        facts.plugins[1].why = anvil::i18n::Message("symbols_builds", {"a, b", "c"});
        facts.plugins[2].why = anvil::i18n::Message("symbols_build", {"a", "c"});
        facts.plugins[3].why = anvil::i18n::Message("requires_missing", {"tongs", "*"});
        facts.plugins[4].why = anvil::i18n::Message("anvil_version", {">=99.0.0", "1.11.0"});
        const auto notices = start_notices(facts);
        CHECK(notices.size() == 2 && find(notices, "unknown-build") == nullptr);
        const Notice* n = find(notices, "waiting-for-build");
        CHECK(n != nullptr);
        if (n == nullptr) return;
        CHECK(n->title == "3 plugins wait for this game version");
        CHECK(n->until_closed && !n->lasting && n->kind == NoticeKind::Warning);
        CHECK(n->lines.size() == 2 && n->lines[1] == "bellows, tongs, on-tongs");
        CHECK(!n->actions.empty() && n->actions[0].type == NoticeAction::Type::OpenPanel);
        const Notice* rest = find(notices, "not-loaded");
        CHECK(rest != nullptr && rest->lines.size() == 1 && rest->lines[0].find("newer: ") == 0 && !rest->until_closed);
        CHECK(plugins_waiting_for_build(facts.plugins) == 3);

        StartFacts one;
        one.plugins = {row("hello", "skipped", "symbols.toml is for build a, the game is c")};
        one.plugins[0].why = anvil::i18n::Message("symbols_build", {"a", "c"});
        const auto single = start_notices(one);
        CHECK(single.size() == 1 && single[0].title == "1 plugin waits for this game version");

        // A skip for any other reason is no wait.
        StartFacts none;
        none.plugins = {row("off", "skipped", "disabled in anvil.toml")};
        CHECK(plugins_waiting_for_build(none.plugins) == 0);
    });

    run("notices: the previous crash offers to disable the plugins it was in", [] {
        StartFacts facts;
        facts.plugins = {row("tongs", "loaded"), row("bellows", "loaded")};
        facts.plugins[1].disabled_next = true;
        CrashInfo crash;
        crash.is_new = true;
        crash.name = "2026-10-20_18-05-11";
        crash.level = "in_plugin_change";
        crash.summary = "in plugin tongs's change (also bellows, gone)";
        crash.plugins = {"tongs", "bellows", "gone"};
        crash.before_game_ready = true;
        facts.crash = crash;
        const auto notices = start_notices(facts);
        const Notice* n = find(notices, "crash");
        CHECK(n != nullptr && n->kind == NoticeKind::Warning);
        if (n == nullptr) return;
        CHECK(n->title.find("before the game was ready") != std::string::npos);
        CHECK(n->lines[0] == "The crash was in plugin tongs's change (also bellows, gone).");
        CHECK(n->lines[1] == "Report: anvil\\crashes\\2026-10-20_18-05-11.txt");
        // bellows is already disabled from the next start, gone is not a plugin of this start.
        CHECK(disable_actions(*n) == 1 && n->actions[0].plugin == "tongs");
        CHECK(n->actions.back().type == NoticeAction::Type::OpenPanel);
        facts.crash->level = "plugin_on_stack";
        CHECK(disable_actions(*find(start_notices(facts), "crash")) == 0);
        facts.crash->level = "no_plugin";
        CHECK(find(start_notices(facts), "crash")->lines[0] == "No plugin was involved.");
    });

    run("notices: the crash is one whole sentence for each level", [] {
        auto sentence = [](const char* level, std::vector<std::string> plugins, const char* summary) {
            CrashInfo crash;
            crash.level = level;
            crash.plugins = std::move(plugins);
            crash.summary = summary;
            return crash_sentence(crash);
        };
        CHECK(sentence("in_plugin_code", {"tongs"}, "in plugin tongs's code") == "The crash was in plugin tongs's code.");
        CHECK(sentence("in_plugin_code", {"tongs", "bellows"}, "") == "The crash was in plugin tongs's code (also bellows).");
        CHECK(sentence("in_plugin_change", {"tongs"}, "") == "The crash was in plugin tongs's change.");
        CHECK(sentence("plugin_on_stack", {"y"}, "plugin y on the stack") ==
              "Plugin y was on the stack when the game crashed.");
        CHECK(sentence("plugin_on_stack", {"y", "z"}, "") == "Plugin y was on the stack when the game crashed (also z).");
        CHECK(sentence("no_plugin", {}, "no plugin involved") == "No plugin was involved.");
        // The level names no plugin, or the report has no words: nothing to blame.
        CHECK(sentence("in_plugin_code", {}, "") == "The report names no plugin.");
        CHECK(sentence("", {}, "") == "The report names no plugin.");
        // Another Anvil's words are quoted as they are, not put in a sentence of ours.
        CHECK(sentence("later_level", {"z"}, "something new") == "The report says: something new");
        CHECK(sentence("in_plugin_code", {"tongs"}, "in X") == "The report says: in X");
        // Each level is a sentence of its own in the catalogs, not English words in a translated frame.
        anvil::i18n::set_language(anvil::i18n::Language::German);
        CHECK(sentence("plugin_on_stack", {"y"}, "") == "Das Plugin y war beim Absturz auf dem Stack.");
        CHECK(sentence("in_plugin_code", {"tongs"}, "") == "Der Absturz geschah im Code des Plugins tongs.");
        anvil::i18n::set_language(anvil::i18n::Language::English);
    });

    run("notices: safe mode explains itself", [] {
        StartFacts facts;
        facts.safe_mode = true;
        facts.unfinished_starts = 2;
        facts.plugins = {row("anvil-game", "loaded"), row("anvil-overlay", "loaded"), row("tongs", "skipped", "safe mode")};
        auto notices = start_notices(facts);
        const Notice* n = find(notices, "safe-mode");
        CHECK(notices.size() == 1 && n != nullptr && n->kind == NoticeKind::Error);
        if (n != nullptr) CHECK(n->lines[0].find("The last 2 starts") == 0);
        CrashInfo crash;
        crash.is_new = true;
        crash.level = "in_plugin_code";
        crash.summary = "in plugin tongs's code";
        crash.plugins = {"tongs"};
        facts.crash = crash;
        notices = start_notices(facts);
        CHECK(find(notices, "safe-mode")->lines.size() == 2 && find(notices, "crash") != nullptr);
        // A standard plugin left out for the crash still shows as not loaded.
        facts.plugins.push_back(row("anvil-x", "skipped", "safe mode; the crash before it was in plugin anvil-x's code"));
        CHECK(find(start_notices(facts), "not-loaded") != nullptr);
    });

    run("notices: plugins from mods that wait for approval lead to the panel", [] {
        const auto mod_row = [](const std::string& id, const std::string& state, bool can_approve = true) {
            ModRow m;
            m.id = id;
            m.version = "1.0.0";
            m.mod = "Mod " + id;
            m.state = state;
            m.can_approve = can_approve;
            m.approved_version = "0.9.0";
            m.others = "Other Mod";
            return m;
        };
        StartFacts facts;
        PluginRow waiting = row("new", "skipped", "from mod 'Mod new' (local mod): waiting for approval");
        waiting.awaiting_approval = true;
        facts.plugins = {waiting};
        facts.mod_plugins = {mod_row("new", "pending"), mod_row("upd", "update-pending"), mod_row("two", "conflict"),
                             mod_row("ok", "approved", false), mod_row("bad", "invalid", false),
                             mod_row("local", "shadowed", false)};
        auto notices = start_notices(facts);
        const Notice* n = find(notices, "approvals");
        CHECK(notices.size() == 1 && n != nullptr && n->kind == NoticeKind::Info && !n->lasting);  // not "not loaded"
        if (n == nullptr) return;
        CHECK(n->title == "3 plugins from mods wait for your approval" && n->lines.size() == 4);
        CHECK(n->lines[0] == "new 1.0.0 from mod 'Mod new': new");
        CHECK(n->lines[1] == "upd 1.0.0 from mod 'Mod upd': an update; the approved 0.9.0 runs");
        CHECK(n->lines[2] == "two 1.0.0 from mod 'Mod two': the same id as mod(s) Other Mod");
        CHECK(n->lines[3].find("native code") != std::string::npos);
        CHECK(n->actions.size() == 1 && n->actions[0].type == NoticeAction::Type::OpenPanel);
        // Approved in this game: nothing waits any more.
        for (ModRow& m : facts.mod_plugins) m.approved_next = true;
        CHECK(find(start_notices(facts), "approvals") == nullptr);
        // A plugin from a mod that cannot load for another reason is "not loaded".
        facts.plugins[0].awaiting_approval = false;
        CHECK(find(start_notices(facts), "not-loaded") != nullptr);
    });

    run("notices: ironman and multiplayer", [] {
        SessionFacts facts;
        CHECK(session_notices(facts).empty());
        facts.session = true;
        CHECK(session_notices(facts).empty());  // single player
        facts.ironman = true;
        CHECK(session_notices(facts).empty());  // no gameplay plugin acts
        facts.gameplay_active = 1;
        auto notices = session_notices(facts);
        CHECK(notices.size() == 1 && notices[0].id == "ironman" && notices[0].lasting);
        CHECK(!notices[0].closable);  // §13: permanent while a gameplay plugin acts by opt-in
        facts.ironman = false;
        facts.multiplayer = true;
        facts.gameplay_hash = "ABCD-EF01-2345-6789";
        notices = session_notices(facts);
        CHECK(notices.size() == 1 && notices[0].id == "multiplayer" && notices[0].lasting);
        CHECK(notices[0].closable);  // it covers the top right corner for the whole session
        CHECK(notices[0].lines[0].find("ABCD-EF01-2345-6789 (1 active)") != std::string::npos);
        facts.gameplay_active = 0;
        CHECK(session_notices(facts)[0].lines[0].find("No plugin") == 0);
        facts.session = false;
        CHECK(session_notices(facts).empty());
    });

    run("menu badge: clear of the main menu's widgets at the usual resolutions and every scale", [] {
        // The main menu of EU4 1.37.5 as Tongs' ui tree reads it at 1280x720 and
        // game_ui_scale 1: the logos keep to the top corners and the bottom bar to
        // the bottom centre (the 3440x1440 menu agrees), all of it grown by the
        // game's interface scale s, which the game keeps at most height / 720
        // (min_gui 1280x720). The Continue row is there when a save exists.
        const auto widgets = [](float w, float h, float s) {
            return std::vector<BadgeRect>{
                {0, 19 * s, 640 * s, 180 * s},                       // mainmenu_panel_upperleft (the logo)
                {w - 210 * s, 14 * s, 192 * s, 192 * s},             // mainmenu_panel_upperright
                {w / 2 - 542 * s, h - 64 * s, 253 * s, 64 * s},      // version_background
                {w / 2 - 339 * s, h - 140 * s, 683 * s, 140 * s},    // frontend_bottom_bg (Single Player ... Exit)
                {w / 2 + 263 * s, h - 65 * s, 377 * s, 65 * s},      // PersistentSocialButtons
                {w / 2 - 205 * s, h - 205 * s, 375 * s, 90 * s},     // the Continue row
            };
        };
        const auto overlap = [](const BadgeRect& a, const BadgeRect& b) {
            return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
        };
        const float sizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080},
                                  {2560, 1080}, {2560, 1440}, {3440, 1440}};
        int clear = 0, cases = 0;
        for (const auto& size : sizes) {
            for (float s = 1.0f; s <= size[1] / 720.0f + 0.001f; s += 0.25f) {
                for (float k = 0.75f; k <= 2.5f; k += 0.25f) {
                    // At most "Anvil 10.10.10" in Segoe UI 17 px, a dot and the window's padding.
                    const BadgeRect r = menu_badge_rect(size[1], 150.0f * k, 40.0f * k);
                    bool ok = r.x >= 0 && r.y >= 0 && r.x + r.w <= size[0] && r.y + r.h <= size[1];
                    for (const BadgeRect& w : widgets(size[0], size[1], s)) ok = ok && !overlap(r, w);
                    if (!ok) std::printf("  covers the menu at %gx%g, game scale %g, overlay scale %g\n", size[0], size[1], s, k);
                    clear += ok ? 1 : 0;
                    ++cases;
                }
            }
        }
        CHECK(clear == cases && cases > 7 * 8);
        // 12 px from the left edge, halfway down.
        const BadgeRect r = menu_badge_rect(1440, 100, 30);
        CHECK(r.x == 12 && r.y == 705 && r.w == 100 && r.h == 30);
        CHECK(menu_badge_rect(20, 100, 30).y == 12);  // never above the top
    });

    run("menu badge: the tooltip", [] {
        AnvilControlStatus status{};
        status.loaded = 4;
        status.skipped = 1;
        auto lines = menu_badge_tooltip(status, 0, false, "Ctrl+Shift+F12");
        CHECK(lines.size() == 2 && lines[0] == "4 loaded, 0 failed, 1 skipped" &&
              lines[1] == "Click to open the Anvil panel, or press Ctrl+Shift+F12.");
        CHECK(menu_badge_tooltip(status, 0, false, "")[1] == "Click to open the Anvil panel.");
        CHECK(menu_badge_tooltip(status, 0, true, "F11")[1] == "Click to close the Anvil panel.");
        lines = menu_badge_tooltip(status, 2, true, "F11");
        CHECK(lines.size() == 3 && lines[1] == "2 plugins wait for this game version" &&
              lines[2] == "Click to close the Anvil panel.");
    });

    run("release check: the player's word, else the Manager's answer, else the question", [] {
        CHECK(release_plan(ReleaseConsent::On, std::nullopt, false) == ReleasePlan::Check);
        CHECK(release_plan(ReleaseConsent::On, false, false) == ReleasePlan::Check);
        CHECK(release_plan(ReleaseConsent::Off, true, false) == ReleasePlan::None);
        CHECK(release_plan(ReleaseConsent::Ask, true, false) == ReleasePlan::Check);
        CHECK(release_plan(ReleaseConsent::Ask, false, false) == ReleasePlan::None);
        CHECK(release_plan(ReleaseConsent::Ask, std::nullopt, false) == ReleasePlan::Question);
        // A benchmark start neither asks nor checks.
        CHECK(release_plan(ReleaseConsent::On, true, true) == ReleasePlan::None);
        CHECK(release_plan(ReleaseConsent::Ask, std::nullopt, true) == ReleasePlan::None);
    });

    run("release check: settings.toml's options are ReleaseConsent's, ask by default", [] {
        const fs::path file = fs::path(__FILE__).parent_path().parent_path() / "plugins" / "anvil-overlay" / "settings.toml";
        const anvil::SettingsFileResult schema = anvil::load_settings(file);
        CHECK(schema.file.has_value());
        bool found = false;
        for (const anvil::Setting& s : schema.file ? schema.file->settings : std::vector<anvil::Setting>{}) {
            if (s.name != "release_check") continue;
            found = true;
            CHECK(s.type == anvil::SettingType::Enum && s.options.size() == 3 && !s.restart);
            CHECK(s.options.size() == 3 && s.options[static_cast<size_t>(ReleaseConsent::Ask)].name == "ask" &&
                  s.options[static_cast<size_t>(ReleaseConsent::On)].name == "on" &&
                  s.options[static_cast<size_t>(ReleaseConsent::Off)].name == "off");
            CHECK(s.default_value.integer == static_cast<int64_t>(ReleaseConsent::Ask));
        }
        CHECK(found);
    });

    run("release check: the question and the notice of a newer release", [] {
        const Notice question = release_question();
        CHECK(question.id == "release-question" && question.until_closed && question.actions.size() == 2);
        CHECK(question.actions[0].type == NoticeAction::Type::ReleaseCheckOn &&
              question.actions[1].type == NoticeAction::Type::ReleaseCheckOff);

        anvil::net::Release latest{"v1.15.0", "https://github.com/happy-dwarf-team/anvil/releases/tag/v1.15.0"};
        const auto notice = release_notice(latest, "1.14.0", "");
        CHECK(notice.has_value());
        if (notice) {
            CHECK(notice->id == "release" && notice->kind == NoticeKind::Info && !notice->until_closed && !notice->lasting);
            CHECK(notice->title == "Anvil v1.15.0 is available.");
            CHECK(!notice->lines.empty() && notice->lines[0].find("Anvil 1.14.0") != std::string::npos &&
                  notice->lines[0].find("AnvilManager.exe") != std::string::npos);
            CHECK(notice->actions.size() == 2 && notice->actions[0].type == NoticeAction::Type::OpenLink &&
                  notice->actions[0].url == latest.url && notice->actions[1].type == NoticeAction::Type::CopyLink);
        }
        CHECK(!release_notice(latest, "1.15.0", ""));        // the same version
        CHECK(!release_notice(latest, "1.16.0", ""));        // a newer one runs
        CHECK(!release_notice(latest, "1.14.0", "v1.15.0"));  // the player closed it
        CHECK(release_notice(latest, "1.14.0", "v1.14.1"));   // a release after the closed one
        CHECK(!release_notice({"nightly", latest.url}, "1.14.0", ""));
        CHECK(dismissed_tag("v1.15.0\r\n") == "v1.15.0" && dismissed_tag("") == "" &&
              dismissed_tag("  v1.2.0 \nx") == "v1.2.0");
    });

    run("panel key: exact match with modifiers", [] {
        const int64_t ctrl_shift_f12 = 0x7B | 0x100 | 0x200;
        CHECK(key_matches(ctrl_shift_f12, VK_F12, true, true, false));
        CHECK(!key_matches(ctrl_shift_f12, VK_F12, false, true, false));
        CHECK(!key_matches(ctrl_shift_f12, VK_F12, true, true, true));
        CHECK(!key_matches(ctrl_shift_f12, VK_F11, true, true, false));
        CHECK(!key_matches(0, VK_F12, false, false, false));
        CHECK(key_code(VK_F11, false, false, false) == VK_F11);
        CHECK(key_code(VK_CONTROL, false, true, false) == 0 && key_code(VK_LSHIFT, true, false, false) == 0);
        CHECK(key_code('A', false, false, true) == ('A' | 0x400));
    });

    run("control client: the core's service through the overlay's client", [] {
        const fs::path dir = fs::temp_directory_path() / "anvil-overlay-client";
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        fs::create_directories(dir / "anvil");
        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        auto p = std::make_unique<AnvilPlugin>();
        p->manifest = manifest(plugin_toml("anvil-overlay", "", "1.0.0", "early"));
        p->state = "loaded";
        auto schema = anvil::parse_settings_text(
            "[setting.mode]\ntype = \"enum\"\nvalues = [{ name = \"off\", label = \"Off\" }, { name = \"on\" }]\n"
            "default = \"on\"\nlabel = \"Mode\"\nrestart = false\n");
        CHECK(schema.file.has_value());
        p->settings.init(std::move(*schema.file), dir / "anvil" / "data" / "anvil-overlay" / "settings.toml",
                         "anvil-overlay", [](const std::string&) { return std::optional<std::string>(); });
        plugins.push_back(std::move(p));
        std::vector<anvil::Skipped> skipped{{"tongs", "safe mode", dir / "tongs", false},
                                            anvil::skipped_for("bellows", anvil::i18n::Message("requires_missing", {"tongs", "*"}),
                                                               dir / "bellows")};
        anvil::ControlSources sources;
        sources.anvil_dir = dir / "anvil";
        sources.anvil_version = "1.1.0";
        sources.language = "de";
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        anvil::Control control(sources);
        control.loading_done();
        ControlClient client(control.service());
        CHECK(static_cast<bool>(client) && client.status().plugins == 3);
        CHECK(client.language() == anvil::i18n::Language::German && ControlClient().language() == anvil::i18n::Language::English);
        const auto rows = client.plugins();
        CHECK(rows.size() == 3 && rows[0].id == "anvil-overlay" && rows[0].early && rows[0].settings == 1);
        CHECK(rows[1].id == "tongs" && rows[1].state == "skipped" && rows[1].reason == "safe mode");
        // The reason by code reaches the overlay, which words it in the player's language (#158).
        CHECK(rows[2].reason == "requires plugin 'tongs' (*), which is not loaded" && rows[2].why.code == "requires_missing");
        CHECK(rows[2].reason_text() == rows[2].reason && rows[1].reason_text() == "safe mode");
        anvil::i18n::set_language(anvil::i18n::Language::German);
        CHECK(rows[2].reason_text() == "benötigt das Plugin 'tongs' (*), das nicht geladen ist");
        anvil::i18n::set_language(anvil::i18n::Language::English);
        CHECK(client.setting_count("anvil-overlay") == 1);
        const auto options = client.setting_options("anvil-overlay", 0);
        CHECK(options.size() == 2 && options[0].first == "off" && options[0].second == "Off" && options[1].second == "on");
        CHECK(client.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT) == "on");
        CHECK(client.set_text("anvil-overlay", 0, "off").empty());
        CHECK(client.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT) == "off");
        CHECK(!client.set_text("anvil-overlay", 0, "sideways").empty());
        CHECK(client.reset("anvil-overlay", 0).empty());
        CHECK(client.disable_next_start("tongs").empty() && client.plugins()[1].disabled_next);
        CHECK(!client.disable_next_start("missing").empty());
        CHECK(!client.previous_crash().has_value());
        CHECK(client.log_tail(10).empty());
        CHECK(split_list("a, b,,c ") == std::vector<std::string>({"a", "b", "c"}) && split_list("").empty());
        CHECK(!ControlClient().status().loading_done && ControlClient().plugins().empty());
        CHECK(client.mod_plugins().empty() && ControlClient().mod_plugins().empty());
        fs::remove_all(dir, ignored);
    });

    run("control client: plugins from mods, approved and revoked", [] {
        const fs::path dir = fs::temp_directory_path() / "anvil-overlay-client-mods";
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        const fs::path anvil = dir / "eu4" / "anvil";
        const fs::path user = dir / "user";
        const auto write = [](const fs::path& file, const std::string& text) {
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << text;
        };
        write(user / "mod" / "m" / "anvil" / "probe" / "plugin.toml",
              plugin_toml("probe", "authors = [\"A\"]\nrepository = \"https://example.org/p\"\n"));
        write(user / "mod" / "m" / "anvil" / "probe" / "probe.dll", "MZ");
        write(user / "mod" / "m.mod", "name=\"M\"\npath=\"mod/m\"\n");
        write(user / "dlc_load.json", "{\"enabled_mods\":[\"mod/m.mod\"]}");
        const anvil::ModDiscovery mods = anvil::discover_mod_plugins(anvil, user, {});
        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        std::vector<anvil::Skipped> skipped;
        anvil::ControlSources sources;
        sources.anvil_dir = anvil;
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        sources.mods = &mods;
        anvil::Control control(sources);
        control.loading_done();
        ControlClient client(control.service());
        auto rows = client.mod_plugins();
        CHECK(rows.size() == 1 && rows[0].id == "probe" && rows[0].mod == "M" && rows[0].state == "pending");
        CHECK(rows[0].authors == "A" && rows[0].repository == "https://example.org/p" && rows[0].hash.size() == 64);
        CHECK(rows[0].waiting() && rows[0].can_approve && !rows[0].can_revoke);
        CHECK(client.approve_mod_plugin(rows[0]).empty());
        rows = client.mod_plugins();
        CHECK(rows[0].approved_next && !rows[0].waiting() && rows[0].can_revoke);
        CHECK(client.revoke_mod_plugin(rows[0]).empty());
        rows = client.mod_plugins();
        CHECK(!rows[0].approved_next && rows[0].waiting());
        ModRow changed = rows[0];
        changed.hash = std::string(64, '0');
        CHECK(client.approve_mod_plugin(changed) == "the plugin's hash is not the one shown");
        CHECK(!client.revoke_mod_plugin(rows[0]).empty());
        fs::remove_all(dir, ignored);
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
