// anvil.control (src/control/anvil_control.h, design §15): editing anvil.toml
// for "disable at the next start", reading crash reports back, safe mode's
// exclusions, the log tail and the service table the overlay calls.
#include "core/anvil_toml_edit.hpp"
#include "core/control.hpp"
#include "core/crash_history.hpp"
#include "core/discovery.hpp"
#include "core/host.hpp"
#include "core/log.hpp"
#include "core/mod_plugins.hpp"
#include "core/safe_mode.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

std::string edit(std::string_view text, std::string_view id) {
    std::string error;
    const auto out = with_disabled_plugin(text, id, error);
    if (!out) std::printf("  edit error: %s\n", error.c_str());
    return out.value_or("<error>");
}

bool edit_fails(std::string_view text, std::string_view id, std::string& error) {
    error.clear();
    return !with_disabled_plugin(text, id, error).has_value() && !error.empty();
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_file(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    return dir;
}

// A report in the core's format 1 (only the keys parse_crash_summary reads,
// laid out as build_report writes them).
std::string report_json(const std::string& level, const std::vector<std::string>& plugins, bool before_ready,
                        const std::string& summary = "", const std::string& issues_url = "") {
    std::string j = "{\n  \"format\": 1,\n  \"anvil\": \"1.1.0\",\n  \"time\": \"2026-10-20 18:05:11\",\n";
    j += std::string("  \"seconds_since_start\": 3.5,\n  \"before_game_ready\": ") + (before_ready ? "true" : "false") +
         ",\n  \"safe_mode\": false,\n  \"unfinished_starts\": 1,\n";
    j += "  \"exception\": {\"code\": \"0xC0000005\", \"name\": \"access violation\"},\n  \"thread\": 7,\n";
    j += "  \"main_thread\": true,\n  \"level\": \"" + level + "\",\n  \"summary\": \"" + summary + "\",\n";
    j += "  \"plugins_involved\": [";
    for (size_t i = 0; i < plugins.size(); ++i) j += (i ? ", \"" : "\"") + plugins[i] + "\"";
    j += "],\n  \"issues\": [";
    if (!issues_url.empty()) j += "{\"plugin\": \"" + plugins.front() + "\", \"url\": \"" + issues_url + "\"}";
    j += "],\n  \"stack\": [\n  ],\n  \"plugins\": [\n    {\"id\": \"x\", \"issues\": \"https://wrong\"}\n  ]\n}\n";
    return j;
}

constexpr const char* kOverlaySchema = R"toml(
[setting.panel_key]
type = "key"
default = "Ctrl+Shift+F12"
label = "Panel key"
restart = false

[setting.scale]
type = "float"
default = 1
min = 0.5
max = 2
label = "Scale"
restart = false

[setting.mode]
type = "enum"
values = [{ name = "off", label = "Off" }, { name = "on", label = "On" }]
default = "on"
label = "Mode"

[setting.title]
type = "string"
default = "Anvil"
label = "Title"
restart = false

[setting.count]
type = "int"
default = 3
min = 1
max = 9
step = 1
label = "Count"

[setting.notify]
type = "bool"
default = true
label = "Notify"
restart = false
)toml";

std::optional<std::string> no_environment(const std::string&) { return std::nullopt; }

std::unique_ptr<AnvilPlugin> make_plugin(const std::string& id, const std::string& state, const std::string& extra,
                                         const fs::path& data_root, const char* schema = nullptr,
                                         const std::string& phase = "main") {
    auto p = std::make_unique<AnvilPlugin>();
    p->manifest = manifest(plugin_toml(id, extra, "1.2.3", phase));
    p->state = state;
    p->data_dir = (data_root / id).wstring();
    if (schema != nullptr) {
        auto parsed = parse_settings_text(schema);
        if (!parsed.file) std::printf("  schema error: %s\n", parsed.error.c_str());
        p->settings.init(std::move(*parsed.file), data_root / id / "settings.toml", id, no_environment);
    }
    return p;
}

struct Text {
    std::string value;
    static void sink(void* context, const char* text, size_t size) {
        static_cast<Text*>(context)->value.assign(text, size);
    }
};

int g_notified = 0;
void count_notification(AnvilSetting, void*) { ++g_notified; }

}  // namespace

void control_tests() {
    run("anvil.toml edit: the shipped file keeps its comments", [] {
        const std::string shipped = read_file(fs::path(ANVIL_SOURCE_DIR) / "dist" / "anvil.toml");
        CHECK(shipped.find("disabled_plugins = []") != std::string::npos);
        std::string expected = shipped;
        expected.replace(expected.find("disabled_plugins = []"), 21, "disabled_plugins = [\"bellows\"]");
        CHECK(edit(shipped, "bellows") == expected);
        const std::string twice = edit(expected, "tongs");
        CHECK(twice.find("disabled_plugins = [\"bellows\", \"tongs\"]") != std::string::npos);
        CHECK(edit(twice, "tongs") == twice);  // already listed: unchanged
    });

    run("anvil.toml edit: lists of every shape", [] {
        CHECK(edit("", "x") == "disabled_plugins = [\"x\"]\n");
        CHECK(edit("disabled_plugins = [\"a\"] # keep\n", "x") == "disabled_plugins = [\"a\", \"x\"] # keep\n");
        CHECK(edit("disabled_plugins = [ ]\n", "x") == "disabled_plugins = [\"x\" ]\n");
        CHECK(edit("disabled_plugins = [\n  \"a\", # first\n  \"b\",\n]\n", "x") ==
              "disabled_plugins = [\n  \"a\", # first\n  \"b\", \"x\"\n]\n");
        CHECK(edit("disabled_plugins = [\n  \"a\" # a ] in a comment\n]\n", "x") ==
              "disabled_plugins = [\n  \"a\", \"x\" # a ] in a comment\n]\n");
        CHECK(edit("disabled_plugins = [\"we]ird\", 'li]t', \"\"\"m]\"\"\"]\n", "x") ==
              "disabled_plugins = [\"we]ird\", 'li]t', \"\"\"m]\"\"\", \"x\"]\n");
        CHECK(edit("\"disabled_plugins\" = [\"a\"]\n", "x") == "\"disabled_plugins\" = [\"a\", \"x\"]\n");
        // Non-ASCII comments, CRLF, a BOM.
        CHECK(edit("# zażółć gęślą jaźń\r\ndisabled_plugins = []\r\nenabled = true\r\n", "x") ==
              "# zażółć gęślą jaźń\r\ndisabled_plugins = [\"x\"]\r\nenabled = true\r\n");
        CHECK(edit("enabled = true\r\n", "x") == "enabled = true\r\ndisabled_plugins = [\"x\"]\r\n");
        CHECK(edit("enabled = true", "x") == "enabled = true\ndisabled_plugins = [\"x\"]\n");
        CHECK(edit("\xEF\xBB\xBFlog_level = \"info\"\n", "x") ==
              "\xEF\xBB\xBFlog_level = \"info\"\ndisabled_plugins = [\"x\"]\n");
        // A new key goes before the first table, where it stays top-level.
        CHECK(edit("enabled = true\n\n[extra]\nk = 1\n", "x") ==
              "enabled = true\n\ndisabled_plugins = [\"x\"]\n\n[extra]\nk = 1\n");
        CHECK(edit("enabled = true\n[[list]]\nk = 1\n", "x") ==
              "enabled = true\ndisabled_plugins = [\"x\"]\n\n[[list]]\nk = 1\n");
    });

    run("anvil.toml edit: removing an id and the other lists", [] {
        const auto unlisted = [](std::string_view text, std::string_view id, std::string_view key = "disabled_plugins") {
            std::string error;
            const auto out = with_plugin_listed(text, key, id, false, error);
            if (!out) std::printf("  edit error: %s\n", error.c_str());
            return out.value_or("<error>");
        };
        CHECK(unlisted("disabled_plugins = [\"x\"]\n", "x") == "disabled_plugins = []\n");
        CHECK(unlisted("disabled_plugins = [\"a\", \"x\"] # keep\n", "x") == "disabled_plugins = [\"a\"] # keep\n");
        CHECK(unlisted("disabled_plugins = [\"x\", \"a\"]\n", "x") == "disabled_plugins = [\"a\"]\n");
        CHECK(unlisted("disabled_plugins = [\"a\", 'x' , \"b\"]\n", "x") == "disabled_plugins = [\"a\", \"b\"]\n");
        CHECK(unlisted("disabled_plugins = [\n  \"a\", # first\n  \"x\",\n]\n", "x") ==
              "disabled_plugins = [\n  \"a\", # first\n]\n");
        CHECK(unlisted("disabled_plugins = [\n  \"a\",\n  \"x\" # gone\n]\n", "x") ==
              "disabled_plugins = [\n  \"a\" # gone\n]\n");
        CHECK(unlisted("disabled_plugins = [\"x\", \"a\", \"x\"]\r\n", "x") == "disabled_plugins = [\"a\"]\r\n");
        CHECK(unlisted("\xEF\xBB\xBF" "disabled_plugins = [\"x\"]\n", "x") == "\xEF\xBB\xBF" "disabled_plugins = []\n");
        // Nothing to remove: the text is unchanged, a missing key included.
        CHECK(unlisted("disabled_plugins = [\"a\"]\n", "x") == "disabled_plugins = [\"a\"]\n");
        CHECK(unlisted("enabled = true\n", "x") == "enabled = true\n");
        // allow_ironman and allow_multiplayer work the same way.
        std::string error;
        CHECK(with_plugin_listed("allow_ironman = []\n", "allow_ironman", "x", true, error) ==
              std::optional<std::string>("allow_ironman = [\"x\"]\n"));
        CHECK(unlisted("allow_multiplayer = [\"x\"]\n", "x", "allow_multiplayer") == "allow_multiplayer = []\n");
        CHECK(!with_plugin_listed("", "log_level", "x", true, error) && error.find("not a list of plugin ids") != std::string::npos);
        // An id written with escapes is not found textually: refused, not mangled.
        CHECK(!with_plugin_listed("disabled_plugins = [\"\\u0078\"]\n", "disabled_plugins", "x", false, error));

        const fs::path dir = fresh_dir("anvil-control-toml-remove");
        const fs::path file = dir / "anvil.toml";
        CHECK(set_plugin_listed(file, "disabled_plugins", "x", false, error) && !fs::exists(file));
        write_file(file, "# mine\ndisabled_plugins = [\"bellows\", \"tongs\"]\n");
        CHECK(set_plugin_listed(file, "disabled_plugins", "bellows", false, error));
        CHECK(read_file(file) == "# mine\ndisabled_plugins = [\"tongs\"]\n");
        fs::remove_all(dir);
    });

    run("anvil.toml edit: refusals", [] {
        std::string error;
        CHECK(edit_fails("", "Bad Id", error) && error.find("not a plugin id") != std::string::npos);
        CHECK(edit_fails("", "", error));
        CHECK(edit_fails("", std::string(65, 'a'), error));
        CHECK(edit_fails("enabled = \n", "x", error) && error.find("does not parse") != std::string::npos);
        CHECK(edit_fails("disabled_plugins = \"x\"\n", "y", error) && error.find("not a list") != std::string::npos);
    });

    run("anvil.toml edit: the file is replaced in one step", [] {
        const fs::path dir = fresh_dir("anvil-control-toml");
        const fs::path file = dir / "anvil" / "anvil.toml";
        std::string error;
        CHECK(add_disabled_plugin(file, "bellows", error));  // a missing file is an empty one
        CHECK(read_file(file) == "disabled_plugins = [\"bellows\"]\n");
        write_file(file, "# mine\nenabled = true\ndisabled_plugins = [\"a\"]  # player\nprofile = false\n");
        CHECK(add_disabled_plugin(file, "tongs", error));
        CHECK(read_file(file) == "# mine\nenabled = true\ndisabled_plugins = [\"a\", \"tongs\"]  # player\nprofile = false\n");
        CHECK(!fs::exists(dir / "anvil" / "anvil.toml.tmp"));
        const AnvilSettings read = read_anvil_settings(file);
        CHECK(read.error.empty() && read.disabled_plugins == std::vector<std::string>({"a", "tongs"}));
        CHECK(add_disabled_plugin(file, "tongs", error));  // already listed
        write_file(file, "enabled = = true\n");
        CHECK(!add_disabled_plugin(file, "x", error) && read_file(file) == "enabled = = true\n");
        fs::remove_all(dir);
    });

    run("crash history: names, summaries, attribution", [] {
        const fs::path dir = fresh_dir("anvil-control-crashes");
        write_file(dir / "2026-10-19_10-00-00.json", report_json("no_plugin", {}, false));
        write_file(dir / "2026-10-20_18-05-11.json",
                   report_json("in_plugin_code", {"anvil-overlay", "tongs"}, true,
                               "in plugin anvil-overlay's code, \\\"quoted\\\" caf\\u00e9 \\ud83d\\ude00",
                               "https://example.org/issues"));
        write_file(dir / "2026-10-20_18-05-11-2.json", "{\n  \"format\": 1,\n  \"level\": \"plugin_on_");  // partial
        write_file(dir / "not-a-report.txt", "x");
        const auto names = crash_report_names(dir);
        CHECK(names == std::vector<std::string>({"2026-10-20_18-05-11-2", "2026-10-20_18-05-11", "2026-10-19_10-00-00"}));
        const auto crash = load_crash_summary(dir, "2026-10-20_18-05-11");
        CHECK(crash.has_value());
        CHECK(crash->name == "2026-10-20_18-05-11" && crash->level == "in_plugin_code" && crash->before_game_ready);
        CHECK(crash->plugins == std::vector<std::string>({"anvil-overlay", "tongs"}));
        CHECK(crash->summary == "in plugin anvil-overlay's code, \"quoted\" caf\xC3\xA9 \xF0\x9F\x98\x80");
        CHECK(crash->issues == "https://example.org/issues");
        CHECK(crash->time == "2026-10-20 18:05:11");
        CHECK(crash_in_plugin(*crash, "anvil-overlay") && crash_in_plugin(*crash, "tongs"));
        CHECK(!crash_in_plugin(*crash, "anvil-game"));
        const auto partial = load_crash_summary(dir, "2026-10-20_18-05-11-2");
        CHECK(partial && partial->level.empty() && partial->plugins.empty() && !partial->before_game_ready);
        CHECK(!load_crash_summary(dir, "missing").has_value());
        CrashSummary on_stack = *crash;
        on_stack.level = "plugin_on_stack";
        CHECK(!crash_in_plugin(on_stack, "anvil-overlay"));
        on_stack.level = "in_plugin_change";
        CHECK(crash_in_plugin(on_stack, "anvil-overlay"));
        CHECK(crash_report_names(dir / "missing").empty());
        fs::remove_all(dir);
    });

    run("safe mode: a standard plugin the crash was in is left out", [] {
        CHECK(loads_in_safe_mode("anvil-game") && loads_in_safe_mode("anvil-overlay") && !loads_in_safe_mode("tongs"));
        const fs::path dir = fresh_dir("anvil-control-safe");
        write_file(dir / "2026-10-20_10-00-00.json", report_json("in_plugin_code", {"anvil-overlay"}, true, "in X"));
        write_file(dir / "2026-10-20_11-00-00.json", report_json("in_plugin_code", {"anvil-game"}, false));
        write_file(dir / "2026-10-20_12-00-00.json", report_json("plugin_on_stack", {"anvil-game"}, true));
        // Two unfinished starts: the two newest reports from before readiness.
        auto crashes = safe_mode_crashes(dir, 2);
        CHECK(crashes.size() == 2 && crashes[0].name == "2026-10-20_12-00-00" && crashes[1].name == "2026-10-20_10-00-00");
        const std::string reason = anvil::i18n::english(safe_mode_exclusion("anvil-overlay", crashes));
        CHECK(reason.find("safe mode") == 0 && reason.find("in X") != std::string::npos &&
              reason.find("2026-10-20_10-00-00") != std::string::npos);
        CHECK(safe_mode_exclusion("anvil-game", crashes).empty());  // only on the stack, or after readiness
        CHECK(safe_mode_crashes(dir, 1).size() == 1);
        CHECK(safe_mode_exclusion("anvil-overlay", safe_mode_crashes(dir, 1)).empty());
        CHECK(safe_mode_crashes(dir / "missing", 2).empty());
        fs::remove_all(dir);
    });

    run("text files: the tail of a log", [] {
        const fs::path dir = fresh_dir("anvil-control-tail");
        const fs::path log = dir / "anvil.log";
        write_file(log, "\xEF\xBB\xBFone\r\ntwo\r\nthree\r\n");
        CHECK(read_text_tail(log, 10, 4096) == "one\r\ntwo\r\nthree\r\n");
        CHECK(read_text_tail(log, 2, 4096) == "two\r\nthree\r\n");
        CHECK(read_text_tail(log, 10, 12) == "three\r\n");  // the cut first line is dropped
        CHECK(read_text_tail(dir / "missing.log", 10, 4096).empty());
        write_file(log, "a\nb");  // no newline at the end
        CHECK(read_text_tail(log, 1, 4096) == "b");
        CHECK(read_text_file(log, 3) == "a\nb" && !read_text_file(log, 2).has_value());
        fs::remove_all(dir);
    });

    run("log: warnings are kept for the overlay", [] {
        const size_t before = Log::instance().warning_count();
        Log::instance().write(ANVIL_LOG_WARNING, "control-test", "a warning");
        Log::instance().write(ANVIL_LOG_INFO, "control-test", "not kept");
        Log::instance().write(ANVIL_LOG_ERROR, "control-test", "an error");
        const auto lines = Log::instance().warnings();
        CHECK(Log::instance().warning_count() == before + 2 || before + 2 > Log::kMaxWarnings);
        if (lines.size() >= 2 && before + 2 <= Log::kMaxWarnings) {
            CHECK(lines[lines.size() - 2] == "[control-test] a warning" && lines.back() == "[control-test] an error");
        }
    });

    run("control: setting text and the plugin-set hash", [] {
        auto schema = parse_settings_text(kOverlaySchema);
        CHECK(schema.file.has_value());
        if (!schema.file) {
            std::printf("  %s\n", schema.error.c_str());
            return;
        }
        for (const Setting& s : schema.file->settings) {
            const std::string text = setting_text(s, s.default_value);
            std::string error;
            const auto back = parse_setting_text(s, text, error);
            CHECK(back.has_value() && *back == s.default_value);
        }
        CHECK(setting_text(*schema.file->find("panel_key"), schema.file->find("panel_key")->default_value) ==
              "Ctrl+Shift+F12");
        CHECK(setting_text(*schema.file->find("mode"), schema.file->find("mode")->default_value) == "on");
        CHECK(setting_text(*schema.file->find("scale"), SettingValue{0, 1.25, {}}) == "1.25");
        CHECK(plugin_set_hash({}).empty());
        const std::string h = plugin_set_hash({"b 1.0.0", "a 2.0.0"});
        CHECK(h.size() == 19 && h[4] == '-' && h[9] == '-' && h[14] == '-');
        CHECK(h == plugin_set_hash({"a 2.0.0", "b 1.0.0"}));
        CHECK(h != plugin_set_hash({"a 2.0.0", "b 1.0.1"}));
    });

    run("control: status, plugins and the service table", [] {
        const fs::path dir = fresh_dir("anvil-control-service");
        const fs::path anvil = dir / "anvil";
        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        plugins.push_back(make_plugin("anvil-overlay", "loaded", "", anvil / "data", kOverlaySchema, "early"));
        plugins.push_back(make_plugin("gameplay", "loaded", "issues = \"https://example.org/g\"\n", anvil / "data"));
        plugins.back()->manifest.affects_gameplay = true;
        plugins.push_back(make_plugin("broken", "failed", "", anvil / "data"));
        plugins.back()->reason = "anvil_plugin_load returned ANVIL_ERROR_INTERNAL";
        std::vector<Skipped> skipped{{"off", "disabled in anvil.toml", anvil / "plugins" / "off", true},
                                     {"tongs", "safe mode", anvil / "plugins" / "tongs", false}};
        ControlSources sources;
        sources.anvil_dir = anvil;
        sources.anvil_version = "1.1.0";
        sources.game_build = "eu4-1.37.5-steam-win64";
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        sources.disabled = {"off"};
        sources.safe_mode = true;
        sources.unfinished_starts = 2;
        Control control(sources);
        const AnvilControlService& svc = *control.service();
        CHECK(svc.size == sizeof(AnvilControlService));

        AnvilControlStatus status{};
        svc.status(&status);
        CHECK(status.size == sizeof(status) && status.safe_mode == 1 && status.unfinished_starts == 2);
        CHECK(std::string(status.anvil_version) == "1.1.0" && std::string(status.game_build) == "eu4-1.37.5-steam-win64");
        CHECK(status.plugins == 5 && status.skipped == 2 && status.loading_done == 0 && status.loaded == 0);
        AnvilControlPlugin p{};
        p.size = sizeof(p);
        CHECK(svc.plugin(0, &p) == 1 && std::string(p.state) == "pending");  // states only after loading

        control.loading_done();
        svc.status(&status);
        CHECK(status.loading_done == 1 && status.loaded == 2 && status.failed == 1 && status.session == 0);
        CHECK(status.gameplay_active == 1 && std::string(status.gameplay_hash) == plugin_set_hash({"gameplay 1.2.3"}));
        CHECK(svc.plugin(0, &p) == 1 && std::string(p.id) == "anvil-overlay" && std::string(p.version) == "1.2.3");
        CHECK(std::string(p.state) == "loaded" && p.early == 1 && p.settings == 6 && p.disabled_next_start == 0);
        CHECK(svc.plugin(1, &p) == 1 && p.affects_gameplay == 1 && std::string(p.issues) == "https://example.org/g");
        CHECK(svc.plugin(2, &p) == 1 && std::string(p.state) == "failed" &&
              std::string(p.reason) == "anvil_plugin_load returned ANVIL_ERROR_INTERNAL");
        CHECK(svc.plugin(3, &p) == 1 && std::string(p.id) == "off" && std::string(p.state) == "skipped" &&
              p.disabled == 1 && p.disabled_next_start == 1 && std::string(p.name).empty());
        CHECK(svc.plugin(4, &p) == 1 && std::string(p.reason) == "safe mode" && p.disabled == 0);
        CHECK(svc.plugin(5, &p) == 0);

        Text text;
        CHECK(status.warnings == Log::instance().warning_count());
        if (status.warnings > 0) CHECK(svc.warning(0, Text::sink, &text) == 1 && !text.value.empty());
        CHECK(svc.warning(status.warnings, Text::sink, &text) == 0);

        // Settings.
        CHECK(svc.setting_count("anvil-overlay") == 6 && svc.setting_count("gameplay") == 0 &&
              svc.setting_count("missing") == 0 && svc.setting_count(nullptr) == 0);
        AnvilControlSetting info{};
        CHECK(svc.setting_info("anvil-overlay", 0, &info) == 1 && std::string(info.name) == "panel_key" &&
              std::string(info.label) == "Panel key" && info.type == ANVIL_CONTROL_SETTING_KEY && info.restart == 0);
        CHECK(svc.setting_info("anvil-overlay", 1, &info) == 1 && info.type == ANVIL_CONTROL_SETTING_FLOAT &&
              info.has_min && info.min == 0.5 && info.has_max && info.max == 2.0 && !info.has_step);
        CHECK(svc.setting_info("anvil-overlay", 4, &info) == 1 && info.type == ANVIL_CONTROL_SETTING_INT &&
              info.min == 1.0 && info.max == 9.0 && info.has_step && info.step == 1.0 && info.restart == 1);
        CHECK(svc.setting_info("anvil-overlay", 2, &info) == 1 && info.options == 2 &&
              std::string(info.label) == "Mode");
        CHECK(svc.setting_info("anvil-overlay", 6, &info) == 0);
        const char* name = nullptr;
        const char* label = nullptr;
        CHECK(svc.setting_option("anvil-overlay", 2, 1, &name, &label) == 1 && std::string(name) == "on" &&
              std::string(label) == "On");
        CHECK(svc.setting_option("anvil-overlay", 2, 2, &name, &label) == 0);
        CHECK(svc.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT, Text::sink, &text) == 1 &&
              text.value == "Ctrl+Shift+F12");
        CHECK(svc.setting_value("anvil-overlay", 0, 7, Text::sink, &text) == 0);

        PluginSettings& settings = plugins[0]->settings;
        g_notified = 0;
        CHECK(settings.subscribe(0, count_notification, nullptr) == ANVIL_OK);
        AnvilControlResult result{};
        CHECK(svc.setting_set_text("anvil-overlay", 0, "Ctrl+F11", &result) == ANVIL_OK);
        CHECK(result.applied == 1 && result.saved == 1 && result.notified == 1 && g_notified == 1);
        CHECK(svc.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_CURRENT, Text::sink, &text) == 1 &&
              text.value == "Ctrl+F11");
        CHECK(read_file(anvil / "data" / "anvil-overlay" / "settings.toml").find("panel_key = \"Ctrl+F11\"") !=
              std::string::npos);
        // restart = true: saved now, current at the next start.
        CHECK(svc.setting_set_text("anvil-overlay", 4, "7", &result) == ANVIL_OK && result.applied == 0 &&
              result.saved == 1);
        CHECK(svc.setting_value("anvil-overlay", 4, ANVIL_CONTROL_VALUE_CURRENT, Text::sink, &text) && text.value == "3");
        CHECK(svc.setting_value("anvil-overlay", 4, ANVIL_CONTROL_VALUE_SAVED, Text::sink, &text) && text.value == "7");
        CHECK(svc.setting_value("anvil-overlay", 4, ANVIL_CONTROL_VALUE_DEFAULT, Text::sink, &text) && text.value == "3");
        AnvilControlValue value{0, 1.5, nullptr};
        CHECK(svc.setting_set("anvil-overlay", 1, &value, &result) == ANVIL_OK && result.applied == 1);
        CHECK(settings.get_float(1) == 1.5);
        value = {0, 0, "Overlay"};
        CHECK(svc.setting_set("anvil-overlay", 3, &value, &result) == ANVIL_OK &&
              std::string(settings.get_string(3)) == "Overlay");
        CHECK(svc.setting_set_text("anvil-overlay", 4, "99", &result) == ANVIL_ERROR_INVALID_ARGUMENT &&
              result.error[0] != '\0');
        CHECK(svc.setting_set_text("anvil-overlay", 42, "1", &result) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(svc.setting_set_text("missing", 0, "1", &result) == ANVIL_ERROR_NOT_FOUND &&
              std::string(result.error) == "no such plugin");
        CHECK(svc.setting_set_text("anvil-overlay", 0, nullptr, &result) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(svc.setting_reset("anvil-overlay", 0, &result) == ANVIL_OK && g_notified == 2);
        CHECK(svc.setting_value("anvil-overlay", 0, ANVIL_CONTROL_VALUE_SAVED, Text::sink, &text) &&
              text.value == "Ctrl+Shift+F12");
        CHECK(svc.setting_warning("anvil-overlay", 0, Text::sink, &text) == 0);

        // The log tail.
        write_file(anvil / "anvil.log", "\xEF\xBB\xBF[1] a\r\n[2] b\r\n[3] c\r\n");
        CHECK(svc.log_tail(2, Text::sink, &text) == 2 && text.value == "[2] b\r\n[3] c\r\n");
        CHECK(svc.log_tail(100, Text::sink, &text) == 3);

        // No crash before this start.
        AnvilControlCrash crash{};
        CHECK(svc.previous_crash(&crash) == 0);

        // Disable at the next start.
        CHECK(svc.disable_next_start("missing", &result) == ANVIL_ERROR_NOT_FOUND && result.error[0] != '\0');
        CHECK(svc.disable_next_start("gameplay", &result) == ANVIL_OK && result.saved == 1);
        CHECK(read_anvil_settings(anvil / "anvil.toml").disabled_plugins == std::vector<std::string>({"gameplay"}));
        CHECK(svc.plugin(1, &p) == 1 && p.disabled_next_start == 1);
        CHECK(svc.disable_next_start("tongs", &result) == ANVIL_OK);  // a skipped plugin
        CHECK(read_anvil_settings(anvil / "anvil.toml").disabled_plugins ==
              std::vector<std::string>({"gameplay", "tongs"}));
        write_file(anvil / "anvil.toml", "broken = = 1\n");
        CHECK(svc.disable_next_start("broken", &result) == ANVIL_ERROR_INTERNAL &&
              std::string(result.error).find("does not parse") != std::string::npos);
        fs::remove_all(dir);
    });


    run("control: plugins in mods, approved and revoked for the next start", [] {
        const fs::path dir = fresh_dir("anvil-control-mods");
        const fs::path anvil = dir / "eu4" / "anvil";
        const fs::path user = dir / "user";
        const fs::path mod = user / "mod" / "probe-mod";
        write_file(mod / "anvil" / "probe" / "plugin.toml",
                   plugin_toml("probe", "symbols = \"symbols.toml\"\nauthors = [\"A\", \"B\"]\nrepository = \"https://example.org/p\"\n"));
        write_file(mod / "anvil" / "probe" / "symbols.toml",
                   "build = \"b\"\n\n[symbol.one]\nrva = 0x1000\nbytes = \"C3\"\nmemory = \"code\"\n\n"
                   "[symbol.two]\nrva = 0x1010\nbytes = \"C3\"\nmemory = \"code\"\n");
        write_file(mod / "anvil" / "probe" / "probe.dll", "MZ");
        write_file(user / "mod" / "probe-mod.mod", "name=\"Probe Mod\"\npath=\"mod/probe-mod\"\n");
        write_file(user / "dlc_load.json", "{\"enabled_mods\":[\"mod/probe-mod.mod\"]}");

        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        ModDiscovery mods = discover_mod_plugins(anvil, user, {});
        std::vector<Skipped> skipped{{"probe", "from mod 'Probe Mod' (local mod): waiting", mods.plugins[0].folder}};
        skipped[0].mod = "Probe Mod";
        skipped[0].awaiting_approval = true;
        ControlSources sources;
        sources.anvil_dir = anvil;
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        sources.mods = &mods;
        std::string hash;
        {
            Control control(sources);
            control.loading_done();
            const AnvilControlService& svc = *control.service();
            CHECK(svc.mod_plugin_count() == 1);
            AnvilControlModPlugin m{};
            m.size = sizeof(m);
            CHECK(svc.mod_plugin(0, &m) == 1 && std::string(m.id) == "probe" && std::string(m.state) == "pending");
            CHECK(std::string(m.mod) == "Probe Mod" && std::string(m.workshop_id).empty() && std::string(m.authors) == "A, B");
            CHECK(std::string(m.repository) == "https://example.org/p" && m.symbols == 2 && m.affects_gameplay == 0);
            CHECK(std::string(m.hash).size() == 64 && m.can_approve == 1 && m.can_revoke == 0 && m.loaded == 0);
            CHECK(std::string(m.approved_hash).empty() && svc.mod_plugin(1, &m) == 0);
            hash = m.hash;
            AnvilControlPlugin p{};
            p.size = sizeof(p);
            CHECK(svc.plugin(0, &p) == 1 && std::string(p.mod) == "Probe Mod" && p.awaiting_approval == 1);
            // A caller built before the fields were appended gets only its part.
            AnvilControlPlugin short_struct{};
            short_struct.size = offsetof(AnvilControlPlugin, mod);
            CHECK(svc.plugin(0, &short_struct) == 1 && short_struct.mod == nullptr &&
                  short_struct.size == offsetof(AnvilControlPlugin, mod));

            AnvilControlResult result{};
            CHECK(svc.approve_mod_plugin(0, "00", &result) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(svc.approve_mod_plugin(1, hash.c_str(), &result) == ANVIL_ERROR_NOT_FOUND);
            CHECK(svc.revoke_mod_plugin(0, &result) == ANVIL_ERROR_NOT_FOUND);
            CHECK(svc.approve_mod_plugin(0, hash.c_str(), &result) == ANVIL_OK && result.saved == 1);
            CHECK(svc.mod_plugin(0, &m) == 1 && m.approved_next_start == 1 && m.can_approve == 0 && m.can_revoke == 1);
            CHECK(std::string(m.state) == "pending");  // this start's decision stays
            CHECK(read_trust(anvil / "trust.toml").find("probe") != nullptr);
            CHECK(fs::exists(approved_copy(anvil, "probe", hash) / "probe.dll"));
            CHECK(svc.revoke_mod_plugin(0, &result) == ANVIL_OK);
            CHECK(svc.mod_plugin(0, &m) == 1 && m.approved_next_start == 0 && m.can_approve == 1 && m.can_revoke == 0);
            CHECK(read_trust(anvil / "trust.toml").find("probe") == nullptr);
            CHECK(svc.approve_mod_plugin(0, hash.c_str(), &result) == ANVIL_OK);
        }
        {
            // The next start: approved and loaded from its copy, which stays when revoked.
            mods = discover_mod_plugins(anvil, user, {});
            CHECK(mods.plugins[0].state == ModPluginState::Approved);
            auto loaded = make_plugin("probe", "loaded", "", anvil / "data");
            loaded->plugin_dir = mods.plugins[0].load_folder.wstring();
            loaded->approved_hash = hash;
            plugins.push_back(std::move(loaded));
            skipped.clear();
            Control control(sources);
            control.loading_done();
            const AnvilControlService& svc = *control.service();
            AnvilControlModPlugin m{};
            m.size = sizeof(m);
            CHECK(svc.mod_plugin(0, &m) == 1 && std::string(m.state) == "approved" && m.loaded == 1);
            CHECK(std::string(m.approved_hash) == hash && m.can_approve == 0 && m.can_revoke == 1 && m.approved_next_start == 0);
            AnvilControlPlugin p{};
            p.size = sizeof(p);
            CHECK(svc.plugin(0, &p) == 1 && std::string(p.mod) == "Probe Mod" && p.awaiting_approval == 0);
            AnvilControlResult result{};
            CHECK(svc.revoke_mod_plugin(0, &result) == ANVIL_OK);
            CHECK(svc.mod_plugin(0, &m) == 1 && m.revoked_next_start == 1 && m.can_approve == 1);
            CHECK(fs::exists(approved_copy(anvil, "probe", hash)));  // in use by this start
        }
        fs::remove_all(dir);
    });
    run("control: the previous crash is new once", [] {
        const fs::path dir = fresh_dir("anvil-control-previous");
        const fs::path anvil = dir / "anvil";
        write_file(anvil / "crashes" / "2026-10-20_18-05-11.json",
                   report_json("in_plugin_change", {"tongs", "bellows"}, true, "in plugin tongs's change",
                               "https://example.org/t"));
        write_file(anvil / "crashes" / "2026-10-20_18-05-11.txt", "Anvil crash report\r\n");
        std::vector<std::unique_ptr<AnvilPlugin>> plugins;
        std::vector<Skipped> skipped;
        ControlSources sources;
        sources.anvil_dir = anvil;
        sources.plugins = &plugins;
        sources.skipped = &skipped;
        sources.previous_crash = crash_report_names(anvil / "crashes").front();
        {
            Control control(sources);
            AnvilControlCrash crash{};
            CHECK(control.service()->previous_crash(&crash) == 1);
            CHECK(crash.is_new == 1 && std::string(crash.name) == "2026-10-20_18-05-11");
            CHECK(std::string(crash.level) == "in_plugin_change" && std::string(crash.plugins) == "tongs, bellows");
            CHECK(std::string(crash.summary) == "in plugin tongs's change" && crash.before_game_ready == 1);
            CHECK(std::string(crash.issues) == "https://example.org/t" &&
                  std::string(crash.report) == "Anvil crash report\r\n");
            CHECK(fs::path(crash.path) == anvil / "crashes" / "2026-10-20_18-05-11.txt");
            AnvilControlCrash again{};
            CHECK(control.service()->previous_crash(&again) == 1 && again.is_new == 1);  // the same in this process
            CHECK(read_file(anvil / "data" / "anvil" / "crash-shown.txt") == "2026-10-20_18-05-11\n");
        }
        {
            Control next_start(sources);
            AnvilControlCrash crash{};
            CHECK(next_start.service()->previous_crash(&crash) == 1 && crash.is_new == 0);
        }
        {
            sources.previous_crash = "missing";
            Control control(sources);
            AnvilControlCrash crash{};
            CHECK(control.service()->previous_crash(&crash) == 0);
        }
        // Without a Control (the table outlives it) every call reports nothing.
        const AnvilControlService* svc = nullptr;
        {
            Control control(sources);
            svc = control.service();
        }
        AnvilControlStatus status{};
        svc->status(&status);
        AnvilControlPlugin p{};
        AnvilControlCrash crash{};
        CHECK(status.size == sizeof(status) && status.plugins == 0 && svc->plugin(0, &p) == 0);
        CHECK(svc->previous_crash(&crash) == 0 && svc->setting_count("x") == 0);
        fs::remove_all(dir);
    });
}
