// anvil-inspector (plugins/anvil-inspector) without the game: its reads through
// a stand-in of anvil-game.world and anvil-game.game, the rows it hands the
// overlay, and its texts. The window and its thread are tried in the game (the
// pull request that added the plugin).
#include "../plugins/anvil-inspector/src/rows.hpp"
#include "../plugins/anvil-inspector/src/snapshot.hpp"
#include "../plugins/anvil-inspector/src/texts.hpp"

#include <anvil/anvil_game.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

void run(const char* name, const std::function<void()>& body) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

namespace ai = anvil_inspector;

// ---- a small world: Moskva (295) of RUS in view, the player SWE -------------

struct Fake {
    bool session = true;
    AnvilGameDate date{sizeof(AnvilGameDate), 1719, 3, 17, 1};
    AnvilResult view = ANVIL_OK;
    std::int32_t view_province = 295;
    std::string player = "SWE";
    std::map<std::int32_t, std::string> flag_names{{0, "rus_reformed"}, {1, "moskva_burnt"}, {2, "met_the_tsar"}};
    int reads = 0;  // calls of the world, to see what a key costs
} g_fake;

AnvilGameDate date(int y, unsigned m, unsigned d) { return AnvilGameDate{sizeof(AnvilGameDate), y, m, d, 1}; }

AnvilResult give_text(const std::string& text, char* out, std::uint32_t capacity, std::uint32_t* length) {
    if (capacity != 0) {
        const std::size_t n = text.size() < capacity ? text.size() : capacity - 1;
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }
    *length = static_cast<std::uint32_t>(text.size());
    return ANVIL_OK;
}

template <class T>
AnvilResult give_list(const std::vector<T>& list, T* out, std::uint32_t capacity, std::uint32_t* count) {
    for (std::uint32_t i = 0; i < capacity && i < list.size(); ++i) out[i] = list[i];
    *count = static_cast<std::uint32_t>(list.size());
    return ANVIL_OK;
}

AnvilResult give_names(const std::vector<std::string>& names, char* out, std::uint32_t capacity, std::uint32_t* length) {
    std::string all;
    for (const std::string& n : names) all += n + '\0';
    std::size_t written = 0;
    for (const std::string& n : names) {
        if (written + n.size() + 1 > capacity) break;
        std::memcpy(out + written, n.c_str(), n.size() + 1);
        written += n.size() + 1;
    }
    *length = static_cast<std::uint32_t>(all.size());
    return ANVIL_OK;
}

bool is(const char* tag, const char* want) { return tag != nullptr && std::strcmp(tag, want) == 0; }

int game_date(AnvilGameDate* out) {
    if (!g_fake.session) return 0;
    *out = g_fake.date;
    return 1;
}

AnvilResult w_player(AnvilGameTag* out) {
    ++g_fake.reads;
    *out = AnvilGameTag{};
    std::memcpy(out->text, g_fake.player.c_str(), g_fake.player.size() < 3 ? g_fake.player.size() : 3);
    return ANVIL_OK;
}
AnvilResult w_province(std::int32_t id, AnvilGameProvince* out) {
    ++g_fake.reads;
    if (id != 295) return ANVIL_ERROR_NOT_FOUND;
    std::memcpy(out->owner.text, "RUS", 4);
    return ANVIL_OK;
}
AnvilResult w_province_name(std::int32_t id, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    return id == 295 ? give_text("Moskva", out, capacity, length) : ANVIL_ERROR_NOT_FOUND;
}
AnvilResult w_country(const char* tag, AnvilGameCountry*) {
    ++g_fake.reads;
    return is(tag, "RUS") || is(tag, "SWE") ? ANVIL_OK : ANVIL_ERROR_NOT_FOUND;
}
AnvilResult w_monarch(const char* tag, std::uint32_t kind, AnvilGameMonarch* out) {
    ++g_fake.reads;
    if (!is(tag, "RUS") || kind == ANVIL_GAME_MONARCH_HEIR) return ANVIL_ERROR_NOT_FOUND;
    out->adm = kind == 0 ? 6 : 2;
    out->dip = kind == 0 ? 4 : 3;
    out->mil = kind == 0 ? 5 : 1;
    return ANVIL_OK;
}
AnvilResult w_monarch_name(const char* tag, std::uint32_t kind, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    if (!is(tag, "RUS")) return ANVIL_ERROR_NOT_FOUND;
    return give_text(kind == 0 ? "Pyotr I" : "Catherine", out, capacity, length);
}
AnvilResult w_monarch_dynasty(const char* tag, std::uint32_t, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    return is(tag, "RUS") ? give_text("Romanov", out, capacity, length) : ANVIL_ERROR_NOT_FOUND;
}
AnvilResult w_flag_name(std::int32_t id, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    const auto it = g_fake.flag_names.find(id);
    return it == g_fake.flag_names.end() ? ANVIL_ERROR_NOT_FOUND : give_text(it->second, out, capacity, length);
}
AnvilResult w_country_flags(const char* tag, AnvilGameFlag* out, std::uint32_t capacity, std::uint32_t* count) {
    ++g_fake.reads;
    if (is(tag, "RUS")) return give_list<AnvilGameFlag>({{0, 0, date(1700, 1, 1)}, {2, 0, date(1700, 8, 19)}}, out, capacity, count);
    return give_list<AnvilGameFlag>({}, out, capacity, count);
}
AnvilResult w_province_flags(std::int32_t, AnvilGameFlag* out, std::uint32_t capacity, std::uint32_t* count) {
    ++g_fake.reads;
    return give_list<AnvilGameFlag>({{1, 0, date(1701, 2, 3)}, {7, 0, date(1650, 1, 1)}}, out, capacity, count);
}
AnvilResult w_monarch_flags(const char* tag, std::uint32_t kind, AnvilGameFlag* out, std::uint32_t capacity,
                            std::uint32_t* count) {
    ++g_fake.reads;
    if (is(tag, "RUS") && kind == 0) return give_list<AnvilGameFlag>({{2, 0, date(1697, 3, 9)}}, out, capacity, count);
    return give_list<AnvilGameFlag>({}, out, capacity, count);
}
AnvilResult w_country_variable(const char* tag, const char* name, std::int32_t* value) {
    ++g_fake.reads;
    if (!is(tag, "RUS")) return ANVIL_ERROR_NOT_FOUND;
    if (std::strcmp(name, "treasury_plan") == 0) *value = 2500;
    else if (std::strcmp(name, "debt") == 0) *value = -1250;
    else return ANVIL_ERROR_NOT_FOUND;
    return ANVIL_OK;
}
AnvilResult w_country_variable_names(const char* tag, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    return give_names(is(tag, "RUS") ? std::vector<std::string>{"treasury_plan", "debt"} : std::vector<std::string>{},
                      out, capacity, length);
}
AnvilResult w_province_variable(std::int32_t, const char*, std::int32_t*) { return ANVIL_ERROR_NOT_FOUND; }
AnvilResult w_province_variable_names(std::int32_t, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    return give_names({}, out, capacity, length);
}
AnvilResult w_country_timed(const char* tag, AnvilGameTimedModifier* out, std::uint32_t capacity, std::uint32_t* count) {
    ++g_fake.reads;
    if (!is(tag, "RUS")) return give_list<AnvilGameTimedModifier>({}, out, capacity, count);
    return give_list<AnvilGameTimedModifier>(
        {{ANVIL_GAME_TIMED_HIDDEN, 0, date(1715, 1, 1), date(1725, 1, 1)},
         {ANVIL_GAME_TIMED_RULER | ANVIL_GAME_TIMED_NO_END | ANVIL_GAME_TIMED_NO_START, 0, {}, {}}},
        out, capacity, count);
}
AnvilResult w_country_timed_keys(const char* tag, char* out, std::uint32_t capacity, std::uint32_t* length) {
    ++g_fake.reads;
    return give_names(is(tag, "RUS") ? std::vector<std::string>{"obstacle_shipbuilding", "tsars_reforms"}
                                     : std::vector<std::string>{},
                      out, capacity, length);
}
AnvilResult w_province_timed(std::int32_t, AnvilGameTimedModifier*, std::uint32_t, std::uint32_t*) {
    ++g_fake.reads;
    return ANVIL_ERROR_NOT_DECLARED;  // as on a build whose port declared them absent
}
AnvilResult w_province_timed_keys(std::int32_t, char*, std::uint32_t, std::uint32_t*) { return ANVIL_ERROR_NOT_DECLARED; }
AnvilResult w_province_view(std::int32_t* province) {
    ++g_fake.reads;
    if (g_fake.view == ANVIL_OK) *province = g_fake.view_province;
    return g_fake.view;
}

AnvilGameService game_service() {
    AnvilGameService g{};
    g.size = sizeof(g);
    g.date = &game_date;
    return g;
}

AnvilGameWorldService world_service() {
    AnvilGameWorldService w{};
    w.size = sizeof(w);
    w.player = &w_player;
    w.province = &w_province;
    w.province_name = &w_province_name;
    w.country = &w_country;
    w.monarch = &w_monarch;
    w.monarch_name = &w_monarch_name;
    w.monarch_dynasty = &w_monarch_dynasty;
    w.flag_name = &w_flag_name;
    w.country_flags = &w_country_flags;
    w.province_flags = &w_province_flags;
    w.monarch_flags = &w_monarch_flags;
    w.country_variable = &w_country_variable;
    w.province_variable = &w_province_variable;
    w.country_variable_names = &w_country_variable_names;
    w.province_variable_names = &w_province_variable_names;
    w.country_timed_modifiers = &w_country_timed;
    w.country_timed_modifier_keys = &w_country_timed_keys;
    w.province_timed_modifiers = &w_province_timed;
    w.province_timed_modifier_keys = &w_province_timed_keys;
    w.province_view = &w_province_view;
    return w;
}

// The texts a source file hands to the table: t("…"), t_("…"), t_.f("…"), N_("…"), texts()("…").
std::set<std::string> used_texts(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    std::set<std::string> out;
    for (const std::string call : {"t(\"", "t_(\"", "t.f(\"", "t_.f(\"", "N_(\"", "texts()(\""}) {
        for (std::size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_' || s[at - 1] == '.')) {
                if (!(s[at - 1] == ')')) continue;
            }
            const std::size_t from = at + call.size();
            out.insert(s.substr(from, s.find('"', from) - from));
        }
    }
    return out;
}

const ai::Row* find(const std::vector<ai::Row>& rows, const std::string& label) {
    for (const ai::Row& r : rows) {
        if (r.label == label) return &r;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const AnvilGameService game = game_service();
    const AnvilGameWorldService world = world_service();
    const ai::Texts en{ai::Lang::en};

    run("snapshot: the province in view, its owner and the player, each read once", [&] {
        const ai::Snapshot s = ai::read_snapshot(game, &world);
        CHECK(s.session && s.date.year == 1719 && s.view == ANVIL_OK && s.scopes.size() == 3);
        if (s.scopes.size() != 3) return;
        const ai::Scope& p = s.scopes[0];
        CHECK(p.role == ai::Role::province && p.province == 295 && p.name == "Moskva" && p.flags.result == ANVIL_OK &&
              p.flags.items.size() == 2 && p.flags.items[0].name == "moskva_burnt" && p.flags.items[1].name == "#7");
        CHECK(p.variables.result == ANVIL_OK && p.variables.items.empty());
        CHECK(p.modifiers.result == ANVIL_ERROR_NOT_DECLARED);
        const ai::Scope& rus = s.scopes[1];
        CHECK(rus.role == ai::Role::owner && rus.tag == "RUS" && rus.flags.items.size() == 2 &&
              rus.flags.items[1].name == "met_the_tsar" && rus.flags.items[1].date.month == 8);
        CHECK(rus.variables.items.size() == 2 && rus.variables.items[0].name == "treasury_plan" &&
              rus.variables.items[0].value == 2500 && rus.variables.items[1].value == -1250);
        CHECK(rus.modifiers.result == ANVIL_OK && rus.modifiers.items.size() == 2 &&
              rus.modifiers.items[0].key == "obstacle_shipbuilding" && rus.modifiers.items[0].end.year == 1725 &&
              rus.modifiers.items[1].key == "tsars_reforms" &&
              (rus.modifiers.items[1].flags & ANVIL_GAME_TIMED_NO_END) != 0);
        CHECK(rus.monarchs[0].result == ANVIL_OK && rus.monarchs[0].name == "Pyotr I" && rus.monarchs[0].dynasty == "Romanov" &&
              rus.monarchs[0].adm == 6 && rus.monarchs[0].flags.items.size() == 1 &&
              rus.monarchs[0].flags.items[0].name == "met_the_tsar");
        CHECK(rus.monarchs[1].result == ANVIL_ERROR_NOT_FOUND && rus.monarchs[2].name == "Catherine");
        CHECK(s.scopes[2].role == ai::Role::player && s.scopes[2].tag == "SWE" && s.scopes[2].flags.items.empty());
    });

    run("snapshot: the owner is the player; no view; no session; an older anvil-game", [&] {
        g_fake.player = "RUS";
        ai::Snapshot s = ai::read_snapshot(game, &world);
        CHECK(s.scopes.size() == 2 && s.scopes[1].role == ai::Role::owner_and_player);
        g_fake.view = ANVIL_ERROR_NOT_FOUND;
        s = ai::read_snapshot(game, &world);
        CHECK(s.view == ANVIL_ERROR_NOT_FOUND && s.scopes.size() == 1 && s.scopes[0].role == ai::Role::player);
        g_fake.view = ANVIL_OK;
        g_fake.player = "SWE";
        g_fake.session = false;
        s = ai::read_snapshot(game, &world);
        CHECK(!s.session && s.scopes.empty());
        g_fake.session = true;
        // anvil-game 1.20: the table ends before the timed modifiers and the view.
        AnvilGameWorldService old = world;
        old.size = static_cast<std::uint32_t>(offsetof(AnvilGameWorldService, country_timed_modifiers));
        s = ai::read_snapshot(game, &old);
        CHECK(s.view == ANVIL_ERROR_UNSUPPORTED && s.scopes.size() == 1 && s.scopes[0].tag == "SWE" &&
              s.scopes[0].modifiers.result == ANVIL_ERROR_UNSUPPORTED && s.scopes[0].flags.result == ANVIL_OK);
        s = ai::read_snapshot(game, nullptr);
        CHECK(s.session && s.scopes.empty());
    });

    run("key: a few calls, and it changes with the date, the view, the player and the session", [&] {
        g_fake.reads = 0;
        const ai::Key a = ai::key_of(game, &world);
        CHECK(g_fake.reads == 2 && a.session == 1 && a.year == 1719 && a.province == 295 && a.player == "SWE");
        CHECK(ai::key_of(game, &world) == a);
        g_fake.date.day = 18;
        CHECK(!(ai::key_of(game, &world) == a));
        g_fake.date.day = 17;
        g_fake.view_province = 1;
        CHECK(!(ai::key_of(game, &world) == a));
        g_fake.view_province = 295;
        g_fake.date.session = 2;  // a save was loaded
        CHECK(!(ai::key_of(game, &world) == a));
        g_fake.date.session = 1;
    });

    run("rows: headings fold, lists and their values, within the panel's limits", [&] {
        const ai::Snapshot s = ai::read_snapshot(game, &world);
        ai::View view;
        std::vector<ai::Row> rows = ai::rows(s, view, en);
        CHECK(rows.size() <= ai::kMaxRows);
        const ai::Row* d = find(rows, "Date");
        CHECK(d != nullptr && d->value == "1719.3.17");
        const ai::Row* refresh = find(rows, "Refresh");
        CHECK(refresh != nullptr && refresh->kind == ai::kButton && ai::action_of(refresh->id) == ai::Action::refresh);
        const ai::Row* province = find(rows, "v Province: Moskva (295)");
        CHECK(province != nullptr && province->kind == ai::kButton);
        CHECK(find(rows, "    v Timed modifiers (-)") != nullptr && find(rows, "        Not available on this game build.") != nullptr);
        const ai::Row* plan = find(rows, "        treasury_plan");
        CHECK(plan != nullptr && plan->value == "2.5");
        const ai::Row* debt = find(rows, "        debt");
        CHECK(debt != nullptr && debt->value == "-1.25");
        const ai::Row* hidden = find(rows, "        obstacle_shipbuilding (hidden)");
        CHECK(hidden != nullptr && hidden->value == "until 1725.1.1");
        const ai::Row* reforms = find(rows, "        tsars_reforms (ruler modifier)");
        CHECK(reforms != nullptr && reforms->value == "no end date");
        const ai::Row* ruler = find(rows, "        Ruler");
        CHECK(ruler != nullptr && ruler->value == "Pyotr I Romanov (6/4/5)");
        const ai::Row* heir = find(rows, "        Heir");
        CHECK(heir != nullptr && heir->value == "none");
        CHECK(find(rows, "            met_the_tsar") != nullptr && find(rows, "v Your country: SWE") != nullptr);
        for (const ai::Row& r : rows) CHECK(r.label.size() <= 4096 && r.value.size() <= 4096 && r.has_value == !r.value.empty());

        // A click on the province's heading folds it, and its rows go.
        CHECK(ai::apply_click(view, province->id));
        rows = ai::rows(s, view, en);
        CHECK(find(rows, "> Province: Moskva (295)") != nullptr && find(rows, "    v Flags (2)") != nullptr);
        CHECK(find(rows, "        moskva_burnt") == nullptr);
        CHECK(!ai::apply_click(view, ai::make_id(ai::Action::refresh, 0)));
        CHECK(ai::apply_click(view, ai::make_id(ai::Action::toggle, ai::heading_key(ai::Role::province, ai::Part::all))));
        CHECK(find(ai::rows(s, view, en), "        moskva_burnt") != nullptr);
    });

    run("rows: a long list stops at its limit; no session; no view", [&] {
        ai::Snapshot s;
        s.session = true;
        s.view = ANVIL_ERROR_NOT_FOUND;
        ai::Scope c;
        c.role = ai::Role::player;
        c.tag = "SWE";
        c.flags.result = ANVIL_OK;
        for (int i = 0; i < 500; ++i) c.flags.items.push_back(ai::Flag{"flag_" + std::to_string(i), date(1444, 11, 11)});
        c.variables.result = ANVIL_OK;
        c.modifiers.result = ANVIL_ERROR_INTERNAL;
        for (int k = 0; k < 3; ++k) c.monarchs[k].result = ANVIL_ERROR_NOT_DECLARED;
        s.scopes = {c, c, c};
        const std::vector<ai::Row> rows = ai::rows(s, ai::View{}, en);
        CHECK(rows.size() <= ai::kMaxRows && find(rows, "        440 more") != nullptr);
        CHECK(find(rows, "Open a province's view to see it and its owner here.") != nullptr);
        CHECK(find(rows, "        Could not be read (" + std::to_string(static_cast<int>(ANVIL_ERROR_INTERNAL)) + ").") != nullptr);
        ai::Snapshot none;
        CHECK(ai::rows(none, ai::View{}, en).size() == 1 && ai::rows(none, ai::View{}, en)[0].label == "No game is running.");
        CHECK(ai::milli_text(0) == "0" && ai::milli_text(1) == "0.001" && ai::milli_text(-1000) == "-1" &&
              ai::milli_text(123450) == "123.45" && ai::milli_text(-500) == "-0.5");
    });

    run("texts: every text has every language, with the same placeholders, and each is used", [&] {
        std::size_t count = 0;
        const ai::TextEntry* entries = ai::text_entries(count);
        const auto holes = [](std::string_view s) {
            std::set<std::string> out;
            for (std::size_t i = 0; i + 2 < s.size(); ++i) {
                if (s[i] == '{' && s[i + 2] == '}' && s[i + 1] >= '0' && s[i + 1] <= '9') out.insert(std::string(s.substr(i, 3)));
            }
            return out;
        };
        std::set<std::string> known;
        for (std::size_t i = 0; i < count; ++i) {
            known.insert(entries[i].english);
            for (const char* tr : entries[i].translated) {
                if (tr == nullptr || *tr == '\0' || holes(tr) != holes(entries[i].english)) {
                    std::printf("  translation of '%s': %s\n", entries[i].english, tr == nullptr ? "(none)" : tr);
                    CHECK(false);
                }
            }
        }
        const std::filesystem::path src = std::filesystem::path(ANVIL_SOURCE_DIR) / "plugins" / "anvil-inspector" / "src";
        std::set<std::string> used;
        for (const char* file : {"rows.cpp", "panel.cpp"}) {
            for (const std::string& text : used_texts(src / file)) used.insert(text);
        }
        CHECK(used.size() > 20);
        for (const std::string& text : used) {
            if (!known.count(text)) std::printf("  not in texts.cpp: %s\n", text.c_str());
            CHECK(known.count(text) == 1);
        }
        for (const std::string& text : known) {
            if (!used.count(text)) std::printf("  not used: %s\n", text.c_str());
            CHECK(used.count(text) == 1);
        }
        CHECK(std::string(ai::Texts{ai::Lang::pl}("Refresh")) == "Odśwież" &&
              ai::Texts{ai::Lang::de}.f("{0} more", {"3"}) == "3 weitere" && ai::lang_of("zh-Hant") == ai::Lang::zh_hant &&
              ai::lang_of("xx") == ai::Lang::en);
    });

    (void)argc;
    (void)argv;
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
