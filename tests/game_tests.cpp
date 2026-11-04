// Unit tests for anvil check --game (design §16) on a made-up game folder:
// a synthetic eu4.exe and plugins in anvil/plugins. Nothing here needs, or
// touches, a real game.
// Usage: anvil_game_tests <path of anvil.exe>
#include "cli/game.hpp"
#include "core/game_build.hpp"
#include "core/mod_plugins.hpp"
#include "test_image.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace anvil;
using namespace anvil::cli;
namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// A symbol over the 8 bytes at 0x1000 of the test image (48 8B 81 18 04 00 00 C3).
std::string symbols(const std::string& build, const std::string& name = "speed_object", uint32_t rva = 0x1000,
                    const std::string& bytes = "48 8B 81 18 04 00 00 C3") {
    char at[16];
    std::snprintf(at, sizeof(at), "0x%X", rva);
    return "build = \"" + build + "\"\n\n[symbol." + name + "]\nrva = " + at + "\nbytes = \"" + bytes +
           "\"\nmemory = \"code\"\n";
}

struct Game {
    fs::path root;
    fs::path dir;  // the EU4 folder
    fs::path user;  // EU4's user folder: dlc_load.json and mod/
    std::string exe_sha256;

    Game() {
        root = fs::temp_directory_path() / ("anvil_game_tests_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root);
        dir = root / "eu4";
        const auto bytes = TestImage().file();
        write(dir / "eu4.exe", std::string(bytes.begin(), bytes.end()));
        exe_sha256 = sha256_file(dir / "eu4.exe");
        fs::create_directories(dir / "anvil" / "plugins");
        user = root / "user";
        fs::create_directories(user / "mod");
    }
    ~Game() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // anvil/plugins/<id> with plugin.toml (+ symbols.toml when given) and its entry DLL.
    void plugin(const std::string& id, const std::string& extra = "", const std::string& symbols_text = "",
                const std::string& phase = "main", bool with_entry = true) const {
        const fs::path folder = dir / "anvil" / "plugins" / id;
        fs::remove_all(folder);
        // Keys before tables: the symbols key goes first.
        write(folder / "plugin.toml", plugin_toml(id, (symbols_text.empty() ? "" : "symbols = \"symbols.toml\"\n") + extra, "1.0.0", phase));
        if (!symbols_text.empty()) write(folder / "symbols.toml", symbols_text);
        if (with_entry) write(folder / (id + ".dll"), "MZ");
    }

    void settings(const std::string& text) const { write(dir / "anvil" / "anvil.toml", text); }

    // A local mod <user>/mod/<mod>, enabled in dlc_load.json, with a plugin in its anvil/<id>.
    fs::path mod_plugin(const std::string& mod, const std::string& id, const std::string& extra = "",
                        const std::string& symbols_text = "") {
        const fs::path folder = user / "mod" / mod / "anvil" / id;
        write(folder / "plugin.toml", plugin_toml(id, (symbols_text.empty() ? "" : "symbols = \"symbols.toml\"\n") + extra));
        if (!symbols_text.empty()) write(folder / "symbols.toml", symbols_text);
        write(folder / (id + ".dll"), "MZ " + mod);
        write(user / "mod" / (mod + ".mod"), "name=\"" + mod + "\"\npath=\"mod/" + mod + "\"\n");
        if (std::find(mods.begin(), mods.end(), mod) == mods.end()) mods.push_back(mod);
        std::string json = "{\"enabled_mods\":[";
        for (size_t i = 0; i < mods.size(); ++i) json += (i ? ",\"mod/" : "\"mod/") + mods[i] + ".mod\"";
        write(user / "dlc_load.json", json + "],\"disabled_dlcs\":[]}");
        return folder;
    }

    // Approves the plugin `id` of `mod` as the overlay or the Manager does.
    bool approve(const std::string& id, const std::string& mod) const {
        const ModDiscovery d = discover_mod_plugins(dir / "anvil", user, {});
        for (const ModPlugin& p : d.plugins) {
            std::string error;
            if (p.id == id && p.mod.name == mod) return approve_mod_plugin(dir / "anvil", p, p.hash, "", error);
        }
        return false;
    }

    std::vector<std::string> mods;

    GameReport check(const fs::path& game = {}) const {
        GameOptions options;
        options.game_dir = game.empty() ? dir : game;
        options.user_dir = user;
        const std::string sha = exe_sha256;
        options.build_of = [sha](std::string_view actual) -> const char* { return actual == sha ? "test-build" : nullptr; };
        return check_game(options);
    }
};

std::string requires_plugin(const std::string& id) { return "\n[[dependencies]]\nid = \"" + id + "\"\nversion = \"*\"\n"; }
std::string conflicts_with(const std::string& id) { return "\n[[conflicts]]\nid = \"" + id + "\"\n"; }

bool skipped_for(const GameReport& report, const std::string& id, const std::string& reason_part, bool disabled = false) {
    for (const auto& s : report.skipped) {
        if (s.id == id) return s.disabled == disabled && contains(s.reason, reason_part);
    }
    return false;
}

std::string loaded(const GameReport& report) {
    std::string out;
    for (const auto& p : report.load_order) out += (out.empty() ? "" : ",") + p.id;
    return out;
}

const GamePlugin* plugin_of(const GameReport& report, const std::string& folder) {
    for (const auto& p : report.plugins) {
        if (p.folder == folder) return &p;
    }
    return nullptr;
}

struct Run {
    int exit_code = -1;
    std::string out;
};

Run run_anvil(const std::string& anvil, const std::string& args) {
    constexpr char kCr = 13;
    const std::string command = "\"\"" + anvil + "\" " + args + " 2>nul\"";
    Run run;
    FILE* pipe = _popen(command.c_str(), "rb");
    if (pipe == nullptr) return run;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0;) run.out.append(buffer, n);
    std::erase(run.out, kCr);  // the console text mode of anvil.exe
    run.exit_code = _pclose(pipe);
    return run;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_game_tests <anvil.exe>\n");
        return 2;
    }
    const std::string anvil = fs::path(argv[1]).make_preferred().string();

    run("a game folder without plugins is fine, one that is not a game folder cannot be checked", [&] {
        const Game game;
        const GameReport empty = game.check();
        CHECK(empty.exit_code == kExitOk && empty.plugins.empty() && empty.load_order.empty() && empty.skipped.empty());
        CHECK(empty.exe_build == "test-build" && empty.enabled);

        CHECK(game.check(game.root / "missing").exit_code == kExitCannotCheck);
        fs::create_directories(game.root / "no-exe" / "anvil");
        const GameReport no_exe = game.check(game.root / "no-exe");
        CHECK(no_exe.exit_code == kExitCannotCheck && contains(no_exe.cannot_check, "no eu4.exe in"));
        fs::create_directories(game.root / "no-anvil");
        write(game.root / "no-anvil" / "eu4.exe", "MZ");
        const GameReport no_anvil = game.check(game.root / "no-anvil");
        CHECK(no_anvil.exit_code == kExitCannotCheck && contains(no_anvil.cannot_check, "no anvil folder in"));
        fs::create_directories(game.root / "bad-exe" / "anvil");
        write(game.root / "bad-exe" / "eu4.exe", "not a pe file");
        CHECK(game.check(game.root / "bad-exe").exit_code == kExitCannotCheck);
        // Without a plugins folder there is nothing to load.
        fs::remove_all(game.dir / "anvil" / "plugins");
        CHECK(game.check().exit_code == kExitOk);
    });

    run("plugins are resolved as at start-up: order, skips and their reasons", [&] {
        const Game game;
        game.plugin("a-core", "", symbols("test-build"), "early");
        game.plugin("b-user", requires_plugin("a-core"), symbols("test-build"));
        game.plugin("c-plain");
        game.plugin("d-stale", "", symbols("other-build"));
        game.plugin("e-conflict", conflicts_with("a-core"));
        game.plugin("f-needs-d", requires_plugin("d-stale"));
        game.plugin("g-off");
        game.plugin("h-no-entry", "", "", "main", false);
        game.plugin("k-one", "load_after = [\"k-two\"]\n");
        game.plugin("k-two", "load_after = [\"k-one\"]\n");
        game.plugin("m-bad-symbol", "", symbols("test-build", "wrong", 0x1000, "90 90"));
        write(game.dir / "anvil" / "plugins" / "i-broken" / "plugin.toml", plugin_toml("i-broken", "bogus = 1\n"));
        write(game.dir / "anvil" / "plugins" / "i-broken" / "i-broken.dll", "MZ");
        write(game.dir / "anvil" / "plugins" / "not-a-plugin" / "readme.txt", "x");
        write(game.dir / "anvil" / "plugins" / "loose-file.txt", "x");
        game.settings("disabled_plugins = [\"g-off\"]\n");

        const GameReport r = game.check();
        CHECK(r.exit_code == kExitProblems);
        CHECK(loaded(r) == "a-core,b-user,c-plain");  // early first, then main by id
        CHECK(r.load_order[0].phase == "early" && r.load_order[1].phase == "main" && r.load_order[0].version == "1.0.0");
        CHECK(r.load_order[0].folder == "a-core");
        CHECK(skipped_for(r, "d-stale", "symbols.toml is for build other-build, the game is test-build"));
        CHECK(skipped_for(r, "e-conflict", "conflicts with a-core 1.0.0; disable one of them"));
        CHECK(skipped_for(r, "f-needs-d", "requires plugin 'd-stale'"));
        CHECK(skipped_for(r, "g-off", "disabled in anvil.toml", true));
        CHECK(skipped_for(r, "h-no-entry", "entry h-no-entry.dll not found"));
        CHECK(skipped_for(r, "k-one", "cycle") && skipped_for(r, "k-two", "cycle"));
        CHECK(skipped_for(r, "m-bad-symbol", "(EU4 updated or eu4.exe modified?)"));
        CHECK(skipped_for(r, "i-broken", "bogus"));
        CHECK(skipped_for(r, "not-a-plugin", "plugin.toml"));
        CHECK(r.plugins.size() == 13);  // folders only, in discovery order
        CHECK(plugin_of(r, "a-core")->status == "loaded" && plugin_of(r, "g-off")->status == "disabled" &&
              plugin_of(r, "d-stale")->status == "skipped");
        // The plugins' own problems are reported at their lines, and nothing is written into the game folder.
        const GamePlugin* stale = plugin_of(r, "d-stale");
        CHECK(stale->report.exit_code == kExitProblems && !stale->report.problems.empty() && stale->report.lock.empty());
        CHECK(!fs::exists(game.dir / "anvil" / "plugins" / "a-core" / "symbols.lock"));
        CHECK(plugin_of(r, "a-core")->report.exit_code == kExitOk);
        CHECK(plugin_of(r, "i-broken")->report.problems.size() == 1 && plugin_of(r, "i-broken")->report.problems[0].line == 8);
        CHECK(plugin_of(r, "not-a-plugin")->report.problems.size() == 1);
        CHECK(r.skipped.size() == 10);
        // The same folder on a second run gives the same answer.
        const GameReport again = game.check();
        CHECK(loaded(again) == loaded(r) && again.skipped.size() == r.skipped.size());
    });

    run("what the player disabled does not make the check fail", [&] {
        const Game game;
        game.plugin("a-ok");
        game.plugin("b-stale", "", symbols("other-build"));
        CHECK(game.check().exit_code == kExitProblems);
        game.settings("disabled_plugins = [\"b-stale\"]\n");
        const GameReport disabled = game.check();
        CHECK(disabled.exit_code == kExitOk && loaded(disabled) == "a-ok");
        CHECK(!plugin_of(disabled, "b-stale")->report.problems.empty() && plugin_of(disabled, "b-stale")->disabled);
        game.settings("enabled = false\n");
        const GameReport off = game.check();
        CHECK(off.exit_code == kExitOk && !off.enabled && off.load_order.empty() && off.skipped.size() == 2);
        CHECK(skipped_for(off, "a-ok", "enabled = false", true));
        // A broken anvil.toml is ignored by the loader, so it is a problem here.
        game.settings("enabled = [\n");
        const GameReport broken = game.check();
        CHECK(broken.exit_code == kExitProblems && broken.problems.size() == 1 && broken.problems[0].line != 0);
        CHECK(contains(broken.problems[0].file, "anvil.toml") && broken.enabled);
    });

    run("overlapping code ranges of plugins that load together are warnings", [&] {
        const Game game;
        game.plugin("a-first", "", symbols("test-build"));
        game.plugin("b-same", "", symbols("test-build", "same_function"));  // the same range: a chain
        game.plugin("c-inside", "", symbols("test-build", "tail", 0x1005, "00 00 C3"));
        game.plugin("d-apart", "", symbols("test-build", "elsewhere", 0x1010, "48 8B 05 F1 1F 00 00"));
        const GameReport r = game.check();
        CHECK(r.exit_code == kExitOk && loaded(r) == "a-first,b-same,c-inside,d-apart");
        CHECK(r.warnings.size() == 2);  // a-first/c-inside and b-same/c-inside
        CHECK(r.warnings[0].plugin == "a-first" && r.warnings[0].other == "c-inside" && r.warnings[0].line == 3);
        CHECK(contains(r.warnings[0].message, "symbol speed_object (rva 0x1000, 8 byte(s)) overlaps symbol tail of plugin c-inside"));
        CHECK(contains(r.warnings[0].file, "a-first") && contains(r.warnings[0].file, "symbols.toml"));
        CHECK(r.warnings[1].plugin == "b-same" && r.warnings[1].other == "c-inside");

        // The same address with fewer verified bytes is a chain too: the length is not what a plugin changes.
        const Game shorter;
        shorter.plugin("a-first", "", symbols("test-build"));
        shorter.plugin("b-shorter", "", symbols("test-build", "shorter", 0x1000, "48 8B 81 18"));
        const GameReport chain = shorter.check();
        CHECK(chain.exit_code == kExitOk && loaded(chain) == "a-first,b-shorter" && chain.warnings.empty());

        // A plugin that is skipped overlaps nothing.
        game.plugin("c-inside", conflicts_with("a-first"), symbols("test-build", "tail", 0x1005, "00 00 C3"));
        const GameReport skipped = game.check();
        CHECK(skipped.warnings.empty() && skipped_for(skipped, "c-inside", "conflicts with a-first"));
        // Overlaps within one plugin are its own business.
        const Game alone;
        alone.plugin("e-self", "", "build = \"test-build\"\n\n[symbol.one]\nrva = 0x1000\nbytes = \"48 8B 81 18\"\nmemory = \"code\"\n"
                                  "\n[symbol.two]\nrva = 0x1002\nbytes = \"81 18 04 00\"\nmemory = \"code\"\n");
        CHECK(alone.check().warnings.empty() && alone.check().exit_code == kExitOk);
    });

    run("plugins in mods: approved, pending, conflicting, shadowed and invalid ones with their reasons", [&] {
        Game game;
        game.plugin("a-local");
        game.plugin("z-needs-mod", requires_plugin("m-approved"));
        game.mod_plugin("Mod One", "m-approved", requires_plugin("a-local"));
        game.mod_plugin("Mod One", "m-pending");
        game.mod_plugin("Mod One", "a-local");  // the local plugin wins
        game.mod_plugin("Mod One", "m-twice");
        game.mod_plugin("Mod Two", "m-twice");
        game.mod_plugin("Mod Two", "Bad");  // not a plugin id
        CHECK(game.approve("m-approved", "Mod One"));
        GameReport r = game.check();
        const auto mod_plugin = [&](const std::string& id, const std::string& mod) -> const GameModPlugin* {
            for (const auto& p : r.mod_plugins) {
                if (p.id == id && p.mod == mod) return &p;
            }
            return nullptr;
        };
        CHECK(r.playset_found && r.enabled_mods == 2 && r.mod_plugins.size() == 6 && r.mod_warnings.empty());
        CHECK(loaded(r) == "a-local,m-approved,z-needs-mod");
        const GameModPlugin* approved = mod_plugin("m-approved", "Mod One");
        CHECK(approved->state == "approved" && approved->status == "loaded" && !approved->copy_missing);
        CHECK(contains(approved->loads_from, "/anvil/data/anvil/approved/m-approved/" + approved->hash));
        CHECK(approved->approved_hash == approved->hash && approved->approved_version == "1.0.0" && approved->workshop_id.empty());
        CHECK(approved->report.manifest && approved->report.manifest->folder.wstring().find(L"approved") != std::wstring::npos);
        CHECK(mod_plugin("m-pending", "Mod One")->state == "pending" && mod_plugin("m-pending", "Mod One")->status == "skipped");
        CHECK(mod_plugin("a-local", "Mod One")->state == "shadowed");
        CHECK(mod_plugin("m-twice", "Mod One")->state == "conflict" && mod_plugin("m-twice", "Mod Two")->others[0] == "Mod One");
        CHECK(mod_plugin("Bad", "Mod Two")->state == "invalid");
        CHECK(r.exit_code == kExitProblems);  // the invalid one; the others wait for the player

        const std::string text = format_game_report(r, Format::Text);
        CHECK(contains(text, "plugins in mods (6, 2 enabled mod(s) in "));
        CHECK(contains(text, "  m-approved 1.0.0 in mod 'Mod One' (local mod): approved; approved on "));
        CHECK(contains(text, " (loads)\n") && contains(text, "  m-pending 1.0.0 in mod 'Mod One' (local mod): pending; waiting for approval"));
        CHECK(contains(text, "  2. m-approved 1.0.0 (main) from mod 'Mod One'\n"));
        CHECK(contains(text, ", 6 in mods (3 waiting for the player): 1 problem(s)\n"));
        const std::string json = format_game_report(r, Format::Json);
        CHECK(contains(json, "\"mods\": {\"user_dir\": ") && contains(json, "\"dlc_load\": true, \"enabled\": 2, \"warnings\": []},"));
        CHECK(contains(json, "\"mod_plugins\": [\n    {\n      \"id\": ") && contains(json, "\"state\": \"conflict\""));
        CHECK(contains(json, "\"phase\": \"main\", \"folder\": \"\", \"mod\": \"Mod One\"}"));
        CHECK(contains(json, "\"approved\": {\"version\": \"1.0.0\", \"hash\": \"" + approved->hash + "\", \"date\": \""));
        CHECK(contains(json, "\"workshop_id\": null") && contains(json, "\"approved\": null"));
        const std::string github = format_game_report(r, Format::Github);
        CHECK(contains(github, "::error file=") && contains(github, "Mod Two/anvil/Bad/plugin.toml,title=anvil check::Bad in mod"));

        fs::remove_all(game.user / "mod" / "Mod Two" / "anvil" / "Bad");
        CHECK(game.check().exit_code == kExitOk);

        // Disabled in anvil.toml: listed as such, and what needs it is skipped.
        game.settings("disabled_plugins = [\"m-approved\"]\n");
        r = game.check();
        CHECK(mod_plugin("m-approved", "Mod One")->status == "disabled" && skipped_for(r, "m-approved", "disabled in anvil.toml", true));
        CHECK(skipped_for(r, "z-needs-mod", "m-approved") && r.exit_code == kExitProblems);
        CHECK(contains(format_game_report(r, Format::Text), "  m-approved from mod 'Mod One' (disabled): disabled in anvil.toml\n"));
    });

    run("plugins in mods: an update loads the approved copy; a missing copy is reported, not made", [&] {
        Game game;
        const fs::path folder = game.mod_plugin("M", "p");
        CHECK(game.approve("p", "M"));
        GameReport r = game.check();
        const std::string hash = r.mod_plugins[0].hash;
        const fs::path copy = approved_copy(game.dir / "anvil", "p", hash);
        write(folder / "p.dll", "MZ new");  // Steam updates the mod
        r = game.check();
        CHECK(r.mod_plugins[0].state == "update-pending" && r.mod_plugins[0].status == "loaded" && loaded(r) == "p");
        CHECK(r.mod_plugins[0].hash != hash && r.mod_plugins[0].approved_hash == hash && r.exit_code == kExitOk);
        CHECK(r.mod_plugins[0].report.manifest && r.mod_plugins[0].report.manifest->folder == copy);

        write(folder / "p.dll", "MZ M");  // back to the approved files, without the copy
        fs::remove_all(copy);
        r = game.check();
        CHECK(r.mod_plugins[0].state == "approved" && r.mod_plugins[0].copy_missing && loaded(r) == "p");
        CHECK(r.mod_plugins[0].report.manifest && r.mod_plugins[0].report.manifest->folder == folder);
        CHECK(!fs::exists(copy));
        CHECK(contains(format_game_report(r, Format::Json), "\"copy_missing\": true"));
    });

    run("plugins in mods: a dlc_load.json or trust.toml that does not parse is a problem", [&] {
        Game game;
        game.mod_plugin("M", "p");
        write(game.user / "dlc_load.json", "{");
        GameReport r = game.check();
        CHECK(r.exit_code == kExitProblems && r.mod_plugins.empty() && r.problems.size() == 1);
        CHECK(contains(r.problems[0].file, "dlc_load.json") && contains(r.problems[0].message, "no plugins from mods"));
        game.mod_plugin("M", "p");
        write(game.dir / "anvil" / "trust.toml", "[[plugin]]\nid = 1\n");
        r = game.check();
        CHECK(r.exit_code == kExitProblems && r.problems.size() == 1 && r.problems[0].line == 2);
        CHECK(contains(r.problems[0].message, "approves no plugin from a mod") && r.mod_plugins[0].state == "pending");
        fs::remove(game.dir / "anvil" / "trust.toml");
        write(game.user / "mod" / "M.mod", "name=\"M\"\n");
        r = game.check();
        CHECK(r.mod_warnings.size() == 1 && contains(r.mod_warnings[0], "mod mod/M.mod: the descriptor has no path"));
        CHECK(r.exit_code == kExitOk && contains(format_game_report(r, Format::Text), "warning: mod mod/M.mod: the descriptor has no path"));
    });

    run("output formats", [&] {
        const Game game;
        game.plugin("a-core", "", symbols("test-build"));
        game.plugin("b-tail", "", symbols("test-build", "tail", 0x1005, "00 00 C3"));
        game.plugin("c-conflict", conflicts_with("a-core"));
        game.plugin("d-stale", "", symbols("other-build"));
        game.plugin("e-off");
        game.settings("disabled_plugins = [\"e-off\"]\n");
        const GameReport r = game.check();

        const std::string text = format_game_report(r, Format::Text);
        CHECK(contains(text, "load order (2):\n  1. a-core 1.0.0 (main)\n  2. b-tail 1.0.0 (main)\n"));
        CHECK(contains(text, "skipped (3):\n"));
        CHECK(contains(text, "  c-conflict: conflicts with a-core 1.0.0; disable one of them\n"));
        CHECK(contains(text, "  e-off (disabled): disabled in anvil.toml\n"));
        CHECK(contains(text, "d-stale/symbols.toml:1: error: symbols.toml is for other-build, eu4.exe is test-build\n"));
        CHECK(contains(text, "a-core/symbols.toml:3: warning: code range of symbol speed_object"));
        CHECK(contains(text, "a-core: 1 symbol(s) for test-build, checked against eu4.exe (test-build): OK\n"));
        CHECK(contains(text, ": 5 plugin(s), 2 will load, 3 skipped, 1 warning(s): 2 problem(s)\n"));

        const std::string github = format_game_report(r, Format::Github);
        CHECK(contains(github, "::error file=" + github_escape(r.game_dir + "/anvil/plugins/c-conflict/plugin.toml", true) + ",title=anvil check::c-conflict is skipped: conflicts with a-core"));
        CHECK(contains(github, "::warning file="));
        CHECK(contains(github, "symbols.toml,line=1,title=anvil check::symbols.toml is for other-build"));
        // A plugin whose own problems explain the skip gets no second annotation.
        CHECK(!contains(github, "d-stale is skipped"));
        CHECK(!contains(github, "e-off is skipped"));

        const std::string json = format_game_report(r, Format::Json);
        CHECK(contains(json, "\"result\": \"problems\"") && contains(json, "\"exit_code\": 1"));
        CHECK(contains(json, "\"game\": {\"folder\": ") && contains(json, "\"exe\": {\"name\": \"eu4.exe\", \"sha256\": \"" + game.exe_sha256));
        CHECK(contains(json, "\"build\": \"test-build\"}"));
        CHECK(contains(json, "\"anvil_toml\": {\"enabled\": true, \"disabled_plugins\": [\"e-off\"]}"));
        CHECK(contains(json, "\"plugins\": [\n    {\n      \"folder\": \"a-core\",\n      \"status\": \"loaded\",\n      \"result\": \"ok\""));
        CHECK(contains(json, "\"status\": \"disabled\"") && contains(json, "\"status\": \"skipped\""));
        CHECK(contains(json, "\"load_order\": [\n    {\"id\": \"a-core\", \"version\": \"1.0.0\", \"phase\": \"main\", \"folder\": \"a-core\"},\n"));
        CHECK(contains(json, "{\"id\": \"e-off\", \"folder\": \"e-off\", \"reason\": \"disabled in anvil.toml\", \"disabled\": true}"));
        CHECK(contains(json, "\"warnings\": [\n    {\"file\": ") && contains(json, "\"plugins\": [\"a-core\", \"b-tail\"]}"));
        CHECK(json.back() == '\n' && json.find("\"tool\": \"anvil ") != std::string::npos);

        // The same folder with nothing wrong.
        const Game clean;
        clean.plugin("a-core", "", symbols("test-build"));
        const std::string clean_json = format_game_report(clean.check(), Format::Json);
        CHECK(contains(clean_json, "\"result\": \"ok\"") && contains(clean_json, "\"problems\": [],\n  \"plugins\": ["));
        CHECK(contains(clean_json, "\"skipped\": [],\n  \"warnings\": []\n}\n"));
        const std::string cannot_json = format_game_report(clean.check(clean.root / "none"), Format::Json);
        CHECK(contains(cannot_json, "\"result\": \"cannot-check\"") && contains(cannot_json, "\"cannot_check\": ") &&
              contains(cannot_json, "\"plugins\": [],"));
        CHECK(format_game_report(clean.check(clean.root / "none"), Format::Github).starts_with("::error title=anvil check::"));
    });

    run("anvil.exe check --game: arguments, exit codes and output", [&] {
        const Game game;
        game.plugin("a-plain");
        game.plugin("b-conflict", conflicts_with("a-plain"));
        // anvil.exe knows only real game builds, so a plugin with symbols fails here by design.
        game.plugin("c-symbols", "", symbols("test-build"));

        const Run json = run_anvil(anvil, "check --game " + quoted(game.dir) + " --user-dir " + quoted(game.user) + " --format json");
        CHECK(json.exit_code == 1 && contains(json.out, "\"load_order\"") && contains(json.out, "\"skipped\""));
        CHECK(contains(json.out, "is not a game build this Anvil knows"));
        const Run text = run_anvil(anvil, "check --game=" + quoted(game.dir) + " --user-dir=" + quoted(game.user));
        std::printf("%d\n%s\n", text.exit_code, text.out.c_str());
        CHECK(text.exit_code == 1 && contains(text.out, "load order (1):\n  1. a-plain 1.0.0 (main)\n"));
        CHECK(contains(text.out, "  b-conflict: conflicts with a-plain 1.0.0"));
        CHECK(!fs::exists(game.dir / "anvil" / "plugins" / "c-symbols" / "symbols.lock"));

        const Game clean;
        clean.plugin("a-plain");
        const Run ok = run_anvil(anvil, "check --game " + quoted(clean.dir) + " --user-dir " + quoted(clean.user) + " --format github");
        CHECK(ok.exit_code == 0 && contains(ok.out, "1 will load, 0 skipped: OK"));

        CHECK(run_anvil(anvil, "check --game " + quoted(game.root / "missing")).exit_code == 2);
        CHECK(run_anvil(anvil, "check --game").exit_code == 2);
        CHECK(run_anvil(anvil, "check --game " + quoted(game.dir) + " " + quoted(game.dir)).exit_code == 2);
        CHECK(run_anvil(anvil, "check --game " + quoted(game.dir) + " --exe " + quoted(game.dir / "eu4.exe")).exit_code == 2);
        CHECK(run_anvil(anvil, "check --game " + quoted(game.dir) + " --format xml").exit_code == 2);
        CHECK(run_anvil(anvil, "check --game " + quoted(game.dir) + " --user-dir").exit_code == 2);
        CHECK(run_anvil(anvil, "check " + quoted(game.dir) + " --user-dir " + quoted(game.user)).exit_code == 2);
        CHECK(contains(run_anvil(anvil, "--help").out, "check --game <EU4 folder> [--format text|json|github]"));
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
