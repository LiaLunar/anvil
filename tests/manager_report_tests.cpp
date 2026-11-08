// Unit tests for the Manager's problem report (issue #186): what it says
// about Anvil, the game and the plugins, and that private paths and the user
// name do not leave in it. Part of anvil_manager_tests.
#include "problem_report.hpp"
#include "test_support.hpp"

#include <string>

using namespace anvil;
using namespace anvil::manager;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

cli::GamePlugin folder_plugin(const std::string& id, const std::string& version, const std::string& status) {
    cli::GamePlugin p;
    p.folder = id;
    p.report.manifest = manifest(plugin_toml(id, "", version));
    p.status = status;
    return p;
}

}  // namespace

void manager_report_tests() {
    run("problem report: paths, the user folder and the user name are replaced", [] {
        const std::vector<PrivateFolder> folders = {
            {L"C:\\Users\\Jan Kowalski", "<user folder>"},
            {L"D:\\Games\\Steam\\steamapps\\common\\Europa Universalis IV", "<game>"},
        };
        const std::string text =
            "discovered 2 plugin folder(s) in D:\\Games\\Steam\\steamapps\\common\\Europa Universalis IV\\anvil\\plugins\n"
            "same with slashes d:/games/steam/steamapps/common/europa universalis iv/anvil\n"
            "the player's settings: C:\\Users\\Jan Kowalski\\Documents\\Paradox Interactive\\Europa Universalis IV\\settings.txt\n"
            "another account: C:\\Users\\Other\\AppData\\Local\\x.dll\n"
            "a folder of its own: E:\\jkowal\\mods\\x.mod and E:\\jkowal-old\\y\n"
            "a longer folder stays: D:\\Games\\Steam\\steamapps\\common\\Europa Universalis IV2\\eu4.exe\n";
        const std::string out = redact_paths(text, folders, "jkowal");
        CHECK(contains(out, "in <game>\\anvil\\plugins"));
        CHECK(contains(out, "slashes <game>/anvil"));
        CHECK(contains(out, "settings: <user folder>\\Documents\\Paradox Interactive"));
        CHECK(contains(out, "account: <user folder>\\AppData"));
        CHECK(contains(out, "E:\\<user>\\mods\\x.mod"));
        CHECK(contains(out, "E:\\jkowal-old\\y"));  // not the whole component
        CHECK(contains(out, "Europa Universalis IV2\\eu4.exe"));
        CHECK(!contains(out, "Kowalski"));
        CHECK(!contains(out, "Other\\AppData"));
    });

    run("problem report: versions, build, plugins with their reasons, the log", [] {
        cli::GameReport game;
        game.enabled = true;
        game.disabled_plugins = {"tongs"};
        game.playset_found = true;
        game.enabled_mods = 3;
        game.plugins.push_back(folder_plugin("anvil-game", "1.8.0", "loaded"));
        game.plugins.push_back(folder_plugin("bellows", "0.19.0", "skipped"));
        game.plugins.push_back(folder_plugin("tongs", "0.26.0", "disabled"));
        game.load_order.push_back({"anvil-game", "1.8.0", "early", "anvil-game", ""});
        cli::SkippedPlugin skipped;
        skipped.id = "bellows";
        skipped.folder = "bellows";
        skipped.reason = "symbols.toml is for build eu4-1.37.5-steam-win64, the game is eu4-1.37.5.2-gog-win64";
        game.skipped.push_back(skipped);
        cli::GameModPlugin mod;
        mod.id = "hamweard";
        mod.mod = "Hamweard | Back to Main Menu";
        mod.workshop_id = "3832650792";
        mod.version = "1.0.0";
        mod.state = "pending";
        mod.status = "skipped";
        mod.reason = "waits for the player's approval";
        game.mod_plugins.push_back(mod);

        ProblemReportInput input;
        input.manager_version = "1.9.0";
        Installation installation;
        installation.dll = Installation::Dll::Anvil;
        installation.dll_version = "1.8.0";
        input.installation = installation;
        input.exe_sha256 = "ABCDEF";
        input.build = "eu4-1.37.5.2-gog-win64";
        input.game = &game;
        CrashSummary crash;
        crash.name = "2026-11-03_01-15-00";
        crash.summary = "in plugin bellows's code";
        crash.before_game_ready = true;
        input.newest_crash = crash;
        input.log_name = "anvil.log";
        input.log_tail = "[ 1.2 ms] info  [anvil] Anvil 1.8.0 (plugin API 2)\n[ 9.7 ms] info  [anvil] discovered in C:\\GOG Games\\EU4\\anvil\\plugins\n```oops\n";
        input.folders = {{L"C:\\GOG Games\\EU4", "<game>"}};

        const std::string out = problem_report(input);
        CHECK(contains(out, "| Anvil in the game folder | 1.8.0 |"));
        CHECK(contains(out, "| Anvil Manager | 1.9.0 |"));
        CHECK(contains(out, "| Game build | `eu4-1.37.5.2-gog-win64` |"));
        CHECK(contains(out, "| eu4.exe SHA-256 | `ABCDEF` |"));
        CHECK(contains(out, "disabled_plugins: tongs"));
        CHECK(contains(out, "| Playset | 3 enabled mod(s) |"));
        CHECK(contains(out, "2026-11-03_01-15-00: in plugin bellows's code, before the game was ready"));
        CHECK(contains(out, "| anvil-game | 1.8.0 | anvil/plugins/anvil-game | loaded (early) |"));
        CHECK(contains(out, "| bellows | 0.19.0 | anvil/plugins/bellows | skipped: symbols.toml is for build eu4-1.37.5-steam-win64"));
        CHECK(contains(out, "| tongs | 0.26.0 | anvil/plugins/tongs | disabled |"));
        CHECK(contains(out, "| hamweard | 1.0.0 | mod 'Hamweard \\| Back to Main Menu' (Workshop 3832650792) | skipped, pending: "
                            "waits for the player's approval |"));
        CHECK(contains(out, "discovered in <game>\\anvil\\plugins"));
        CHECK(!contains(out, "GOG Games"));
        CHECK(contains(out, "````text\n"));  // longer than the log's own fence
        CHECK(out.size() > 2 && out.substr(out.size() - 5) == "````\n");
    });

    run("problem report: no anvil folder, no build, an empty log", [] {
        ProblemReportInput input;
        input.manager_version = "1.9.0";
        input.installation = Installation{};
        input.log_name = "anvil.log";
        const std::string out = problem_report(input);
        CHECK(contains(out, "| Anvil in the game folder | not installed |"));
        CHECK(contains(out, "| Game build | unknown to this Anvil |"));
        CHECK(contains(out, "| anvil.toml | no anvil folder |"));
        CHECK(contains(out, "| Newest crash report | none |"));
        CHECK(!contains(out, "#### Plugins"));
        CHECK(contains(out, "anvil.log is empty or missing"));
        CHECK(contains(out, "| Game folders found | Steam 0, GOG 0; this folder is neither Steam's nor GOG's |"));
        CHECK(!contains(out, "| eu4.exe running |"));
        CHECK(!contains(out, "| Last game from this folder |"));
    });

    run("problem report: the game folders found and a game that ran without Anvil (issue #253)", [] {
        ProblemReportInput input;
        input.manager_version = "1.9.0";
        input.installation = Installation{};
        input.log_name = "anvil.log";
        input.steam_folders = 2;
        input.chosen_steam = 1;
        input.running_elsewhere = 1;
        input.anvil_missed = true;
        std::string out = problem_report(input);
        CHECK(contains(out, "| Game folders found | Steam 2, GOG 0; this is Steam's folder 2 of 2 |"));
        CHECK(contains(out, "| eu4.exe running | from 1 other folder(s) |"));
        CHECK(contains(out, "| Last game from this folder | it ran, but Anvil did not load"));

        input.chosen_steam.reset();
        input.gog_folder = true;
        input.chosen_gog = true;
        out = problem_report(input);
        CHECK(contains(out, "| Game folders found | Steam 2, GOG 1; this is GOG's folder |"));
    });
}
