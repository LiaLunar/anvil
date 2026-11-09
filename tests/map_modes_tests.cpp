// anvil-map (plugins/anvil-map): the registry of plugin modes, their borrowed
// numbers, colour buffers and the text rules, without the game. The hooks
// are tried in the game (docs/spec/map-modes-v1.md §9).
#include "test_support.hpp"

#include "../plugins/anvil-map/src/modes.hpp"
#include "../plugins/anvil-map/src/text.hpp"

#include <anvil/anvil_map.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace anvil_map;

AnvilMapModeInfo info(const char* id, const char* label = "Cores", std::uint32_t tab = 0) {
    AnvilMapModeInfo i{};
    i.size = sizeof(i);
    i.tab = tab;
    i.id = id;
    i.label = label;
    return i;
}

void id_tests() {
    run("map modes: ids are <plugin-id>.<name>", [] {
        CHECK(valid_id("player-cores.cores"));
        CHECK(valid_id("a.b_c-1"));
        CHECK(!valid_id("cores"));
        CHECK(!valid_id(".cores"));
        CHECK(!valid_id("player-cores."));
        CHECK(!valid_id("Player.cores"));
        CHECK(!valid_id("player.co.res"));
        CHECK(!valid_id("player.cores!"));
        CHECK(valid_id("p." + std::string(32, 'x')));
        CHECK(!valid_id("p." + std::string(33, 'x')));
    });
}

void registry_tests() {
    run("map modes: numbers are lent from the pool in order, 80-83 first", [] {
        Registry r;
        std::vector<std::int32_t> numbers;
        for (int k = 0; k < 13; ++k) {
            const std::string id = "p.m" + std::to_string(k);
            const AnvilMapModeInfo i = info(id.c_str());
            const AnvilMapMode* m = nullptr;
            std::string why;
            CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
            numbers.push_back(m->number);
            CHECK(r.by_number(m->number) == m);
        }
        CHECK((numbers == std::vector<std::int32_t>{80, 81, 82, 83, 50, 57, 58, 59, 60, 66, 68, 71, 84}));
        const AnvilMapModeInfo extra = info("p.fourteenth");
        const AnvilMapMode* m = nullptr;
        std::string why;
        CHECK(r.add(&extra, false, &m, why) == ANVIL_ERROR_UNSUPPORTED && m == nullptr);
        CHECK(r.by_number(1) == nullptr && r.by_number(-1) == nullptr && r.by_number(85) == nullptr);
    });

    run("map modes: a bad registration is refused and lends nothing", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        CHECK(r.add(nullptr, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo small = info("p.a");
        small.size = 8;
        CHECK(r.add(&small, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo empty = info("p.a", "");
        CHECK(r.add(&empty, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        const std::string long_label(65, 'x');
        AnvilMapModeInfo too_long = info("p.a", long_label.c_str());
        CHECK(r.add(&too_long, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo bad_utf8 = info("p.a", "\xC3(");
        CHECK(r.add(&bad_utf8, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo tab = info("p.a", "A", 5);
        CHECK(r.add(&tab, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilMapModeInfo sprite = info("p.a");
        sprite.sprite = "GFX bad";
        CHECK(r.add(&sprite, false, &m, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(r.size() == 0);
        const AnvilMapModeInfo good = info("p.a");
        CHECK(r.add(&good, false, &m, why) == ANVIL_OK && m->number == 80);
        CHECK(r.add(&good, false, &m, why) == ANVIL_ERROR_ALREADY_EXISTS);
        CHECK(r.size() == 1);
    });

    run("map modes: tabs keep the order of registration; the default is the last tab", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        const AnvilMapModeInfo a = info("p.a"), b = info("p.b", "B", 2), c = info("p.c", "C", 4);
        CHECK(r.add(&a, false, &m, why) == ANVIL_OK && m->tab == 4);
        CHECK(r.add(&b, false, &m, why) == ANVIL_OK && m->tab == 2);
        CHECK(r.add(&c, false, &m, why) == ANVIL_OK);
        std::int32_t out[13] = {};
        CHECK(r.tab_numbers(4, out, 13) == 2 && out[0] == 80 && out[1] == 82);
        CHECK(r.tab_numbers(2, out, 13) == 1 && out[0] == 81);
        CHECK(r.tab_numbers(1, out, 13) == 0);
        CHECK(r.tab_numbers(4, out, 1) == 1);
    });

    run("map modes: labels are written as the game draws text", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        AnvilMapModeInfo i = info("p.a", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");  // "Zażółć"
        i.description = "Cores \xE2\x80\x93 yours";                                   // en dash
        CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
        CHECK(m->label == "Za?\xF3??");  // of "żółć" only ó is in Windows-1252
        CHECK(m->description == "Cores \x96 yours");
        AnvilMapModeInfo j = info("p.b", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
        CHECK(r.add(&j, true, &m, why) == ANVIL_OK && m->label == "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
    });
}

void color_tests() {
    run("map modes: a colour buffer is checked and copied", [] {
        Colors c;
        const std::vector<std::uint32_t> values = {ANVIL_MAP_RGB(1, 2, 3), ANVIL_MAP_RGB(255, 0, 0),
                                                   ANVIL_MAP_NO_COLOR, ANVIL_MAP_RGB(0, 0, 255)};
        CHECK(make_colors(values.data(), 4, 10, c) == ANVIL_OK);
        CHECK(c.values.size() == 4 && c.values[0] == ANVIL_MAP_NO_COLOR && c.values[1] == 0xFF0000u &&
              c.values[2] == ANVIL_MAP_NO_COLOR && !c.complete);
        CHECK(make_colors(values.data(), 11, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_colors(nullptr, 1, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(make_colors(nullptr, 0, 10, c) == ANVIL_OK && c.values.empty() && !c.complete);
        const std::uint32_t bad[] = {0, 0x01000000u};
        CHECK(make_colors(bad, 2, 10, c) == ANVIL_ERROR_INVALID_ARGUMENT);
        const std::uint32_t full[] = {0x123456, 1, 2};  // province 0 counts for nothing
        CHECK(make_colors(full, 3, 3, c) == ANVIL_OK && c.complete && c.values[0] == ANVIL_MAP_NO_COLOR);
        const std::uint32_t none[] = {0, 1, 0xFFABCDEFu};
        CHECK(make_colors(none, 3, 3, c) == ANVIL_OK && !c.complete && c.values[2] == ANVIL_MAP_NO_COLOR);
    });

    run("map modes: a new buffer is a new generation; the end of a session drops it", [] {
        Registry r;
        const AnvilMapMode* m = nullptr;
        std::string why;
        const AnvilMapModeInfo i = info("p.a");
        CHECK(r.add(&i, false, &m, why) == ANVIL_OK);
        auto* mode = const_cast<AnvilMapMode*>(m);
        const std::uint64_t g = mode->generation.load();
        set_colors(*mode, std::make_shared<Colors>());
        CHECK(mode->generation.load() == g + 1 && mode->snapshot() != nullptr);
        keep_text(*mode, 7, "Yours");
        CHECK(kept_text(*mode, 7) == "Yours" && kept_text(*mode, 8).empty());
        r.clear_session();
        CHECK(mode->snapshot() == nullptr && mode->generation.load() == g + 2 && kept_text(*mode, 7).empty());
    });
}

void text_tests() {
    run("map modes: UTF-8 is cut at a whole character and at the first bad byte", [] {
        CHECK(utf8_prefix("abc") == "abc");
        CHECK(utf8_prefix("a\xC5\x82", 2) == "a");
        CHECK(utf8_prefix("a\xC5\x82", 3) == "a\xC5\x82");
        CHECK(utf8_prefix("ok\xFFno") == "ok");
        CHECK(utf8_prefix("\xED\xA0\x80") == "");          // a surrogate
        CHECK(utf8_prefix("\xC0\xAF") == "");              // overlong
        CHECK(utf8_prefix("\xF0\x9F\x98\x80!") == "\xF0\x9F\x98\x80!");
        CHECK(valid_utf8("Kraków") && !valid_utf8("\x80"));
    });

    run("map modes: text for the game is literal, in UTF-8 or Windows-1252", [] {
        CHECK(game_text("Line 1\nLine 2", false) == "Line 1\nLine 2");
        CHECK(game_text("tab\there\r", false) == "tabhere");
        CHECK(game_text("\xE2\x82\xAC 5", false) == "\x80 5");
        CHECK(game_text("\xE2\x82\xAC 5", true) == "\xE2\x82\xAC 5");
        CHECK(game_text("\xF0\x9F\x98\x80", false) == "?");
        CHECK(game_text("\xC2\xA7Rred", false) == "?Rred");
        CHECK(game_text("\xC2\xA7Rred", true) == "?Rred");
        CHECK(game_text("$PROV$", false) == "$PROV$");
        CHECK(game_text("ok\xFFtail", true) == "ok");
        CHECK(windows_1252(0x153) == 0x9C && windows_1252(0x81) == 0 && windows_1252(0xE9) == 0xE9);
    });
}

}  // namespace

int main() {
    id_tests();
    registry_tests();
    color_tests();
    text_tests();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
