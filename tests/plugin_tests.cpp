// Plugin API 2 end to end: the plugin set (src/core/plugin_set.cpp) loads the
// example plugins (hello-c, hello-cpp, and the Rust hello-rust and panel-rust
// when cargo built them) and the test plugins (probe, throwing)
// as the game does, through the host table, against a synthetic image that holds the code their
// symbols.toml declare, at the same RVAs (those of eu4.exe 1.37.5 for the
// examples). Usage: anvil_plugin_tests <folder with one folder per plugin>
#include "plugin_set_fixture.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
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

// The services examples/panel-rust asks for, as anvil-game and the overlay give
// them: a session in Russia on 1719.3.17 and a panel that keeps its rows.
namespace fake {

std::vector<std::string> rows;  // "label=value", "[button 1]", "---"
AnvilPanelClick click = nullptr;
void* click_context = nullptr;
int shown = 0;

AnvilResult panel_create(const char*, AnvilPanel** out) {
    *out = reinterpret_cast<AnvilPanel*>(uintptr_t{0x10});
    return ANVIL_OK;
}
AnvilResult panel_set_rows(AnvilPanel*, const AnvilPanelRow* r, uint32_t count, uint32_t row_size) {
    if (row_size != sizeof(AnvilPanelRow)) return ANVIL_ERROR_INVALID_ARGUMENT;
    rows.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (r[i].kind == ANVIL_PANEL_SEPARATOR) {
            rows.push_back("---");
        } else if (r[i].kind == ANVIL_PANEL_BUTTON) {
            rows.push_back("[" + std::string(r[i].label) + " " + std::to_string(r[i].id) + "]");
        } else {
            rows.push_back(std::string(r[i].label) + (r[i].value != nullptr ? "=" + std::string(r[i].value) : ""));
        }
    }
    return ANVIL_OK;
}
AnvilResult panel_on_click(AnvilPanel*, AnvilPanelClick c, void* context) {
    click = c;
    click_context = context;
    return ANVIL_OK;
}
AnvilResult panel_show(AnvilPanel*, int s) {
    shown = s;
    return ANVIL_OK;
}
int panel_shown(const AnvilPanel*) { return shown; }
const char* panel_language() { return "en"; }

bool in_session = false;
int game_ready() { return 1; }
uint32_t game_main_thread_id() { return GetCurrentThreadId(); }
uint64_t game_frame_number() { return 1; }
AnvilResult game_run_on_main_thread(AnvilGameWork work, void* context) {
    work(context);
    return ANVIL_OK;
}
int game_session(AnvilGameSession* out) {
    if (out != nullptr && in_session) *out = {sizeof(AnvilGameSession), ANVIL_GAME_SESSION_STARTED, 0, 0, 1};
    return in_session ? 1 : 0;
}
int game_date(AnvilGameDate* out) {
    if (out != nullptr && in_session) *out = {sizeof(AnvilGameDate), 1719, 3, 17, 1};
    return in_session ? 1 : 0;
}

void tag(AnvilGameTag* out, const char* text) { std::memcpy(out->text, text, 4); }
AnvilResult world_player(AnvilGameTag* out) {
    if (!in_session) return ANVIL_ERROR_WRONG_PHASE;
    tag(out, "RUS");
    return ANVIL_OK;
}
AnvilResult world_country(const char* t, AnvilGameCountry* out) {
    if (std::string(t) != "RUS") return ANVIL_ERROR_NOT_FOUND;
    if (out->size < sizeof(AnvilGameCountry)) return ANVIL_ERROR_INVALID_ARGUMENT;
    *out = {};
    out->size = sizeof(AnvilGameCountry);
    tag(&out->tag, "RUS");
    out->capital = 295;
    out->province_count = 123;
    out->values = ANVIL_GAME_VALUE_TREASURY | ANVIL_GAME_VALUE_STABILITY;
    out->treasury = 1234500;
    out->stability = 1000;
    return ANVIL_OK;
}
AnvilResult world_province_name(int32_t id, char* utf8, uint32_t capacity, uint32_t* length) {
    if (id != 295) return ANVIL_ERROR_NOT_FOUND;
    const std::string name = "Moskva";
    *length = static_cast<uint32_t>(name.size());
    if (capacity > 0) {
        const size_t fits = std::min<size_t>(name.size(), capacity - 1);
        std::memcpy(utf8, name.data(), fits);
        utf8[fits] = 0;
    }
    return ANVIL_OK;
}
AnvilResult world_selected_units(AnvilGameUnitRef*, uint32_t, uint32_t* count) {
    *count = 0;
    return ANVIL_OK;
}

AnvilPanelService panel_service() {
    AnvilPanelService s{};
    s.size = sizeof(s);
    s.create = panel_create;
    s.set_rows = panel_set_rows;
    s.on_click = panel_on_click;
    s.show = panel_show;
    s.shown = panel_shown;
    s.language = panel_language;
    return s;
}
AnvilGameService game_service() {
    AnvilGameService s{};
    s.size = sizeof(s);
    s.ready = game_ready;
    s.main_thread_id = game_main_thread_id;
    s.frame_number = game_frame_number;
    s.run_on_main_thread = game_run_on_main_thread;
    s.session = game_session;
    s.date = game_date;
    return s;
}
AnvilGameWorldService world_service() {
    AnvilGameWorldService s{};
    s.size = sizeof(s);
    s.player = world_player;
    s.country = world_country;
    s.province_name = world_province_name;
    s.selected_units = world_selected_units;
    return s;
}

}  // namespace fake

template <class F> F export_of(const AnvilPlugin* plugin, const char* name) {
    return plugin != nullptr ? reinterpret_cast<F>(GetProcAddress(plugin->record->dll(), name)) : nullptr;
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
    TestStart& start = *new TestStart(game.address(0), FakeGame::kSize, kBuild);
    // Every plugin of the run in load order, the Rust examples when cargo
    // built them: the set is made before the first one loads.
    // The Rust examples are built by cargo, not CMake: CI lays them out in the
    // plugins folder before ctest (ANVIL_REQUIRE_RUST_EXAMPLES then makes a
    // missing plugin a failure); elsewhere a test is skipped when it is not there.
    const bool rust_required = GetEnvironmentVariableA("ANVIL_REQUIRE_RUST_EXAMPLES", nullptr, 0) != 0;
    const bool hello_rust_built = std::filesystem::exists(plugins_dir / "hello-rust" / "hello_rust.dll");
    const bool panel_rust_built = std::filesystem::exists(plugins_dir / "panel-rust" / "panel_rust.dll");
    for (const char* folder : {"probe", "throwing", "hello-c", "hello-cpp"}) start.add(plugins_dir / folder);
    if (hello_rust_built) start.add(plugins_dir / "hello-rust");
    if (panel_rust_built) start.add(plugins_dir / "panel-rust");
    start.make();
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
        probe = start.load("probe");
        CHECK(probe != nullptr);
        if (probe == nullptr) return start.dump();
        // Staged during load, enabled together afterwards.
        CHECK(start.changes.count(probe->owner) == 5 && start.logged_containing("probe 1.0.0: loaded (main, ") == 1);
        CHECK(start.logged("probe: twice(21) = 42"));
        CHECK(start.logged("probe: counter 7 through the same address"));
        CHECK(start.logged("probe: patch_bytes twice: invalid argument (1)"));
        CHECK(start.logged("probe: begin_group: wrong phase"));
        CHECK(start.logged("probe: patch_bytes twice refused: invalid argument"));
        if (g_failures != 0) start.dump();
    });

    run("api 2: wrapper changes act on the game", [&] {
        CHECK(add_one(1) == 102);                // function hook: next + 100
        CHECK(adjust(5) == 1010);                // mid hook: rcx + 1000
        CHECK(triple(7) == 21);                  // block replacement
        CHECK((*tick_slot)(1) == 1020);          // pointer swap: previous (add_one, hooked: 102) * 10
        CHECK(start.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr) == 1);
        CHECK(start.logged("probe: plugins loaded"));
    });

    run("api 2: an exception in a callback is logged once and the game continues", [&] {
        CHECK(thrower(1) == 7);    // rcx + 1, then + 5
        CHECK(thrower(13) == 18);  // threw before changing rcx
        CHECK(thrower(13) == 18);
        CHECK(start.logged_containing("mid_hook thrower: the callback threw (unlucky thirteen)") == 1);
        CHECK(thrower(2) == 8);
    });

    run("api 2: a throw in load() rolls back what it staged", [&] {
        CHECK(start.load("throwing") == nullptr);
        const PluginRecord& record = start.plugin("throwing");
        const AnvilPlugin* throwing = record.handle();
        CHECK(record.outcome() != nullptr && record.outcome()->kind == Outcome::Kind::failed);
        CHECK(start.logged("anvil: throwing 1.0.0: anvil_plugin_load returned internal error; 2 change(s) rolled back; plugin stays "
                           "inactive"));
        CHECK(start.changes.count(throwing->owner) == 0 && start.changes.staged_count(throwing->owner) == 0);
        CHECK(start.logged("throwing: load() failed: refused on purpose"));
        // Its hook (-1) and mid hook (rcx = 0) never reached the game.
        CHECK(add_one(1) == 102);
        CHECK(adjust(5) == 1010);
    });

    run("api 2: the C and C++ hello examples", [&] {
        AnvilPlugin* hello_c = start.load("hello-c");
        AnvilPlugin* hello_cpp = start.load("hello-cpp");
        CHECK(hello_c != nullptr && start.changes.count(hello_c->owner) == 2);
        CHECK(hello_cpp != nullptr && start.changes.count(hello_cpp->owner) == 2);
        if (hello_c == nullptr || hello_cpp == nullptr) return;
        CHECK(start.logged_containing("hello-c: hello from C on Anvil ") == 1);
        CHECK(start.logged("hello-cpp: hello from C++ on Anvil " ANVIL_VERSION ", game build eu4-1.37.5-steam-win64"));
        // Both hook speed_object: one chain, hello-c outermost.
        CHECK((start.changes.chain_at(game.address(0x8024D0)) ==
               std::vector<OwnerId>{hello_c->owner, hello_cpp->owner}));
        alignas(8) uint8_t session[0x420] = {};
        void* const speed = &session[0];
        std::memcpy(session + 0x418, &speed, sizeof(speed));
        const auto speed_object = game.function<void* (*)(void*)>(0x8024D0);
        CHECK(speed_object(session) == speed);
        CHECK(speed_object(session) == speed);
        CHECK(start.logged_containing("hello-c: first game-speed object: ") == 1);
        CHECK(start.logged_containing("hello-cpp: first game-speed object: ") == 1);
        const auto idle_exit = game.function<uintptr_t (*)()>(0x815CAE);
        idle_exit();
        idle_exit();
        CHECK(start.logged_containing("hello-c: returning to the menu (rcx=0x") == 2);
        CHECK(start.logged_containing("hello-cpp: returning to the menu (rcx=0x") == 2);
        CHECK(start.logged_containing(", 2 so far)") == 1);
        start.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
        CHECK(start.logged("hello-c: every plugin is loaded") && start.logged("hello-cpp: every plugin is loaded"));

        // Its setting (restart = false), changed as the overlay would: the
        // subscription runs and the next return to the menu reads the new value.
        PluginSettings& settings = hello_cpp->record->settings();
        CHECK(settings.size() == 1 && start.host.setting_bool(hello_cpp, 0) == 1);
        const auto change = settings.set_text(0, "false");
        CHECK(change.result == ANVIL_OK && change.applied && change.saved && change.notified == 1);
        CHECK(start.logged("hello-cpp: logging returns to the menu: off"));
        idle_exit();
        CHECK(start.logged_containing("hello-cpp: returning to the menu (rcx=0x") == 2);
        CHECK(start.logged_containing("hello-c: returning to the menu (rcx=0x") == 3);
        settings.reset(0);
        CHECK(start.logged("hello-cpp: logging returns to the menu: on"));
        idle_exit();
        CHECK(start.logged_containing(", 4 so far)") == 1);
        if (g_failures != 0) start.dump();
    });

    run("api 2: the Rust hello example", [&] {
        const bool required = rust_required;
        if (!hello_rust_built) {
            std::printf("  hello-rust is not built (cargo build -p hello-rust --release); skipped\n");
            CHECK(!required);
            return;
        }
        AnvilPlugin* hello_rust = start.load("hello-rust");
        CHECK(hello_rust != nullptr && start.changes.count(hello_rust->owner) == 2);
        if (hello_rust == nullptr) return;
        CHECK(start.logged("hello-rust: hello from Rust on Anvil " ANVIL_VERSION ", game build eu4-1.37.5-steam-win64"));
        // After hello-c and hello-cpp on speed_object: the third change of the chain.
        const auto chain = start.changes.chain_at(game.address(0x8024D0));
        CHECK(chain.size() == 3 && chain.back() == hello_rust->owner);
        alignas(8) uint8_t session[0x420] = {};
        void* const speed = &session[0];
        std::memcpy(session + 0x418, &speed, sizeof(speed));
        const auto speed_object = game.function<void* (*)(void*)>(0x8024D0);
        CHECK(speed_object(session) == speed);
        CHECK(speed_object(session) == speed);
        CHECK(start.logged_containing("hello-rust: first game-speed object: 0x") == 1);
        const auto idle_exit = game.function<uintptr_t (*)()>(0x815CAE);
        idle_exit();
        CHECK(start.logged_containing("hello-rust: returning to the menu (rcx=0x") == 1);
        start.bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
        CHECK(start.logged("hello-rust: every plugin is loaded"));
        if (g_failures != 0) start.dump();
    });

    // The safe crate's services from Rust: the world read in anvil-game's
    // events and in panel clicks, shown in an overlay panel.
    static const AnvilGameService game_service = fake::game_service();
    static const AnvilGameWorldService world_service = fake::world_service();
    static const AnvilPanelService panel_service = fake::panel_service();
    run("api 2: the Rust panel example", [&] {
        if (!panel_rust_built) {
            std::printf("  panel-rust is not built (cargo build -p panel-rust --release); skipped\n");
            CHECK(!rust_required);
            return;
        }
        CHECK(start.bus.register_service("anvil-game", ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, &game_service) ==
              ANVIL_OK);
        CHECK(start.bus.register_service("anvil-game", ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION,
                                          &world_service) == ANVIL_OK);
        CHECK(start.bus.register_service("anvil-overlay", ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION,
                                          &panel_service) == ANVIL_OK);
        AnvilPlugin* panel = start.load("panel-rust");
        CHECK(panel != nullptr && start.changes.count(panel->owner) == 0);
        if (panel == nullptr) return;
        CHECK(start.logged("panel-rust: country panel loaded (world service: yes, panel: yes)"));
        CHECK(fake::shown == 1 && fake::click != nullptr);
        CHECK((fake::rows == std::vector<std::string>{"No game is loaded."}));

        fake::in_session = true;
        const AnvilGameSession started{sizeof(AnvilGameSession), ANVIL_GAME_SESSION_STARTED, 0, 0, 1};
        start.bus.publish(ANVIL_GAME_EVENT_SESSION, &started);
        const std::string summary = "RUS on 1719.3.17: capital Moskva (295), 123 provinces, treasury 1234.5, stability 1";
        CHECK(start.logged("panel-rust: session 1: " + summary));
        CHECK((fake::rows == std::vector<std::string>{"Country=RUS", "Date=1719.3.17", "Capital=Moskva (295)",
                                                       "Provinces=123", "Treasury=1234.5", "Monthly income=?",
                                                       "Stability=1", "Prestige=?", "Manpower=?", "---",
                                                       "[Refresh 1]", "[Log the selected units 2]"}));
        // A month with nothing new: the rows again, no new log line.
        AnvilGameDateChange month{};
        month.size = sizeof(month);
        month.days = 1;
        start.bus.publish(ANVIL_GAME_EVENT_MONTH, &month);
        CHECK(start.logged_containing(summary) == 1 && fake::rows.size() == 12);
        // Clicks, as the overlay delivers them on the main thread.
        fake::click(fake::click_context, 1);
        CHECK(start.logged("panel-rust: refresh: " + summary));
        fake::click(fake::click_context, 2);
        CHECK(start.logged("panel-rust: selected units: none"));
        fake::in_session = false;
        const AnvilGameSession ended{sizeof(AnvilGameSession), ANVIL_GAME_SESSION_ENDED, 0, 0, 1};
        start.bus.publish(ANVIL_GAME_EVENT_SESSION, &ended);
        CHECK((fake::rows == std::vector<std::string>{"No game is loaded."}));
        // No adapter panicked on the way.
        CHECK(start.logged_containing("panicked") == 0);
        if (g_failures != 0) start.dump();
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
        CHECK(start.changes.count(probe->owner) == 4);  // triple, thrower, swap, patch
    });

    run("api 2: host calls refuse what the C ABI forbids", [&] {
        if (probe == nullptr) return;
        const AnvilHost& h = start.host;
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
        CHECK(start.logged("probe: subscribe_setting setting 0 (not declared) refused: not declared in symbols.toml or "
                            "settings.toml"));
        CHECK(h.setting_int(nullptr, 0) == 0 && std::string(h.setting_string(nullptr, 0)).empty());
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
