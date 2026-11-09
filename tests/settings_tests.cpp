// settings.toml (design §5.3, §11): the schema, key bindings, the player's
// values, ANVIL_<ID>_<NAME> overrides and changes while the game runs.
#include "core/plugin_settings.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"
#include "ui/settings_order.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace anvil;

constexpr const char* kSchema = R"toml(# a comment
[setting.enabled]
type = "bool"
default = true
label = "Enabled"
restart = false

[setting.budget_mb]
type = "int"
default = 1024
min = 64
max = 8192
step = 64
label = "RAM prefetch budget (MB)"
description = "RAM for unpacked archive members."
group = "RAM prefetch"

[setting.scale]
type = "float"
default = 1
min = 0.5
max = 2.5
label = "Scale"
restart = false

[setting.mode]
type = "enum"
values = [{ name = "off", label = "Off" }, { name = "verify", label = "Verify only" }, { name = "on" }]
default = "verify"
label = "Mode"
restart = false

[setting.greeting]
type = "string"
default = "hello"
label = "Greeting"
restart = false

[setting.panel_key]
type = "key"
default = "Ctrl+Shift+F12"
label = "Panel key"
)toml";

SettingsFile schema() {
    const SettingsFileResult r = parse_settings_text(kSchema);
    if (!r.file) std::printf("  schema error: %s\n", r.error.c_str());
    return r.file ? *r.file : SettingsFile{};
}

// The error of a settings.toml with one [setting.x] table of `body`.
SettingsFileResult one(const std::string& body) { return parse_settings_text("[setting.x]\n" + body); }

bool fails(const std::string& body, const std::string& part, uint32_t line = 0) {
    const SettingsFileResult r = one(body);
    const bool ok = !r.file && r.error.find(part) != std::string::npos && (line == 0 || r.line == line);
    if (!ok) std::printf("  expected \"%s\" at line %u, got \"%s\" at line %u\n", part.c_str(), line, r.error.c_str(), r.line);
    return ok;
}

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / ("anvil_settings_tests_" + std::to_string(GetCurrentProcessId())) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct Recorder {
    std::vector<AnvilSetting> calls;
    static void callback(AnvilSetting setting, void* context) { static_cast<Recorder*>(context)->calls.push_back(setting); }
};

PluginSettings::Environment environment(std::map<std::string, std::string> values) {
    return [values](const std::string& name) -> std::optional<std::string> {
        const auto it = values.find(name);
        if (it == values.end()) return std::nullopt;
        return it->second;
    };
}

}  // namespace

void settings_tests() {
    run("settings: every type, in declaration order", [] {
        const SettingsFile f = schema();
        CHECK(f.settings.size() == 6);
        if (f.settings.size() != 6) return;
        const Setting& enabled = f.settings[0];
        CHECK(enabled.name == "enabled" && enabled.id == 0 && enabled.line == 2 && enabled.type == SettingType::Bool);
        CHECK(enabled.default_value.integer == 1 && !enabled.restart && enabled.label == "Enabled");
        const Setting& budget = *f.find("budget_mb");
        CHECK(budget.id == 1 && budget.type == SettingType::Int && budget.default_value.integer == 1024);
        CHECK(budget.min->integer == 64 && budget.max->integer == 8192 && budget.step->integer == 64);
        CHECK(budget.restart && budget.group == "RAM prefetch" && budget.description == "RAM for unpacked archive members.");
        const Setting& scale = *f.find("scale");
        CHECK(scale.type == SettingType::Float && scale.default_value.real == 1.0 && scale.min->real == 0.5);
        const Setting& mode = *f.find("mode");
        CHECK(mode.type == SettingType::Enum && mode.options.size() == 3 && mode.default_value.integer == 1);
        CHECK(mode.options[1].label == "Verify only" && mode.options[2].label == "on");  // label defaults to the name
        CHECK(f.find("greeting")->default_value.text == "hello");
        const Setting& key = *f.find("panel_key");
        CHECK(key.id == 5 && key.default_value.integer == (kKeyCtrl | kKeyShift | 0x7B));
        CHECK(f.get(6) == nullptr && f.find("nope") == nullptr);
        CHECK(parse_settings_text("").file->settings.empty());  // a plugin may declare none
    });

    run("settings: the overlay and the Manager show each group once", [] {
        using Order = std::vector<std::size_t>;
        using anvil::ui::settings_display_order;
        CHECK(settings_display_order({}).empty());
        CHECK(settings_display_order({"", "", ""}) == (Order{0, 1, 2}));
        CHECK(settings_display_order({"A", "A", "A"}) == (Order{0, 1, 2}));
        // Interleaved groups: each once, in the order of its first setting; file order inside a group.
        CHECK(settings_display_order({"Start-up", "Loading", "Start-up", "Simulation", "Loading", "Simulation", "Start-up"}) ==
              (Order{0, 2, 6, 1, 4, 3, 5}));
        // Ungrouped settings come first, wherever they stand in the file.
        CHECK(settings_display_order({"A", "", "B", "A", ""}) == (Order{1, 4, 0, 3, 2}));
        // The schema's groups: RAM prefetch (budget_mb), after the ungrouped ones.
        const SettingsFile f = schema();
        std::vector<std::string_view> groups;
        for (const Setting& s : f.settings) groups.emplace_back(s.group);
        CHECK(settings_display_order(groups) == (Order{0, 2, 3, 4, 5, 1}));
    });

    run("settings: the schema refuses what it does not know", [] {
        CHECK(fails("type = \"int\"\ndefault = 1\nlabel = \"X\"\ncolour = 1\n", "setting x: unknown key 'colour'", 5));
        CHECK(fails("default = 1\nlabel = \"X\"\n", "missing 'type'", 1));
        CHECK(fails("type = \"number\"\ndefault = 1\nlabel = \"X\"\n", "type must be bool, int, float, enum, string or key", 2));
        CHECK(fails("type = \"int\"\nlabel = \"X\"\n", "missing 'default'", 1));
        CHECK(fails("type = \"int\"\ndefault = 1\n", "missing 'label'", 1));
        CHECK(fails("type = \"int\"\ndefault = 1\nlabel = \"\"\n", "label must not be empty", 4));
        CHECK(fails("type = \"int\"\ndefault = 1.5\nlabel = \"X\"\n", "default must be an integer", 3));
        CHECK(fails("type = \"bool\"\ndefault = 1\nlabel = \"X\"\n", "default must be true or false", 3));
        CHECK(fails("type = \"float\"\ndefault = nan\nlabel = \"X\"\n", "default must be a finite number", 3));
        CHECK(fails("type = \"int\"\ndefault = 1\nmin = 5\nlabel = \"X\"\n", "default: 1 is below the minimum 5", 3));
        CHECK(fails("type = \"int\"\ndefault = 9\nmax = 5\nlabel = \"X\"\n", "default: 9 is above the maximum 5", 3));
        CHECK(fails("type = \"int\"\ndefault = 1\nmin = 5\nmax = 2\nlabel = \"X\"\n", "max 2 is below min 5", 5));
        CHECK(fails("type = \"float\"\ndefault = 1\nstep = 0\nlabel = \"X\"\n", "step must be greater than 0", 4));
        CHECK(fails("type = \"bool\"\ndefault = true\nmin = 0\nlabel = \"X\"\n", "min applies only to int and float settings", 4));
        CHECK(fails("type = \"int\"\ndefault = 1\nvalues = []\nlabel = \"X\"\n", "values apply only to enum settings", 4));
        CHECK(fails("type = \"enum\"\ndefault = \"a\"\nlabel = \"X\"\n", "an enum setting needs 'values'", 1));
        CHECK(fails("type = \"enum\"\nvalues = []\ndefault = \"a\"\nlabel = \"X\"\n", "values must be a non-empty array", 3));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\" }, { name = \"a\" }]\ndefault = \"a\"\nlabel = \"X\"\n",
                    "values: 'a' is declared twice"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"A\" }]\ndefault = \"A\"\nlabel = \"X\"\n",
                    "values: name 'A' must match [a-z][a-z0-9_]*"));
        CHECK(fails("type = \"enum\"\nvalues = [{ label = \"A\" }]\ndefault = \"a\"\nlabel = \"X\"\n", "an option without 'name'"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\", hint = 1 }]\ndefault = \"a\"\nlabel = \"X\"\n",
                    "values: unknown key 'hint'"));
        CHECK(fails("type = \"enum\"\nvalues = [{ name = \"a\" }, { name = \"b\" }]\ndefault = \"c\"\nlabel = \"X\"\n",
                    "default: \"c\" is not one of a, b", 4));
        CHECK(fails("type = \"key\"\ndefault = \"Ctrl+Hyper+K\"\nlabel = \"X\"\n", "is not a key binding", 3));
        CHECK(fails("type = \"bool\"\ndefault = true\nlabel = \"X\"\nrestart = \"yes\"\n", "restart must be true or false", 5));
        const SettingsFileResult name = parse_settings_text("[setting.Bad-Name]\ntype = \"bool\"\n");
        CHECK(!name.file && name.error.find("the name must match") != std::string::npos && name.line == 1);
        const SettingsFileResult top = parse_settings_text("version = 1\n");
        CHECK(!top.file && top.error == "settings.toml: unknown key 'version'" && top.line == 1);
        const SettingsFileResult syntax = parse_settings_text("[setting.x\n");
        CHECK(!syntax.file && syntax.error.starts_with("settings.toml: ") && syntax.line == 1);
    });

    run("settings: key bindings", [] {
        CHECK(parse_key("Ctrl+Shift+F12") == (kKeyCtrl | kKeyShift | 0x7B));
        CHECK(parse_key("shift+ctrl+f12") == parse_key("Ctrl+Shift+F12"));
        CHECK(parse_key("control+alt+delete") == (kKeyCtrl | kKeyAlt | 0x2E));
        CHECK(parse_key("A") == 0x41 && parse_key("7") == 0x37 && parse_key("F24") == 0x87 && parse_key("Numpad5") == 0x65);
        CHECK(parse_key("") == 0);
        CHECK(!parse_key("F25") && !parse_key("F0") && !parse_key("Ctrl") && !parse_key("Ctrl+") && !parse_key("Ctrl+Ctrl+A"));
        CHECK(!parse_key("+") && !parse_key("A+B") && !parse_key("Hyper+A"));
        CHECK(key_name(kKeyCtrl | kKeyShift | 0x7B) == "Ctrl+Shift+F12" && key_name(0x41 | kKeyAlt) == "Alt+A");
        CHECK(key_name(0) == "" && key_name(0x07) == "" && key_name(0x10000) == "");
        CHECK(key_name(*parse_key("backquote")) == "Backquote");
    });

    run("settings: values from text and as TOML", [] {
        const SettingsFile f = schema();
        if (f.settings.size() != 6) return;
        std::string error;
        CHECK(parse_setting_text(f.settings[0], "FALSE", error)->integer == 0);
        CHECK(parse_setting_text(f.settings[0], "1", error)->integer == 1);
        CHECK(!parse_setting_text(f.settings[0], "yes", error) && error == "\"yes\" is not true or false");
        error.clear();
        CHECK(parse_setting_text(f.settings[1], "2048", error)->integer == 2048);
        CHECK(!parse_setting_text(f.settings[1], "99999", error) && error == "99999 is above the maximum 8192");
        error.clear();
        CHECK(!parse_setting_text(f.settings[1], "12a", error) && error == "\"12a\" is not an integer");
        error.clear();
        CHECK(parse_setting_text(f.settings[2], "2.25", error)->real == 2.25);
        CHECK(!parse_setting_text(f.settings[2], "0.25", error) && error == "0.25 is below the minimum 0.5");
        error.clear();
        CHECK(parse_setting_text(f.settings[3], "on", error)->integer == 2);
        CHECK(!parse_setting_text(f.settings[3], "On", error) && error == "\"On\" is not one of off, verify, on");
        error.clear();
        CHECK(parse_setting_text(f.settings[4], "", error)->text.empty());
        CHECK(parse_setting_text(f.settings[5], "Alt+A", error)->integer == (kKeyAlt | 0x41));
        CHECK(setting_toml(f.settings[0], {1, 0, {}}) == "true" && setting_toml(f.settings[2], {0, 2.0, {}}) == "2.0");
        CHECK(setting_toml(f.settings[2], {0, 0.1, {}}) == "0.1" && setting_toml(f.settings[3], {2, 0, {}}) == "\"on\"");
        CHECK(setting_toml(f.settings[4], {0, 0, "a \"b\"\n\\"}) == "\"a \\\"b\\\"\\n\\\\\"");
        CHECK(setting_toml(f.settings[5], {kKeyCtrl | 0x70, 0, {}}) == "\"Ctrl+F1\"");
        CHECK(setting_environment_name("ram-prefetch", "budget_mb") == "ANVIL_RAM_PREFETCH_BUDGET_MB");
    });

    run("settings: the player's file keeps only what differs, and invalid values fall back", [] {
        const SettingsFile f = schema();
        if (f.settings.size() != 6) return;
        const PlayerSettings p = parse_player_settings(f, "budget_mb = 99999\nscale = 2\nmode = \"on\"\ncolour = 3\n"
                                                          "enabled = \"no\"\ngreeting = \"hi\"\npanel_key = \"\"\n");
        CHECK(!p.values[1] && p.values[2]->real == 2.0 && p.values[3]->integer == 2 && !p.values[0]);
        CHECK(p.values[4]->text == "hi" && p.values[5]->integer == 0);
        CHECK(p.warnings.size() == 3);
        if (p.warnings.size() == 3) {
            CHECK(p.warnings[0] == "line 1: budget_mb: 99999 is above the maximum 8192; the default 1024 applies");
            CHECK(p.warnings[1] == "line 4: 'colour' is not a setting of this plugin; ignored");
            CHECK(p.warnings[2] == "line 5: enabled must be true or false; the default true applies");
        }
        const PlayerSettings broken = parse_player_settings(f, "budget_mb = \n");
        CHECK(broken.warnings.size() == 1 && broken.warnings[0].starts_with("line 1: ") && !broken.values[1]);

        std::vector<SettingValue> values;
        for (const auto& s : f.settings) values.push_back(s.default_value);
        CHECK(player_settings_text(f, values).empty());
        values[1].integer = 2048;
        values[3].integer = 0;
        const std::string text = player_settings_text(f, values);
        CHECK(text.starts_with("# ") && text.ends_with("\nbudget_mb = 2048\nmode = \"off\"\n"));
        const PlayerSettings again = parse_player_settings(f, text);
        CHECK(again.warnings.empty() && again.values[1]->integer == 2048 && again.values[3]->integer == 0 && !again.values[0]);

        const auto dir = scratch("player");
        const auto file = dir / "data" / "x" / "settings.toml";
        std::string error;
        CHECK(write_player_settings(f, values, file, error) && read(file) == text && error.empty());
        CHECK(!std::filesystem::exists(file.string() + ".tmp"));
        CHECK(load_player_settings(f, file).values[1]->integer == 2048);
        for (const auto& s : f.settings) values[s.id] = s.default_value;
        CHECK(write_player_settings(f, values, file, error) && !std::filesystem::exists(file));
        CHECK(load_player_settings(f, file).warnings.empty());  // a missing file: the defaults
    });

    run("settings: the store starts from defaults, the player's file and the environment", [] {
        const auto dir = scratch("store");
        const auto file = dir / "settings.toml";
        write(file, "budget_mb = 2048\nscale = 9\n");
        PluginSettings store;
        const auto notes = store.init(schema(), file, "ram-prefetch",
                                      environment({{"ANVIL_RAM_PREFETCH_MODE", "off"}, {"ANVIL_RAM_PREFETCH_ENABLED", "maybe"}}));
        CHECK(notes.size() == 3);
        if (notes.size() == 3) {
            CHECK(notes[0].level == ANVIL_LOG_WARNING &&
                  notes[0].message == "data/ram-prefetch/settings.toml, line 2: scale: 9.0 is above the maximum 2.5; "
                                      "the default 1.0 applies");
            CHECK(notes[1].level == ANVIL_LOG_WARNING &&
                  notes[1].message == "ANVIL_RAM_PREFETCH_ENABLED: \"maybe\" is not true or false; ignored");
            CHECK(notes[2].level == ANVIL_LOG_INFO &&
                  notes[2].message == "setting mode = \"off\" from ANVIL_RAM_PREFETCH_MODE (environment)");
        }
        CHECK(store.warnings().size() == 2);
        CHECK(store.get_bool(0) == 1 && store.get_int(0) == 1);
        CHECK(store.get_int(1) == 2048 && store.get_float(1) == 2048.0 && std::string(store.get_string(1)).empty());
        CHECK(store.get_float(2) == 1.0 && store.get_int(2) == 0);
        CHECK(store.get_int(3) == 0 && std::string(store.get_string(3)) == "off");
        CHECK(store.saved(3)->integer == 1 && store.current(3)->integer == 0);  // the override is not saved
        CHECK(std::string(store.get_string(4)) == "hello" && store.get_int(4) == 0 && store.get_bool(4) == 0);
        CHECK(store.get_int(5) == (kKeyCtrl | kKeyShift | 0x7B) && std::string(store.get_string(5)) == "Ctrl+Shift+F12");
        CHECK(store.get_int(99) == 0 && store.get_float(99) == 0.0 && std::string(store.get_string(99)).empty());
        CHECK(!store.current(99) && !store.saved(99));
    });

    run("settings: a change saves, applies restart = false at once and calls the subscribers", [] {
        const auto dir = scratch("change");
        const auto file = dir / "data" / "p" / "settings.toml";
        PluginSettings store;
        CHECK(store.init(schema(), file, "p", environment({})).empty());
        Recorder a, b;
        CHECK(store.subscribe(4, &Recorder::callback, &a) == ANVIL_OK);
        CHECK(store.subscribe(4, &Recorder::callback, &b) == ANVIL_OK);
        CHECK(store.subscribe(1, &Recorder::callback, &a) == ANVIL_ERROR_WRONG_KIND);  // restart = true
        CHECK(store.subscribe(42, &Recorder::callback, &a) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(store.subscribe(4, nullptr, &a) == ANVIL_ERROR_INVALID_ARGUMENT);

        const char* before = store.get_string(4);
        auto c = store.set_text(4, "good day");
        CHECK(c.result == ANVIL_OK && c.applied && c.saved && c.notified == 2 && c.error.empty());
        CHECK(std::string(store.get_string(4)) == "good day" && std::string(before) == "hello");  // the old text stays valid
        CHECK(a.calls == std::vector<AnvilSetting>{4} && b.calls == std::vector<AnvilSetting>{4});
        CHECK(read(file).ends_with("\ngreeting = \"good day\"\n"));

        c = store.set_text(1, "4096");  // restart = true: saved, applies at the next start
        CHECK(c.result == ANVIL_OK && !c.applied && c.saved && c.notified == 0);
        CHECK(store.get_int(1) == 1024 && store.saved(1)->integer == 4096 && store.current(1)->integer == 1024);
        CHECK(read(file).ends_with("\nbudget_mb = 4096\ngreeting = \"good day\"\n"));

        c = store.set_text(1, "1");
        CHECK(c.result == ANVIL_ERROR_INVALID_ARGUMENT && c.error == "budget_mb: 1 is below the minimum 64" && !c.saved);
        CHECK(store.set(3, {7, 0, {}}).result == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(store.set(42, {}).result == ANVIL_ERROR_NOT_DECLARED);
        CHECK(store.set_text(5, "Alt+F4").applied == false && store.get_int(5) == (kKeyCtrl | kKeyShift | 0x7B));

        // Back to the defaults: the file goes away, and the next start reads them.
        CHECK(store.reset(4).notified == 2 && store.reset(1).saved && store.reset(5).saved);
        CHECK(!std::filesystem::exists(file) && std::string(store.get_string(4)) == "hello");
        store.drop_subscriptions();
        CHECK(store.set_text(4, "bye").notified == 0 && a.calls.size() == 2);

        PluginSettings next;
        next.init(schema(), file, "p", environment({}));
        CHECK(std::string(next.get_string(4)) == "bye");
    });

    run("settings: the process environment", [] {
        SetEnvironmentVariableW(L"ANVIL_ENV_TEST_BUDGET_MB", L"512");
        PluginSettings store;
        const auto notes = store.init(schema(), scratch("env") / "none.toml", "env-test");
        SetEnvironmentVariableW(L"ANVIL_ENV_TEST_BUDGET_MB", nullptr);
        CHECK(store.get_int(1) == 512 && notes.size() == 1 &&
              notes[0].message == "setting budget_mb = 512 from ANVIL_ENV_TEST_BUDGET_MB (environment)");
    });
}
