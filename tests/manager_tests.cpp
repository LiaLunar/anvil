// Unit tests for the Manager's core (tools/manager, design §15, issue #59):
// zips, Steam's files, the install, repair and uninstall plans, plugin zips
// the release check and the plugin catalog, on made-up game folders in the
// temporary folder.
// Nothing here needs, or touches, a real game.
// Usage: anvil_manager_tests <path of version.dll>
#include "catalog.hpp"
#include "core/game_build.hpp"
#include "files.hpp"
#include "game.hpp"
#include "install.hpp"
#include "mods.hpp"
#include "core/trust.hpp"
#include "plugin_zip.hpp"
#include "net/release_check.hpp"
#include "start_menu.hpp"
#include "test_image.hpp"
#include "test_support.hpp"
#include "zip.hpp"

#include <windows.h>

#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace anvil;
using namespace anvil::manager;
using namespace anvil::net;
namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string zip_refusal(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string error;
    return ZipArchive::open(make_zip(files), error) ? std::string() : error;
}

std::optional<ZipArchive> zip_of(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string error;
    auto zip = ZipArchive::open(make_zip(files), error);
    if (!zip) std::printf("  zip error: %s\n", error.c_str());
    return zip;
}

// A temporary folder removed at the end of a test.
struct TempDir {
    fs::path dir;
    explicit TempDir(const std::string& name) {
        dir = fs::temp_directory_path() / ("anvil_manager_tests_" + name + "_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// A game folder with a synthetic eu4.exe (the build is unknown to Anvil).
fs::path make_game(const fs::path& dir) {
    const auto bytes = TestImage().file();
    write(dir / "eu4.exe", std::string(bytes.begin(), bytes.end()));
    return dir;
}

// A package like the one the Manager carries, with the real version.dll.
std::optional<Package> make_package(const std::string& version_dll) {
    std::string error;
    auto zip = ZipArchive::open(make_zip({
                                    {"version.dll", version_dll},
                                    {"anvil/anvil.toml", "enabled = true\ndisabled_plugins = []\n"},
                                    {"anvil/plugins/README.txt", "Put each plugin into its own folder here.\n"},
                                    {"anvil/plugins/anvil-game/plugin.toml", plugin_toml("anvil-game")},
                                    {"anvil/plugins/anvil-game/anvil-game.dll", "MZ game"},
                                    {"anvil/plugins/anvil-overlay/plugin.toml", plugin_toml("anvil-overlay")},
                                    {"anvil/plugins/anvil-overlay/anvil-overlay.dll", "MZ overlay"},
                                    {"anvil/docs/LICENSE", "MIT"},
                                    {"anvil/docs/third-party/miniz-LICENSE.txt", "MIT"},
                                }),
                                error);
    if (!zip) return std::nullopt;
    auto package = load_package(std::move(*zip), ANVIL_VERSION, error);
    if (!package) std::printf("  package error: %s\n", error.c_str());
    return package;
}

bool has_step(const Plan& plan, Step::Kind kind, const std::string& path) {
    return std::any_of(plan.steps.begin(), plan.steps.end(),
                       [&](const Step& s) { return s.kind == kind && s.path == path; });
}

// The synthetic PE with another Machine: 0x014C makes it a 32-bit x86 file.
std::string pe_with_machine(uint16_t machine) {
    auto bytes = TestImage().file();
    uint32_t pe = 0;
    std::memcpy(&pe, bytes.data() + 0x3C, 4);
    std::memcpy(bytes.data() + pe + 4, &machine, 2);
    return std::string(bytes.begin(), bytes.end());
}

// A game folder with EU4DLL as release 93 lays it out (lab#104).
void add_eu4dll(const fs::path& game) {
    write(game / "version.dll", read(game / "eu4.exe"));  // a PE without Anvil's resource
    write(game / "d3d9.dll", pe_with_machine(0x014C));
    write(game / ".dist.v1.json", "{}");
    write(game / "README.md", "EU4DLL");
    for (const char* file : {"plugin64.dll", "plugin.ini", "Plugin.dll", "dllautoupdater.exe", "autoupdate64.bat"}) {
        write(game / "plugins" / file, file);
    }
}

bool make_junction(const fs::path& link, const fs::path& target) {
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
    return _wsystem(command.c_str()) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--sleep") {  // the "game" of the running-game test
        Sleep(30000);
        return 0;
    }
    // The runner tests' game; also started as eu4.exe by a Manager (no arguments, the arm in its environment).
    if ((argc >= 2 && std::string(argv[1]) == "--bench-game") || (argc == 1 && GetEnvironmentVariableW(L"ANVIL_BENCHMARK", nullptr, 0) != 0)) {
        return fake_benchmark_game();
    }
    if (argc < 2) {
        std::printf("usage: anvil_manager_tests <version.dll>\n");
        return 2;
    }
    const std::string version_dll = read(argv[1]);
    CHECK(!version_dll.empty());

    run("zip: names that would leave the folder or alias another are refused", [] {
        CHECK(zip_refusal({{"a/b.txt", "x"}, {"c.txt", "y"}}).empty());
        CHECK(contains(zip_refusal({{"../evil.dll", "x"}}), "'..' part"));
        CHECK(contains(zip_refusal({{"a/../../evil.dll", "x"}}), "'..' part"));
        CHECK(contains(unsafe_path_reason("/abs.dll"), "absolute path"));  // miniz will not even write it
        CHECK(contains(zip_refusal({{"\\abs.dll", "x"}}), "absolute path"));
        CHECK(contains(zip_refusal({{"C:/x.dll", "x"}}), "drive or stream"));
        CHECK(contains(zip_refusal({{"a.dll:stream", "x"}}), "drive or stream"));
        CHECK(contains(zip_refusal({{"a/./b", "x"}}), "'.' part"));
        CHECK(contains(zip_refusal({{"a//b", "x"}}), "empty folder name"));
        CHECK(contains(zip_refusal({{"a/b.", "x"}}), "ending in a dot or space"));
        CHECK(contains(zip_refusal({{"a /b", "x"}}), "ending in a dot or space"));
        CHECK(contains(zip_refusal({{"con.txt", "x"}}), "reserved device name"));
        CHECK(contains(zip_refusal({{"a/LPT1", "x"}}), "reserved device name"));
        CHECK(zip_refusal({{"a/com10.txt", "x"}, {"console.txt", "y"}}).empty());
        CHECK(contains(zip_refusal({{"a?.txt", "x"}}), "does not allow"));
        CHECK(contains(zip_refusal({{"A.txt", "x"}, {"a.txt", "y"}}), "differ only in case"));
        CHECK(contains(zip_refusal({{"a", "x"}, {"a/b", "y"}}), "both a file and a folder"));
        std::string error;
        CHECK(!ZipArchive::open(std::vector<uint8_t>{'P', 'K', 3, 4}, error) && contains(error, "not a zip archive"));
    });

    run("zip: backslashes are separators, UTF-8 names and contents read back", [] {
        const auto zip = zip_of({{"dir\\file.txt", "one"}, {"zaż\xC3\xB3\xC5\x82\xC4\x87.txt", std::string(100000, 'z')}});
        CHECK(zip.has_value());
        if (!zip) return;
        CHECK(zip->entries().size() == 2);
        const ZipEntry* file = zip->find("dir/file.txt");
        CHECK(file != nullptr && file->size == 3);
        std::string error;
        CHECK(file != nullptr && zip->read(*file, error) == std::optional<std::string>("one"));
        const ZipEntry* utf8 = zip->find("zaż\xC3\xB3\xC5\x82\xC4\x87.txt");
        CHECK(utf8 != nullptr && zip->read(*utf8, error)->size() == 100000);
    });

    run("files: remove_tree removes a junction, never what it points to", [] {
        const TempDir temp("tree");
        write(temp.dir / "outside" / "keep.txt", "keep");
        write(temp.dir / "tree" / "a" / "b.txt", "b");
        CHECK(make_junction(temp.dir / "tree" / "link", temp.dir / "outside"));
        std::vector<fs::path> files;
        list_files(temp.dir / "tree", files);
        CHECK(files.size() == 2);  // a/b.txt and the link itself
        CHECK(SetFileAttributesW((temp.dir / "tree" / "a" / "b.txt").c_str(), FILE_ATTRIBUTE_READONLY));
        CHECK(remove_tree(temp.dir / "tree") == 0);
        CHECK(!fs::exists(temp.dir / "tree") && read(temp.dir / "outside" / "keep.txt") == "keep");
        CHECK(remove_tree(temp.dir / "missing") == 0);
    });

    run("steam: libraryfolders.vdf in both formats, app manifests, game folders", [] {
        const std::string current =
            "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
            "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"1\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\Games\\\\Steam\"\n"
            "\t\t\"apps\"\n\t\t{\n\t\t\t\"236850\"\t\t\"123\"\n\t\t}\n\t}\n}\n";
        std::string error;
        const auto vdf = parse_vdf(current, error);
        CHECK(vdf.has_value());
        if (vdf) {
            const auto libraries = steam_libraries(*vdf);
            // The library that has the game comes first.
            CHECK(libraries.size() == 2 && libraries[0] == fs::path(L"D:\\Games\\Steam") &&
                  libraries[1] == fs::path(L"C:\\Program Files (x86)\\Steam"));
        }
        const auto old = parse_vdf("\"LibraryFolders\" { \"TimeNextStatsReport\" \"1\" \"1\" \"E:\\\\Lib\" } // old\n", error);
        CHECK(old.has_value() && steam_libraries(*old) == std::vector<fs::path>({fs::path(L"E:\\Lib")}));
        CHECK(!parse_vdf("\"a\" { \"b\" \"c\"", error) && contains(error, "not closed"));
        CHECK(!parse_vdf("\"a\" \"b\" }", error));
        CHECK(!parse_vdf("\"a\" \"unterminated", error));

        const TempDir temp("steam");
        const fs::path steam = temp.dir / "Steam";
        const fs::path library = temp.dir / "Library";
        std::string escaped = library.string();
        for (size_t at = escaped.find('\\'); at != std::string::npos; at = escaped.find('\\', at + 2)) escaped.insert(at, "\\");
        write(steam / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\" { \"0\" { \"path\" \"" + escaped + "\" \"apps\" { \"236850\" \"1\" } } }");
        CHECK(find_steam_games(steam).empty());  // no eu4.exe yet
        write(library / "steamapps" / "appmanifest_236850.acf", "\"AppState\" { \"appid\" \"236850\" \"installdir\" \"EU4 Custom\" }");
        make_game(library / "steamapps" / "common" / "EU4 Custom");
        make_game(steam / "steamapps" / "common" / "Europa Universalis IV");  // the default name in Steam's own library
        const auto games = find_steam_games(steam);
        CHECK(games.size() == 2);
        if (games.size() == 2) {
            CHECK(same_file(games[0], library / "steamapps" / "common" / "EU4 Custom"));
            CHECK(same_file(games[1], steam / "steamapps" / "common" / "Europa Universalis IV"));
        }
        // An installdir that would leave steamapps/common is not followed.
        write(library / "steamapps" / "appmanifest_236850.acf", "\"AppState\" { \"installdir\" \"..\\\\..\" }");
        CHECK(find_steam_games(steam).size() == 1);
    });

    run("game folder: eu4.exe hashed, its build looked up", [] {
        const TempDir temp("folder");
        CHECK(inspect_game_folder(temp.dir / "missing").error == "the folder does not exist");
        CHECK(contains(inspect_game_folder(temp.dir).error, "no eu4.exe"));
        make_game(temp.dir);
        const GameFolder game = inspect_game_folder(temp.dir);
        CHECK(game.error.empty() && game.has_exe && game.exe_sha256 == sha256_file(temp.dir / "eu4.exe"));
        CHECK(game.build.empty());  // the synthetic image is no known build
        CHECK(!is_gog_folder(temp.dir));
        CHECK(gog_program(temp.dir) == temp.dir / "eu4.exe");
        write(temp.dir / "goggame-2057001589.info", "{}");
        write(temp.dir / "dowser.exe", "launcher");
        CHECK(is_gog_folder(temp.dir));
        CHECK(inspect_game_folder(temp.dir).gog);
        CHECK(gog_program(temp.dir) == temp.dir / "dowser.exe");
        CHECK(same_file(temp.dir / "eu4.exe", temp.dir / "." / "EU4.EXE"));
        CHECK(!same_file(temp.dir / "eu4.exe", temp.dir / "missing.exe"));
        CHECK(runs_from(temp.dir / "AnvilManager.exe", temp.dir));
    });

    run("game running: an eu4.exe from this folder makes the Manager read-only", [&] {
        const TempDir temp("running");
        const fs::path game = make_game(temp.dir / "game");
        const fs::path other = make_game(temp.dir / "other");
        CHECK(!game_running(game));
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        CHECK(CopyFileW(self, (game / "eu4.exe").c_str(), FALSE));
        std::wstring command = L"\"" + (game / "eu4.exe").wstring() + L"\" --sleep";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
        CHECK(game_running(game));
        CHECK(!game_running(other));
        // Seen from the other folder, the game runs elsewhere (issue #253); from its own, it started now.
        FILETIME created{}, exited{}, kernel{}, user{};
        CHECK(GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user));
        const GameProcesses here = game_processes(game);
        CHECK(here.here && here.here_started == ((static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime));
        CHECK(std::none_of(here.elsewhere.begin(), here.elsewhere.end(), [&](const fs::path& f) { return same_file(f, game); }));
        const GameProcesses there = game_processes(other);
        CHECK(!there.here && there.here_started == 0);
        CHECK(std::any_of(there.elsewhere.begin(), there.elsewhere.end(), [&](const fs::path& f) { return same_file(f, game); }));
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CHECK(!game_running(game));
        const GameProcesses after = game_processes(other);
        CHECK(std::none_of(after.elsewhere.begin(), after.elsewhere.end(), [&](const fs::path& f) { return same_file(f, game); }));
    });

    run("anvil.log: a game that ended without writing it ran without Anvil (issue #253)", [&] {
        const TempDir temp("missed");
        const fs::path log = temp.dir / "anvil.log";
        CHECK(!file_written(log));
        CHECK(!file_written(temp.dir));  // a folder is no log
        write(log, "anvil 1.0 starting\n");
        const auto written = file_written(log);
        CHECK(written.has_value() && *written > 0);
        CHECK(!anvil_missed_game(written, *written));       // written as the game started
        CHECK(!anvil_missed_game(written, *written - 10));  // written during the game
        CHECK(anvil_missed_game(written, *written + 10));   // from an earlier game
        CHECK(anvil_missed_game(std::nullopt, *written));   // never written
        CHECK(!anvil_missed_game(std::nullopt, 0));         // the start is not known
    });

    run("package: version.dll's resource names Anvil and its version", [&] {
        const TempDir temp("product");
        write(temp.dir / "version.dll", version_dll);
        const auto product = read_dll_product(temp.dir / "version.dll");
        CHECK(product && product->name == "Anvil" && product->version == ANVIL_VERSION);
        write(temp.dir / "plain.dll", read(temp.dir / "version.dll").substr(0, 64));
        CHECK(!read_dll_product(temp.dir / "plain.dll"));
        const auto exe = TestImage().file();
        write(temp.dir / "image.dll", std::string(exe.begin(), exe.end()));
        CHECK(!read_dll_product(temp.dir / "image.dll"));  // no version resource

        std::string error;
        auto not_package = zip_of({{"readme.txt", "x"}});
        CHECK(not_package && !load_package(std::move(*not_package), "1.0.0", error) && contains(error, "no version.dll"));
    });

    run("install: everything is written, version.dll last, the Manager copied", [&] {
        const auto package = make_package(version_dll);
        CHECK(package.has_value());
        if (!package) return;
        CHECK(package->standard_plugins == std::vector<std::string>({"anvil-game", "anvil-overlay"}));
        const TempDir temp("install");
        const fs::path game = make_game(temp.dir / "game");
        write(temp.dir / "Downloads" / "AnvilManager.exe", "manager");
        const fs::path manager = temp.dir / "Downloads" / "AnvilManager.exe";

        Installation before = inspect_installation(*package, game);
        CHECK(before.dll == Installation::Dll::Missing && !before.anvil_folder && !before.intact());
        CHECK(before.missing.size() == package->files.size());
        const Plan plan = plan_install(*package, before, game, manager, false);
        CHECK(plan.refused.empty() && plan.steps.size() == package->files.size() + 1);
        CHECK(plan.steps.back().kind == Step::Kind::Write && plan.steps.back().path == "version.dll");
        CHECK(has_step(plan, Step::Kind::CopyManager, "AnvilManager.exe") && has_step(plan, Step::Kind::Write, kPlayerToml));
        const ApplyResult applied = apply_plan(*package, plan, game, manager);
        CHECK(applied.ok && applied.done == plan.steps.size());
        CHECK(read(game / "AnvilManager.exe") == "manager" && read(game / "version.dll") == version_dll);
        CHECK(read(game / "anvil" / "plugins" / "anvil-game" / "anvil-game.dll") == "MZ game");

        const Installation after = inspect_installation(*package, game, manager);
        CHECK(after.intact() && after.installed() && after.dll_version == ANVIL_VERSION && after.manager_copy &&
              after.manager_current);
        // Nothing to do: the same Manager again, or the Manager running from the folder.
        CHECK(plan_install(*package, after, game, manager, false).steps.empty());
        CHECK(plan_install(*package, inspect_installation(*package, game, game / "AnvilManager.exe"), game,
                           game / "AnvilManager.exe", false).steps.empty());
        // A newer Manager replaces the copy in the folder.
        write(manager, "newer manager");
        const Installation older_copy = inspect_installation(*package, game, manager);
        CHECK(!older_copy.manager_current);
        const Plan copy = plan_install(*package, older_copy, game, manager, false);
        CHECK(copy.steps.size() == 1 && copy.steps[0].kind == Step::Kind::CopyManager);
        CHECK(apply_plan(*package, copy, game, manager).ok && read(game / "AnvilManager.exe") == "newer manager");
        CHECK(!fs::exists(game / "anvil" / "anvil.toml.anvil-new"));
    });

    run("repair: what differs is written again, extra files go, anvil.toml stays", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("repair");
        const fs::path game = make_game(temp.dir / "game");
        apply_plan(*package, plan_install(*package, inspect_installation(*package, game), game, {}, false), game, {});
        write(game / "anvil" / "anvil.toml", "# mine\ndisabled_plugins = [\"x\"]\n");
        write(game / "anvil" / "plugins" / "anvil-game" / "anvil-game.dll", "changed");
        fs::remove(game / "anvil" / "plugins" / "README.txt");
        write(game / "anvil" / "plugins" / "anvil-overlay" / "stale.txt", "old");
        write(temp.dir / "outside" / "keep.txt", "keep");
        CHECK(make_junction(game / "anvil" / "plugins" / "anvil-overlay" / "link", temp.dir / "outside"));
        write(game / "anvil" / "plugins" / "mine" / "plugin.toml", plugin_toml("mine"));

        const Installation broken = inspect_installation(*package, game);
        CHECK(!broken.intact() && broken.installed());
        CHECK(broken.differing == std::vector<std::string>({"anvil/plugins/anvil-game/anvil-game.dll"}));
        CHECK(broken.missing == std::vector<std::string>({"anvil/plugins/README.txt"}));
        CHECK(broken.extra == std::vector<std::string>({"anvil/plugins/anvil-overlay/link", "anvil/plugins/anvil-overlay/stale.txt"}));
        const Plan plan = plan_install(*package, broken, game, {}, false);
        CHECK(plan.steps.size() == 4 && has_step(plan, Step::Kind::Remove, "anvil/plugins/anvil-overlay/link"));
        CHECK(apply_plan(*package, plan, game, {}).ok);
        CHECK(inspect_installation(*package, game).intact());
        CHECK(read(game / "anvil" / "anvil.toml") == "# mine\ndisabled_plugins = [\"x\"]\n");
        CHECK(read(temp.dir / "outside" / "keep.txt") == "keep");
        CHECK(fs::exists(game / "anvil" / "plugins" / "mine" / "plugin.toml"));
    });

    run("install: another mod's version.dll is replaced only on purpose", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("foreign");
        const fs::path game = make_game(temp.dir / "game");
        write(game / "version.dll", read(game / "eu4.exe"));  // a PE without Anvil's resource
        const Installation foreign = inspect_installation(*package, game);
        CHECK(foreign.dll == Installation::Dll::Foreign && !foreign.installed());
        const Plan refused = plan_install(*package, foreign, game, {}, false);
        CHECK(!refused.refused.empty() && refused.steps.empty());
        CHECK(!apply_plan(*package, refused, game, {}).ok);
        CHECK(foreign.legacy.empty());  // not a legacy text patch: an unknown proxy
        const Plan replace = plan_install(*package, foreign, game, {}, true, "2026-11-09");
        CHECK(replace.refused.empty() && replace.steps.back().path == "version.dll");
        // It is not overwritten: it moves to the backup first (lab#104).
        CHECK(replace.steps.front().kind == Step::Kind::Move && replace.steps.front().path == "version.dll" &&
              replace.steps.front().to == "anvil-legacy-backup/version-dll-2026-11-09/version.dll");
        CHECK(apply_plan(*package, replace, game, {}).ok && inspect_installation(*package, game).intact());
        CHECK(read(game / "anvil-legacy-backup" / "version-dll-2026-11-09" / "version.dll") == read(game / "eu4.exe"));
        const auto unknown_backup = inspect_installation(*package, game).backup;
        CHECK(unknown_backup && unknown_backup->name == "version.dll" && unknown_backup->taken.empty());
        // Uninstall leaves a foreign version.dll alone.
        write(game / "version.dll", read(game / "eu4.exe"));
        const Plan uninstall = plan_uninstall(*package, inspect_installation(*package, game), false);
        CHECK(!has_step(uninstall, Step::Kind::Remove, "version.dll"));
        CHECK(apply_plan(*package, uninstall, game, {}).ok && fs::exists(game / "version.dll"));
    });

    run("pe_machine: x64, x86 and not a PE", [&] {
        const TempDir temp("pe_machine");
        write(temp.dir / "x64.dll", pe_with_machine(0x8664));
        write(temp.dir / "x86.dll", pe_with_machine(0x014C));
        write(temp.dir / "text.dll", "not a PE");
        CHECK(pe_machine(temp.dir / "x64.dll") == uint16_t(0x8664));
        CHECK(pe_machine(temp.dir / "x86.dll") == uint16_t(0x014C));
        CHECK(!pe_machine(temp.dir / "text.dll") && !pe_machine(temp.dir / "missing.dll"));
    });

    run("legacy text patch: EU4DLL is named, moved to a backup and replaced in one plan", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("legacy_eu4dll");
        const fs::path game = make_game(temp.dir / "game");
        add_eu4dll(game);
        const std::string original = read(game / "version.dll");
        const Installation found = inspect_installation(*package, game);
        CHECK(found.dll == Installation::Dll::Foreign && found.legacy.size() == 1 && !found.legacy_over_anvil());
        CHECK(found.legacy_name() == "EU4DLL" && found.other_plugins.empty() && !found.backup);
        CHECK(found.legacy[0].files == std::vector<std::string>({"d3d9.dll", ".dist.v1.json", "README.md", "plugins/plugin64.dll",
                                                                 "plugins/plugin.ini", "plugins/Plugin.dll",
                                                                 "plugins/dllautoupdater.exe", "plugins/autoupdate64.bat"}));
        // No checkbox: the plan is the one confirmation.
        const Plan plan = plan_install(*package, found, game, {}, false, "2026-11-09");
        CHECK(plan.refused.empty());
        CHECK(plan.steps.front().kind == Step::Kind::Move && plan.steps.front().path == "version.dll");  // vanilla at once
        CHECK(has_step(plan, Step::Kind::RemoveIfEmpty, "plugins") && plan.steps.back().path == "version.dll");
        CHECK(!plan.notes.empty() && contains(plan.notes[0], "EU4DLL") &&
              contains(plan.notes[0], "anvil-legacy-backup/eu4dll-2026-11-09"));
        CHECK(apply_plan(*package, plan, game, {}).ok);
        CHECK(inspect_installation(*package, game).intact());
        const fs::path backup = game / "anvil-legacy-backup" / "eu4dll-2026-11-09";
        CHECK(read(backup / "version.dll") == original && fs::exists(backup / "plugins" / "plugin64.dll"));
        CHECK(fs::exists(backup / "d3d9.dll") && fs::exists(backup / ".dist.v1.json") && fs::exists(backup / "README.md"));
        CHECK(!fs::exists(game / "plugins") && !fs::exists(game / "d3d9.dll") && !fs::exists(game / ".dist.v1.json"));

        // Uninstall offers it back; without the choice it stays in the backup.
        const Installation installed = inspect_installation(*package, game);
        CHECK(installed.backup && installed.backup->name == "EU4DLL" && installed.backup->files.size() == 9 &&
              installed.backup->taken.empty());
        const Plan keep = plan_uninstall(*package, installed, false);
        CHECK(std::none_of(keep.steps.begin(), keep.steps.end(), [](const Step& s) { return s.kind == Step::Kind::Move; }));
        write(game / "README.md", "another mod's");  // its place is taken: that file stays in the backup
        const Installation taken = inspect_installation(*package, game);
        CHECK(taken.backup && taken.backup->taken == std::vector<std::string>({"README.md"}));
        const Plan restore = plan_uninstall(*package, taken, false, true);
        CHECK(restore.steps.front().kind == Step::Kind::Remove && restore.steps.front().path == "version.dll");
        const auto last_move = std::find_if(restore.steps.rbegin(), restore.steps.rend(),
                                            [](const Step& s) { return s.kind == Step::Kind::Move; });
        CHECK(last_move != restore.steps.rend() && last_move->to == "version.dll");  // the loader last
        CHECK(!has_step(restore, Step::Kind::Move, "anvil-legacy-backup/eu4dll-2026-11-09/README.md"));
        CHECK(std::any_of(restore.notes.begin(), restore.notes.end(), [](const std::string& n) { return contains(n, "Mei"); }));
        CHECK(std::any_of(restore.notes.begin(), restore.notes.end(), [](const std::string& n) { return contains(n, "README.md"); }));
        CHECK(apply_plan(*package, restore, game, {}).ok);
        CHECK(read(game / "version.dll") == original && read(game / "plugins" / "plugin64.dll") == "plugin64.dll");
        CHECK(fs::exists(game / "d3d9.dll") && read(game / "README.md") == "another mod's");
        CHECK(read(backup / "README.md") == "EU4DLL" && !fs::exists(backup / "plugins"));
        // With nothing left behind, the backup folders go too.
        fs::remove(game / "README.md");
        const Plan rest = plan_uninstall(*package, inspect_installation(*package, game), false, true);
        CHECK(apply_plan(*package, rest, game, {}).ok);
        CHECK(read(game / "README.md") == "EU4DLL" && !fs::exists(game / "anvil-legacy-backup"));
    });

    run("legacy text patch: a 64-bit d3d9.dll is not EU4DLL's", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("legacy_d3d9");
        const fs::path game = make_game(temp.dir / "game");
        write(game / "version.dll", read(game / "eu4.exe"));
        write(game / "d3d9.dll", pe_with_machine(0x8664));  // another tool's, such as a shader injector
        CHECK(inspect_installation(*package, game).legacy.empty());
        write(game / "d3d9.dll", pe_with_machine(0x014C));
        const Installation eu4dll = inspect_installation(*package, game);
        CHECK(eu4dll.legacy.size() == 1 && eu4dll.legacy[0].files == std::vector<std::string>({"d3d9.dll"}));
    });

    run("legacy text patch: Unicode Patch goes, other files in plugins stay and are named", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("legacy_unicode");
        const fs::path game = make_game(temp.dir / "game");
        write(game / "VERSION.dll", read(game / "eu4.exe"));
        write(game / "plugins" / "eu4_unicode_patch.dll", "patch");
        write(game / "plugins" / "eu4_unicode_patch" / "config.toml", "x = 1");
        write(game / "plugins" / "eu4_unicode_patch.log", "log");
        write(game / "plugins" / "menu_patch.dll", "another plugin");
        write(game / "gfx" / "fonts" / "eu4-unicode" / "cache" / "a.dds", "atlas");
        write(game / "gfx" / "fonts" / "vanilla.fnt", "game's own");
        write(game / "EU4UnicodePatch.crash.txt", "crash");
        write(game / "EU4UnicodePatchNotes.md", "not one of its files");
        const Installation found = inspect_installation(*package, game);
        CHECK(found.legacy.size() == 1 && found.legacy_name() == "EU4 Unicode Patch");
        CHECK(found.legacy[0].files == std::vector<std::string>({"plugins/eu4_unicode_patch.dll", "plugins/eu4_unicode_patch",
                                                                 "plugins/eu4_unicode_patch.log", "gfx/fonts/eu4-unicode",
                                                                 "EU4UnicodePatch.crash.txt"}));
        CHECK(found.other_plugins == std::vector<std::string>({"plugins/menu_patch.dll"}));
        const Plan plan = plan_install(*package, found, game, {}, false, "2026-11-09");
        CHECK(std::any_of(plan.notes.begin(), plan.notes.end(),
                          [](const std::string& n) { return contains(n, "plugins/menu_patch.dll"); }));
        CHECK(apply_plan(*package, plan, game, {}).ok && inspect_installation(*package, game).intact());
        const fs::path backup = game / "anvil-legacy-backup" / "eu4-unicode-patch-2026-11-09";
        CHECK(fs::exists(backup / "plugins" / "eu4_unicode_patch" / "config.toml"));
        CHECK(fs::exists(backup / "gfx" / "fonts" / "eu4-unicode" / "cache" / "a.dds") && fs::exists(backup / "version.dll"));
        CHECK(fs::exists(backup / "EU4UnicodePatch.crash.txt") && fs::exists(game / "EU4UnicodePatchNotes.md"));
        CHECK(read(game / "plugins" / "menu_patch.dll") == "another plugin" && !fs::exists(game / "plugins" / "eu4_unicode_patch"));
        CHECK(fs::exists(game / "gfx" / "fonts" / "vanilla.fnt") && !fs::exists(game / "gfx" / "fonts" / "eu4-unicode"));
    });

    run("legacy text patch over Anvil: told apart, the same replace brings Anvil back", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        const TempDir temp("legacy_over_anvil");
        const fs::path game = make_game(temp.dir / "game");
        CHECK(apply_plan(*package, plan_install(*package, inspect_installation(*package, game), game, {}, false), game, {}).ok);
        write(game / "anvil" / "anvil.toml", "# mine\n");
        write(game / "anvil-legacy-backup" / "eu4dll-2026-11-09" / "plugins" / "plugin64.dll", "an earlier one");
        add_eu4dll(game);  // as the translation mod's FAQ says: install EU4DLL again
        const Installation over = inspect_installation(*package, game);
        CHECK(over.legacy_over_anvil() && !over.installed());
        const Plan plan = plan_install(*package, over, game, {}, false, "2026-11-09");
        CHECK(plan.refused.empty() && plan.steps.front().to == "anvil-legacy-backup/eu4dll-2026-11-09-2/version.dll");
        CHECK(apply_plan(*package, plan, game, {}).ok);
        const Installation back = inspect_installation(*package, game);
        CHECK(back.intact() && read(game / "anvil" / "anvil.toml") == "# mine\n");
        CHECK(back.backup && back.backup->folder == "anvil-legacy-backup/eu4dll-2026-11-09-2");  // the newest
        CHECK(read(game / "anvil-legacy-backup" / "eu4dll-2026-11-09" / "plugins" / "plugin64.dll") == "an earlier one");
    });

    run("update: an older Anvil is noted and replaced", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        Installation older;
        older.dll = Installation::Dll::Anvil;
        older.dll_version = "0.9.0";
        older.differing = {"version.dll"};
        const Plan plan = plan_install(*package, older, fs::temp_directory_path(), {}, false);
        CHECK(plan.steps.size() == 1 && plan.steps[0].path == "version.dll");
        CHECK(!plan.notes.empty() && contains(plan.notes[0], "Anvil 0.9.0 is updated to"));
    });

    run("uninstall: the player's data stays unless everything goes", [&] {
        const auto package = make_package(version_dll);
        if (!package) return;
        for (const bool everything : {false, true}) {
            const TempDir temp(everything ? "uninstall_all" : "uninstall");
            const fs::path game = make_game(temp.dir / "game");
            write(temp.dir / "AnvilManager.exe", "manager");
            apply_plan(*package, plan_install(*package, inspect_installation(*package, game), game, temp.dir / "AnvilManager.exe", false),
                       game, temp.dir / "AnvilManager.exe");
            write(game / "anvil" / "plugins" / "mine" / "plugin.toml", plugin_toml("mine"));
            write(game / "anvil" / "data" / "mine" / "settings.toml", "x = 1\n");
            write(game / "anvil" / "crashes" / "2026-10-20_10-00-00.json", "{}");
            write(game / "anvil" / "anvil.log", "log");

            const Plan plan = plan_uninstall(*package, inspect_installation(*package, game), everything);
            CHECK(plan.steps.front().kind == Step::Kind::Remove && plan.steps.front().path == "version.dll");
            CHECK(has_step(plan, Step::Kind::RemoveManager, "AnvilManager.exe"));
            const ApplyResult result = apply_plan(*package, plan, game, temp.dir / "AnvilManager.exe");
            CHECK(result.ok && !result.manager_left);
            CHECK(!fs::exists(game / "version.dll") && !fs::exists(game / "AnvilManager.exe") && fs::exists(game / "eu4.exe"));
            CHECK(fs::exists(temp.dir / "AnvilManager.exe"));  // the one that ran
            if (everything) {
                CHECK(!fs::exists(game / "anvil"));
            } else {
                CHECK(!fs::exists(game / "anvil" / "plugins" / "anvil-game") && !fs::exists(game / "anvil" / "plugins" / "anvil-overlay"));
                CHECK(!fs::exists(game / "anvil" / "plugins" / "README.txt") && !fs::exists(game / "anvil" / "docs"));
                CHECK(fs::exists(game / "anvil" / "plugins" / "mine" / "plugin.toml"));
                CHECK(fs::exists(game / "anvil" / "data" / "mine" / "settings.toml"));
                CHECK(fs::exists(game / "anvil" / "crashes") && fs::exists(game / "anvil" / "anvil.log"));
                CHECK(fs::exists(game / "anvil" / "anvil.toml"));
            }
        }
        // Running from the folder: the exe is left to the caller.
        const TempDir temp("uninstall_self");
        const fs::path game = make_game(temp.dir / "game");
        apply_plan(*package, plan_install(*package, inspect_installation(*package, game), game, {}, false), game, {});
        write(game / "AnvilManager.exe", "manager");
        fs::create_directories(game / "anvil" / "crashes");  // as crash capture leaves it after a start
        const Plan plan = plan_uninstall(*package, inspect_installation(*package, game), false);
        CHECK(has_step(plan, Step::Kind::RemoveIfEmpty, "anvil/crashes"));
        const ApplyResult result = apply_plan(*package, plan, game, game / "AnvilManager.exe");
        CHECK(result.ok && result.manager_left && fs::exists(game / "AnvilManager.exe"));
        // Without a plugin of the player's or a crash report, the empty folders
        // go too (issue #108); anvil.toml keeps anvil/.
        CHECK(!fs::exists(game / "anvil" / "plugins") && !fs::exists(game / "anvil" / "crashes"));
        CHECK(fs::exists(game / "anvil" / "anvil.toml"));
    });

    run("plugin zip: a folder the player cannot write offers the administrator", [&] {
        // Program Files without Steam's permissions (GOG Galaxy's default), and no
        // anvil/data yet: the game never ran with Anvil.
        const TempDir temp("plugin_zip_denied");
        const fs::path game = make_game(temp.dir / "game");
        fs::create_directories(game / "anvil" / "plugins");
        const auto zip = zip_of({{"plugin.toml", plugin_toml("hello")}, {"hello.dll", "MZ"}});
        CHECK(zip.has_value());
        if (!zip) return;
        const PluginZipCheck check = check_plugin_zip(*zip, game, temp.dir / "tmp", {});
        CHECK(check.error.empty());
        const std::wstring folder = L"\"" + (game / "anvil").wstring() + L"\"";
        CHECK(_wsystem((L"icacls " + folder + L" /deny *S-1-1-0:(AD) >nul").c_str()) == 0);
        const PluginInstallResult installed = install_plugin_zip(*zip, check, game);
        _wsystem((L"icacls " + folder + L" /remove:d *S-1-1-0 >nul").c_str());
        CHECK(!installed.ok && installed.access_denied);
    });

    run("plugin zip: checked against eu4.exe before anything is copied", [&] {
        const TempDir temp("plugin_zip");
        const fs::path game = make_game(temp.dir / "game");
        fs::create_directories(game / "anvil" / "plugins");
        const std::vector<std::string> standard = {"anvil-game", "anvil-overlay"};
        std::string gameplay = plugin_toml("hello", "", "1.2.0");
        gameplay.replace(gameplay.find("affects_gameplay = false"), 24, "affects_gameplay = true");

        // The template's layout: <id>/... at the root of the zip.
        auto zip = zip_of({{"hello/plugin.toml", plugin_toml("hello", "authors = [\"Ann\"]\n")}, {"hello/hello.dll", "MZ"}});
        CHECK(zip.has_value());
        if (!zip) return;
        PluginZipCheck check = check_plugin_zip(*zip, game, temp.dir / "tmp", standard);
        CHECK(check.error.empty() && check.id == "hello" && check.version == "1.0.0" && check.root == "hello/");
        CHECK(!check.would_skip && check.installed_folder.empty() && check.files.size() == 2);
        CHECK(check.report.manifest && check.report.manifest->authors == std::vector<std::string>({"Ann"}));
        CHECK(!fs::exists(game / "anvil" / "plugins" / "hello"));  // checking copies nothing into the game
        CHECK(fs::is_empty(temp.dir / "tmp"));
        PluginInstallResult installed = install_plugin_zip(*zip, check, game);
        CHECK(installed.ok && read(game / "anvil" / "plugins" / "hello" / "hello.dll") == "MZ");

        // A newer version replaces it; the player's data stays.
        write(game / "anvil" / "data" / "hello" / "settings.toml", "x = 1\n");
        write(game / "anvil" / "plugins" / "hello" / "old.txt", "old");
        zip = zip_of({{"plugin.toml", plugin_toml("hello", "", "1.1.0")}, {"hello.dll", "MZ 1.1"}});
        check = check_plugin_zip(*zip, game, temp.dir / "tmp", standard);
        CHECK(check.error.empty() && check.root.empty() && check.installed_folder == "hello" && check.installed_version == "1.0.0");
        installed = install_plugin_zip(*zip, check, game);
        CHECK(installed.ok && read(game / "anvil" / "plugins" / "hello" / "hello.dll") == "MZ 1.1");
        CHECK(!fs::exists(game / "anvil" / "plugins" / "hello" / "old.txt"));
        CHECK(read(game / "anvil" / "data" / "hello" / "settings.toml") == "x = 1\n");
        CHECK(fs::is_empty(game / "anvil" / "data" / "anvil" / "manager"));

        // Installed under another folder name: that folder is replaced by anvil/plugins/<id>.
        fs::rename(game / "anvil" / "plugins" / "hello", game / "anvil" / "plugins" / "hello-1.1");
        zip = zip_of({{"anvil/plugins/hello/plugin.toml", gameplay}, {"anvil/plugins/hello/hello.dll", "MZ 1.2"}});
        check = check_plugin_zip(*zip, game, temp.dir / "tmp", standard);
        CHECK(check.error.empty() && check.root == "anvil/plugins/hello/" && check.installed_folder == "hello-1.1");
        CHECK(check.report.manifest && check.report.manifest->affects_gameplay);
        CHECK(install_plugin_zip(*zip, check, game).ok);
        CHECK(!fs::exists(game / "anvil" / "plugins" / "hello-1.1") && read(game / "anvil" / "plugins" / "hello" / "hello.dll") == "MZ 1.2");

        // Symbols for a build this eu4.exe is not: the loader would skip it.
        zip = zip_of({{"sym/plugin.toml", plugin_toml("sym", "symbols = \"symbols.toml\"\n")}, {"sym/sym.dll", "MZ"},
                      {"sym/symbols.toml", "build = \"eu4-1.37.5-steam-win64\"\n\n[symbol.f]\nrva = 0x1000\nbytes = \"48 8B\"\nmemory = \"code\"\n"}});
        check = check_plugin_zip(*zip, game, temp.dir / "tmp", standard);
        CHECK(check.error.empty() && check.would_skip && contains(check.report.skip_reason, "game build unknown"));
        CHECK(!contains(check.report.skip_reason, "anvil-manager-check"));  // no temporary paths shown

        remove_plugin_folder(game, "hello", standard, check.error);
        CHECK(!fs::exists(game / "anvil" / "plugins" / "hello") && fs::exists(game / "anvil" / "data" / "hello"));
        std::string error;
        CHECK(!remove_plugin_folder(game, "anvil-game", standard, error) && contains(error, "comes with Anvil"));
        CHECK(!remove_plugin_folder(game, "..", standard, error));
    });

    run("plugin zip: refusals", [&] {
        const TempDir temp("plugin_zip_refusals");
        const fs::path game = make_game(temp.dir / "game");
        const std::vector<std::string> standard = {"anvil-game", "anvil-overlay"};
        const auto refusal = [&](const std::vector<std::pair<std::string, std::string>>& files) {
            const auto zip = zip_of(files);
            return zip ? check_plugin_zip(*zip, game, temp.dir / "tmp", standard).error : std::string("<no zip>");
        };
        CHECK(contains(refusal({{"readme.txt", "x"}}), "no plugin.toml"));
        CHECK(contains(refusal({{"a/plugin.toml", plugin_toml("a")}, {"b/plugin.toml", plugin_toml("b")}}), "more than one plugin"));
        {
            // Files next to the plugin's folder (a release's SHA256SUMS.txt) are left out.
            const auto zip = zip_of({{"a/plugin.toml", plugin_toml("a")}, {"a/a.dll", "MZ"}, {"SHA256SUMS.txt", "x"}});
            const PluginZipCheck check = check_plugin_zip(*zip, game, temp.dir / "tmp", standard);
            CHECK(check.error.empty() && check.files.size() == 2 && check.ignored == std::vector<std::string>({"SHA256SUMS.txt"}));
        }
        CHECK(refusal({{"a/plugin.toml", plugin_toml("a")}, {"a/a.dll", "MZ"}, {"__MACOSX/a/._a.dll", "x"}}).empty());
        CHECK(contains(refusal({{"plugin.toml", plugin_toml("anvil-game")}, {"anvil-game.dll", "MZ"}}), "comes with Anvil"));
        CHECK(contains(refusal({{"plugin.toml", plugin_toml("a")}}), "a.dll"));  // no entry DLL
        // anvil/plugins/<id> holds another plugin: it is not replaced.
        write(game / "anvil" / "plugins" / "a" / "plugin.toml", plugin_toml("b"));
        write(game / "anvil" / "plugins" / "a" / "b.dll", "MZ");
        CHECK(contains(refusal({{"plugin.toml", plugin_toml("a")}, {"a.dll", "MZ"}}), "holds the plugin b"));
        fs::remove_all(game / "anvil");
        CHECK(!refusal({{"plugin.toml", "id = \"a\"\nbroken = \n"}}).empty());
        CHECK(!fs::exists(game / "anvil"));
    });

    run("start menu: a shortcut to the copy in the game folder, read back and removed", [] {
        CHECK(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) >= 0);
        const fs::path folder = fs::temp_directory_path() / L"anvil-start-menu-test \u0105\u0107";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        const fs::path link = folder / "Anvil Manager.lnk";
        const fs::path exe = folder / "game" / "AnvilManager.exe";
        const fs::path other = folder / "other" / "AnvilManager.exe";
        write(exe, "x");
        write(other, "x");
        CHECK(start_menu_link().filename() == "Anvil Manager.lnk");
        CHECK(!link_starts(link, exe));  // none yet

        std::string error;
        CHECK(write_link(link, other, error));
        CHECK(link_starts(link, other));
        CHECK(!link_starts(link, exe));
        remove_link(link, exe);  // another game's shortcut stays
        CHECK(fs::exists(link));
        CHECK(write_link(link, exe, error));  // and the button moves it to this game
        CHECK(link_starts(link, exe));
        CHECK(link_target(link).filename() == "AnvilManager.exe");
        remove_link(link, exe);
        CHECK(!fs::exists(link));

        CHECK(!write_link(folder / "no folder" / "x.lnk", exe, error));
        CHECK(!error.empty());
        CHECK(!write_link({}, exe, error));
        fs::remove_all(folder, ec);
        CoUninitialize();
    });

    run("release check: the latest release, a link only to the repository", [] {
        const std::string json = "{\"url\":\"https://api.github.com/x\",\"assets\":[{\"tag_name\":\"v0.0.1\"}],"
                                 "\"html_url\":\"https://github.com/happy-dwarf-team/anvil/releases/tag/v1.2.0\","
                                 "\"tag_name\":\"v1.2.0\",\"draft\":false,\"body\":\"caf\\u00e9 \\\"quoted\\\"\",\"n\":-1.5e3}";
        const auto release = parse_release(json);
        CHECK(release && release->tag == "v1.2.0" &&
              release->url == "https://github.com/happy-dwarf-team/anvil/releases/tag/v1.2.0");
        CHECK(!parse_release("{\"tag_name\":\"v1.2.0\",\"html_url\":\"https://evil.example/releases\"}"));
        CHECK(!parse_release("{\"tag_name\":\"v1.2.0\",\"html_url\":\"https://github.com/happy-dwarf-team/anvil/releases/x y\"}"));
        CHECK(!parse_release("{\"tag_name\":\"v1.2.0\"}"));
        CHECK(!parse_release("{\"tag_name\":\"v1.2.0\",") && !parse_release("[]") && !parse_release(""));
        CHECK(newer_release("v1.2.0", "1.1.0") && newer_release("1.1.1", "1.1.0"));
        CHECK(!newer_release("v1.1.0", "1.1.0") && !newer_release("v1.0.9", "1.1.0") && !newer_release("latest", "1.1.0"));
        CHECK(!newer_release("v1.2.0-rc.1", "1.2.0"));
    });

    run("release check: the Manager's answer as the overlay reads manager.toml", [] {
        // As save_manager_settings writes it (toml++).
        CHECK(manager_release_answer("# Anvil Manager's settings.\nasked_releases = true\ncheck_releases = true\n"
                                     "game_folder = 'D:\\EU4'\n") == std::optional(true));
        CHECK(manager_release_answer("check_releases = false\r\nasked_releases = true\r\n") == std::optional(false));
        // Not answered yet, or nothing said.
        CHECK(!manager_release_answer("asked_releases = false\ncheck_releases = false\n"));
        CHECK(!manager_release_answer("check_releases = true\n"));
        CHECK(!manager_release_answer("asked_releases = true\n"));
        CHECK(!manager_release_answer(""));
        CHECK(!manager_release_answer("asked_releases = yes\ncheck_releases = true\n"));
        // Keys of a table are not the top level's.
        CHECK(!manager_release_answer("[other]\nasked_releases = true\ncheck_releases = true\n"));
        CHECK(manager_settings_file().filename() == "manager.toml");
    });

    run("catalog: a mod that presents itself as a catalog plugin is told apart from its Workshop item", [] {
        Catalog catalog;
        CatalogPlugin bellows;
        bellows.id = "bellows";
        bellows.name = "Bellows";
        bellows.workshop = std::string(kCatalogWorkshopPrefix) + "3823747170";
        bellows.zip.url = "https://github.com/happy-dwarf-team/bellows/releases/download/v1.2.0/bellows-windows-x64.zip";
        catalog.plugins.push_back(bellows);
        const auto claimed = [&](std::string_view id, std::string_view name, std::string_view repository, std::string_view item) {
            return impersonated_plugin(catalog, id, name, repository, item) != nullptr;
        };
        CHECK(!claimed("bellows", "Bellows", "https://github.com/happy-dwarf-team/bellows", "3823747170"));
        CHECK(claimed("bellows", "Anything", "", "111"));
        CHECK(claimed("bellows", "Anything", "", ""));
        CHECK(claimed("faster-game", "BELLOWS", "", "111"));
        CHECK(claimed("faster-game", "Faster", "https://github.com/Happy-Dwarf-Team/bellows.git/", "111"));
        CHECK(!claimed("faster-game", "Faster", "https://example.com/bellows", "111"));
        CHECK(!claimed("faster-game", "Faster", "", ""));
    });

    run("catalog: the list in the repository validates and pins every plugin", [] {
        const auto text = read(fs::path(ANVIL_SOURCE_DIR) / "catalog" / "plugins.json");
        std::string error;
        const auto catalog = parse_catalog(text, error);
        CHECK(catalog && error.empty());
        if (!catalog) {
            std::printf("    %s\n", error.c_str());
            return;
        }
        CHECK(!catalog->plugins.empty());
        for (const CatalogPlugin& plugin : catalog->plugins) {
            CHECK(!plugin.name.empty() && !plugin.summary.empty() && plugin.zip.size > 0);
            CHECK(plugin.zip.url.starts_with("https://github.com/happy-dwarf-team/" + plugin.id + "/releases/download/v" +
                                             plugin.version + "/"));
            for (const std::string& build : plugin.builds) CHECK(build.starts_with("eu4-1.37.5"));
        }
    });

    run("catalog: only the team's release zips, links and pins", [] {
        const auto entry = [](const std::string& fields) {
            return "{\"schema\":1,\"plugins\":[{\"id\":\"bellows\",\"name\":\"Bellows\",\"version\":\"0.19.0\","
                   "\"page\":\"https://anvil.happydwarf.dev/plugins/bellows/\"," +
                   fields + "}]}";
        };
        const std::string zip = "\"zip\":{\"url\":\"https://github.com/happy-dwarf-team/bellows/releases/download/v0.19.0/"
                                "bellows-windows-x64.zip\",\"size\":1115323,\"sha256\":\"" +
                                std::string(64, 'a') + "\"}";
        std::string error;
        const auto good = parse_catalog(entry(zip + ",\"new_key\":[1,2],\"builds\":[\"eu4-1.37.5-steam-win64\"],"
                                                    "\"workshop\":\"https://steamcommunity.com/sharedfiles/filedetails/?id=3823747170\""),
                                        error);
        CHECK(good && good->plugins.size() == 1 && good->plugins[0].zip.size == 1115323 &&
              good->plugins[0].builds.size() == 1 && good->plugins[0].workshop.ends_with("=3823747170"));

        const auto refused = [&](const std::string& json, const std::string& why) {
            std::string e;
            const bool ok = !parse_catalog(json, e) && contains(e, why);
            if (!ok) std::printf("    expected '%s', got '%s'\n", why.c_str(), e.c_str());
            return ok;
        };
        const auto with_zip = [&](const std::string& url, const std::string& size, const std::string& sha) {
            return entry("\"zip\":{\"url\":\"" + url + "\",\"size\":" + size + ",\"sha256\":\"" + sha + "\"}");
        };
        const std::string url = "https://github.com/happy-dwarf-team/bellows/releases/download/v0.19.0/bellows-windows-x64.zip";
        const std::string sha(64, 'a');
        CHECK(refused(with_zip("https://github.com/someone/bellows/releases/download/v1/b.zip", "1", sha), "zip.url"));
        CHECK(refused(with_zip("http://github.com/happy-dwarf-team/bellows/releases/download/v1/b.zip", "1", sha), "zip.url"));
        CHECK(refused(with_zip("https://github.com/happy-dwarf-team/bellows/archive/v1.zip", "1", sha), "release asset"));
        CHECK(refused(with_zip("https://github.com/happy-dwarf-team/bellows/releases/download/v1/b.exe", "1", sha), "release asset"));
        CHECK(refused(with_zip("https://github.com/happy-dwarf-team/bellows/releases/download/../../x/b.zip", "1", sha), "release asset"));
        CHECK(refused(with_zip("https://github.com/happy-dwarf-team/bellows/releases/download/v1/b c.zip", "1", sha), "zip.url"));
        CHECK(refused(with_zip(url, "0", sha), "zip.size"));
        CHECK(refused(with_zip(url, "1.5", sha), "zip.size"));
        CHECK(refused(with_zip(url, "268435457", sha), "zip.size"));
        CHECK(refused(with_zip(url, "1", std::string(64, 'A')), "zip.sha256"));
        CHECK(refused(with_zip(url, "1", std::string(63, 'a')), "zip.sha256"));
        CHECK(refused(entry(zip + ",\"workshop\":\"https://steamcommunity.com/sharedfiles/filedetails/?id=1&x=2\""), "workshop"));
        CHECK(refused(entry(zip + ",\"workshop\":\"https://evil.example/?id=1\""), "workshop"));
        CHECK(refused(entry(zip + ",\"anvil\":\"soon\""), "anvil"));
        CHECK(refused(entry(zip + ",\"builds\":\"eu4\""), "builds"));
        CHECK(refused(entry(zip + ",\"affects_gameplay\":1"), "affects_gameplay"));
        CHECK(refused(entry("\"zip\":null"), "zip is not an object"));
        CHECK(refused(entry("\"note\":1," + zip), "summary or note"));
        CHECK(refused("{\"schema\":1,\"plugins\":[{\"id\":\"Bad Id\"}]}", "id is not a plugin id"));
        CHECK(refused("{\"schema\":1,\"plugins\":[{\"id\":\"a\",\"name\":\"A\",\"version\":\"1\",\"page\":\"https://anvil.happydwarf.dev/a/\"," + zip + "}]}",
                      "version"));
        CHECK(refused("{\"schema\":1,\"plugins\":[{\"id\":\"a\",\"name\":\"A\",\"version\":\"1.0.0\",\"page\":\"https://example.com/\"," + zip + "}]}",
                      "page"));
        const std::string one = entry(zip).substr(std::string_view("{\"schema\":1,\"plugins\":[").size());
        CHECK(refused("{\"schema\":1,\"plugins\":[" + one.substr(0, one.size() - 2) + "," + one, "listed twice"));
        CHECK(refused("{\"schema\":2,\"plugins\":[]}", "needs a newer Manager"));
        CHECK(refused("{\"schema\":0,\"plugins\":[]}", "not a plugin catalog"));
        CHECK(refused("{\"plugins\":[]}", "not a plugin catalog"));
        CHECK(refused("{\"schema\":1}", "plugins is not a list"));
        CHECK(refused(std::string(kMaxCatalogBytes + 1, ' '), "too long"));
    });

    run("catalog: what the game folder has of a plugin, and the pin of a download", [] {
        CatalogPlugin plugin;
        plugin.id = "bellows";
        plugin.version = "0.19.0";
        plugin.anvil = ">=1.7.0";
        plugin.builds = {"eu4-1.37.5-steam-win64"};
        const std::vector<std::pair<std::string, std::string>> none;
        CatalogStatus status = catalog_status(plugin, "eu4-1.37.5-steam-win64", "1.8.0", none, {});
        CHECK(status.for_build && status.anvil_ok && status.installed.empty() && !status.from_mod && !status.current(plugin));
        CHECK(!catalog_status(plugin, "eu4-1.37.5.2-gog-win64", "1.8.0", none, {}).for_build);
        CHECK(!catalog_status(plugin, "", "1.8.0", none, {}).for_build);
        CHECK(!catalog_status(plugin, "eu4-1.37.5-steam-win64", "1.6.1", none, {}).anvil_ok);
        CHECK(catalog_status(plugin, "eu4-1.37.5-steam-win64", "", none, {}).anvil_ok);  // unknown: not refused here
        status = catalog_status(plugin, "eu4-1.37.5-steam-win64", "1.8.0", {{"tongs", "0.26.0"}, {"bellows", "0.19.0"}}, {"bellows"});
        CHECK(status.current(plugin) && status.from_mod);
        status = catalog_status(plugin, "eu4-1.37.5-steam-win64", "1.8.0", {{"bellows", "0.18.0"}}, {});
        CHECK(status.installed == "0.18.0" && !status.current(plugin));
        plugin.builds.clear();
        CHECK(catalog_status(plugin, "", "1.8.0", none, {}).for_build);  // no symbols: every build
        plugin.builds = {"eu4-1.37.5-steam-win64"};

        // Updates (ADR 0006): an older copy in anvil/plugins that this build and Anvil can run.
        const std::string steam = "eu4-1.37.5-steam-win64";
        CHECK(catalog_status(plugin, steam, "1.8.0", {{"bellows", "0.18.0"}}, {}).update(plugin));
        CHECK(catalog_status(plugin, steam, "1.8.0", {{"bellows", "0.18.0"}}, {"bellows"}).update(plugin));  // its copy loads
        CHECK(!catalog_status(plugin, steam, "1.8.0", {{"bellows", "0.19.0"}}, {}).update(plugin));       // current
        CHECK(!catalog_status(plugin, steam, "1.8.0", {{"bellows", "0.20.0-dev"}}, {}).update(plugin));   // newer, from main
        CHECK(catalog_status(plugin, steam, "1.8.0", {{"bellows", "0.19.0-rc.1"}}, {}).update(plugin));   // a pre-release of the pin
        CHECK(!catalog_status(plugin, steam, "1.8.0", none, {"bellows"}).update(plugin));  // only from a mod: Steam updates it
        CHECK(!catalog_status(plugin, steam, "1.8.0", none, {}).update(plugin));           // not installed
        CHECK(!catalog_status(plugin, steam, "1.8.0", {{"bellows", "?"}}, {}).update(plugin));  // version unknown
        CHECK(!catalog_status(plugin, steam, "1.6.1", {{"bellows", "0.18.0"}}, {}).update(plugin));  // needs a newer Anvil
        CHECK(!catalog_status(plugin, "eu4-1.37.5.2-gog-win64", "1.8.0", {{"bellows", "0.18.0"}}, {}).update(plugin));
        Catalog catalog;
        catalog.plugins.push_back(plugin);
        CatalogPlugin tongs = plugin;
        tongs.id = "tongs";
        tongs.version = "0.27.0";
        catalog.plugins.push_back(tongs);
        CatalogPlugin mei = plugin;
        mei.id = "mei";
        catalog.plugins.push_back(mei);
        const auto updates =
            catalog_updates(catalog, steam, "1.8.0", {{"tongs", "0.26.1"}, {"bellows", "0.18.0"}, {"mei", "0.19.0"}}, {});
        CHECK(updates.size() == 2 && updates[0]->id == "bellows" && updates[1]->id == "tongs");
        CHECK(catalog_updates(catalog, steam, "1.8.0", none, {"bellows", "tongs"}).empty());

        const std::vector<uint8_t> bytes = {'a', 'b', 'c'};
        plugin.zip.size = 3;
        plugin.zip.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";  // SHA-256 of "abc"
        CHECK(check_pinned_zip(plugin, bytes).empty());
        CHECK(contains(check_pinned_zip(plugin, {'a', 'b', 'd'}), "SHA-256"));
        CHECK(contains(check_pinned_zip(plugin, {'a', 'b'}), "2 bytes"));
        CHECK(check_pinned_manifest(plugin, "bellows", "0.19.0").empty());
        CHECK(contains(check_pinned_manifest(plugin, "tongs", "0.19.0"), "'tongs'"));
        CHECK(contains(check_pinned_manifest(plugin, "bellows", "0.18.0"), "version 0.18.0"));

        const auto [host, path] = split_https_url("https://github.com/happy-dwarf-team/bellows/releases/download/v1/b.zip");
        CHECK(host == L"github.com" && path == L"/happy-dwarf-team/bellows/releases/download/v1/b.zip");
        CHECK(split_https_url("http://github.com/x").first.empty() && split_https_url("https://github.com").first.empty());
    });

    run("plugins from mods: approved and revoked as the Manager showed them", [] {
        TempDir temp("mods");
        const fs::path game = make_game(temp.dir / "game");
        const fs::path user = temp.dir / "user";
        fs::create_directories(game / "anvil" / "plugins");
        const fs::path folder = user / "mod" / "m" / "anvil" / "probe";
        write(folder / "plugin.toml", plugin_toml("probe"));
        write(folder / "probe.dll", "MZ");
        write(user / "mod" / "m.mod", "name=\"M\"\npath=\"mod/m\"\n");
        write(user / "dlc_load.json", "{\"enabled_mods\":[\"mod/m.mod\"]}");
        cli::GameOptions options;
        options.game_dir = game;
        options.user_dir = user;
        cli::GameReport report = cli::check_game(options);
        CHECK(report.mod_plugins.size() == 1 && report.mod_plugins[0].state == "pending");
        std::string error;
        CHECK(!revoke_shown_plugin(game, report.mod_plugins[0], error) && contains(error, "no approval binds probe"));

        // The files changed after the check: no approval.
        cli::GameModPlugin shown = report.mod_plugins[0];
        write(folder / "probe.dll", "MZ changed");
        CHECK(!approve_shown_plugin(game, user, report, shown, error) && contains(error, "changed since they were shown"));
        report = cli::check_game(options);
        shown = report.mod_plugins[0];
        CHECK(approve_shown_plugin(game, user, report, shown, error));
        report = cli::check_game(options);
        CHECK(report.mod_plugins[0].state == "approved" && report.mod_plugins[0].status == "loaded");
        CHECK(report.load_order.size() == 1 && report.load_order[0].mod == "M");

        // The mod is no longer enabled.
        write(user / "dlc_load.json", "{\"enabled_mods\":[]}");
        CHECK(!approve_shown_plugin(game, user, report, shown, error) && contains(error, "no longer has this plugin"));
        CHECK(revoke_shown_plugin(game, report.mod_plugins[0], error));
        CHECK(read_trust(game / "anvil" / "trust.toml").entries.empty());
        CHECK(!fs::exists(game / "anvil" / "data" / "anvil" / "approved" / "probe"));
    });

    run("plugins from mods: an update is shown by its new files, and only what can be approved is offered", [] {
        TempDir temp("mod-update");
        const fs::path game = make_game(temp.dir / "game");
        const fs::path user = temp.dir / "user";
        fs::create_directories(game / "anvil" / "plugins");
        const fs::path folder = user / "mod" / "m" / "anvil" / "probe";
        write(folder / "plugin.toml", plugin_toml("probe"));
        write(folder / "probe.dll", "MZ");
        write(user / "mod" / "m.mod", "name=\"M\"\npath=\"mod/m\"\n");
        write(user / "dlc_load.json", "{\"enabled_mods\":[\"mod/m.mod\"]}");
        cli::GameOptions options;
        options.game_dir = game;
        options.user_dir = user;
        cli::GameReport report = cli::check_game(options);
        CHECK(report.mod_plugins[0].state == "pending" && can_approve_shown(report.mod_plugins[0]));
        CHECK(!report.mod_plugins[0].update && !report.mod_plugins[0].offered().manifest->affects_gameplay);
        std::string error;
        CHECK(approve_shown_plugin(game, user, report, report.mod_plugins[0], error));
        report = cli::check_game(options);
        CHECK(report.mod_plugins[0].state == "approved" && !can_approve_shown(report.mod_plugins[0]));

        // The update changes the game: the copy that still loads says otherwise.
        std::string changed = plugin_toml("probe", "", "2.0.0");
        changed.replace(changed.find("affects_gameplay = false"), 24, "affects_gameplay = true");
        write(folder / "plugin.toml", changed);
        report = cli::check_game(options);
        cli::GameModPlugin shown = report.mod_plugins[0];
        CHECK(shown.state == "update-pending" && shown.status == "loaded" && can_approve_shown(shown));
        CHECK(shown.report.manifest && !shown.report.manifest->affects_gameplay);
        CHECK(shown.update && shown.offered().manifest && shown.offered().manifest->affects_gameplay);
        CHECK(shown.offered().manifest->version.to_string() == "2.0.0" && shown.version == "2.0.0");

        // A broken update still loads the copy, but is refused, so it is not offered.
        write(folder / "plugin.toml", "id = ");
        report = cli::check_game(options);
        shown = report.mod_plugins[0];
        CHECK(shown.state == "update-pending" && !shown.problem.empty() && !can_approve_shown(shown));
        CHECK(shown.report.manifest && !shown.offered().manifest);
        CHECK(!approve_shown_plugin(game, user, report, shown, error) && contains(error, "cannot be approved"));
    });

    manager_benchmark_tests();
    manager_runner_tests();
    manager_report_tests();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
