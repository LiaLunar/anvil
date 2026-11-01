// anvil-game (plugins/anvil-game) end to end: the plugin DLL loads through the
// host table against a synthetic image laid out like eu4.exe 1.37.5 (same
// sections, the declared bytes of plugins/anvil-game/symbols.toml at their
// RVAs), its frame hook runs on a callable stand-in of the frame function, and
// the gameplay policy listens to its session event as the core does. Also the
// session tracker and the work queue on their own.
// Usage: anvil_game_plugin_tests <folder with one folder per plugin>
#include "core/benchmark.hpp"
#include "core/changes.hpp"
#include "core/gameplay_policy.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-game/src/session.hpp"
#include "../plugins/anvil-game/src/work_queue.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace anvil;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool g_multiplayer = false;  // what the stand-in of the session's vtable slot 0x318 answers
bool fake_is_multiplayer_slot(void*) { return g_multiplayer; }

// The types of the events the stand-in of SDL_PushEvent got, and its time stamp.
std::vector<uint32_t> g_pushed;
int fake_push_event_rest(const void* event) {
    uint32_t type = 0;
    std::memcpy(&type, event, sizeof(type));
    g_pushed.push_back(type);
    return 1;
}

// eu4.exe's size and sections, reserved; pages are committed only where the
// symbols of anvil-game lie:
//   .text  0x1000    code   the declared bytes; frame and is_multiplayer callable
//   .rdata 0x1B66000 read   the game session's vtable 0x1CB8460
//   .data  0x1FB0000 write  root global 0x233FE78, ready flag 0x233FF4E
struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        for (uint32_t page : {0x205000u, 0x6E9000u, 0x6EA000u, 0x756000u, 0x802000u, 0x848000u, 0x1432000u,
                              0x14C2000u, 0x1732000u, 0x1746000u, 0x1761000u}) {
            commit(page, PAGE_EXECUTE_READWRITE);
        }
        commit(0x1CB8000, PAGE_READWRITE);
        commit(0x1FB0000, PAGE_READWRITE);
        commit(0x233F000, PAGE_READWRITE);
        write_headers();

        // frame: the declared prologue, then inc qword [rcx+320h] and the epilogue.
        put(0x14C25A0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                        0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70,
                        0x48, 0xFF, 0x81, 0x20, 0x03, 0x00, 0x00,              // inc qword [rcx+320h]
                        0x48, 0x83, 0xC4, 0x70, 0x41, 0x5E,                     // add rsp,70h; pop r14
                        0x48, 0x8B, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x6C, 0x24, 0x10,
                        0x48, 0x8B, 0x74, 0x24, 0x18, 0x48, 0x8B, 0x7C, 0x24, 0x20,
                        0x31, 0xC0, 0xC3});                                    // xor eax,eax; ret
        put(0x14C2A23, {0x48, 0xFF, 0x87, 0x20, 0x03, 0x00, 0x00});
        put(0x14C2E0E, {0xB2, 0x01, 0x48, 0x8B, 0xCE, 0xE8, 0x88, 0xF7, 0xFF, 0xFF, 0x44, 0x38, 0xB6, 0x83, 0x00, 0x00, 0x00});
        put(0x14C2DD0, {0x44, 0x38, 0xB6, 0xF8, 0x02, 0x00, 0x00, 0x75, 0x48});
        put(0x205EED, {0x48, 0x89, 0x1D, 0x0C, 0x36, 0x14, 0x02, 0xC6, 0x05, 0x53, 0xA0, 0x13, 0x02, 0x01});
        put(0x14321FC, {0x48, 0x8B, 0x05, 0x75, 0xDC, 0xF0, 0x00, 0x8B, 0x98, 0xD0, 0x1D, 0x00, 0x00});
        put(0x848976, {0x48, 0x8B, 0x05, 0xFB, 0x74, 0xAF, 0x01, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x88, 0x00, 0x1E,
                       0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x00, 0x01, 0x00, 0x00});
        put(0x8024D0, {0x48, 0x8B, 0x81, 0x18, 0x04, 0x00, 0x00, 0xC3});
        // set_session: the declared bytes (it stores the session at root+0x1E00), then
        // add rsp,20h; pop rdi; mov rbp,[rsp+10h]; mov rsi,[rsp+18h]; ret.
        put(0x756A30, {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20,
                       0x48, 0x89, 0x91, 0x00, 0x1E, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0x48, 0x8B, 0x6C,
                       0x24, 0x10, 0x48, 0x8B, 0x74, 0x24, 0x18, 0xC3});
        put(0x6E9128, {0x48, 0x8B, 0x0D, 0x49, 0x6D, 0xC5, 0x01, 0xC6, 0x40, 0x18, 0x01, 0x48, 0x8B, 0x89,
                       0x00, 0x1E, 0x00, 0x00, 0xE8, 0xD1, 0x18, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0x32, 0x6D,
                       0xC5, 0x01, 0x0F, 0xB6, 0xD8, 0x0F, 0xB6, 0xB9, 0xD0, 0x24, 0x00, 0x00});
        // is_multiplayer: the declared bytes (it calls vtable slot 0x318), then
        // add rsp,20h; pop rdi; mov rbx,[rsp+8]; ret.
        put(0x6EAA10, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B,
                       0xF9, 0xFF, 0x90, 0x18, 0x03, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0x48, 0x8B, 0x5C,
                       0x24, 0x08, 0xC3});
        // SDL's dynamic API: the init stores SDL_PushEvent in slot 88 (0x1FB02C0).
        put(0x17325DD, {0x48, 0x8D, 0x0D, 0xAC, 0x45, 0x01, 0x00, 0x48, 0x89, 0x0D, 0xD5, 0xDC, 0x87, 0x00});
        put_pointer(0x1FB02C0, address(0x1746B90));
        // SDL_PushEvent: the declared bytes (push rdi; sub rsp,20h; mov rdi,rcx;
        // call SDL_GetTicks; mov [rdi+4],eax), then mov rcx,rdi; mov rax,
        // fake_push_event_rest; call rax; add rsp,20h; pop rdi; ret.
        put(0x1746B90, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0x82, 0xB3, 0x01, 0x00, 0x89, 0x47,
                        0x04, 0x48, 0x89, 0xF9, 0x48, 0xB8});
        put_pointer(0x1746BA6, reinterpret_cast<uintptr_t>(&fake_push_event_rest));
        put(0x1746BAE, {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3});
        put(0x1761F20, {0xB8, 0xD2, 0x04, 0x00, 0x00, 0xC3});  // SDL_GetTicks: mov eax,1234; ret
        // The game session's vtable: slot 0x100 = speed_object, slot 0x318 = the multiplayer answer.
        put_pointer(0x1CB8460 + 0x100, address(0x8024D0));
        put_pointer(0x1CB8460 + 0x318, reinterpret_cast<uintptr_t>(&fake_is_multiplayer_slot));
        DWORD old = 0;
        VirtualProtect(base + 0x1CB8000, 0x1000, PAGE_READONLY, &old);
    }
    ~FakeEu4() { VirtualFree(base, 0, MEM_RELEASE); }

    void commit(uint32_t page, DWORD protect) { VirtualAlloc(base + page, 0x1000, MEM_COMMIT, protect); }
    uintptr_t address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(base + rva, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
    }
    void put_pointer(uint32_t rva, uintptr_t value) { std::memcpy(base + rva, &value, sizeof(value)); }

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
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::memcpy(base + 0x80, &nt, sizeof(nt));
        struct Layout {
            const char* name;
            uint32_t rva, size, flags;
        };
        const Layout sections[] = {
            {".text", 0x1000, 0x1B64D78, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            {".rdata", 0x1B66000, 0x44999A, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
            {".data", 0x1FB0000, 0x4B4FB4, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
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

// The game objects anvil-game reads: the application (frame counter, quit
// flags), the root (date, session, ironman) and a session of the game class.
struct GameObjects {
    alignas(16) uint8_t app[0x400] = {};
    alignas(16) uint8_t root[0x2500] = {};
    alignas(16) uint8_t session[0x420] = {};
    alignas(16) uint8_t speed[0x60] = {};

    uint64_t frame_counter() const {
        uint64_t v = 0;
        std::memcpy(&v, app + 0x320, sizeof(v));
        return v;
    }
    void set_date(int32_t raw) { std::memcpy(root + 0x1DD0, &raw, sizeof(raw)); }
    void set_session(const void* p) {
        const auto v = reinterpret_cast<uintptr_t>(p);
        std::memcpy(root + 0x1E00, &v, sizeof(v));
    }
};

// What a plugin hears from anvil-game, in order.
struct Heard {
    std::vector<std::string> events;  // "ready", "frame <n>", "session <started|ended> <n> im=<0|1> mp=<0|1>"
    std::vector<uint32_t> threads;
};

void listen(const char* event, const void* payload, void* context) {
    auto* h = static_cast<Heard*>(context);
    h->threads.push_back(GetCurrentThreadId());
    const std::string name = event;
    if (name == ANVIL_GAME_EVENT_READY) {
        h->events.push_back("ready");
    } else if (name == ANVIL_GAME_EVENT_FRAME) {
        const auto* f = static_cast<const AnvilGameFrame*>(payload);
        h->events.push_back("frame " + std::to_string(f->number));
    } else if (name == ANVIL_GAME_EVENT_SESSION) {
        const auto* s = static_cast<const AnvilGameSession*>(payload);
        h->events.push_back(std::string("session ") + (s->change == ANVIL_GAME_SESSION_STARTED ? "started " : "ended ") +
                            std::to_string(s->number) + " im=" + std::to_string(s->ironman) +
                            " mp=" + std::to_string(s->multiplayer));
    }
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_game_tests <plugins folder>\n");
        return 2;
    }
    const std::filesystem::path plugins_dir = std::filesystem::absolute(argv[1]);

    run("anvil-game: the session tracker", [] {
        anvil_game::SessionTracker t;
        anvil_game::SessionView none;
        CHECK(t.update(none).count == 0 && !t.active());
        anvil_game::SessionView a;
        a.running = true;
        a.session = 0x1000;
        auto c = t.update(a);
        CHECK(c.count == 1 && c.items[0].change == ANVIL_GAME_SESSION_STARTED && c.items[0].number == 1 &&
              c.items[0].size == sizeof(AnvilGameSession) && t.active());
        CHECK(t.update(a).count == 0);
        // A read that faulted says nothing: the session goes on.
        anvil_game::SessionView fault;
        fault.readable = false;
        CHECK(t.update(fault).count == 0 && t.active());
        // Another save loaded from the game: the old session ends, the new one starts.
        anvil_game::SessionView b = a;
        b.session = 0x2000;
        b.ironman = true;
        c = t.update(b);
        CHECK(c.count == 2 && c.items[0].change == ANVIL_GAME_SESSION_ENDED && c.items[0].number == 1 &&
              c.items[0].ironman == 0 && c.items[1].change == ANVIL_GAME_SESSION_STARTED && c.items[1].number == 2 &&
              c.items[1].ironman == 1);
        // A mode that changes within the same session object counts as a new session.
        anvil_game::SessionView b_mp = b;
        b_mp.multiplayer = true;
        c = t.update(b_mp);
        CHECK(c.count == 2 && c.items[1].multiplayer == 1 && c.items[1].number == 3);
        // A save loaded from the game menu: the game puts the same session object
        // back into the root, with the same flags; only the install count tells.
        anvil_game::SessionView reloaded = b_mp;
        reloaded.installs = b_mp.installs + 1;
        c = t.update(reloaded);
        CHECK(c.count == 2 && c.items[0].change == ANVIL_GAME_SESSION_ENDED && c.items[0].number == 3 &&
              c.items[1].change == ANVIL_GAME_SESSION_STARTED && c.items[1].number == 4 && c.items[1].ironman == 1 &&
              c.items[1].multiplayer == 1);
        CHECK(t.update(reloaded).count == 0);
        c = t.update(none);
        CHECK(c.count == 1 && c.items[0].change == ANVIL_GAME_SESSION_ENDED && c.items[0].ironman == 1 &&
              c.items[0].multiplayer == 1 && !t.active());
    });

    run("anvil-game: dates decode as the game counts them", [] {
        const auto is = [](int32_t raw, int32_t year, uint32_t month, uint32_t day) {
            const AnvilGameDate d = anvil_game::decode_date(raw);
            return d.size == sizeof(AnvilGameDate) && d.year == year && d.month == month && d.day == day &&
                   d.session == 0;
        };
        constexpr int32_t kDay = 24;
        constexpr int32_t k1444 = 43'800'000 + 1444 * 365 * kDay;  // 1 January 1444, hour 0
        CHECK(is(k1444, 1444, 1, 1));
        CHECK(is(k1444 + 314 * kDay, 1444, 11, 11));
        CHECK(is(k1444 + 59 * kDay, 1444, 3, 1));  // no 29 February
        CHECK(is(k1444 + 364 * kDay + 23, 1444, 12, 31) && is(k1444 + 365 * kDay, 1445, 1, 1));
        CHECK(is(58'860'240, 1719, 3, 17));
        CHECK(is(anvil_game::kUnsetDate, 1, 1, 1) && is(0, -5000, 1, 1) && is(-1, -5001, 12, 31));
    });

    run("anvil-game: the work queue", [] {
        anvil_game::WorkQueue q;
        static std::vector<int> ran;
        ran.clear();
        static anvil_game::WorkQueue* queue = &q;
        q.push([](void* c) { ran.push_back(static_cast<int>(reinterpret_cast<intptr_t>(c))); }, reinterpret_cast<void*>(1));
        // Work queued by work waits for the next run.
        q.push([](void*) {
            ran.push_back(2);
            queue->push([](void*) { ran.push_back(3); }, nullptr);
        }, nullptr);
        CHECK(q.pending() == 2 && q.run() == 2 && (ran == std::vector<int>{1, 2}) && q.pending() == 1);
        CHECK(q.run() == 1 && (ran == std::vector<int>{1, 2, 3}) && q.run() == 0);
    });

    // As in the game, the image, the host and the policy outlive the plugin.
    FakeEu4& image = *new FakeEu4;
    GameObjects& game = *new GameObjects;
    ChangeRegistry& changes = *new ChangeRegistry(image.address(0), FakeEu4::kSize);
    ServiceBus& bus = *new ServiceBus;
    std::vector<std::string>& log = *new std::vector<std::string>;
    HostContext& context = *new HostContext;
    context.changes = &changes;
    context.bus = &bus;
    context.image = GetModuleHandleW(nullptr);
    context.log = [&log](const AnvilPlugin& p, AnvilLogLevel, std::string_view message) {
        log.push_back(p.manifest.id + ": " + std::string(message));
    };
    AnvilGameInfo& info = *new AnvilGameInfo{};
    info.size = sizeof(info);
    info.build_id = kBuild;
    info.image_base = image.address(0);
    info.image_size = FakeEu4::kSize;
    const AnvilHost& host = *new AnvilHost(make_host(&info));  // the DLL's handles use it while it detaches
    GameplayPolicy& policy = *new GameplayPolicy(changes, bus, {}, [&log](AnvilLogLevel, const std::string& id, const std::string& m) {
        log.push_back(id + ": " + m);
    });
    policy.subscribe();
    // A benchmark start, as the core registers it before any plugin loads.
    Benchmark& bench = *new Benchmark(true, [&log](const AnvilPlugin& p, const std::string& m) {
        log.push_back(p.manifest.id + ": " + m);
    });
    bus.register_service("anvil", ANVIL_BENCHMARK_SERVICE, ANVIL_BENCHMARK_SERVICE_VERSION, bench.service());
    Heard& heard = *new Heard;
    for (const char* event : {ANVIL_GAME_EVENT_READY, ANVIL_GAME_EVENT_FRAME, ANVIL_GAME_EVENT_SESSION}) {
        bus.subscribe("listener", event, listen, &heard);
    }
    volatile uint8_t* const ready_flag = image.base + 0x233FF4E;
    const auto frame = reinterpret_cast<uintptr_t (*)(void*, uint8_t)>(image.address(0x14C25A0));
    AnvilPlugin* plugin = nullptr;
    const AnvilGameService* service = nullptr;

    run("anvil-game: symbols verify and the plugin loads", [&] {
        const auto dir = plugins_dir / ANVIL_GAME_ID;
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest && parsed.manifest->phase == Phase::Early && !parsed.manifest->affects_gameplay);
        if (!parsed.manifest) return;
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes, image.address(0),
                                                        &verified, &declared);
        if (!reason.empty()) std::printf("  %s\n", reason.c_str());
        CHECK(reason.empty() && verified.symbols.size() == 14);
        if (!reason.empty()) return;
        auto* p = new AnvilPlugin;
        p->context = &context;
        p->manifest = *parsed.manifest;
        p->symbols = PluginSymbols(declared, verified, image.address(0));
        p->owner = changes.add_owner(p->manifest.id);
        p->plugin_dir = dir.wstring();
        p->module = LoadLibraryExW((dir / p->manifest.entry).c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(p->module != nullptr);
        if (p->module == nullptr) return;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(p->module, "anvil_plugin_load"));
        const LoadOutcome outcome = run_plugin_load(*p, host, fn);
        CHECK(outcome.result == ANVIL_OK && outcome.staged == 2 && changes.count(p->owner) == 2);
        CHECK(changes.chain_at(image.address(0x14C25A0)) == std::vector<OwnerId>{p->owner});
        CHECK(changes.chain_at(image.address(0x756A30)) == std::vector<OwnerId>{p->owner});
        plugin = p;
        service = static_cast<const AnvilGameService*>(bus.get_service(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION));
        CHECK(service != nullptr && service->size == sizeof(AnvilGameService) && ANVIL_GAME_HAS(service, date));
        CHECK(contains(log, "anvil-game: frame hook staged; events anvil-game.ready, anvil-game.frame, "
                            "anvil-game.session; service anvil-game.game v1"));
    });
    if (plugin == nullptr || service == nullptr) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    run("anvil-game: frames before readiness, nested frames, other threads", [&] {
        CHECK(service->ready() == 0 && service->main_thread_id() == 0 && service->frame_number() == 0);
        // A nested frame (outer = 0) runs the game's frame and nothing else.
        CHECK(frame(game.app, 0) == 0 && game.frame_counter() == 1 && heard.events.empty() &&
              service->main_thread_id() == 0);
        // The first outer frame names the main thread.
        frame(game.app, 1);
        CHECK(game.frame_counter() == 2 && service->main_thread_id() == GetCurrentThreadId());
        CHECK(heard.events == std::vector<std::string>{"frame 2"} && service->frame_number() == 2 && service->ready() == 0);
        // An outer frame on another thread is passed on, unreported.
        std::thread([&] { frame(game.app, 1); }).join();
        CHECK(game.frame_counter() == 3 && heard.events.size() == 1);
    });

    run("anvil-game: readiness once, from the game's ready flag", [&] {
        *ready_flag = 1;
        frame(game.app, 1);
        frame(game.app, 1);
        CHECK((heard.events == std::vector<std::string>{"frame 2", "ready", "frame 4", "frame 5"}));
        CHECK(service->ready() == 1 && contains(log, "anvil-game: game ready (frame 4)"));
        CHECK(std::all_of(heard.threads.begin(), heard.threads.end(),
                          [](uint32_t t) { return t == GetCurrentThreadId(); }));
    });

    run("anvil-game: work runs on the main thread after the next frame, before its event", [&] {
        static std::vector<std::string>* events = &heard.events;
        static std::atomic<uint32_t> ran_on{0};
        heard.events.clear();
        std::thread([&] {
            CHECK(service->run_on_main_thread([](void* c) {
                ran_on = GetCurrentThreadId();
                events->push_back(std::string("work ") + static_cast<const char*>(c));
            }, const_cast<char*>("a")) == ANVIL_OK);
        }).join();
        CHECK(service->run_on_main_thread(nullptr, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(heard.events.empty());
        frame(game.app, 1);
        CHECK((heard.events == std::vector<std::string>{"work a", "frame 6"}) && ran_on == GetCurrentThreadId());
        // Quitting: no work, no events.
        service->run_on_main_thread([](void*) { events->push_back("late work"); }, nullptr);
        game.app[0x2F8] = 1;
        frame(game.app, 1);
        game.app[0x2F8] = 0;
        game.app[0x83] = 1;
        frame(game.app, 1);
        CHECK(heard.events.size() == 2 && game.frame_counter() == 8);
        game.app[0x83] = 0;
        frame(game.app, 1);
        CHECK((heard.events == std::vector<std::string>{"work a", "frame 6", "late work", "frame 9"}));
    });

    // A plugin that affects gameplay, with one byte patch, as the policy sees it.
    const OwnerId gp = changes.add_owner("gp");
    {
        const uint8_t patch[1] = {0x90};
        const ChangeTarget target{image.address(0x205000), true, nullptr, nullptr, 0};
        CHECK(changes.patch_bytes(gp, target, patch, 1) == ChangeStatus::Ok);
        std::string toml = plugin_toml("gp");
        toml.replace(toml.find("affects_gameplay = false"), 24, "affects_gameplay = true");
        policy.add(manifest(toml), gp);
    }

    run("anvil-game: sessions from the game root; the policy suspends in ironman and multiplayer", [&] {
        heard.events.clear();
        // No root yet, then a root with a menu session (another class).
        frame(game.app, 1);
        const uintptr_t root = reinterpret_cast<uintptr_t>(game.root);
        std::memcpy(image.base + 0x233FE78, &root, sizeof(root));
        game.set_date(43'800'000 + 365 * 24);
        alignas(16) static uint8_t menu_session[0x420] = {};
        game.set_session(menu_session);
        frame(game.app, 1);
        AnvilGameDate d{};
        CHECK(service->session(nullptr) == 0 && heard.events.size() == 2);
        CHECK(service->date(&d) == 0 && d.size == 0 && service->date(nullptr) == 0);  // the main menu

        // A game session: the class's vtable, a speed object, a date.
        const uintptr_t vtable = image.address(0x1CB8460);
        std::memcpy(game.session, &vtable, sizeof(vtable));
        const uintptr_t speed = reinterpret_cast<uintptr_t>(game.speed);
        std::memcpy(game.session + 0x418, &speed, sizeof(speed));
        game.set_session(game.session);
        frame(game.app, 1);
        CHECK(service->session(nullptr) == 0);  // the date is still unset: loading
        CHECK(service->date(&d) == 0);
        game.set_date(58'860'240);  // 17 March 1719
        frame(game.app, 1);
        AnvilGameSession s{};
        CHECK(service->session(&s) == 1 && s.number == 1 && s.ironman == 0 && s.multiplayer == 0);
        CHECK(service->date(&d) == 1 && d.size == sizeof(AnvilGameDate) && d.year == 1719 && d.month == 3 &&
              d.day == 17 && d.session == 1 && service->date(nullptr) == 1);
        CHECK(heard.events[3] == "session started 1 im=0 mp=0" && heard.events[4] == "frame 13");
        CHECK(image.base[0x205000] == 0x90 && !changes.suspended(gp));

        // The player loads an ironman save: the old session ends, the new one starts, gp goes.
        alignas(16) static uint8_t second[0x420] = {};
        std::memcpy(second, game.session, sizeof(second));
        game.set_session(second);
        game.root[0x24D0] = 1;
        heard.events.clear();
        frame(game.app, 1);
        CHECK((heard.events == std::vector<std::string>{"session ended 1 im=0 mp=0", "session started 2 im=1 mp=0",
                                                        "frame 14"}));
        CHECK(changes.suspended(gp) && image.base[0x205000] != 0x90 && policy.suspended(gp));
        CHECK(contains(log, "anvil: game session started (ironman): 1 gameplay plugin(s) suspended, 0 allowed by the "
                            "player"));
        CHECK(service->date(&d) == 1 && d.year == 1719 && d.day == 17 && d.session == 2);

        // Multiplayer as well (the game's own check through vtable slot 0x318).
        g_multiplayer = true;
        heard.events.clear();
        frame(game.app, 1);
        CHECK(heard.events[1] == "session started 3 im=1 mp=1" && changes.suspended(gp));

        // Back to the menu: the session ends and gp comes back.
        game.set_session(menu_session);
        heard.events.clear();
        frame(game.app, 1);
        CHECK((heard.events == std::vector<std::string>{"session ended 3 im=1 mp=1", "frame 16"}));
        CHECK(!changes.suspended(gp) && image.base[0x205000] == 0x90 && service->session(&s) == 0);
        CHECK(service->date(&d) == 0);
        CHECK(contains(log, "anvil-game: session 3 ended (ironman, multiplayer)"));
    });

    run("anvil-game: a fault while reading the session changes nothing", [&] {
        game.set_session(game.session);
        g_multiplayer = false;
        game.root[0x24D0] = 0;
        frame(game.app, 1);
        CHECK(service->session(nullptr) == 1);
        heard.events.clear();
        // A session pointer into reserved, uncommitted memory of the image.
        game.set_session(image.base + 0x10000);
        frame(game.app, 1);
        CHECK(service->session(nullptr) == 1 && (heard.events == std::vector<std::string>{"frame 18"}));
        AnvilGameDate d{};
        CHECK(service->date(&d) == 1 && d.day == 17 && d.session == 4);  // the last date read
        game.set_session(game.session);
    });

    run("anvil-game: a save loaded from the game menu ends the session and starts the next", [&] {
        const auto set_session = reinterpret_cast<void (*)(void*, void*)>(image.address(0x756A30));
        // As in the game: inside one frame of the main loop the root's session is
        // taken out, the save is read and the same session object is put back
        // through the setter, five times.
        const auto load_in_game = [&] {
            game.set_session(nullptr);
            for (int i = 0; i < 5; ++i) set_session(game.root, game.session);
        };
        const auto session_events = [&](uint64_t ended, int ironman_before, int ironman_after) {
            return std::vector<std::string>{
                "session ended " + std::to_string(ended) + " im=" + std::to_string(ironman_before) + " mp=0",
                "session started " + std::to_string(ended + 1) + " im=" + std::to_string(ironman_after) + " mp=0"};
        };
        AnvilGameSession s{};
        CHECK(service->session(&s) == 1 && s.ironman == 0 && !changes.suspended(gp));

        // A save of the same mode: the session events come, nothing to suspend.
        heard.events.clear();
        load_in_game();
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && std::equal(heard.events.begin(), heard.events.begin() + 2,
                                                     session_events(s.number, 0, 0).begin()));
        CHECK(service->session(&s) == 1 && !changes.suspended(gp) && image.base[0x205000] == 0x90);
        heard.events.clear();
        frame(game.app, 1);
        CHECK(heard.events.size() == 1);  // the next frame: no session event

        // Days pass inside game updates; the date shows each after the next frame.
        AnvilGameDate d{};
        CHECK(service->date(&d) == 1 && d.session == s.number && d.day == 17);
        for (int day = 1; day <= 3; ++day) {
            game.set_date(58'860'240 + day * 24);
            frame(game.app, 1);
        }
        CHECK(service->date(&d) == 1 && d.year == 1719 && d.month == 3 && d.day == 20 && d.session == s.number);
        game.set_date(58'860'240 + 15 * 24);  // 1 April, before the next frame: still 20 March
        CHECK(service->date(&d) == 1 && d.month == 3 && d.day == 20);
        frame(game.app, 1);
        CHECK(service->date(&d) == 1 && d.month == 4 && d.day == 1);

        // An ironman save: gp is suspended.
        heard.events.clear();
        load_in_game();
        game.root[0x24D0] = 1;
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && std::equal(heard.events.begin(), heard.events.begin() + 2,
                                                     session_events(s.number, 0, 1).begin()));
        CHECK(service->session(&s) == 1 && changes.suspended(gp) && image.base[0x205000] != 0x90);

        // Another ironman save: gp stays suspended once both events are out.
        heard.events.clear();
        load_in_game();
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && std::equal(heard.events.begin(), heard.events.begin() + 2,
                                                     session_events(s.number, 1, 1).begin()));
        CHECK(service->session(&s) == 1 && changes.suspended(gp) && policy.suspended(gp) &&
              image.base[0x205000] != 0x90);

        // Back to a normal save: gp comes back.
        heard.events.clear();
        load_in_game();
        game.root[0x24D0] = 0;
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && std::equal(heard.events.begin(), heard.events.begin() + 2,
                                                     session_events(s.number, 1, 0).begin()));
        CHECK(service->session(&s) == 1 && !changes.suspended(gp) && image.base[0x205000] == 0x90);
    });

    run("anvil-game: only anvil-game publishes its events", [&] {
        AnvilPlugin other;
        other.context = &context;
        other.manifest = manifest(plugin_toml("other"));
        AnvilGameSession forged{};
        forged.size = sizeof(forged);
        forged.change = ANVIL_GAME_SESSION_STARTED;
        forged.ironman = 1;
        CHECK(host.publish(&other, ANVIL_GAME_EVENT_SESSION, &forged) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(host.publish(&other, "other.event", nullptr) == ANVIL_OK);
        CHECK(!changes.suspended(gp));
        CHECK(contains(log, "other: publish anvil-game.session refused: plugins publish only events named other.<name>"));
    });

    run("anvil-game: a benchmark start quits on the core's event, on the main thread", [&] {
        CHECK(contains(log, "anvil-game: benchmark start: the game quits after ready once every participant is done"));
        g_pushed.clear();
        // From another thread, as the core publishes it.
        std::thread([&] { bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr); }).join();
        CHECK(g_pushed.empty());  // only after the next frame of the main loop
        frame(game.app, 1);
        CHECK((g_pushed == std::vector<uint32_t>{0x100}));
        CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) {
            return l.starts_with("anvil-game: benchmark start: SDL_QUIT pushed (frame ") && l.ends_with(", result 1)");
        }));

        // Someone took over SDL's table: no quit.
        image.put_pointer(0x1FB02C0, image.address(0x1761F20));
        bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr);
        frame(game.app, 1);
        CHECK((g_pushed == std::vector<uint32_t>{0x100}));
        CHECK(contains(log, "anvil-game: benchmark start: SDL's SDL_PushEvent slot points elsewhere; not quitting"));
        image.put_pointer(0x1FB02C0, image.address(0x1746B90));
    });

    if (g_failures != 0) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
