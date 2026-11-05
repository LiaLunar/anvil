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
#include "../plugins/anvil-game/src/world.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace anvil;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool g_multiplayer = false;  // what the stand-in of the session's vtable slot 0x318 answers
bool fake_is_multiplayer_slot(void*) { return g_multiplayer; }

// Called by the stand-in of the frame function inside every frame, as the
// game draws there; a test sets it to look at anvil-game from inside a frame.
void (*g_inside_frame)() = nullptr;
void fake_frame_body() {
    if (g_inside_frame != nullptr) g_inside_frame();
}

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
//   .rdata 0x1B66000 read   the game session's vtable 0x1CB8460, the front end's 0x1D5A8C8
//   .data  0x1FB0000 write  root global 0x233FE78, ready flag 0x233FF4E
struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        for (uint32_t page : {0x205000u, 0x6E9000u, 0x6EA000u, 0x756000u, 0x802000u, 0x848000u, 0x1432000u,
                              0x10DB000u, 0x14C2000u, 0x1732000u, 0x1746000u, 0x1761000u}) {
            commit(page, PAGE_EXECUTE_READWRITE);
        }
        commit(0x1CB8000, PAGE_READWRITE);
        commit(0x1D5A000, PAGE_READWRITE);
        commit(0x1FB0000, PAGE_READWRITE);
        commit(0x233D000, PAGE_READWRITE);
        commit(0x233F000, PAGE_READWRITE);
        write_headers();

        // frame: the declared prologue, then inc qword [rcx+320h], a call of
        // fake_frame_body (mov rax, imm64; call rax) and the epilogue.
        put(0x14C25A0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                        0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70,
                        0x48, 0xFF, 0x81, 0x20, 0x03, 0x00, 0x00,              // inc qword [rcx+320h]
                        0x48, 0xB8});
        put_pointer(0x14C25A0 + 35, reinterpret_cast<uintptr_t>(&fake_frame_body));
        put(0x14C25A0 + 43, {0xFF, 0xD0,                                       // call rax
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
        // The front end's state change, which stores the state at frontend+0x900.
        put(0x10DB037, {0x41, 0x8B, 0x8E, 0x00, 0x09, 0x00, 0x00, 0x49, 0x8B, 0x86, 0xE8, 0x08, 0x00, 0x00, 0x48, 0x8B,
                        0x0C, 0xC8, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18, 0x41, 0x89, 0xBE, 0x00, 0x09, 0x00, 0x00});
        // The game session's vtable: slot 0x100 = speed_object, slot 0x318 = the multiplayer answer.
        put_pointer(0x1CB8460 + 0x100, address(0x8024D0));
        put_pointer(0x1CB8460 + 0x318, reinterpret_cast<uintptr_t>(&fake_is_multiplayer_slot));
        // The front end's vtable: slot 0 = its destructor.
        put_pointer(0x1D5A8C8, address(0x10D2C00));
        DWORD old = 0;
        VirtualProtect(base + 0x1CB8000, 0x1000, PAGE_READONLY, &old);
        VirtualProtect(base + 0x1D5A000, 0x1000, PAGE_READONLY, &old);
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
    alignas(16) uint8_t frontend[0x910] = {};

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
    void set_front_end_state(uint32_t state) { std::memcpy(frontend + 0x900, &state, sizeof(state)); }
};

// The country database's global, from the bytes of province_base_tax on Steam.
constexpr uint32_t kCountriesDb = 0x233D8D0;

// A tag as the game stores it: three characters, the country index, the kind, the valid byte.
uint64_t game_tag(const char* text, uint16_t index, uint8_t kind = 0, uint8_t valid = 1) {
    uint64_t tag = 0;
    std::memcpy(&tag, text, std::min<size_t>(std::strlen(text), 3));
    return tag | (uint64_t{index} << 32) | (uint64_t{kind} << 48) | (uint64_t{valid} << 56);
}

// A small world, laid out as eu4.exe 1.37.5 on Steam does: provinces 1-3 and
// the countries RUS, TUR and the observer.
struct FakeWorld {
    static constexpr size_t kProvinceSize = 0x2E10;
    static constexpr int kProvinces = 4;  // the dummy entry 0 included

    std::vector<uint8_t> provinces = std::vector<uint8_t>(kProvinceSize * kProvinces);
    uintptr_t pointers[kProvinces] = {};
    alignas(8) uint8_t map[kProvinces][0x240] = {};
    uint64_t cores[2] = {game_tag("RUS", 0), game_tag("TUR", 1)};
    int32_t owned[2] = {1, 3};
    alignas(8) uint8_t rus[0x1500] = {};
    alignas(8) uint8_t tur[0x1500] = {};
    alignas(8) uint8_t observer[0x1500] = {};
    uintptr_t countries[3] = {};
    alignas(8) uint8_t db[0x800] = {};
    std::vector<uint64_t> bucket_tags[64];
    std::string long_name = "S\xE3o Tom\xE9 and a name long enough for the heap";
    alignas(8) uint8_t war_a[0x218] = {};
    alignas(8) uint8_t war_b[0x218] = {};
    uintptr_t wars[2] = {};
    uint64_t attackers[2] = {game_tag("RUS", 0), game_tag("O00", 2, 7)};
    uint64_t defenders[1] = {game_tag("TUR", 1)};

    template <class T>
    static void set(void* at, size_t offset, T value) {
        std::memcpy(static_cast<uint8_t*>(at) + offset, &value, sizeof(value));
    }
    uint8_t* province(int id) { return provinces.data() + id * kProvinceSize; }
    void set_name(int id, const std::string& name) {
        uint8_t* at = map[id] + 0x10;
        const uint64_t size = name.size();
        if (name.size() < 16) {
            std::memcpy(at, name.data(), name.size());
            set(at, 0x18, uint64_t{15});
        } else {
            set(at, 0, reinterpret_cast<uintptr_t>(name.data()));
            set(at, 0x18, size);
        }
        set(at, 0x10, size);
    }
    void add_country(uint8_t* country, uint64_t tag, uint16_t index) {
        set(country, 0x20, tag);
        countries[index] = reinterpret_cast<uintptr_t>(country);
        bucket_tags[anvil_game::tag_bucket(std::string_view(reinterpret_cast<const char*>(&tag), 3), 0x3F)].push_back(tag);
    }
    void install(uint8_t* root, FakeEu4& image) {
        for (int id = 0; id < kProvinces; ++id) {
            pointers[id] = reinterpret_cast<uintptr_t>(province(id));
            set(province(id), 0x20, id);
            set(province(id), 0x28, reinterpret_cast<uintptr_t>(map[id]));
        }
        set(root, 0x1C90, reinterpret_cast<uintptr_t>(pointers));
        set(root, 0x1C98, reinterpret_cast<uintptr_t>(pointers + kProvinces));
        set(root, 0x1CA8, reinterpret_cast<uintptr_t>(provinces.data()));
        // Moskva: RUS, core RUS and TUR, land.
        set(province(1), 0x468, game_tag("RUS", 0));
        set(province(1), 0x470, game_tag("TUR", 1));
        set(province(1), 0x490, reinterpret_cast<uintptr_t>(cores));
        set(province(1), 0x498, reinterpret_cast<uintptr_t>(cores + 2));
        set(province(1), 0x3E4, 15000);
        set(province(1), 0x3E8, 8000);
        set(province(1), 0x3E0, 9500);
        map[1][0x228] = 1 << 3;
        set_name(1, "Moskva");
        // A wasteland without an owner, with a long name in Windows-1252.
        set(province(2), 0x468, game_tag("---", 0, 0, 0));
        set(province(2), 0x470, game_tag("---", 0, 0, 0));
        map[2][0x228] = 1 << 3;
        map[2][0x229] = 1 << 3;
        set_name(2, long_name);
        // A sea with a name in UTF-8, as Mei keeps it.
        set_name(3, "\xE5\x8C\x97\xE4\xBA\xAC");
        add_country(rus, game_tag("RUS", 0), 0);
        add_country(tur, game_tag("TUR", 1), 1);
        add_country(observer, game_tag("O00", 2, 7), 2);
        set(rus, 0x14B8, 1);
        set(rus, 0x1338, reinterpret_cast<uintptr_t>(owned));
        set(rus, 0x1344, 2);
        set(rus, 0xBB0, 123456);
        set(rus, 0xBD8, 2000);
        set(rus, 0xCF8, -5500);
        set(rus, 0xDB8, 25000);
        set(rus, 0xE38, 9750);
        set(tur, 0x14B8, 99);  // no such province
        set(db, 0x118, reinterpret_cast<uintptr_t>(countries));
        for (int n = 0; n < 64; ++n) {
            set(db, 0x148 + n * 0x18, reinterpret_cast<uintptr_t>(bucket_tags[n].data()));
            set(db, 0x154 + n * 0x18, static_cast<int32_t>(bucket_tags[n].size()));
        }
        image.put_pointer(kCountriesDb, reinterpret_cast<uintptr_t>(db));
        // The Great War: RUS and O00 against TUR.
        set(war_a, 0x28 + 0x10, uint64_t{9});
        set(war_a, 0x28 + 0x18, uint64_t{15});
        std::memcpy(war_a + 0x28, "Great War", 9);
        set(war_a, 0x150, game_tag("RUS", 0));
        set(war_a, 0x158, game_tag("TUR", 1));
        set(war_a, 0x48, reinterpret_cast<uintptr_t>(attackers));
        set(war_a, 0x50, reinterpret_cast<uintptr_t>(attackers + 2));
        set(war_a, 0x78, reinterpret_cast<uintptr_t>(defenders));
        set(war_a, 0x80, reinterpret_cast<uintptr_t>(defenders + 1));
        // A second one, ready to start.
        set(war_b, 0x28 + 0x10, uint64_t{5});
        set(war_b, 0x28 + 0x18, uint64_t{15});
        std::memcpy(war_b + 0x28, "Small", 5);
        set(war_b, 0x150, game_tag("TUR", 1));
        set(war_b, 0x158, game_tag("RUS", 0));
        wars[0] = reinterpret_cast<uintptr_t>(war_a);
        wars[1] = reinterpret_cast<uintptr_t>(war_b);
        set(root, 0x1E30, reinterpret_cast<uintptr_t>(wars));
        set(root, 0x1E3C, int32_t{1});
    }
};

// What a plugin hears of the campaign: "day 1 1719.3.18", "player RUS TUR".
void listen_campaign(const char* event, const void* payload, void* context) {
    auto* heard = static_cast<std::vector<std::string>*>(context);
    const std::string name = event;
    if (name == ANVIL_GAME_EVENT_PLAYER) {
        const auto* p = static_cast<const AnvilGamePlayerChange*>(payload);
        heard->push_back(std::string("player ") + p->previous.text + " " + p->tag.text);
        return;
    }
    if (name == ANVIL_GAME_EVENT_WAR) {
        const auto* w = static_cast<const AnvilGameWarChange*>(payload);
        heard->push_back(std::string(w->change == ANVIL_GAME_WAR_STARTED ? "war started " : "war ended ") + w->name + " " +
                         w->attacker.text + " " + w->defender.text);
        return;
    }
    const auto* c = static_cast<const AnvilGameDateChange*>(payload);
    heard->push_back(name.substr(std::strlen("anvil-game.")) + " " + std::to_string(c->days) + " " +
                     std::to_string(c->date.year) + "." + std::to_string(c->date.month) + "." +
                     std::to_string(c->date.day));
}

// Runs in the handler of the frame event, then clears itself.
std::function<void()> g_in_frame_handler;
void run_in_frame_handler(const char*, const void*, void*) {
    if (g_in_frame_handler) {
        const auto work = std::move(g_in_frame_handler);
        g_in_frame_handler = nullptr;
        work();
    }
}

const AnvilGameService* g_service = nullptr;

// What a plugin hears from anvil-game, in order.
struct Heard {
    // "ready", "frame <n>", "session <started|ended> <n> im=<0|1> mp=<0|1>", "main menu <0|1>"
    std::vector<std::string> events;
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
    } else if (name == ANVIL_GAME_EVENT_MAIN_MENU) {
        h->events.push_back("main menu " + std::to_string(g_service->main_menu()));
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

    run("anvil-game: the date's events", [] {
        constexpr int32_t kDay = 24;
        constexpr int32_t k31Jan = 43'800'000 + 1719 * 365 * kDay + 30 * kDay;
        const auto kinds = [](const anvil_game::DateEvents& e) {
            return std::string(e.day ? "d" : "") + (e.month ? "m" : "") + (e.year ? "y" : "");
        };
        anvil_game::DateTracker t;
        CHECK(kinds(t.update(true, 1, k31Jan)).empty());      // the first date of a session
        CHECK(kinds(t.update(true, 1, k31Jan)).empty());      // a frame without a new day
        CHECK(kinds(t.update(true, 1, k31Jan + 5)).empty());  // hours are not days
        const auto day = t.update(true, 1, k31Jan + kDay);    // 1 February
        CHECK(kinds(day) == "dm" && day.change.days == 1 && day.change.size == sizeof(AnvilGameDateChange) &&
              day.change.date.month == 2 && day.change.date.day == 1 && day.change.date.session == 1 &&
              day.change.previous.month == 1 && day.change.previous.day == 31 && day.change.previous.session == 1);
        CHECK(kinds(t.update(true, 1, k31Jan + 2 * kDay)) == "d");
        // Three days in one frame, across a year.
        CHECK(kinds(t.update(true, 1, k31Jan + 334 * kDay)) == "dm");  // 31 December
        const auto year = t.update(true, 1, k31Jan + 337 * kDay);      // 3 January
        CHECK(kinds(year) == "dmy" && year.change.days == 3 && year.change.date.year == 1720);
        // A date that goes back, a new session, leaving: only a new baseline.
        CHECK(kinds(t.update(true, 1, k31Jan)).empty());
        CHECK(kinds(t.update(true, 1, k31Jan + kDay)) == "dm");
        CHECK(kinds(t.update(true, 2, k31Jan + 100 * kDay)).empty());
        CHECK(kinds(t.update(false, 0, 0)).empty());
        CHECK(kinds(t.update(true, 2, k31Jan + 101 * kDay)).empty());
    });

    run("anvil-game: text and tags of the world", [] {
        using anvil_game::game_text_to_utf8;
        CHECK(game_text_to_utf8("Moskva") == "Moskva");
        CHECK(game_text_to_utf8("S\xE3o") == "S\xC3\xA3o");                       // Windows-1252
        CHECK(game_text_to_utf8("\x80") == "\xE2\x82\xAC");                       // the euro sign
        CHECK(game_text_to_utf8("\xA7Ygold\xA7!") == "gold");                      // colour codes
        CHECK(game_text_to_utf8("\xE5\x8C\x97\xE4\xBA\xAC") == "\xE5\x8C\x97\xE4\xBA\xAC");  // UTF-8 stays
        CHECK(game_text_to_utf8("\xED\xA0\x80") != "\xED\xA0\x80");              // a surrogate is not UTF-8
        CHECK(anvil_game::fit_utf8("abc", 4) == 3 && anvil_game::fit_utf8("abc", 3) == 2 &&
              anvil_game::fit_utf8("abc", 1) == 0 && anvil_game::fit_utf8("abc", 0) == 0);
        CHECK(anvil_game::fit_utf8("\xE5\x8C\x97\xE4\xBA\xAC", 5) == 3);  // never half a character
        CHECK(anvil_game::tag_bucket("RUS", 0x3F) == (('R' + 'U' + 'S') & 0x3Fu) &&
              anvil_game::tag_bucket("rus", 0x3F) == anvil_game::tag_bucket("RUS", 0x3F));
    });

    run("anvil-game: the wars' changes", [] {
        const auto war = [](uint64_t id, const char* name) {
            anvil_game::WarSeen w;
            w.id = id;
            w.name = name;
            return w;
        };
        anvil_game::WarTracker t;
        CHECK(t.find(1, 1) == nullptr);
        auto c = t.update(1, {war(1, "a")});  // the baseline
        CHECK(c.started.empty() && c.ended.empty() && t.find(1, 1) != nullptr && t.find(2, 1) == nullptr);
        c = t.update(1, {war(1, "a"), war(2, "b")});
        CHECK(c.started.size() == 1 && c.started[0].name == "b" && c.ended.empty());
        c = t.update(1, {war(2, "b")});
        CHECK(c.ended.size() == 1 && c.ended[0].name == "a" && c.started.empty());
        c = t.update(2, {war(3, "c")});  // another session: a new baseline
        CHECK(c.started.empty() && c.ended.empty());
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
    std::vector<std::string>& campaign = *new std::vector<std::string>;
    for (const char* event : {ANVIL_GAME_EVENT_DAY, ANVIL_GAME_EVENT_MONTH, ANVIL_GAME_EVENT_YEAR, ANVIL_GAME_EVENT_PLAYER,
                              ANVIL_GAME_EVENT_WAR}) {
        bus.subscribe("listener", event, listen_campaign, &campaign);
    }
    bus.subscribe("listener", ANVIL_GAME_EVENT_FRAME, run_in_frame_handler, nullptr);
    for (const char* event : {ANVIL_GAME_EVENT_READY, ANVIL_GAME_EVENT_FRAME, ANVIL_GAME_EVENT_SESSION,
                              ANVIL_GAME_EVENT_MAIN_MENU}) {
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
        // The world's symbols: their declared bytes at their RVAs, read from the file.
        std::ifstream symbols_file(dir / parsed.manifest->symbols, std::ios::binary);
        std::stringstream symbols_text;
        symbols_text << symbols_file.rdbuf();
        SymbolFileResult symbols = parse_symbols_text(symbols_text.str());
        CHECK(symbols.file.has_value());
        if (!symbols.file) return;
        CHECK(select_build(*symbols.file, kBuild));
        bool world = false;
        for (const Symbol& s : symbols.file->symbols) {
            world = world || s.name == "province_count";
            if (!world) continue;
            for (uint32_t page = *s.rva & ~0xFFFu; page < *s.rva + s.bytes.size(); page += 0x1000) {
                image.commit(page, PAGE_EXECUTE_READWRITE);
            }
            std::memcpy(image.base + *s.rva, s.bytes.data(), s.bytes.size());
        }
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = anvil::i18n::english(check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes,
                                                                          image.address(0), &verified, &declared));
        if (!reason.empty()) std::printf("  %s\n", reason.c_str());
        CHECK(reason.empty() && verified.symbols.size() == 39);
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
        CHECK(service != nullptr && service->size == sizeof(AnvilGameService) && ANVIL_GAME_HAS(service, date) &&
              ANVIL_GAME_HAS(service, main_menu));
        CHECK(contains(log, "anvil-game: frame hook staged; events anvil-game.ready, anvil-game.frame, "
                            "anvil-game.session, anvil-game.main_menu; service anvil-game.game v1"));
        CHECK(contains(log, "anvil-game: service anvil-game.world v1; events anvil-game.day, anvil-game.month, "
                            "anvil-game.year, anvil-game.player, anvil-game.war"));
        g_service = service;
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

    run("anvil-game: the main menu from the front end's state, not over a loading screen", [&] {
        static const AnvilGameService* svc = service;
        static std::vector<int>* inside = new std::vector<int>;
        const uintptr_t vtable = image.address(0x1D5A8C8);
        std::memcpy(game.frontend, &vtable, sizeof(vtable));
        game.set_front_end_state(0);
        // Leaving the game for the menu: the session ends, the front end is the root's session.
        game.set_session(game.frontend);
        heard.events.clear();
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && heard.events[0].starts_with("session ended") && heard.events[1] == "main menu 1");
        CHECK(service->main_menu() == 1 && contains(log, "anvil-game: main menu shown"));
        heard.events.clear();
        frame(game.app, 1);
        CHECK(heard.events.size() == 1);  // no change, no event

        // What is drawn inside an outer frame is the menu; inside a nested one (a save
        // loading, a game update) it is not, and afterwards it is again.
        g_inside_frame = [] { inside->push_back(svc->main_menu()); };
        frame(game.app, 1);
        frame(game.app, 0);
        g_inside_frame = nullptr;
        CHECK((*inside == std::vector<int>{1, 0}) && service->main_menu() == 1);

        // The nation selection (state 3), and back.
        heard.events.clear();
        game.set_front_end_state(3);
        frame(game.app, 1);
        CHECK(heard.events.size() == 2 && heard.events[0] == "main menu 0" && service->main_menu() == 0);
        CHECK(contains(log, "anvil-game: main menu left (front end state 3)"));
        game.set_front_end_state(0);
        frame(game.app, 1);
        CHECK(service->main_menu() == 1);

        // A front end that cannot be read is not the menu.
        game.set_session(image.base + 0x10000);
        frame(game.app, 1);
        CHECK(service->main_menu() == 0 && contains(log, "anvil-game: main menu left (no front end)"));

        // A game: the front end is not read at all.
        game.set_session(game.frontend);
        frame(game.app, 1);
        CHECK(service->main_menu() == 1);
        heard.events.clear();
        game.set_session(game.session);
        frame(game.app, 1);
        CHECK(heard.events.size() == 3 && heard.events[0].starts_with("session started") && heard.events[1] == "main menu 0");
        CHECK(service->main_menu() == 0);
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

    run("anvil-game.world: reads on the main thread after an outer frame, in a session", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && world->size == sizeof(AnvilGameWorldService) &&
              ANVIL_GAME_WORLD_HAS(world, country_provinces) && ANVIL_GAME_WORLD_HAS(world, war_side));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        const uintptr_t root = reinterpret_cast<uintptr_t>(game.root);
        std::memcpy(image.base + 0x233FE78, &root, sizeof(root));
        game.set_session(game.session);
        FakeWorld::set(game.root, 0x1E60, game_tag("RUS", 0));
        FakeWorld::set(game.root, 0x1E68, game_tag("---", 0, 0, 0));
        game.set_date(58'860'240);
        frame(game.app, 1);
        CHECK(service->session(nullptr) == 1);

        // Off the moment: from here between frames, from a frame inside a game update, from another thread.
        AnvilGameTag tag{};
        AnvilGameProvince p{};
        p.size = sizeof(p);
        AnvilGameCountry c{};
        c.size = sizeof(c);
        CHECK(world->player(&tag) == ANVIL_ERROR_WRONG_PHASE && world->province_limit() == 0);
        CHECK(world->province(1, &p) == ANVIL_ERROR_WRONG_PHASE && world->country("RUS", &c) == ANVIL_ERROR_WRONG_PHASE);
        static AnvilResult inside = ANVIL_OK;
        static const AnvilGameWorldService* world_inside = nullptr;
        world_inside = world;
        g_inside_frame = [] {
            AnvilGameTag t{};
            inside = world_inside->player(&t);
        };
        frame(game.app, 0);
        CHECK(inside == ANVIL_ERROR_WRONG_PHASE);
        inside = ANVIL_OK;
        frame(game.app, 1);
        CHECK(inside == ANVIL_ERROR_WRONG_PHASE);
        g_inside_frame = nullptr;

        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(world->province_limit() == 4);
            std::thread([&] {
                AnvilGameTag t{};
                CHECK(world->player(&t) == ANVIL_ERROR_WRONG_PHASE && world->province_limit() == 0);
            }).join();

            AnvilGameTag me{};
            CHECK(world->player(&me) == ANVIL_OK && std::strcmp(me.text, "RUS") == 0);
            CHECK(world->player(nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);

            AnvilGameProvince moskva{};
            moskva.size = sizeof(moskva);
            CHECK(world->province(1, &moskva) == ANVIL_OK && moskva.id == 1 && moskva.size == sizeof(moskva));
            CHECK(std::strcmp(moskva.owner.text, "RUS") == 0 && std::strcmp(moskva.controller.text, "TUR") == 0);
            CHECK(moskva.base_tax == 15000 && moskva.base_production == 8000 && moskva.base_manpower == 9500);
            CHECK(moskva.flags == ANVIL_GAME_PROVINCE_LAND);
            AnvilGameProvince waste{};
            waste.size = sizeof(waste);
            CHECK(world->province(2, &waste) == ANVIL_OK && waste.owner.text[0] == 0 &&
                  waste.flags == (ANVIL_GAME_PROVINCE_LAND | ANVIL_GAME_PROVINCE_WASTELAND));
            CHECK(world->province(3, &waste) == ANVIL_OK && waste.flags == 0);
            CHECK(world->province(0, &waste) == ANVIL_ERROR_NOT_FOUND && world->province(4, &waste) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->province(1, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            // A struct as an older header knows it: nothing behind its size is written.
            struct Older {
                AnvilGameProvince known;
            } older{};
            std::memset(&older, 0xCC, sizeof(older));
            older.known.size = offsetof(AnvilGameProvince, owner);
            CHECK(world->province(1, &older.known) == ANVIL_OK && older.known.id == 1 &&
                  static_cast<uint8_t>(older.known.owner.text[0]) == 0xCC);

            char name[64] = {};
            uint32_t length = 0;
            CHECK(world->province_name(1, name, sizeof(name), &length) == ANVIL_OK && std::strcmp(name, "Moskva") == 0 &&
                  length == 6);
            CHECK(world->province_name(2, name, sizeof(name), &length) == ANVIL_OK &&
                  std::string(name) == "S\xC3\xA3o Tom\xC3\xA9 and a name long enough for the heap");
            CHECK(world->province_name(3, name, 5, &length) == ANVIL_OK && length == 6 &&
                  std::strcmp(name, "\xE5\x8C\x97") == 0);
            CHECK(world->province_name(3, nullptr, 0, &length) == ANVIL_OK && length == 6);
            CHECK(world->province_name(3, nullptr, 8, &length) == ANVIL_ERROR_INVALID_ARGUMENT);

            AnvilGameTag cores[4] = {};
            uint32_t count = 0;
            CHECK(world->province_cores(1, cores, 4, &count) == ANVIL_OK && count == 2 &&
                  std::strcmp(cores[1].text, "TUR") == 0);
            CHECK(world->province_cores(1, cores, 1, &count) == ANVIL_OK && count == 2);
            CHECK(world->province_cores(2, nullptr, 0, &count) == ANVIL_OK && count == 0);

            AnvilGameCountry rus{};
            rus.size = sizeof(rus);
            CHECK(world->country("rus", &rus) == ANVIL_OK && std::strcmp(rus.tag.text, "RUS") == 0 && rus.capital == 1 &&
                  rus.province_count == 2 && rus.flags == 0);
            CHECK(rus.values == (ANVIL_GAME_VALUE_TREASURY | ANVIL_GAME_VALUE_STABILITY | ANVIL_GAME_VALUE_PRESTIGE |
                                 ANVIL_GAME_VALUE_MANPOWER | ANVIL_GAME_VALUE_MONTHLY_INCOME));
            CHECK(rus.treasury == 123456 && rus.stability == 2000 && rus.prestige == -5500 && rus.manpower == 25000 &&
                  rus.monthly_income == 9750);
            AnvilGameCountry other{};
            other.size = sizeof(other);
            CHECK(world->country("TUR", &other) == ANVIL_OK && other.capital == 0 && other.province_count == 0);
            CHECK(world->country("O00", &other) == ANVIL_OK && other.flags == ANVIL_GAME_COUNTRY_OBSERVER);
            CHECK(world->country("XXX", &other) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->country("RU", &other) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country("R-S", &other) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country(nullptr, &other) == ANVIL_ERROR_INVALID_ARGUMENT);
            int32_t ids[4] = {};
            CHECK(world->country_provinces("RUS", ids, 4, &count) == ANVIL_OK && count == 2 && ids[0] == 1 && ids[1] == 3);
            CHECK(world->country_provinces("TUR", nullptr, 0, &count) == ANVIL_OK && count == 0);

            uint64_t war_ids[4] = {};
            CHECK(world->war_ids(war_ids, 4, &count) == ANVIL_OK && count == 1 &&
                  war_ids[0] == reinterpret_cast<uintptr_t>(fake.war_a));
            AnvilGameWar war{};
            war.size = sizeof(war);
            CHECK(world->war(war_ids[0], &war) == ANVIL_OK && war.id == war_ids[0] &&
                  std::strcmp(war.attacker.text, "RUS") == 0 && std::strcmp(war.defender.text, "TUR") == 0);
            CHECK(world->war(0x1000, &war) == ANVIL_ERROR_NOT_FOUND && world->war(war_ids[0], nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->war_name(war_ids[0], name, sizeof(name), &length) == ANVIL_OK && std::strcmp(name, "Great War") == 0);
            CHECK(world->war_name(0x1000, name, sizeof(name), &length) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->war_side(war_ids[0], ANVIL_GAME_WAR_ATTACKERS, cores, 4, &count) == ANVIL_OK && count == 2 &&
                  std::strcmp(cores[1].text, "O00") == 0);
            CHECK(world->war_side(war_ids[0], ANVIL_GAME_WAR_DEFENDERS, cores, 4, &count) == ANVIL_OK && count == 1 &&
                  std::strcmp(cores[0].text, "TUR") == 0);
            CHECK(world->war_side(war_ids[0], 2, cores, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT);
        };
        frame(game.app, 1);
        CHECK(ran);

        // Memory that is not what it should be costs that answer only.
        ran = false;
        FakeWorld::set(fake.province(1), 0x20, 7);  // the entry holds another ID
        FakeWorld::set(fake.tur, 0x20, game_tag("SWE", 1));
        FakeWorld::set(fake.province(2), 0x490, uintptr_t{0x1000});
        FakeWorld::set(fake.province(2), 0x498, uintptr_t{0x1004});
        FakeWorld::set(fake.province(3), 0x28, uintptr_t{0x10});
        g_in_frame_handler = [&] {
            ran = true;
            AnvilGameProvince q{};
            q.size = sizeof(q);
            AnvilGameCountry r{};
            r.size = sizeof(r);
            uint32_t count = 0;
            char name[8] = {};
            CHECK(world->province(1, &q) == ANVIL_ERROR_INTERNAL && world->country("TUR", &r) == ANVIL_ERROR_INTERNAL);
            CHECK(world->province_cores(2, nullptr, 0, &count) == ANVIL_ERROR_INTERNAL);
            CHECK(world->province(3, &q) == ANVIL_ERROR_INTERNAL && world->province_name(3, name, 8, &count) == ANVIL_ERROR_INTERNAL);
            CHECK(world->province(2, &q) == ANVIL_OK && world->country("RUS", &r) == ANVIL_OK);
        };
        frame(game.app, 1);
        CHECK(ran);
        FakeWorld::set(fake.province(1), 0x20, 1);
        FakeWorld::set(fake.tur, 0x20, game_tag("TUR", 1));

        // A country value absent on a build: its bit is clear and the field 0.
        anvil_game::WorldLayout l;
        l.root = image.address(0x233FE78);
        l.countries_db = image.address(kCountriesDb);
        l.provinces = 0x1C90, l.provinces_end = 0x1C98, l.pointer_shift = 3;
        l.bucket_mask = 0x3F, l.bucket_stride = 0x18, l.buckets = 0x148, l.bucket_count = 0x154, l.compared = 3;
        l.countries = 0x118, l.country_index_shift = 32, l.country_tag = 0x20, l.tag_kind = 6, l.tag_valid = 7;
        l.observer_kind = 7, l.capital = 0x14B8, l.owned_count = 0x1344;
        l.treasury = 0xBB0;
        std::vector<uint64_t> no_wars;
        CHECK(anvil_game::read_war_ids(l, no_wars) == ANVIL_ERROR_NOT_DECLARED);
        AnvilGameCountry partial{};
        CHECK(anvil_game::read_country(l, "RUS", partial) == ANVIL_OK && partial.values == ANVIL_GAME_VALUE_TREASURY &&
              partial.treasury == 123456 && partial.stability == 0);
    });

    run("anvil-game: the campaign's events after outer frames", [&] {
        constexpr int32_t kDay = 24;
        constexpr int32_t k31Dec = 43'800'000 + 1719 * 365 * kDay + 364 * kDay;
        game.set_date(k31Dec - 2 * kDay);
        frame(game.app, 1);  // far ahead: a day and a month
        campaign.clear();
        game.set_date(k31Dec - kDay);
        frame(game.app, 1);
        CHECK((campaign == std::vector<std::string>{"day 1 1719.12.30"}));
        // A frame inside a game update moves the date; the next outer frame reports it.
        game.set_date(k31Dec);
        frame(game.app, 0);
        CHECK(campaign.size() == 1);
        frame(game.app, 1);
        CHECK(campaign.size() == 2 && campaign[1] == "day 1 1719.12.31");
        campaign.clear();
        game.set_date(k31Dec + kDay);
        frame(game.app, 1);
        CHECK((campaign == std::vector<std::string>{"day 1 1720.1.1", "month 1 1720.1.1", "year 1 1720.1.1"}));

        // Wars: the Great War ran from the start; a new one is told on the next day, once.
        auto& fake_wars = *reinterpret_cast<int32_t*>(game.root + 0x1E3C);
        campaign.clear();
        fake_wars = 2;
        frame(game.app, 1);
        CHECK(campaign.empty());  // no new day: the wars are not read
        game.set_date(k31Dec + 2 * kDay);
        frame(game.app, 1);
        CHECK((campaign == std::vector<std::string>{"day 1 1720.1.2", "war started Small TUR RUS"}));
        game.set_date(k31Dec + 3 * kDay);
        frame(game.app, 1);
        CHECK(campaign.size() == 3);
        fake_wars = 1;
        game.set_date(k31Dec + 4 * kDay);
        frame(game.app, 1);
        CHECK(campaign.size() == 5 && campaign[4] == "war ended Small TUR RUS");

        // The player's country: formed, observed, then a new session starts a new baseline.
        campaign.clear();
        FakeWorld::set(game.root, 0x1E60, game_tag("TUR", 1));
        frame(game.app, 1);
        CHECK((campaign == std::vector<std::string>{"player RUS TUR"}));
        FakeWorld::set(game.root, 0x1E60, game_tag("O00", 2, 7));
        FakeWorld::set(game.root, 0x1E68, game_tag("RUS", 0));
        frame(game.app, 1);
        CHECK(campaign.size() == 2 && campaign[1] == "player TUR RUS");
        frame(game.app, 1);
        CHECK(campaign.size() == 2);
        campaign.clear();
        const auto set_session = reinterpret_cast<void (*)(void*, void*)>(image.address(0x756A30));
        FakeWorld::set(game.root, 0x1E60, game_tag("TUR", 1));
        game.set_date(k31Dec - 100 * kDay);
        fake_wars = 2;
        set_session(game.root, game.session);
        frame(game.app, 1);
        CHECK(campaign.empty());
        fake_wars = 1;
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
