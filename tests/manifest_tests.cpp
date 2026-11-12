// plugin.toml v2 (design §5.1): schema, strict keys, API 1 rejection.
#include "core/manifest.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace anvil;

// Error for `text`, or "" when it parses.
std::string error_of(const std::string& text) {
    const auto r = parse_manifest_text(text, L"C:\\p");
    return r.manifest ? std::string() : r.error;
}

bool fails(const std::string& text) { return !error_of(text).empty(); }

// The minimal manifest without affects_gameplay, for tests that set it themselves.
std::string without_gameplay(const std::string& id, const std::string& extra = "") {
    return "id = \"" + id + "\"\nname = \"" + id + "\"\nversion = \"1.0.0\"\napi = 2\nentry = \"" + id + ".dll\"\n" +
           extra;
}

}  // namespace

void manifest_tests() {
    run("manifest v2: all keys", [] {
        const auto r = parse_manifest_text(plugin_toml("demo", R"(authors = ["A", "B"]
description = "Demo"
anvil = ">=1.0.0"
symbols = "symbols.toml"
settings = "config/settings.toml"
allow_ironman = "opt-in"
allow_multiplayer = false
load_after = ["base"]
issues = "https://github.com/you/demo/issues"
repository = "https://github.com/you/demo"

[[dependencies]]
id = "base"
version = "^1"
optional = true

[[conflicts]]
id = "other-renderer"
version = "<2"

[[conflicts]]
id = "legacy"
)"),
                                           L"C:\\p");
        CHECK(r.manifest.has_value());
        if (!r.manifest) return;
        const Manifest& m = *r.manifest;
        CHECK(m.api == 2 && m.authors.size() == 2 && m.description == "Demo");
        CHECK(m.symbols == "symbols.toml" && m.settings == "config/settings.toml");
        CHECK(!m.affects_gameplay && m.allow_ironman == Allow::OptIn && m.allow_multiplayer == Allow::No);
        CHECK(m.issues == "https://github.com/you/demo/issues" && m.repository == "https://github.com/you/demo");
        CHECK(m.dependencies.size() == 1 && m.dependencies[0].optional && m.dependencies[0].range.text() == "^1");
        CHECK(m.conflicts.size() == 2 && m.conflicts[0].id == "other-renderer" && m.conflicts[0].range.text() == "<2");
        CHECK(m.conflicts[0].range.matches(*Version::parse("1.9.0")) && !m.conflicts[0].range.matches(*Version::parse("2.0.0")));
        CHECK(m.conflicts[1].id == "legacy" && m.conflicts[1].range.matches(*Version::parse("9.0.0")));  // default "*"

        const auto minimal = parse_manifest_text(plugin_toml("min"), L"C:\\p");
        CHECK(minimal.manifest && minimal.manifest->symbols.empty() && minimal.manifest->settings.empty() &&
              minimal.manifest->allow_ironman == Allow::No && minimal.manifest->conflicts.empty());
    });

    run("manifest: a plugin.toml past the limit is not read", [] {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / L"anvil-manifest-limit";
        fs::create_directories(folder);
        {
            std::ofstream file(folder / "plugin.toml", std::ios::binary);
            file << "id = \"p\"\n" << std::string(kMaxManifestBytes, '#');
        }
        const ManifestResult result = load_manifest(folder);
        CHECK(!result.manifest && result.error.find("larger than 1 MB") != std::string::npos);
        fs::remove_all(folder);
    });

    run("manifest v2: unknown keys are errors", [] {
        CHECK(error_of(plugin_toml("a", "custom = 1\n")) == "plugin.toml: unknown key 'custom'");
        CHECK(error_of(without_gameplay("a", "afects_gameplay = false\n")) ==
              "plugin.toml: unknown key 'afects_gameplay' (did you mean 'affects_gameplay'?)");
        CHECK(error_of(plugin_toml("a", "load_afer = []\n")).find("did you mean 'load_after'") != std::string::npos);
        CHECK(error_of(plugin_toml("a", "[[dependencies]]\nid = \"b\"\nversoin = \"^1\"\n")) ==
              "plugin.toml: [[dependencies]] #1: unknown key 'versoin' (did you mean 'version'?)");
        CHECK(error_of(plugin_toml("a", "[[conflicts]]\nid = \"b\"\noptional = true\n")) ==
              "plugin.toml: [[conflicts]] #1: unknown key 'optional'");
        // Removed in v2.
        CHECK(error_of(plugin_toml("a", "game_builds = [\"*\"]\n")) == "plugin.toml: unknown key 'game_builds'");
        CHECK(error_of(plugin_toml("a", "[[expect]]\nbuild = \"*\"\nrva = 0\nbytes = \"4D 5A\"\n")) ==
              "plugin.toml: unknown key 'expect'");
    });

    run("manifest v2: api", [] {
        // An API 1 manifest is rejected for its API, not for its v1 keys.
        CHECK(error_of("id = \"old\"\nname = \"Old\"\nversion = \"1.0.0\"\napi = 1\nentry = \"old.dll\"\n"
                       "game_builds = [\"*\"]\n") == "plugin API 1: update the plugin");
        auto with_api = [](const std::string& api) {
            std::string text = plugin_toml("a");
            text.replace(text.find("api = 2"), 7, "api = " + api);
            return error_of(text);
        };
        CHECK(with_api("3").find("plugin API 3 is newer than this Anvil supports (API 2)") != std::string::npos);
        CHECK(with_api("0") == "plugin.toml: missing or invalid 'api'");
        CHECK(with_api("\"2\"") == "plugin.toml: missing or invalid 'api'");
        CHECK(with_api("2.0") == "plugin.toml: missing or invalid 'api'");
        CHECK(fails("id = \"a\"\nname = \"a\"\nversion = \"1.0.0\"\nentry = \"a.dll\"\naffects_gameplay = false\n"));
    });

    run("manifest v2: affects_gameplay is required", [] {
        CHECK(error_of(without_gameplay("a")) == "plugin.toml: missing 'affects_gameplay' (true or false)");
        CHECK(error_of(without_gameplay("a", "affects_gameplay = \"no\"\n")) ==
              "plugin.toml: affects_gameplay must be true or false");
        const auto yes = parse_manifest_text(without_gameplay("a", "affects_gameplay = true\n"), L"C:\\p");
        CHECK(yes.manifest && yes.manifest->affects_gameplay);
    });

    run("manifest v2: allow_ironman / allow_multiplayer", [] {
        CHECK(!fails(plugin_toml("a", "allow_ironman = false\nallow_multiplayer = \"opt-in\"\n")));
        CHECK(error_of(plugin_toml("a", "allow_ironman = true\n")) ==
              "plugin.toml: allow_ironman must be false or \"opt-in\"");
        CHECK(fails(plugin_toml("a", "allow_multiplayer = \"yes\"\n")));
        CHECK(fails(plugin_toml("a", "allow_multiplayer = \"opt_in\"\n")));
        CHECK(fails(plugin_toml("a", "allow_ironman = 0\n")));
    });

    run("manifest v2: conflicts", [] {
        CHECK(error_of(plugin_toml("a", "conflicts = \"b\"\n")) ==
              "plugin.toml: conflicts must be an array of tables ([[conflicts]])");
        CHECK(fails(plugin_toml("a", "conflicts = [\"b\"]\n")));
        CHECK(fails(plugin_toml("a", "[[conflicts]]\nversion = \"<2\"\n")));  // no id
        CHECK(fails(plugin_toml("a", "[[conflicts]]\nid = \"Bad_Id\"\n")));
        CHECK(fails(plugin_toml("a", "[[conflicts]]\nid = \"b\"\nversion = \">=x\"\n")));
        CHECK(fails(plugin_toml("a", "[[conflicts]]\nid = \"b\"\nversion = 2\n")));
        CHECK(error_of(plugin_toml("a", "[[conflicts]]\nid = \"a\"\n")) ==
              "plugin.toml: [[conflicts]] #1: a plugin cannot conflict with itself");
        const auto inline_form = parse_manifest_text(plugin_toml("a", "conflicts = [{ id = \"b\", version = \"^1\" }]\n"),
                                                     L"C:\\p");
        CHECK(inline_form.manifest && inline_form.manifest->conflicts.size() == 1);
    });

    run("manifest v2: symbols, settings, issues, repository", [] {
        CHECK(!fails(plugin_toml("a", "symbols = \"data/symbols.toml\"\nsettings = \"settings.toml\"\n")));
        for (const char* bad : {"\"../symbols.toml\"", "\"C:\\\\symbols.toml\"", "\"/symbols.toml\"", "\"\\\\symbols.toml\"",
                                "\"symbols.txt\"", "\"\"", "\"a//symbols.toml\"", "\"./symbols.toml\"", "1"}) {
            CHECK(fails(plugin_toml("a", std::string("symbols = ") + bad + "\n")));
        }
        CHECK(error_of(plugin_toml("a", "settings = \"..\\\\settings.toml\"\n")) ==
              "plugin.toml: settings must be a .toml file inside the plugin folder");
        CHECK(!fails(plugin_toml("a", "issues = \"http://example.org/issues\"\n")));
        for (const char* bad : {"\"github.com/you/a\"", "\"ftp://example.org\"", "\"https://\"", "\"https://a b\"", "1"}) {
            CHECK(fails(plugin_toml("a", std::string("repository = ") + bad + "\n")));
            CHECK(fails(plugin_toml("a", std::string("issues = ") + bad + "\n")));
        }
    });

    run("manifest v2: required keys and types", [] {
        CHECK(error_of(plugin_toml("Bad_Id")).find("must match [a-z0-9-]+") != std::string::npos);
        CHECK(fails(plugin_toml("a", "", "1.0")));                                // not SemVer
        CHECK(fails(plugin_toml("a", "", "1.0.0", "late")));                      // phase
        CHECK(fails(without_gameplay("a", "affects_gameplay = false\nphase = 1\n")));
        CHECK(fails(plugin_toml("a", "description = 1\n")));
        CHECK(fails(plugin_toml("a", "authors = \"A\"\n")));
        CHECK(fails(plugin_toml("a", "[[dependencies]]\nid = \"b\"\noptional = \"yes\"\n")));
        CHECK(fails(plugin_toml("a", "anvil = 1\n")));
        std::string entry = plugin_toml("a");
        entry.replace(entry.find("a.dll"), 5, "..\\\\a.dll");
        CHECK(fails(entry));                                                      // path in entry
        std::string no_name = plugin_toml("a");
        no_name.replace(no_name.find("name = \"a\""), 10, "name = \"\"");
        CHECK(error_of(no_name) == "plugin.toml: name must not be empty");
        CHECK(error_of("api = 2\nid = \"a\"\nname = \"a\"\nversion = \"1.0.0\"\naffects_gameplay = false\n") ==
              "plugin.toml: missing 'entry'");
        CHECK(fails("id = = broken"));
    });

    run("manifest v2: load_manifest checks the named files", [] {
        const auto folder = std::filesystem::temp_directory_path() /
                            ("anvil-manifest-test-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder / "data");
        auto write = [&](const std::filesystem::path& name, const std::string& text) {
            std::ofstream(folder / name, std::ios::binary) << text;
        };
        write("plugin.toml", "\xEF\xBB\xBF" + plugin_toml("a", "symbols = \"data/symbols.toml\"\nsettings = \"settings.toml\"\n"));
        write("data/symbols.toml", "build = \"eu4-1.37.5-steam-win64\"\n");
        CHECK(load_manifest(folder).error == "plugin.toml: settings file 'settings.toml' not found in the plugin folder");
        write("settings.toml", "");
        const auto ok = load_manifest(folder);
        CHECK(ok.manifest && ok.manifest->folder == folder);
        std::filesystem::remove(folder / "data" / "symbols.toml");
        CHECK(load_manifest(folder).error ==
              "plugin.toml: symbols file 'data/symbols.toml' not found in the plugin folder");
        std::filesystem::create_directories(folder / "data" / "symbols.toml");  // a folder is not a file
        CHECK(!load_manifest(folder).manifest);
        write("plugin.toml", "id = \"a\"\nname = \"a\"\nversion = \"1.0.0\"\napi = 1\nentry = \"a.dll\"\n");
        CHECK(load_manifest(folder).error == "plugin API 1: update the plugin");
        std::filesystem::remove_all(folder);
        CHECK(load_manifest(folder).error == "plugin.toml not found");
    });

    run("manifest v2: modules (ADR 0011)", [] {
        const auto r = parse_manifest_text(plugin_toml("smithy", R"([[modules]]
id = "ui_size_fix"
name = "Country views at a raised GUI scale"
affects_gameplay = false

[[modules]]
id = "drill_all"
name = "Drill all selected armies"
description = "One key drills every selected army."
affects_gameplay = true
allow_ironman = "opt-in"
allow_multiplayer = "opt-in"
default = false
)"),
                                           L"C:\\p");
        CHECK(r.manifest.has_value());
        if (!r.manifest) return;
        const Manifest& m = *r.manifest;
        CHECK(m.modules.size() == 2 && m.modules[0].id == "ui_size_fix" && m.modules[1].id == "drill_all");
        CHECK(!m.modules[0].affects_gameplay && m.modules[0].enabled_by_default &&
              m.modules[0].allow_ironman == Allow::No && m.modules[0].description.empty());
        CHECK(m.modules[1].affects_gameplay && !m.modules[1].enabled_by_default &&
              m.modules[1].allow_ironman == Allow::OptIn && m.modules[1].allow_multiplayer == Allow::OptIn);
        CHECK(!m.affects_gameplay && affects_gameplay_anywhere(m));
        CHECK(!affects_gameplay_anywhere(manifest(plugin_toml("plain"))));
        const Manifest drill = module_manifest(m, m.modules[1]);
        CHECK(module_key(m, m.modules[1]) == "smithy.drill_all" && drill.id == "smithy.drill_all");
        CHECK(drill.name == "smithy: Drill all selected armies" && drill.description == "One key drills every selected army.");
        CHECK(drill.affects_gameplay && drill.allow_ironman == Allow::OptIn && drill.modules.empty() &&
              drill.version == m.version && drill.entry == m.entry);

        const auto with = [](const std::string& body) { return plugin_toml("smithy", "[[modules]]\n" + body); };
        CHECK(error_of(with("id = \"a\"\nname = \"A\"\n")) == "plugin.toml: [[modules]] #1: missing 'affects_gameplay'");
        CHECK(error_of(with("name = \"A\"\naffects_gameplay = false\n")) == "plugin.toml: [[modules]] #1: missing 'id'");
        CHECK(error_of(with("id = \"Drill\"\nname = \"A\"\naffects_gameplay = false\n")) ==
              "plugin.toml: [[modules]] #1: id 'Drill' must match [a-z][a-z0-9_]* (max 32 characters)");
        CHECK(fails(with("id = \"drill-all\"\nname = \"A\"\naffects_gameplay = false\n")));
        CHECK(fails(with("id = \"1a\"\nname = \"A\"\naffects_gameplay = false\n")));
        CHECK(fails(with("id = \"" + std::string(33, 'a') + "\"\nname = \"A\"\naffects_gameplay = false\n")));
        CHECK(!fails(with("id = \"" + std::string(32, 'a') + "\"\nname = \"A\"\naffects_gameplay = false\n")));
        CHECK(error_of(with("id = \"a\"\nname = \"\"\naffects_gameplay = false\n")) ==
              "plugin.toml: [[modules]] #1: name must not be empty");
        CHECK(error_of(with("id = \"a\"\nname = \"A\"\naffects_gameplay = false\nallow_ironman = \"opt-in\"\n")) ==
              "plugin.toml: [[modules]] #1: a module with affects_gameplay = false is never suspended; remove its opt-in");
        CHECK(error_of(with("id = \"a\"\nname = \"A\"\naffects_gameplay = true\nphase = \"early\"\n")) ==
              "plugin.toml: [[modules]] #1: unknown key 'phase'");
        CHECK(error_of(with("id = \"a\"\nname = \"A\"\naffects_gameplay = true\ndefault = \"on\"\n")) ==
              "plugin.toml: [[modules]] #1: default must be true or false");
        CHECK(error_of(with("id = \"a\"\nname = \"A\"\naffects_gameplay = false\n\n[[modules]]\nid = \"a\"\nname = \"B\"\n"
                            "affects_gameplay = false\n")) ==
              "plugin.toml: [[modules]] #2: module 'a' is declared twice");
        CHECK(fails(plugin_toml("smithy", "modules = []\n")));
        CHECK(fails(plugin_toml("smithy", "modules = \"a\"\n")));
        std::string many;
        for (int i = 0; i <= 32; ++i) {
            many += "[[modules]]\nid = \"m" + std::to_string(i) + "\"\nname = \"M\"\naffects_gameplay = false\n";
        }
        CHECK(error_of(plugin_toml("smithy", many)) == "plugin.toml: more than 32 [[modules]]");
    });

    run("manifest v2: the hello examples", [] {
        for (const char* id : {"hello-c", "hello-cpp"}) {
            const auto hello = load_manifest(std::filesystem::path(ANVIL_SOURCE_DIR) / "examples" / id);
            if (!hello.manifest) std::printf("  %s\n", hello.error.c_str());
            CHECK(hello.manifest && hello.manifest->id == id && hello.manifest->api == kManifestApi &&
                  hello.manifest->symbols == "symbols.toml");
        }
    });
}
