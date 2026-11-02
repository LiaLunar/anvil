// Plugin API 2 end to end: the host table of src/core/host.cpp loads the
// example plugins (hello-c, hello-cpp) and the test plugins (probe, throwing)
// as the game would, against a synthetic image that holds the code their
// symbols.toml declare, at the same RVAs (those of eu4.exe 1.37.5 for the
// examples). Usage: anvil_plugin_tests <folder with one folder per plugin>
#include "core/changes.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/settings.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace anvil;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

// A 16 MB image reserved in this process, with pages committed only where
// the headers and the declared symbols lie:
//   .text  0x1000..0xFFE000 code   test symbols at 0x1000.., hello's at 0x8024D0 and 0x815CAE
//   .rdata 0xFFE000         read   tick_slot -> add_one
//   .data  0xFFF000         write  counter
struct FakeGame {
    static constexpr uint32_t kSize = 0x1000000;
    uint8_t* base = nullptr;

    FakeGame() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        for (uint32_t page : {0x0u, 0x1000u, 0x802000u, 0x815000u, 0xFFE000u, 0xFFF000u}) {
            VirtualAlloc(base + page, 0x1000, MEM_COMMIT, page == 0 || page >= 0xFFE000 ? PAGE_READWRITE : PAGE_EXECUTE_READWRITE);
        }
        write_headers();
        // Test plugins (tests/plugins/symbols.toml).
        put(0x1000, {0x89, 0xC8, 0x83, 0xC0, 0x01, 0xC3});
        put(0x1100, {0x48, 0x89, 0xC8, 0x48, 0x83, 0xC0, 0x05, 0xC3});
        put(0x1200, {0x48, 0x89, 0xC8, 0x48, 0x83, 0xC0, 0x05, 0xC3});
        put(0x1300, {0x8D, 0x04, 0x09, 0xC3});
        put(0x1400, {0x48, 0x89, 0xC8, 0x48, 0x83, 0xC0, 0x05, 0xC3});
        put(0x1500, {0x8B, 0x05, 0xFA, 0xDA, 0xFF, 0x00, 0xC3});
        put(0x1600, {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        const uint64_t add_one = address(0x1000);
        std::memcpy(base + 0xFFE000, &add_one, sizeof(add_one));
        DWORD old = 0;
        VirtualProtect(base + 0xFFE000, 0x1000, PAGE_READONLY, &old);
        // The examples (examples/*/symbols.toml): speed_object returns
        // [rcx+0x418]; idle_exit's lea is followed by mov rax, rcx; ret.
        put(0x8024D0, {0x48, 0x8B, 0x81, 0x18, 0x04, 0x00, 0x00, 0xC3});
        put(0x815CAE, {0x48, 0x8D, 0x8D, 0x40, 0x02, 0x00, 0x00, 0x48, 0x89, 0xC8, 0xC3});
    }
    ~FakeGame() { VirtualFree(base, 0, MEM_RELEASE); }

    uintptr_t address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
    template <class F> F function(uint32_t rva) const { return reinterpret_cast<F>(address(rva)); }

    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(base + rva, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
    }

    void write_headers() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(base, &dos, sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 3;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = kSize;
        nt.OptionalHeader.SizeOfHeaders = 0x200;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::memcpy(base + 0x80, &nt, sizeof(nt));
        struct Layout {
            const char* name;
            uint32_t rva, size, flags;
        };
        const Layout sections[] = {
            {".text", 0x1000, 0xFFD000, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            {".rdata", 0xFFE000, 0x1000, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
            {".data", 0xFFF000, 0x1000, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
        };
        uint32_t at = 0x80 + sizeof(IMAGE_NT_HEADERS64);
        for (const auto& l : sections) {
            IMAGE_SECTION_HEADER s{};
            std::memcpy(s.Name, l.name, std::strlen(l.name));
            s.VirtualAddress = l.rva;
            s.Misc.VirtualSize = l.size;
            s.Characteristics = l.flags;
            std::memcpy(base + at, &s, sizeof(s));
            at += sizeof(s);
        }
    }
};

// The host side of one test run: the registry over the fake image, the bus,
// the host table, and every plugin's log lines.
struct Loader {
    FakeGame& game;
    std::filesystem::path plugins_dir;
    ChangeRegistry changes;
    ServiceBus bus;
    HostContext context;
    AnvilGameInfo info{};
    AnvilHost host{};
    std::vector<std::unique_ptr<AnvilPlugin>> plugins;
    std::vector<std::string> log;  // "<id>: <message>"

    Loader(FakeGame& g, std::filesystem::path dir)
        : game(g), plugins_dir(std::move(dir)), changes(g.address(0), FakeGame::kSize) {
        context.changes = &changes;
        context.bus = &bus;
        context.image = GetModuleHandleW(nullptr);
        context.log = [this](const AnvilPlugin& p, AnvilLogLevel, std::string_view message) {
            log.push_back(p.manifest.id + ": " + std::string(message));
        };
        info.size = sizeof(info);
        info.build_id = kBuild;
        info.image_base = g.address(0);
        info.image_size = FakeGame::kSize;
        host = make_host(&info);
    }

    // As runtime.cpp does: manifest, symbols verified against the image,
    // owner in load order, the DLL, then anvil_plugin_load as one group.
    AnvilPlugin* load(const char* folder, LoadOutcome* outcome) {
        const auto dir = plugins_dir / folder;
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest.has_value());
        if (!parsed.manifest) return nullptr;
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = anvil::i18n::english(
            check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes, game.address(0), &verified, &declared));
        if (!reason.empty()) std::printf("  %s: %s\n", folder, reason.c_str());
        CHECK(reason.empty());
        if (!reason.empty()) return nullptr;
        auto plugin = std::make_unique<AnvilPlugin>();
        plugin->context = &context;
        plugin->manifest = *parsed.manifest;
        plugin->symbols = PluginSymbols(declared, verified, game.address(0));
        plugin->owner = changes.add_owner(plugin->manifest.id);
        plugin->plugin_dir = dir.wstring();
        if (!parsed.manifest->settings.empty()) {
            SettingsFileResult settings = load_settings(dir / parsed.manifest->settings);
            CHECK(settings.file.has_value());
            if (!settings.file) return nullptr;
            // The player's file in a fresh folder: each run starts from the defaults.
            const auto data = std::filesystem::temp_directory_path() /
                              ("anvil_plugin_tests_" + std::to_string(GetCurrentProcessId())) / plugin->manifest.id;
            std::filesystem::remove_all(data);
            plugin->data_dir = data.wstring();
            const auto notes = plugin->settings.init(std::move(*settings.file), data / "settings.toml", plugin->manifest.id,
                                                     [](const std::string&) { return std::optional<std::string>(); });
            for (const auto& note : notes) log.push_back(plugin->manifest.id + ": " + note.message);
        }
        plugin->module = LoadLibraryExW((dir / plugin->manifest.entry).c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(plugin->module != nullptr);
        if (plugin->module == nullptr) return nullptr;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(plugin->module, "anvil_plugin_load"));
        CHECK(fn != nullptr);
        if (fn == nullptr) return nullptr;
        *outcome = run_plugin_load(*plugin, host, fn);
        plugins.push_back(std::move(plugin));
        return plugins.back().get();
    }

    bool logged(const std::string& line) const {
        for (const auto& l : log) {
            if (l == line) return true;
        }
        return false;
    }
    size_t logged_containing(const std::string& part) const {
        size_t n = 0;
        for (const auto& l : log) n += l.find(part) != std::string::npos ? 1 : 0;
        return n;
    }
    void dump() const {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }
};

template <class F> F export_of(const AnvilPlugin* plugin, const char* name) {
    return plugin != nullptr ? reinterpret_cast<F>(GetProcAddress(plugin->module, name)) : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_plugin_tests <plugins folder>\n");
        return 2;
    }
    const std::filesystem::path plugins_dir = std::filesystem::absolute(argv[1]);

    run("api 2: the host table is complete", [] {
        const AnvilHost h = make_host(nullptr);
        static_assert(ANVIL_API_VERSION == 2 && sizeof(AnvilCpuContext) == 408);
        CHECK(h.size == sizeof(AnvilHost) && h.api_version == ANVIL_API_VERSION);
        CHECK(ANVIL_HOST_HAS(&h, publish));
        // Every function pointer is set (the fields after `game` are all functions).
        const auto* first = reinterpret_cast<const void* const*>(&h.log);
        const auto* end = reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(&h) + sizeof(h));
        for (const auto* p = first; p < end; ++p) CHECK(*p != nullptr);
        CHECK(to_result(ChangeStatus::NotDeclared) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(to_result(ChangeStatus::UnsafeSite) == ANVIL_ERROR_UNSAFE_SITE);
        CHECK(to_result(ChangeStatus::NotFound) == ANVIL_ERROR_NOT_FOUND);
    });

    // As in the game, the image and the host outlive the plugins: their global
    // handles (HookOf, PointerSwap) remove themselves when the DLLs detach at
    // exit, which the host answers without touching anything.
    FakeGame& game = *new FakeGame;
    Loader& loader = *new Loader(game, plugins_dir);
    using IntFn = int (*)(int);
    using U64Fn = uint64_t (*)(uint64_t);
    const auto add_one = game.function<IntFn>(0x1000);
    const auto adjust = game.function<U64Fn>(0x1100);
    const auto triple = game.function<U64Fn>(0x1200);
    const auto thrower = game.function<U64Fn>(0x1400);
    const auto patch_site = game.function<int (*)()>(0x1600);
    const auto tick_slot = reinterpret_cast<IntFn const*>(game.address(0xFFE000));
    AnvilPlugin* probe = nullptr;

    run("api 2: the probe loads through the C++ wrapper", [&] {
        LoadOutcome outcome;
        probe = loader.load("probe", &outcome);
        CHECK(probe != nullptr && outcome.result == ANVIL_OK && outcome.exception == 0);
        if (probe == nullptr) return;
        // Staged during load, enabled together afterwards.
        CHECK(outcome.staged == 5 && loader.changes.count(probe->owner) == 5);
        CHECK(loader.logged("probe: twice(21) = 42"));
        CHECK(loader.logged("probe: counter 7 through the same address"));
        CHECK(loader.logged("probe: patch_bytes twice: invalid argument (1)"));
        CHECK(loader.logged("probe: begin_group: wrong phase"));
        CHECK(loader.logged("probe: patch_bytes twice refused: invalid argument"));
        if (g_failures != 0) loader.dump();
    });

    run("api 2: wrapper changes act on the game", [&] {
        CHECK(add_one(1) == 102);                // function hook: next + 100
        CHECK(adjust(5) == 1010);                // mid hook: rcx + 1000
        CHECK(triple(7) == 21);                  // block replacement
        CHECK((*tick_slot)(1) == 1020);          // pointer swap: previous (add_one, hooked: 102) * 10
        CHECK(loader.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr) == 1);
        CHECK(loader.logged("probe: plugins loaded"));
    });

    run("api 2: an exception in a callback is logged once and the game continues", [&] {
        CHECK(thrower(1) == 7);    // rcx + 1, then + 5
        CHECK(thrower(13) == 18);  // threw before changing rcx
        CHECK(thrower(13) == 18);
        CHECK(loader.logged_containing("mid_hook thrower: the callback threw (unlucky thirteen)") == 1);
        CHECK(thrower(2) == 8);
    });

    run("api 2: a throw in load() rolls back what it staged", [&] {
        LoadOutcome outcome;
        AnvilPlugin* throwing = loader.load("throwing", &outcome);
        CHECK(throwing != nullptr && outcome.result == ANVIL_ERROR_INTERNAL && outcome.staged == 2);
        if (throwing == nullptr) return;
        CHECK(loader.changes.count(throwing->owner) == 0 && loader.changes.staged_count(throwing->owner) == 0);
        CHECK(loader.logged("throwing: load() failed: refused on purpose"));
        // Its hook (-1) and mid hook (rcx = 0) never reached the game.
        CHECK(add_one(1) == 102);
        CHECK(adjust(5) == 1010);
    });

    run("api 2: the C and C++ hello examples", [&] {
        LoadOutcome c_outcome, cpp_outcome;
        AnvilPlugin* hello_c = loader.load("hello-c", &c_outcome);
        AnvilPlugin* hello_cpp = loader.load("hello-cpp", &cpp_outcome);
        CHECK(hello_c != nullptr && c_outcome.result == ANVIL_OK && c_outcome.staged == 2);
        CHECK(hello_cpp != nullptr && cpp_outcome.result == ANVIL_OK && cpp_outcome.staged == 2);
        if (hello_c == nullptr || hello_cpp == nullptr) return;
        CHECK(loader.logged_containing("hello-c: hello from C on Anvil ") == 1);
        CHECK(loader.logged("hello-cpp: hello from C++ on Anvil " ANVIL_VERSION ", game build eu4-1.37.5-steam-win64"));
        // Both hook speed_object: one chain, hello-c outermost.
        CHECK((loader.changes.chain_at(game.address(0x8024D0)) ==
               std::vector<OwnerId>{hello_c->owner, hello_cpp->owner}));
        alignas(8) uint8_t session[0x420] = {};
        void* const speed = &session[0];
        std::memcpy(session + 0x418, &speed, sizeof(speed));
        const auto speed_object = game.function<void* (*)(void*)>(0x8024D0);
        CHECK(speed_object(session) == speed);
        CHECK(speed_object(session) == speed);
        CHECK(loader.logged_containing("hello-c: first game-speed object: ") == 1);
        CHECK(loader.logged_containing("hello-cpp: first game-speed object: ") == 1);
        const auto idle_exit = game.function<uintptr_t (*)()>(0x815CAE);
        idle_exit();
        idle_exit();
        CHECK(loader.logged_containing("hello-c: returning to the menu (rcx=0x") == 2);
        CHECK(loader.logged_containing("hello-cpp: returning to the menu (rcx=0x") == 2);
        CHECK(loader.logged_containing(", 2 so far)") == 1);
        loader.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
        CHECK(loader.logged("hello-c: every plugin is loaded") && loader.logged("hello-cpp: every plugin is loaded"));

        // Its setting (restart = false), changed as the overlay would: the
        // subscription runs and the next return to the menu reads the new value.
        CHECK(hello_cpp->settings.size() == 1 && loader.host.setting_bool(hello_cpp, 0) == 1);
        const auto change = hello_cpp->settings.set_text(0, "false");
        CHECK(change.result == ANVIL_OK && change.applied && change.saved && change.notified == 1);
        CHECK(loader.logged("hello-cpp: logging returns to the menu: off"));
        idle_exit();
        CHECK(loader.logged_containing("hello-cpp: returning to the menu (rcx=0x") == 2);
        CHECK(loader.logged_containing("hello-c: returning to the menu (rcx=0x") == 3);
        hello_cpp->settings.reset(0);
        CHECK(loader.logged("hello-cpp: logging returns to the menu: on"));
        idle_exit();
        CHECK(loader.logged_containing(", 4 so far)") == 1);
        if (g_failures != 0) loader.dump();
    });

    // examples/hello-rust is built by cargo, not CMake: CI lays it out in the
    // plugins folder before ctest (ANVIL_REQUIRE_HELLO_RUST then makes a missing
    // plugin a failure); elsewhere the test is skipped when it is not there.
    run("api 2: the Rust hello example", [&] {
        const bool required = GetEnvironmentVariableA("ANVIL_REQUIRE_HELLO_RUST", nullptr, 0) != 0;
        if (!std::filesystem::exists(plugins_dir / "hello-rust" / "hello_rust.dll")) {
            std::printf("  hello-rust is not built (cargo build -p hello-rust --release); skipped\n");
            CHECK(!required);
            return;
        }
        LoadOutcome outcome;
        AnvilPlugin* hello_rust = loader.load("hello-rust", &outcome);
        CHECK(hello_rust != nullptr && outcome.result == ANVIL_OK && outcome.staged == 2);
        if (hello_rust == nullptr) return;
        CHECK(loader.logged("hello-rust: hello from Rust on Anvil " ANVIL_VERSION ", game build eu4-1.37.5-steam-win64"));
        // After hello-c and hello-cpp on speed_object: the third change of the chain.
        const auto chain = loader.changes.chain_at(game.address(0x8024D0));
        CHECK(chain.size() == 3 && chain.back() == hello_rust->owner);
        alignas(8) uint8_t session[0x420] = {};
        void* const speed = &session[0];
        std::memcpy(session + 0x418, &speed, sizeof(speed));
        const auto speed_object = game.function<void* (*)(void*)>(0x8024D0);
        CHECK(speed_object(session) == speed);
        CHECK(speed_object(session) == speed);
        CHECK(loader.logged_containing("hello-rust: first game-speed object: 0x") == 1);
        const auto idle_exit = game.function<uintptr_t (*)()>(0x815CAE);
        idle_exit();
        CHECK(loader.logged_containing("hello-rust: returning to the menu (rcx=0x") == 1);
        loader.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
        CHECK(loader.logged("hello-rust: every plugin is loaded"));
        if (g_failures != 0) loader.dump();
    });

    run("api 2: anvil::Host and Change after load", [&] {
        const auto adjusted = export_of<int (*)()>(probe, "probe_adjusted");
        const auto remove_adjust = export_of<void (*)()>(probe, "probe_remove_adjust");
        const auto remove_add = export_of<void (*)()>(probe, "probe_remove_add");
        const auto patch_in_group = export_of<int (*)(int)>(probe, "probe_patch_in_group");
        const auto hook_again = export_of<int (*)()>(probe, "probe_hook_again");
        CHECK(adjusted && remove_adjust && remove_add && patch_in_group && hook_again);
        if (!adjusted || !remove_adjust || !remove_add || !patch_in_group || !hook_again) return;
        const int before = adjusted();
        CHECK(adjust(5) == 1010 && adjusted() == before + 1);
        remove_adjust();  // Change::reset
        CHECK(adjust(5) == 10 && adjusted() == before + 1);
        // One change per hook object: again only after remove().
        CHECK(hook_again() == ANVIL_ERROR_INVALID_ARGUMENT && add_one(1) == 102);
        remove_add();     // FunctionHook::remove
        CHECK(add_one(1) == 2);
        CHECK(hook_again() == ANVIL_OK && add_one(1) == 102);
        remove_add();
        CHECK(add_one(1) == 2);
        // A group after load: enabled together, or dropped when its body throws.
        CHECK(patch_in_group(1) == 1 && patch_site() == 1);
        CHECK(patch_in_group(0) == 0 && patch_site() == 2);
        CHECK(loader.changes.count(probe->owner) == 4);  // triple, thrower, swap, patch
    });

    run("api 2: host calls refuse what the C ABI forbids", [&] {
        if (probe == nullptr) return;
        const AnvilHost& h = loader.host;
        void* next = nullptr;
        AnvilChange* change = nullptr;
        auto detour = reinterpret_cast<void*>(+[](int x) { return x; });
        CHECK(h.hook_function(probe, 999, detour, &next, &change) == ANVIL_ERROR_NOT_DECLARED && change == nullptr);
        CHECK(h.hook_function(probe, 2 /* triple: a site */, detour, &next, &change) == ANVIL_ERROR_WRONG_KIND);
        CHECK(h.swap_pointer(probe, 0 /* add_one */, detour, nullptr, nullptr) == ANVIL_ERROR_WRONG_KIND);
        // Raw addresses inside the image are not declared symbols.
        CHECK(h.hook_function_at(probe, reinterpret_cast<void*>(game.address(0x1000)), nullptr, 0, detour, &next,
                                 nullptr) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(h.remove(probe, reinterpret_cast<AnvilChange*>(uintptr_t{99999})) == ANVIL_ERROR_NOT_FOUND);
        uint64_t value = 0;
        CHECK(h.symbol_value(probe, 5, "counter_ref", &value) == ANVIL_OK && value == 0xFFF000);
        CHECK(h.symbol_value(probe, 5, "nope", &value) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(h.address_of(probe, 0) == reinterpret_cast<void*>(game.address(0x1000)) && h.address_of(probe, 99) == nullptr);
        CHECK(h.publish(probe, "anvil.forged", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        // The probe declares no settings: every one reads as 0 or "".
        CHECK(h.setting_int(probe, 0) == 0 && h.setting_bool(probe, 0) == 0 && h.setting_float(probe, 0) == 0.0);
        CHECK(std::string(h.setting_string(probe, 0)).empty());
        CHECK(h.subscribe_setting(probe, 0, [](AnvilSetting, void*) {}, nullptr) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(loader.logged("probe: subscribe_setting setting 0 (not declared) refused: not declared in symbols.toml or "
                            "settings.toml"));
        CHECK(h.setting_int(nullptr, 0) == 0 && std::string(h.setting_string(nullptr, 0)).empty());
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
