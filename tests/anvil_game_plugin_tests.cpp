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
#include "control/anvil_control.h"
#include "test_support.hpp"

#include "../plugins/anvil-game/src/commands.hpp"
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
#include <map>
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
        for (uint32_t page : {0x205000u, 0x6E9000u, 0x6EA000u, 0x742000u, 0x756000u, 0x7E0000u, 0x802000u, 0x815000u, 0x848000u,
                              0x1432000u, 0x10DB000u, 0x14C2000u, 0x1732000u, 0x1746000u, 0x1761000u}) {
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
        put(0x815730, {0x49, 0x8B, 0x8D, 0xD8, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x09});
        put(0x742D18, {0x41, 0x80, 0x7C, 0x24, 0x08, 0x00, 0x75, 0x17});
        put(0x7E04AC, {0x44, 0x88, 0x7F, 0x19, 0x45, 0x84, 0xFF});
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
    alignas(16) uint8_t session[0x1C00] = {};  // the map input's pointer is at +0x1B80
    alignas(16) uint8_t speed[0x60] = {};
    alignas(16) uint8_t oos_handler[0x50] = {};
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
// CModifier::GetValue in FakeEu4: the n-th int32 from +0x20, and a fault for type 2.
int32_t* fake_modifier_value(const uint8_t* modifier, int32_t* out, int32_t type) {
    const uint8_t* at = type == 2 ? nullptr : modifier + 0x20 + type * 4;
    *out = *reinterpret_cast<const volatile int32_t*>(at);
    return out;
}

struct FakeWorld {
    static constexpr uint32_t kDefinitions = 0x2358080;
    static constexpr uint32_t kModifierValue = 0x8E6BD0;
    static constexpr size_t kProvinceSize = 0x2E10;
    static constexpr int kProvinces = 4;  // the dummy entry 0 included

    std::vector<uint8_t> provinces = std::vector<uint8_t>(kProvinceSize * kProvinces);
    uintptr_t pointers[kProvinces] = {};
    alignas(8) uint8_t map[kProvinces][0x240] = {};
    uint64_t cores[2] = {game_tag("RUS", 0), game_tag("TUR", 1)};
    int32_t owned[2] = {1, 3};
    alignas(8) uint8_t rus[0x3000] = {};
    alignas(8) uint8_t tur[0x3000] = {};
    alignas(8) uint8_t observer[0x3000] = {};
    alignas(8) uint8_t definitions[3][0x60] = {};
    uintptr_t countries[3] = {};
    alignas(8) uint8_t db[0x800] = {};
    std::vector<uint64_t> bucket_tags[64];
    std::string long_name = "S\xE3o Tom\xE9 and a name long enough for the heap";
    std::string tax_key = "local_tax_modifier";
    std::string long_key = "a_modifier_key_too_long_to_be_kept_in_place";
    alignas(8) uint8_t war_a[0x218] = {};
    alignas(8) uint8_t war_b[0x218] = {};
    uintptr_t wars[2] = {};
    uint64_t attackers[2] = {game_tag("RUS", 0), game_tag("O00", 2, 7)};
    uint64_t defenders[1] = {game_tag("TUR", 1)};

    // RUS's units: an army with a leader drilling in Moskva, three regiments
    // (1000, 1000 and 500 men), and an idle navy at sea without a leader: as in
    // the game, it points at a placeholder leader with id 0 named "No Leader". The
    // selection: a province, the navy and the army.
    static constexpr uint32_t kArmyKind = 0x1761F40;   // xor eax,eax; ret
    static constexpr uint32_t kNavyKind = 0x1761F48;   // mov eax,1; ret
    static constexpr uint32_t kDrillType = 0x1761F50;  // mov eax,15; ret
    static constexpr uint32_t kPlaceKind = 0x1761F58;  // mov eax,2; ret: a province
    static constexpr uint32_t kComputed = 0x1761F60;   // mov eax,[rcx+8]; ret: not a constant
    static constexpr uint64_t kArmyId = 0x000003E900000036;
    static constexpr uint64_t kNavyId = 0x000003EA00000036;
    static constexpr uint64_t kLeaderId = 0x0000004D00000031;
    alignas(8) uint8_t army[0x260] = {};
    alignas(8) uint8_t navy[0x260] = {};
    alignas(8) uint8_t leader[0xA8] = {};
    alignas(8) uint8_t no_leader[0xA8] = {};
    alignas(8) uint8_t regiments[3][0x90] = {};
    uintptr_t army_subunits[3] = {};
    alignas(8) uint8_t drill[0x10] = {};
    alignas(8) uint8_t idle[0x10] = {};
    uintptr_t army_vtable[2] = {}, navy_vtable[2] = {}, place_vtable[2] = {};
    uintptr_t drill_vtable[11] = {}, idle_vtable[11] = {};
    alignas(8) uint8_t unit_nodes[2][0x20] = {};
    alignas(8) uint8_t map_input[0x168] = {};
    alignas(8) uint8_t selected_nodes[3][0x20] = {};
    alignas(8) uint8_t place[0x10] = {};

    static void set_short_string(uint8_t* at, const char* text) {
        const uint64_t size = std::strlen(text);
        std::memcpy(at, text, size);
        set(at, 0x10, size);
        set(at, 0x18, uint64_t{15});
    }
    void install_units(uint8_t* session, FakeEu4& image) {
        image.put(kArmyKind, {0x33, 0xC0, 0xC3});
        image.put(kNavyKind, {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        image.put(kDrillType, {0xB8, 0x0F, 0x00, 0x00, 0x00, 0xC3});
        image.put(kPlaceKind, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3});
        image.put(kComputed, {0x8B, 0x41, 0x08, 0xC3});
        army_vtable[1] = image.address(kArmyKind);
        navy_vtable[1] = image.address(kNavyKind);
        place_vtable[1] = image.address(kPlaceKind);
        drill_vtable[10] = image.address(kDrillType);
        idle_vtable[10] = image.address(kArmyKind);  // returns 0: no mission
        set(drill, 0, reinterpret_cast<uintptr_t>(drill_vtable));
        set(idle, 0, reinterpret_cast<uintptr_t>(idle_vtable));
        set(place, 0, reinterpret_cast<uintptr_t>(place_vtable));

        set(army, 0, reinterpret_cast<uintptr_t>(army_vtable));
        set(army, 0x20, kArmyId);
        set(army, 0xD0, game_tag("RUS", 0));
        set(army, 0xD8, reinterpret_cast<uintptr_t>(leader));
        set(army, 0xE8, reinterpret_cast<uintptr_t>(province(1)));
        set(army, 0x1F8, reinterpret_cast<uintptr_t>(drill));
        set_short_string(army + 0x148, "1st Army");
        const int32_t strengths[3] = {1000, 1000, 500};
        for (int i = 0; i < 3; ++i) {
            set(regiments[i], 0x28, strengths[i]);
            army_subunits[i] = reinterpret_cast<uintptr_t>(regiments[i]);
        }
        set(army, 0x30, reinterpret_cast<uintptr_t>(army_subunits));
        set(army, 0x38, reinterpret_cast<uintptr_t>(army_subunits + 3));
        set(leader, 0x10, kLeaderId);
        set_short_string(leader + 0x20, "Sheremetev");

        set(navy, 0, reinterpret_cast<uintptr_t>(navy_vtable));
        set(navy, 0x20, kNavyId);
        set(navy, 0xD0, game_tag("RUS", 0));
        set(navy, 0xD8, reinterpret_cast<uintptr_t>(no_leader));
        set(no_leader, 0x10, uint64_t{0x31});
        set_short_string(no_leader + 0x20, "No Leader");
        set(navy, 0xE8, reinterpret_cast<uintptr_t>(province(3)));
        set(navy, 0x1F8, reinterpret_cast<uintptr_t>(idle));
        set_short_string(navy + 0x148, "1st Fleet");

        set(unit_nodes[0], 0, reinterpret_cast<uintptr_t>(army));
        set(unit_nodes[0], 0x10, reinterpret_cast<uintptr_t>(unit_nodes[1]));
        set(unit_nodes[1], 0, reinterpret_cast<uintptr_t>(navy));
        set(rus, 0xD18, reinterpret_cast<uintptr_t>(unit_nodes[0]));
        set(rus, 0xD18 + 0x10, int32_t{2});

        uint8_t* selected[3] = {place, navy, army};
        for (int i = 0; i < 3; ++i) {
            set(selected_nodes[i], 0, reinterpret_cast<uintptr_t>(selected[i]));
            if (i < 2) set(selected_nodes[i], 0x10, reinterpret_cast<uintptr_t>(selected_nodes[i + 1]));
        }
        set(map_input, 0x28, reinterpret_cast<uintptr_t>(selected_nodes[0]));
        set(map_input, 0x38, int32_t{3});
        set(session, 0x1B80, reinterpret_cast<uintptr_t>(map_input));
    }

    // Rulers, heirs and consorts (1.37.5 on Steam): RUS has a ruler (a leader
    // too, with a flag), the game's placeholder heir and a consort; TUR a
    // regency council whose heir's dynasty the game shows, and no consort.
    static constexpr uint32_t kNullMonarch = 0x233FFA0;
    static constexpr uint32_t kNullHeir = 0x233FFA8;
    static constexpr uint32_t kNullConsort = 0x233FFB0;
    static constexpr uint32_t kFlagNames = 0x2357D80;
    static constexpr uint32_t kNotQueenRegent = 0x1761F68;  // xor al,al; ret
    static constexpr uint32_t kQueenRegent = 0x1761F70;     // mov al,1; ret
    static constexpr uint64_t kRulerId = 0x0000123400000030;
    alignas(8) uint8_t ruler[0x150] = {};
    alignas(8) uint8_t consort[0x150] = {};
    alignas(8) uint8_t null_heir[0x150] = {};
    alignas(8) uint8_t regent[0x150] = {};
    alignas(8) uint8_t tur_heir[0x150] = {};
    alignas(8) uint8_t romanov[0x30] = {};
    alignas(8) uint8_t osman[0x30] = {};
    alignas(8) uint8_t regency[0x20] = {};
    uintptr_t monarch_vtable[16] = {};
    alignas(8) uint8_t flag_names[4][0x20] = {};
    std::string flag_texts[4] = {"rus_reformed", "moskva_burnt", "great_northern_war", "met_the_tsar"};
    int16_t rus_flags[2] = {0, 2};
    int32_t rus_flag_dates[2] = {};
    int16_t moskva_flags[1] = {1};
    int16_t moskva_permanent[2] = {1, 3};
    int32_t moskva_dates[1] = {}, moskva_permanent_dates[2] = {};
    int16_t ruler_flags[1] = {3};
    int32_t ruler_flag_dates[1] = {};
    int16_t global[1] = {2};
    int32_t global_dates[1] = {};
    // RUS's variables: a list of two nodes behind a sentinel; Moskva has none.
    alignas(8) uint8_t variable_head[0x40] = {};
    alignas(8) uint8_t variable_nodes[2][0x40] = {};
    alignas(8) uint8_t moskva_head[0x40] = {};
    std::string long_variable = "a_variable_name_long_enough_for_the_heap";

    // Hours since 1 January -5000, the game's dates.
    static int32_t hours_of(int year, int month, int day) {
        static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int64_t days = int64_t{year} * 365 + day - 1;
        for (int m = 0; m < month - 1; ++m) days += kDays[m];
        return static_cast<int32_t>(43'800'000 + days * 24);
    }
    static void set_flags(uint8_t* flags, int16_t* ids, int32_t* dates, size_t n) {
        set(flags, 0x10, reinterpret_cast<uintptr_t>(ids));
        set(flags, 0x18, reinterpret_cast<uintptr_t>(ids + n));
        set(flags, 0x28, reinterpret_cast<uintptr_t>(dates));
        set(flags, 0x30, reinterpret_cast<uintptr_t>(dates + n));
    }
    void install_rulers(uint8_t* root, FakeEu4& image) {
        image.put(kNotQueenRegent, {0x32, 0xC0, 0xC3});
        image.put(kQueenRegent, {0xB0, 0x01, 0xC3});
        monarch_vtable[0x60 / 8] = image.address(kNotQueenRegent);
        for (uint8_t* m : {ruler, consort, null_heir, regent, tur_heir}) set(m, 0, reinterpret_cast<uintptr_t>(monarch_vtable));
        set_key(romanov + 8, "Romanov");
        set_key(osman + 8, "Osmanoglu");

        set(ruler, 0x10, kRulerId);
        set_short_string(ruler + 0x28, "Pyotr I");
        const int32_t skills[3] = {6, 4, 5};
        std::memcpy(ruler + 0x48, skills, sizeof skills);
        set(ruler, 0x68, reinterpret_cast<uintptr_t>(romanov));
        set(ruler, 0x70, hours_of(1672, 6, 9));
        set(ruler, 0x98, kLeaderId);
        set(consort, 0x10, uint64_t{0x0000123500000030});
        set_short_string(consort + 0x28, "Catherine");
        set(consort, 0x68, reinterpret_cast<uintptr_t>(romanov));
        consort[0x74] = 1;
        set(consort, 0x98, uint64_t{0x31});  // the type of a leader's id, without one
        set_short_string(null_heir + 0x28, "No Heir");
        set(rus, 0x1280, reinterpret_cast<uintptr_t>(ruler));
        set(rus, 0x1298, reinterpret_cast<uintptr_t>(null_heir));
        set(rus, 0x12B0, reinterpret_cast<uintptr_t>(consort));
        image.put_pointer(kNullHeir, reinterpret_cast<uintptr_t>(null_heir));
        image.put_pointer(kNullMonarch, 0);  // not made yet
        image.put_pointer(kNullConsort, 0);

        set_short_string(regent + 0x28, "Regency Council");
        set(regent, 0x68, reinterpret_cast<uintptr_t>(romanov));
        set(regent, 0x78, reinterpret_cast<uintptr_t>(regency));
        set_short_string(tur_heir + 0x28, "Mehmed");
        set(tur_heir, 0x68, reinterpret_cast<uintptr_t>(osman));
        set(tur_heir, 0x148, int32_t{75000});
        set(tur, 0x1280, reinterpret_cast<uintptr_t>(regent));
        set(tur, 0x1298, reinterpret_cast<uintptr_t>(tur_heir));

        // Flags: four names; RUS has 0 and 2, Moskva 1 and permanently 1 and 3,
        // the ruler 3, the world 2.
        for (int i = 0; i < 4; ++i) set_key(flag_names[i], flag_texts[i]);
        image.commit(kFlagNames & ~0xFFFu, PAGE_READWRITE);
        image.put_pointer(kFlagNames, reinterpret_cast<uintptr_t>(flag_names));
        image.put_pointer(kFlagNames + 8, reinterpret_cast<uintptr_t>(flag_names + 4));
        rus_flag_dates[0] = hours_of(1700, 1, 1);
        rus_flag_dates[1] = hours_of(1700, 8, 19);
        set_flags(rus + 0x270, rus_flags, rus_flag_dates, 2);
        moskva_dates[0] = hours_of(1701, 2, 3);
        moskva_permanent_dates[0] = hours_of(1650, 1, 1);
        moskva_permanent_dates[1] = hours_of(1651, 1, 1);
        set_flags(province(1) + 0x1B8, moskva_flags, moskva_dates, 1);
        set_flags(province(1) + 0x1F8, moskva_permanent, moskva_permanent_dates, 2);
        ruler_flag_dates[0] = hours_of(1697, 3, 9);
        set_flags(ruler + 0xA0, ruler_flags, ruler_flag_dates, 1);
        global_dates[0] = hours_of(1700, 8, 19);
        set_flags(root + 0xB38, global, global_dates, 1);

        // RUS: treasury_plan = 2.5, then the long name = -1.
        set(variable_head, 0, reinterpret_cast<uintptr_t>(variable_nodes[0]));
        set(variable_nodes[0], 0, reinterpret_cast<uintptr_t>(variable_nodes[1]));
        set(variable_nodes[1], 0, reinterpret_cast<uintptr_t>(variable_head));
        set_key(variable_nodes[0] + 0x10, "treasury_plan");
        set(variable_nodes[0], 0x30, int32_t{2500});
        set_key(variable_nodes[1] + 0x10, long_variable);
        set(variable_nodes[1], 0x30, int32_t{-1000});
        set(rus, 0x330 + 0x18, reinterpret_cast<uintptr_t>(variable_head));
        set(rus, 0x330 + 0x20, uint64_t{2});
        set(moskva_head, 0, reinterpret_cast<uintptr_t>(moskva_head));
        set(province(1), 0x238 + 0x18, reinterpret_cast<uintptr_t>(moskva_head));
    }

    // Timed modifiers (1.21.0): RUS has a hidden one with both dates and a ruler
    // modifier without dates, in that order; Moskva the hidden one too. The
    // province view shows Moskva in a shown window.
    static constexpr int32_t kUnsetHours = 43'808'760;
    static constexpr uint32_t kNever = 0x1FD149C;  // the global date of a modifier given for good
    static constexpr int32_t kNeverHours = 43'791'240;  // -1.1.1, as the save writes it
    std::string timed_texts[2] = {"obstacle_shipbuilding_industry", "tsars_reforms"};
    alignas(8) uint8_t static_modifiers[2][0x180] = {};
    alignas(8) uint8_t timed[3][0x50] = {};
    alignas(8) uint8_t timed_nodes[3][0x20] = {};
    uintptr_t moskva_timed[1] = {};
    alignas(8) uint8_t selection_object[0x20] = {};
    alignas(8) uint8_t province_view[0x1000] = {};
    alignas(8) uint8_t view_window[0x200] = {};

    void install_timed(uint8_t* session, FakeEu4& image) {
        image.commit(kNever & ~0xFFFu, PAGE_READWRITE);
        std::memcpy(image.base + kNever, &kNeverHours, sizeof kNeverHours);
        for (int i = 0; i < 3; ++i) {
            set_key(static_modifiers[i] + 0x158, timed_texts[i]);
            set(timed[i], 0x30, reinterpret_cast<uintptr_t>(static_modifiers[i]));
            set(timed_nodes[i], 0, reinterpret_cast<uintptr_t>(timed[i]));
        }
        set(timed[0], 0x40, hours_of(1715, 1, 1));
        set(timed[0], 0x44, hours_of(1725, 1, 1));
        timed[0][0x4B] = 1;
        set(timed[1], 0x40, kUnsetHours);
        set(timed[1], 0x44, kUnsetHours);
        timed[1][0x48] = 1;
        // A third, given for good: the same key as the first, ended at the date for never.
        set(timed[2], 0x30, reinterpret_cast<uintptr_t>(static_modifiers[0]));
        set(timed[2], 0x40, hours_of(1719, 3, 17));
        set(timed[2], 0x44, kNeverHours);
        set(timed_nodes[0], 0x10, reinterpret_cast<uintptr_t>(timed_nodes[1]));
        set(timed_nodes[1], 0x10, reinterpret_cast<uintptr_t>(timed_nodes[2]));
        set(rus, 0x1F0, reinterpret_cast<uintptr_t>(timed_nodes[0]));
        moskva_timed[0] = reinterpret_cast<uintptr_t>(timed[0]);
        set(province(1), 0x2F0, reinterpret_cast<uintptr_t>(moskva_timed));
        set(province(1), 0x2F8, reinterpret_cast<uintptr_t>(moskva_timed + 1));

        set(session, 0x1220, reinterpret_cast<uintptr_t>(selection_object));
        set(selection_object, 0x10, reinterpret_cast<uintptr_t>(province_view));
        set(province_view, 0x958, reinterpret_cast<uintptr_t>(province(1)));
        set(province_view, 0xA0, reinterpret_cast<uintptr_t>(view_window));
        view_window[0x30 + 0xC5] = 1;
    }

    // Pending events (1.37.5 on Steam): four entries in the root's list. 5 and 8
    // wait for RUS (its own scope, then TUR's scope with RUS as the override), 6
    // for Moskva's owner (a province scope without a country), 7 for TUR,
    // hidden. Slot 4 is free for a test.
    alignas(8) uint8_t event_a[0xA68] = {};
    alignas(8) uint8_t event_b[0xA68] = {};
    uintptr_t options_a[3] = {1, 2, 3};
    uintptr_t options_b[1] = {1};
    alignas(8) uint8_t scopes[5][0xD0] = {};
    alignas(8) uint8_t entries[5][0x28] = {};
    std::string long_event_key = "an_event_key_long_enough_for_the_heap.1";
    // Statistics: income for RUS (two years) and TUR, an empty score series.
    alignas(8) uint8_t series_heads[4][0x10] = {};
    alignas(8) uint8_t series_nodes[2][0x90] = {};
    int32_t rus_income[2] = {0, 1584};
    int32_t rus_years[2] = {1717, 1718};
    int32_t tur_income[1] = {2210};
    int32_t tur_years[1] = {1718};

    void set_entry(int slot, int32_t id, uint8_t* event, uint8_t* scope, int32_t auto_select) {
        set(entries[slot], 0x10, reinterpret_cast<uintptr_t>(event));
        set(entries[slot], 0x18, reinterpret_cast<uintptr_t>(scope));
        set(entries[slot], 0x20, auto_select);
        set(entries[slot], 0x24, id);
    }
    void install_events(uint8_t* root) {
        set_key(event_a + 0x20, "flavor_rus.1");
        set_key(event_a + 0x7E0, "flavor_rus.1.t");
        set(event_a, 0x790, reinterpret_cast<uintptr_t>(options_a));
        set(event_a, 0x798, reinterpret_cast<uintptr_t>(options_a + 3));
        set_key(event_b + 0x20, long_event_key);
        set_key(event_b + 0x7E0, "EVTNAME732");
        set(event_b, 0x790, reinterpret_cast<uintptr_t>(options_b));
        set(event_b, 0x798, reinterpret_cast<uintptr_t>(options_b + 1));
        event_b[0xA61] = 1;
        set(scopes[0], 0x2C, game_tag("RUS", 0));
        set(scopes[1], 0x28, int32_t{1});  // a province event: Moskva, no country
        set(scopes[2], 0x2C, game_tag("TUR", 1));
        set(scopes[3], 0x2C, game_tag("TUR", 1));
        set(scopes[3], 0x44, game_tag("RUS", 0));
        scopes[3][0x4B] = 1;
        set(scopes[4], 0x2C, game_tag("TUR", 1));
        set_entry(0, 5, event_a, scopes[0], hours_of(1719, 7, 17));
        set_entry(1, 6, event_a, scopes[1], hours_of(1719, 7, 18));
        set_entry(2, 7, event_b, scopes[2], hours_of(1719, 3, 17));
        set_entry(3, 8, event_a, scopes[3], hours_of(1719, 7, 19));
        set(root, 0x1EA0, reinterpret_cast<uintptr_t>(entries));
        set(root, 0x1EAC, int32_t{4});

        for (int i = 0; i < 4; ++i) {
            set(series_heads[i], 0, reinterpret_cast<uintptr_t>(series_heads[i]));  // empty
            set(root, 0x1ED8 + i * 0x88 + 0x18, reinterpret_cast<uintptr_t>(series_heads[i]));
            set(root, 0x1ED8 + i * 0x88 + 0x20, uint64_t{0});
        }
        set(series_heads[0], 0, reinterpret_cast<uintptr_t>(series_nodes[0]));
        set(series_nodes[0], 0, reinterpret_cast<uintptr_t>(series_nodes[1]));
        set(series_nodes[1], 0, reinterpret_cast<uintptr_t>(series_heads[0]));
        set_key(series_nodes[0] + 0x10, "TUR");
        set(series_nodes[0], 0x60, reinterpret_cast<uintptr_t>(tur_income));
        set(series_nodes[0], 0x68, reinterpret_cast<uintptr_t>(tur_income + 1));
        set(series_nodes[0], 0x78, reinterpret_cast<uintptr_t>(tur_years));
        set(series_nodes[0], 0x80, reinterpret_cast<uintptr_t>(tur_years + 1));
        set_key(series_nodes[1] + 0x10, "RUS");
        set(series_nodes[1], 0x60, reinterpret_cast<uintptr_t>(rus_income));
        set(series_nodes[1], 0x68, reinterpret_cast<uintptr_t>(rus_income + 2));
        set(series_nodes[1], 0x78, reinterpret_cast<uintptr_t>(rus_years));
        set(series_nodes[1], 0x80, reinterpret_cast<uintptr_t>(rus_years + 2));
        set(root, 0x1ED8 + 0x20, uint64_t{2});
    }

    // Diplomacy (1.22.0, 1.37.5 on Steam): RUS and TUR are allied since
    // 1700.1.1, RUS guarantees TUR, has military access to it and holds it as a
    // vassal; TUR warns RUS until 1720.1.1. One relation object sits in both
    // countries' lists, keyed by the other's index. RUS's status for TUR and the
    // opinion caches (RUS of TUR -108, TUR of RUS -200) as in the Russia 1719 save.
    alignas(8) uint8_t relation[5][0x48] = {};  // alliance, guarantee, access, dependency, warning
    alignas(8) uint8_t vassal[0x30] = {};
    alignas(8) uint8_t rus_entries[5][0x10] = {};
    alignas(8) uint8_t tur_entries[5][0x10] = {};
    alignas(8) uint8_t rus_statuses[3][0x48] = {};
    alignas(8) uint8_t tur_statuses[2][0x48] = {};
    alignas(8) uint8_t rival_node[0x38] = {};
    uint64_t rus_subjects[1] = {game_tag("TUR", 1)};
    int16_t rus_opinions[4] = {0, -108, 0, 0};  // the cache holds three
    int16_t tur_opinions[4] = {-200, 0, 0, 0};  // and two

    static void set_relation_entry(uint8_t* entry, int32_t index, uint8_t* object) {
        set(entry, 0, index);
        set(entry, 8, reinterpret_cast<uintptr_t>(object));
    }
    static void set_relations(uint8_t* country, uint32_t data, uint8_t* entries, int32_t count) {
        set(country, 0x14F8 + data, reinterpret_cast<uintptr_t>(entries));
        set(country, 0x14F8 + data + 0xC, count);
    }
    void install_diplomacy() {
        set(tur, 0x1338, reinterpret_cast<uintptr_t>(owned + 1));
        set(tur, 0x1344, 1);  // TUR owns a province now
        const uint64_t first[5] = {game_tag("RUS", 0), game_tag("RUS", 0), game_tag("RUS", 0), game_tag("RUS", 0),
                                   game_tag("TUR", 1)};
        const uint64_t second[5] = {game_tag("TUR", 1), game_tag("TUR", 1), game_tag("TUR", 1), game_tag("TUR", 1),
                                    game_tag("RUS", 0)};
        for (int i = 0; i < 5; ++i) {
            set(relation[i], 0x10, first[i]);
            set(relation[i], 0x18, second[i]);
            set(relation[i], 0x20, kUnsetHours);
            set(relation[i], 0x24, kUnsetHours);
            set_relation_entry(rus_entries[i], 1, relation[i]);
            set_relation_entry(tur_entries[i], 0, relation[i]);
        }
        set(relation[0], 0x20, hours_of(1700, 1, 1));
        set(relation[4], 0x24, hours_of(1720, 1, 1));
        set_key(vassal + 0x10, "vassal");
        set(relation[3], 0x40, reinterpret_cast<uintptr_t>(vassal));
        set_relations(rus, 0x80, rus_entries[0], 1);   // alliance
        set_relations(rus, 0x1A0, rus_entries[1], 1);  // guarantee
        set_relations(rus, 0xB0, rus_entries[2], 1);   // military access: RUS is first
        set_relations(rus, 0x128, rus_entries[3], 1);  // dependency
        set_relations(rus, 0x1B8, rus_entries[4], 1);  // warning
        set_relations(tur, 0x80, tur_entries[0], 1);
        set_relations(tur, 0x1A0, tur_entries[1], 1);
        set_relations(tur, 0xC8, tur_entries[2], 1);   // giving military access: TUR is second
        set_relations(tur, 0x128, tur_entries[3], 1);
        set_relations(tur, 0x1B8, tur_entries[4], 1);

        const uint64_t rus_targets[3] = {game_tag("RUS", 0), game_tag("TUR", 1), game_tag("O00", 2, 7)};
        for (int i = 0; i < 3; ++i) {
            set(rus_statuses[i], 0, reinterpret_cast<uintptr_t>(rus + 0x14F8));
            set(rus_statuses[i], 0x10, rus_targets[i]);
            rus_statuses[i][0x1E] = 50;
        }
        set(rus_statuses[1], 0x1A, int16_t{-399});
        rus_statuses[1][0x1E] = 99;
        set(rus_statuses[1], 0x20, int32_t{37590});
        rus_statuses[1][0x45] = 0x3;  // the truce bit and another
        for (int i = 0; i < 2; ++i) {
            set(tur_statuses[i], 0, reinterpret_cast<uintptr_t>(tur + 0x14F8));
            set(tur_statuses[i], 0x10, rus_targets[i]);
            tur_statuses[i][0x1E] = 50;
            tur_statuses[i][0x45] = 0x2;  // not the truce bit
        }
        set(rus, 0x1510, reinterpret_cast<uintptr_t>(rus_statuses));
        set(rus, 0x1518, reinterpret_cast<uintptr_t>(rus_statuses + 3));
        set(tur, 0x1510, reinterpret_cast<uintptr_t>(tur_statuses));
        set(tur, 0x1518, reinterpret_cast<uintptr_t>(tur_statuses + 2));
        set(rus, 0x2380, reinterpret_cast<uintptr_t>(rus_opinions));
        set(rus, 0x2388, reinterpret_cast<uintptr_t>(rus_opinions + 3));
        set(tur, 0x2380, reinterpret_cast<uintptr_t>(tur_opinions));
        set(tur, 0x2388, reinterpret_cast<uintptr_t>(tur_opinions + 2));

        set(rival_node, 0x10, game_tag("TUR", 1));
        set(rus, 0x2248, reinterpret_cast<uintptr_t>(rival_node));
        set(tur, 0x18F0, game_tag("RUS", 0));
        set(rus, 0x18F0, game_tag("---", 0, 0, 0));
        set(rus, 0x1A18, reinterpret_cast<uintptr_t>(rus_subjects));
        set(rus, 0x1A20, reinterpret_cast<uintptr_t>(rus_subjects + 1));
    }

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
    static void set_key(uint8_t* at, const std::string& key) {
        if (key.size() < 16) {
            std::memcpy(at, key.data(), key.size());
            set(at, 0x18, uint64_t{15});
        } else {
            set(at, 0, reinterpret_cast<uintptr_t>(key.data()));
            set(at, 0x18, uint64_t{key.size()});
        }
        set(at, 0x10, uint64_t{key.size()});
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
        // Three modifier definitions, and values for fake_modifier_value.
        set_key(definitions[0], "land_morale");
        set_key(definitions[1], tax_key);
        set_key(definitions[2], long_key);
        image.commit(kDefinitions & ~0xFFFu, PAGE_READWRITE);
        image.put_pointer(kDefinitions, reinterpret_cast<uintptr_t>(definitions));
        image.put_pointer(kDefinitions + 8, reinterpret_cast<uintptr_t>(definitions + 3));
        set(rus, 0x28C8, int32_t{2});
        set(rus, 0x28D0 + 0x20, int32_t{750});
        set(rus, 0x28D0 + 0x24, int32_t{-250});
        set(province(1), 0x998 + 0x24, int32_t{100});
        // The value function: its declared bytes are in place (the symbols test
        // copied them), then add rsp,20h; pop rbx; mov rax, fake_modifier_value; jmp rax.
        image.put(kModifierValue + 16, {0x48, 0x83, 0xC4, 0x20, 0x5B, 0x48, 0xB8});
        image.put_pointer(kModifierValue + 23, reinterpret_cast<uintptr_t>(&fake_modifier_value));
        image.put(kModifierValue + 31, {0xFF, 0xE0});
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

// "added 5 RUS", "removed 8 RUS": the pending changes a plugin hears.
void listen_pending(const char*, const void* payload, void* context) {
    const auto* c = static_cast<const AnvilGamePendingChange*>(payload);
    static_cast<std::vector<std::string>*>(context)->push_back(
        std::string(c->change == ANVIL_GAME_PENDING_ADDED ? "added " : "removed ") + std::to_string(c->pending_id) + " " +
        c->country.text);
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
    } else if (name == ANVIL_GAME_EVENT_OOS) {
        const auto* o = static_cast<const AnvilGameOosChange*>(payload);
        h->events.push_back("oos " + std::to_string(o->previous) + " to " + std::to_string(o->flags) + " in " +
                            std::to_string(o->session));
    }
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// The game's side of anvil-game.commands for its rules alone: units by id, and
// what was made, handed to the session and freed, in order.
struct FakeCommands final : anvil_game::CommandBackend {
    std::map<uint64_t, anvil_game::CommandUnit> units;  // RUS's
    bool affects = true, suspended = false, is_open = true, hot = false, take = true;
    uint32_t absent = 0;
    int memory = -1;  // allocations left; -1: no end
    uint64_t frame_number = 0;
    std::vector<std::string> made;  // "drill 1001", "split 1001 2", "merge 1001 1002"
    std::vector<int> submitted, destroyed;  // indices into made

    bool caller(const AnvilPlugin* self, bool& a, bool& s) override {
        if (self != reinterpret_cast<const AnvilPlugin*>(this)) return false;
        a = affects;
        s = suspended;
        return true;
    }
    bool declared(uint32_t type) override { return type != absent; }
    bool open() override { return is_open; }
    bool hot_join() override { return hot; }
    uint64_t frame() override { return frame_number; }
    AnvilResult find(const AnvilGameUnitRef& ref, anvil_game::CommandUnit& out) override {
        if (std::strcmp(ref.country.text, "RUS") != 0) return ANVIL_ERROR_NOT_FOUND;
        const auto it = units.find(ref.id);
        if (it == units.end()) return ANVIL_ERROR_NOT_FOUND;
        out = it->second;
        return ANVIL_OK;
    }
    void* make(std::string what) {
        if (memory == 0) return nullptr;
        if (memory > 0) --memory;
        made.push_back(std::move(what));
        return reinterpret_cast<void*>(static_cast<uintptr_t>(made.size()));  // index + 1
    }
    static std::string hex(uintptr_t v) {
        char text[20];
        std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(v));
        return text;
    }
    void* make_drill(const anvil_game::CommandUnit& u) override { return make("drill " + hex(u.object)); }
    void* make_split(const anvil_game::CommandUnit& u, int32_t count) override {
        return make("split " + hex(u.object) + " " + std::to_string(count));
    }
    void* make_merge(const std::vector<anvil_game::CommandUnit>& list) override {
        std::string what = "merge";
        for (const auto& u : list) what += " " + hex(u.object);
        return make(what);
    }
    bool submit(void* command) override {
        if (!take) return false;
        submitted.push_back(static_cast<int>(reinterpret_cast<uintptr_t>(command)) - 1);
        return true;
    }
    void destroy(void* command) override { destroyed.push_back(static_cast<int>(reinterpret_cast<uintptr_t>(command)) - 1); }

    // Event windows by pending id: whether one waits, and its buttons by option (enabled or not).
    std::map<int32_t, bool> windows;
    std::map<int32_t, std::map<int32_t, bool>> buttons;
    bool post_ok = true;
    std::vector<std::string> chosen;  // "<pending> <option>"
    AnvilResult find_event_window(int32_t pending, int32_t option, anvil_game::EventWindow& out) override {
        const auto w = windows.find(pending);
        if (w == windows.end()) return ANVIL_ERROR_NOT_FOUND;
        out = {};
        out.object = 0x5000 + static_cast<uintptr_t>(pending);
        out.keep_alive = w->second;
        const auto b = buttons[pending].find(option);
        out.has_button = b != buttons[pending].end();
        out.enabled = out.has_button && b->second;
        return ANVIL_OK;
    }
    bool post_allowed() override { return post_ok; }
    void choose_option(const anvil_game::EventWindow& w, int32_t option) override {
        chosen.push_back(std::to_string(w.object - 0x5000) + " " + std::to_string(option));
        windows[static_cast<int32_t>(w.object - 0x5000)] = false;  // OptionSelected clears _KeepAlive
    }
};

// The game's side of anvil-game.commands in FakeEu4: its functions are
// overwritten with jumps here once the symbols verified. A command is 0x80
// bytes of the test's heap; slot 0 of its vtable frees it.
struct GameCommandsLog {
    std::vector<std::string> calls;  // "new 68", "drill <unit> 36", "split <unit> 2", "merge <units> (n)", "post <session>", "free 1"
    std::vector<void*> posted;
    bool allowed = true;  // what the stand-in of CSession::Post answers
};
GameCommandsLog g_game_commands;
std::string pointer_text(const void* p) {
    char text[24];
    std::snprintf(text, sizeof text, "%p", p);
    return text;
}
void* fake_command_free(void* command, unsigned flags) {
    g_game_commands.calls.push_back("free " + std::to_string(flags));
    std::free(command);
    return command;
}
uintptr_t g_command_vtable[1] = {reinterpret_cast<uintptr_t>(&fake_command_free)};
void* fake_command_new(size_t size) {
    char text[16];
    std::snprintf(text, sizeof text, "new %zx", size);
    g_game_commands.calls.push_back(text);
    void* p = std::calloc(1, 0x80);
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(g_command_vtable);
    std::memcpy(p, &vtable, sizeof vtable);
    return p;
}
void* fake_drill_command(void* command, const void* unit, uint8_t reason) {
    char text[64];
    std::snprintf(text, sizeof text, "drill %s %x", pointer_text(unit).c_str(), reason);
    g_game_commands.calls.push_back(text);
    return command;
}
void* fake_split_command(void* command, const void* unit, int32_t count) {
    g_game_commands.calls.push_back("split " + pointer_text(unit) + " " + std::to_string(count));
    return command;
}
// It walks the list as CMergeCommand's constructor does: head, count, a node's unit and next.
void* fake_merge_command(void* command, const void* list) {
    std::string text = "merge";
    const auto* bytes = static_cast<const uint8_t*>(list);
    int32_t count = 0;
    std::memcpy(&count, bytes + 0x10, sizeof count);
    uintptr_t node = 0;
    std::memcpy(&node, bytes, sizeof node);
    for (int i = 0; i < count && node != 0; ++i) {
        uintptr_t unit = 0;
        std::memcpy(&unit, reinterpret_cast<const void*>(node), sizeof unit);
        text += " " + pointer_text(reinterpret_cast<const void*>(unit));
        std::memcpy(&node, reinterpret_cast<const void*>(node + 0x10), sizeof node);
    }
    g_game_commands.calls.push_back(text + " (" + std::to_string(count) + ")");
    return command;
}
bool fake_command_post(void* session, void* command) {
    g_game_commands.calls.push_back("post " + pointer_text(session));
    if (!g_game_commands.allowed) return false;
    g_game_commands.posted.push_back(command);
    return true;
}

// CEventWindow::OptionSelected in FakeEu4: what the button does, as far as the service sees it.
void fake_option_selected(void* window, int32_t option) {
    g_game_commands.calls.push_back("option " + pointer_text(window) + " " + std::to_string(option));
    static_cast<uint8_t*>(window)[0x124] = 0;
}

// anvil.control's plugin_of, for one gameplay plugin.
AnvilPlugin* g_command_caller = nullptr;
int g_command_caller_suspended = 0;
int fake_plugin_of(const AnvilPlugin* plugin, AnvilControlPlugin* out) {
    if (plugin == nullptr || plugin != g_command_caller) return 0;
    out->affects_gameplay = 1;
    out->suspended = g_command_caller_suspended;
    return 1;
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

    run("anvil-game: the pending tracker", [] {
        using anvil_game::PendingKey;
        anvil_game::PendingTracker t;
        const PendingKey a{1, 0x100, 0x200}, b{2, 0x100, 0x300}, ai{3, 0x400, 0x500};
        const AnvilGameTag rus{"RUS"}, tur{"TUR"};
        CHECK(!t.counts(a));
        // A session's first list counts whole, and the player's first call is a baseline.
        t.see(1, {a});
        CHECK(t.counts(a));
        CHECK(t.player(1, rus, {a}).added.empty());
        // A new entry counts at the next frame; an AI's that leaves at once never.
        t.see(1, {a, b, ai});
        CHECK(t.counts(a) && !t.counts(b) && !t.counts(ai));
        CHECK(t.player(1, rus, {a}).added.empty());
        t.see(1, {a, b});
        CHECK(t.counts(b) && !t.counts(ai));
        auto c = t.player(1, rus, {a, b});
        CHECK(c.added.size() == 1 && c.added[0] == b && c.removed.empty());
        // An id given again to another entry is a new entry.
        const PendingKey b2{2, 0x100, 0x600};
        t.see(1, {a, b2});
        CHECK(!t.counts(b2));
        c = t.player(1, rus, {a});
        CHECK(c.removed.size() == 1 && c.removed[0] == b && c.added.empty());
        // Another country of the player, or another session: a new baseline.
        CHECK(t.player(1, tur, {}).removed.empty());
        t.see(2, {b2});
        CHECK(t.counts(b2) && !t.counts(a));
        CHECK(t.player(2, tur, {b2}).added.empty());
        t.forget();
        CHECK(!t.counts(b2));
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
        anvil_game::OosTracker oos;
        CHECK(!oos.update(false, 0, 3).changed && oos.flags() == 0);  // no session
        CHECK(!oos.update(true, 1, 0).changed);
        const auto detected = oos.update(true, 1, ANVIL_GAME_OOS_DETECTED);
        CHECK(detected.changed && detected.event.size == sizeof(AnvilGameOosChange) &&
              detected.event.flags == ANVIL_GAME_OOS_DETECTED && detected.event.previous == 0 && detected.event.session == 1);
        CHECK(!oos.update(true, 1, ANVIL_GAME_OOS_DETECTED).changed);  // once per change
        const auto next = oos.update(true, 2, ANVIL_GAME_OOS_DETECTED);  // a new session starts from 0
        CHECK(next.changed && next.event.previous == 0 && next.event.session == 2);
        CHECK(!oos.update(true, 2, ANVIL_GAME_OOS_DETECTED).changed);
        CHECK(!oos.update(false, 0, 0).changed && oos.flags() == 0);  // its end says nothing

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

    run("anvil-game.commands: the rules, with a fake game", [] {
        FakeCommands game;
        anvil_game::CommandPoster poster;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(&game);
        const AnvilGameUnitRef army = {1, {"RUS"}, ANVIL_GAME_UNIT_ARMY};    // drilling
        const AnvilGameUnitRef army2 = {2, {"RUS"}, ANVIL_GAME_UNIT_ARMY};   // idle, with Moskva's 1st Army
        const AnvilGameUnitRef navy = {3, {"RUS"}, ANVIL_GAME_UNIT_NAVY};
        const AnvilGameUnitRef foreign = {9, {"TUR"}, ANVIL_GAME_UNIT_ARMY};
        const AnvilGameUnitRef gone = {7, {"RUS"}, ANVIL_GAME_UNIT_ARMY};
        game.units[1] = {0x1001, 1, ANVIL_GAME_UNIT_ARMY, 1, true, 3};
        game.units[2] = {0x1002, 2, ANVIL_GAME_UNIT_ARMY, 1, false, 1};
        game.units[3] = {0x1003, 3, ANVIL_GAME_UNIT_NAVY, 5, false, 4};
        const auto command = [](uint32_t type, uint32_t flags, std::vector<AnvilGameUnitRef>& refs) {
            AnvilGameCommand c{};
            c.size = sizeof(c);
            c.type = type;
            c.flags = flags;
            c.count = static_cast<uint32_t>(refs.size());
            c.units = refs.data();
            return c;
        };
        AnvilGameCommandResult out{};
        out.size = sizeof(out);

        // Start over a mixed selection: only the idle army; stop: only the drilling one.
        std::vector<AnvilGameUnitRef> both = {army, army2};
        AnvilGameCommand c = command(ANVIL_GAME_COMMAND_DRILL, ANVIL_GAME_COMMAND_START, both);
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 1 && out.skipped == 1 && out.refused == 0 &&
              out.size == sizeof(out));
        CHECK((game.made == std::vector<std::string>{"drill 1002"}) && (game.submitted == std::vector<int>{0}));
        c.flags = 0;
        CHECK(poster.post(game, self, &c, nullptr) == ANVIL_OK && game.made.back() == "drill 1001");
        // A caller built against a shorter result gets only that much.
        AnvilGameCommandResult shorter{};
        shorter.size = offsetof(AnvilGameCommandResult, skipped);
        shorter.skipped = 77;
        CHECK(poster.post(game, self, &c, &shorter) == ANVIL_OK && shorter.posted == 1 && shorter.skipped == 77);

        // Split: half, rounded up; merge: the units in one province, into the first.
        std::vector<AnvilGameUnitRef> one = {army};
        c = command(ANVIL_GAME_COMMAND_SPLIT, 0, one);
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 1 && game.made.back() == "split 1001 2");
        CHECK(anvil_game::split_count(2) == 1 && anvil_game::split_count(3) == 2 && anvil_game::split_count(40) == 20);
        c = command(ANVIL_GAME_COMMAND_MERGE, 0, both);
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 1 && game.made.back() == "merge 1001 1002");

        // Nothing posted for a bad call.
        const size_t made = game.made.size();
        const auto refused = [&](AnvilGameCommand call, AnvilResult expected) {
            return poster.post(game, self, &call, &out) == expected;
        };
        std::vector<AnvilGameUnitRef> list = {army, foreign};
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, list), ANVIL_ERROR_NOT_FOUND));
        list = {army, gone};
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, list), ANVIL_ERROR_NOT_FOUND));
        list = {navy};
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, ANVIL_GAME_COMMAND_START, list), ANVIL_ERROR_INVALID_ARGUMENT));
        list = {{1, {"RUS"}, ANVIL_GAME_UNIT_NAVY}};  // the army, called a navy
        CHECK(refused(command(ANVIL_GAME_COMMAND_SPLIT, 0, list), ANVIL_ERROR_INVALID_ARGUMENT));
        list = {army, army};
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, list), ANVIL_ERROR_INVALID_ARGUMENT));
        list = {army2};  // one regiment
        CHECK(refused(command(ANVIL_GAME_COMMAND_SPLIT, 0, list), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(command(ANVIL_GAME_COMMAND_SPLIT, 0, both), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(command(ANVIL_GAME_COMMAND_MERGE, 0, one), ANVIL_ERROR_INVALID_ARGUMENT));
        list = {army, navy};
        CHECK(refused(command(ANVIL_GAME_COMMAND_MERGE, 0, list), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(command(ANVIL_GAME_COMMAND_MERGE, ANVIL_GAME_COMMAND_START, both), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0x2, both), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(command(4, 0, both), ANVIL_ERROR_INVALID_ARGUMENT));
        list.clear();
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, list), ANVIL_ERROR_INVALID_ARGUMENT));
        AnvilGameCommand small = command(ANVIL_GAME_COMMAND_DRILL, 0, both);
        small.size = offsetof(AnvilGameCommand, units);
        CHECK(refused(small, ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(poster.post(game, self, nullptr, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilGameCommandResult no_size{};
        c = command(ANVIL_GAME_COMMAND_DRILL, 0, both);
        CHECK(poster.post(game, self, &c, &no_size) == ANVIL_ERROR_INVALID_ARGUMENT);
        std::vector<AnvilGameUnitRef> many(65, army);
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, many), ANVIL_ERROR_LIMIT));
        CHECK(game.made.size() == made);

        // The caller: not a gameplay plugin, no plugin at all, suspended.
        game.affects = false;
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, both), ANVIL_ERROR_UNSUPPORTED));
        game.affects = true;
        CHECK(poster.post(game, nullptr, &c, &out) == ANVIL_ERROR_UNSUPPORTED);
        game.suspended = true;
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, both), ANVIL_ERROR_SUSPENDED));
        game.suspended = false;
        // The moment: off the main thread or between sessions, in a hot-join.
        game.is_open = false;
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, both), ANVIL_ERROR_WRONG_PHASE));
        game.is_open = true;
        game.hot = true;
        CHECK(refused(command(ANVIL_GAME_COMMAND_DRILL, 0, both), ANVIL_ERROR_WRONG_PHASE));
        game.hot = false;
        // A type this build lacks; the others still work.
        game.absent = ANVIL_GAME_COMMAND_SPLIT;
        CHECK(refused(command(ANVIL_GAME_COMMAND_SPLIT, 0, one), ANVIL_ERROR_NOT_DECLARED));
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 1);
        game.absent = 0;
        CHECK(game.made.size() == made + 1);

        // A refused post is freed, once, and counted.
        game.take = false;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 0 && out.refused == 1 &&
              game.destroyed.size() == 1 && game.destroyed[0] == static_cast<int>(game.made.size()) - 1);
        game.take = true;
        // No memory: what was posted before stays posted, and is told.
        game.memory = 1;
        list = {army, {4, {"RUS"}, ANVIL_GAME_UNIT_ARMY}};
        game.units[4] = {0x1004, 4, ANVIL_GAME_UNIT_ARMY, 1, true, 2};
        AnvilGameCommand two = command(ANVIL_GAME_COMMAND_DRILL, 0, list);
        CHECK(poster.post(game, self, &two, &out) == ANVIL_ERROR_OUT_OF_MEMORY && out.posted == 1);
        game.memory = -1;

        // 256 commands a frame for every caller together, then the next frame.
        std::vector<AnvilGameUnitRef> drilling;
        for (uint64_t id = 100; id < 164; ++id) {
            game.units[id] = {0x2000 + id, id, ANVIL_GAME_UNIT_ARMY, 1, true, 2};
            drilling.push_back({id, {"RUS"}, ANVIL_GAME_UNIT_ARMY});
        }
        game.frame_number = 50;
        c = command(ANVIL_GAME_COMMAND_DRILL, 0, drilling);
        for (int i = 0; i < 4; ++i) CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 64);
        const size_t full = game.made.size();
        CHECK(refused(command(ANVIL_GAME_COMMAND_SPLIT, 0, one), ANVIL_ERROR_LIMIT) && game.made.size() == full);
        game.frame_number = 51;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 64);
    });

    run("anvil-game.commands: choosing an event option, with a fake game", [] {
        FakeCommands game;
        anvil_game::CommandPoster poster;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(&game);
        game.windows[41] = true;
        game.buttons[41] = {{0, true}, {2, false}};  // option 1 has no button: its trigger is false
        AnvilGameCommand c{};
        c.size = sizeof(c);
        c.type = ANVIL_GAME_COMMAND_EVENT_OPTION;
        c.pending_event_id = 41;
        AnvilGameCommandResult out{};
        out.size = sizeof(out);
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 1 && out.skipped == 0 && out.refused == 0);
        CHECK((game.chosen == std::vector<std::string>{"41 0"}));
        // The same frame again: the window is there, already chosen.
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.posted == 0 && out.skipped == 1 && game.chosen.size() == 1);
        game.windows[41] = true;
        c.option_index = 1;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_INVALID_ARGUMENT);  // no button
        c.option_index = 2;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_WRONG_PHASE);  // disabled
        c.option_index = -1;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
        c.option_index = 0;
        c.pending_event_id = 42;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_NOT_FOUND);
        c.pending_event_id = 41;
        // The post's conditions fail: refused, the window left alone.
        game.post_ok = false;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_OK && out.refused == 1 && out.posted == 0 && game.chosen.size() == 1 &&
              game.windows[41]);
        game.post_ok = true;
        // Bad calls: units, flags, a version 1 size; the moment and the build.
        AnvilGameUnitRef unit = {1, {"RUS"}, ANVIL_GAME_UNIT_ARMY};
        AnvilGameCommand bad = c;
        bad.count = 1;
        bad.units = &unit;
        CHECK(poster.post(game, self, &bad, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
        bad = c;
        bad.flags = ANVIL_GAME_COMMAND_START;
        CHECK(poster.post(game, self, &bad, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
        bad = c;
        bad.size = offsetof(AnvilGameCommand, pending_event_id);
        CHECK(poster.post(game, self, &bad, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
        game.hot = true;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_WRONG_PHASE);
        game.hot = false;
        game.suspended = true;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_SUSPENDED);
        game.suspended = false;
        game.absent = ANVIL_GAME_COMMAND_EVENT_OPTION;
        CHECK(poster.post(game, self, &c, &out) == ANVIL_ERROR_NOT_DECLARED);
        game.absent = 0;
        CHECK(game.chosen.size() == 1);
        // A caller built against version 1 still posts its types.
        game.units[1] = {0x1001, 1, ANVIL_GAME_UNIT_ARMY, 1, true, 3};
        AnvilGameCommand v1{};
        v1.size = offsetof(AnvilGameCommand, pending_event_id);
        v1.type = ANVIL_GAME_COMMAND_DRILL;
        v1.count = 1;
        v1.units = &unit;
        CHECK(poster.post(game, self, &v1, &out) == ANVIL_OK && out.posted == 1);
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
    std::vector<std::string> pending_heard;
    bus.subscribe("listener", ANVIL_GAME_EVENT_PENDING, listen_pending, &pending_heard);
    for (const char* event : {ANVIL_GAME_EVENT_READY, ANVIL_GAME_EVENT_FRAME, ANVIL_GAME_EVENT_SESSION,
                              ANVIL_GAME_EVENT_MAIN_MENU, ANVIL_GAME_EVENT_OOS}) {
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
            if (!world && !s.name.starts_with("gui_")) continue;
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
        CHECK(reason.empty() && verified.symbols.size() == 149);
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
                            "anvil-game.year, anvil-game.player, anvil-game.war, anvil-game.pending_event"));
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
        CHECK(contains(log, "anvil: game session started (ironman): 1 gameplay plugin(s) or module(s) suspended, "
                            "0 allowed by the player"));
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

    run("anvil-game: out of sync from the handler and the game-speed object", [&] {
        CHECK(ANVIL_GAME_HAS(service, out_of_sync));
        const auto oos_events = [&] {
            return std::count_if(heard.events.begin(), heard.events.end(), [](const std::string& e) { return e.starts_with("oos"); });
        };
        const uintptr_t root = reinterpret_cast<uintptr_t>(game.root);
        std::memcpy(image.base + 0x233FE78, &root, sizeof(root));
        game.set_session(game.session);
        game.set_date(58'860'240);
        frame(game.app, 1);
        AnvilGameSession s{};
        CHECK(service->session(&s) == 1);
        const std::string number = std::to_string(s.number);
        heard.events.clear();

        // The session has no handler yet, then one in sync.
        CHECK(service->out_of_sync() == 0);
        const uintptr_t handler = reinterpret_cast<uintptr_t>(game.oos_handler);
        std::memcpy(game.session + 0x4D8, &handler, sizeof(handler));
        frame(game.app, 1);
        CHECK(service->out_of_sync() == 0 && oos_events() == 0);

        game.oos_handler[8] = 1;
        frame(game.app, 1);
        CHECK(service->out_of_sync() == ANVIL_GAME_OOS_DETECTED);
        CHECK(contains(heard.events, "oos 0 to 1 in " + number) && oos_events() == 1);
        CHECK(contains(log, "anvil-game: out of sync: detected true, paused false (session " + number + ")"));
        frame(game.app, 1);
        CHECK(oos_events() == 1);

        game.speed[0x19] = 1;
        frame(game.app, 1);
        CHECK(service->out_of_sync() == (ANVIL_GAME_OOS_DETECTED | ANVIL_GAME_OOS_PAUSED));
        CHECK(contains(heard.events, "oos 1 to 3 in " + number));

        // The session ends with the flags up: the service says 0 and no event comes.
        heard.events.clear();
        alignas(16) static uint8_t other_session[0x4E0] = {};
        game.set_session(other_session);
        frame(game.app, 1);
        CHECK(service->out_of_sync() == 0 && service->session(nullptr) == 0 && oos_events() == 0);

        game.oos_handler[8] = 0;
        game.speed[0x19] = 0;
        std::memset(game.session + 0x4D8, 0, sizeof(handler));
        game.set_session(game.session);
        frame(game.app, 1);
        CHECK(service->session(nullptr) == 1 && service->out_of_sync() == 0);
    });

    run("anvil-game: the GUI's keyboard from its text receiver and the shortcut editor", [&] {
        CHECK(ANVIL_GAME_HAS(service, keyboard));
        if (!ANVIL_GAME_HAS(service, keyboard)) return;
        // CGui's global, from the bytes of gui_text_receiver on Steam.
        constexpr uint32_t kGui = 0x23494F0;
        image.commit(kGui & ~0xFFFu, PAGE_READWRITE);
        uint32_t flags = 0xFF;
        image.put_pointer(kGui, 0);
        CHECK(service->keyboard(&flags) == 1 && flags == 0);
        alignas(16) static uint8_t gui[0x278] = {};
        image.put_pointer(kGui, reinterpret_cast<uintptr_t>(gui));
        CHECK(service->keyboard(&flags) == 1 && flags == 0 && service->keyboard(nullptr) == 1);
        const uintptr_t receiver = reinterpret_cast<uintptr_t>(gui + 0x100);
        std::memcpy(gui + 0x210, &receiver, sizeof(receiver));
        CHECK(service->keyboard(&flags) == 1 && flags == ANVIL_GAME_KEYBOARD_TEXT);
        // The shortcut editor on, but with no listener yet, takes nothing.
        gui[0x230] = 1;
        CHECK(service->keyboard(&flags) == 1 && flags == ANVIL_GAME_KEYBOARD_TEXT);
        const uint64_t listeners = 1;
        std::memcpy(gui + 0x248, &listeners, sizeof(listeners));
        CHECK(service->keyboard(&flags) == 1 && flags == (ANVIL_GAME_KEYBOARD_TEXT | ANVIL_GAME_KEYBOARD_SHORTCUT));
        std::memset(gui + 0x210, 0, sizeof(receiver));
        gui[0x230] = 0;
        CHECK(service->keyboard(&flags) == 1 && flags == 0);
        // A pointer to nowhere is caught.
        image.put_pointer(kGui, 0x10);
        CHECK(service->keyboard(&flags) == 1 && flags == 0);
        image.put_pointer(kGui, 0);
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
              ANVIL_GAME_WORLD_HAS(world, country_provinces) && ANVIL_GAME_WORLD_HAS(world, war_side) &&
              ANVIL_GAME_WORLD_HAS(world, province_modifier));
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
        std::int32_t outside = 0;
        CHECK(world->modifier_id("land_morale", &outside) == ANVIL_ERROR_WRONG_PHASE &&
              world->country_modifier("RUS", 0, &outside) == ANVIL_ERROR_WRONG_PHASE &&
              world->province_modifier(1, 0, &outside) == ANVIL_ERROR_WRONG_PHASE);
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

            std::int32_t morale = -1, local = -1, key_id = -1, value = 0;
            CHECK(world->modifier_id("land_morale", &morale) == ANVIL_OK && morale == 0);
            CHECK(world->modifier_id("local_tax_modifier", &local) == ANVIL_OK && local == 1);
            CHECK(world->modifier_id(fake.long_key.c_str(), &key_id) == ANVIL_OK && key_id == 2);
            CHECK(world->modifier_id("Land_Morale", &key_id) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->modifier_id("", &key_id) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->modifier_id(nullptr, &key_id) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->modifier_id("land_morale", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->country_modifier("rus", morale, &value) == ANVIL_OK && value == 750);
            CHECK(world->country_modifier("RUS", local, &value) == ANVIL_OK && value == -250);
            CHECK(world->province_modifier(1, local, &value) == ANVIL_OK && value == 100);
            CHECK(world->province_modifier(1, morale, &value) == ANVIL_OK && value == 0);
            CHECK(world->country_modifier("ZZZ", morale, &value) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->country_modifier("RUS", 3, &value) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->country_modifier("RUS", -1, &value) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->country_modifier(nullptr, morale, &value) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->country_modifier("RUS", morale, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->province_modifier(4, morale, &value) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->province_modifier(1, 3, &value) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->province_modifier(1, morale, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            // A fault in the game's function costs that answer, and so do
            // modifiers that are being worked out.
            CHECK(world->country_modifier("RUS", 2, &value) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rus, 0x28C8, int32_t{1});
            CHECK(world->country_modifier("RUS", morale, &value) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rus, 0x28C8, int32_t{2});
            CHECK(world->country_modifier("RUS", morale, &value) == ANVIL_OK && value == 750);
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
        std::int32_t no_modifier = 0;
        CHECK(anvil_game::read_modifier_id(l, "land_morale", no_modifier) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(anvil_game::read_country_modifier(l, "RUS", 0, no_modifier) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(anvil_game::read_province_modifier(l, 1, 0, no_modifier) == ANVIL_ERROR_NOT_DECLARED);
        AnvilGameCountry partial{};
        CHECK(anvil_game::read_country(l, "RUS", partial) == ANVIL_OK && partial.values == ANVIL_GAME_VALUE_TREASURY &&
              partial.treasury == 123456 && partial.stability == 0);
    });

    run("anvil-game.world: units and the selection", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && ANVIL_GAME_WORLD_HAS(world, selected_units));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        fake.install_units(game.session, image);
        game.set_session(game.session);
        frame(game.app, 1);

        uint64_t ids[4] = {};
        uint32_t count = 0;
        CHECK(world->country_units("RUS", ids, 4, &count) == ANVIL_ERROR_WRONG_PHASE);  // between frames
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(world->country_units("RUS", ids, 4, &count) == ANVIL_OK && count == 2 && ids[0] == FakeWorld::kArmyId &&
                  ids[1] == FakeWorld::kNavyId);
            CHECK(world->country_units("rus", nullptr, 0, &count) == ANVIL_OK && count == 2);
            CHECK(world->country_units("TUR", ids, 4, &count) == ANVIL_OK && count == 0);
            CHECK(world->country_units("XXX", ids, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->country_units(nullptr, ids, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT);

            AnvilGameUnit army{};
            army.size = sizeof(army);
            CHECK(world->unit("RUS", FakeWorld::kArmyId, &army) == ANVIL_OK && army.size == sizeof(army) &&
                  army.kind == ANVIL_GAME_UNIT_ARMY && army.id == FakeWorld::kArmyId &&
                  std::strcmp(army.country.text, "RUS") == 0 && army.location == 1 &&
                  army.mission == ANVIL_GAME_MISSION_DRILL && army.subunits == 3 && army.strength == 2500 &&
                  army.leader == FakeWorld::kLeaderId);
            AnvilGameUnit navy{};
            navy.size = sizeof(navy);
            CHECK(world->unit("RUS", FakeWorld::kNavyId, &navy) == ANVIL_OK && navy.kind == ANVIL_GAME_UNIT_NAVY &&
                  navy.location == 3 && navy.mission == ANVIL_GAME_MISSION_NONE && navy.subunits == 0 &&
                  navy.strength == 0 && navy.leader == 0);
            // A caller built against a shorter struct gets only that much.
            AnvilGameUnit shorter{};
            shorter.size = offsetof(AnvilGameUnit, location);
            shorter.location = 77;
            CHECK(world->unit("RUS", FakeWorld::kArmyId, &shorter) == ANVIL_OK && shorter.kind == ANVIL_GAME_UNIT_ARMY &&
                  shorter.location == 77);
            CHECK(world->unit("TUR", FakeWorld::kArmyId, &army) == ANVIL_ERROR_NOT_FOUND &&
                  world->unit("RUS", 777, &army) == ANVIL_ERROR_NOT_FOUND &&
                  world->unit("RUS", FakeWorld::kArmyId, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);

            char name[32] = {};
            uint32_t length = 0;
            CHECK(world->unit_name("RUS", FakeWorld::kArmyId, name, sizeof(name), &length) == ANVIL_OK &&
                  std::strcmp(name, "1st Army") == 0 && length == 8);
            CHECK(world->unit_name("RUS", FakeWorld::kNavyId, name, 4, &length) == ANVIL_OK &&
                  std::strcmp(name, "1st") == 0 && length == 9);
            CHECK(world->unit_leader_name("RUS", FakeWorld::kArmyId, name, sizeof(name), &length) == ANVIL_OK &&
                  std::strcmp(name, "Sheremetev") == 0);
            CHECK(world->unit_leader_name("RUS", FakeWorld::kNavyId, name, sizeof(name), &length) == ANVIL_ERROR_NOT_FOUND);

            AnvilGameUnitRef selected[4] = {};
            CHECK(world->selected_units(selected, 4, &count) == ANVIL_OK && count == 2 &&
                  selected[0].id == FakeWorld::kNavyId && selected[0].kind == ANVIL_GAME_UNIT_NAVY &&
                  selected[1].id == FakeWorld::kArmyId && std::strcmp(selected[1].country.text, "RUS") == 0);
            CHECK(world->selected_units(nullptr, 0, &count) == ANVIL_OK && count == 2);
        };
        frame(game.app, 1);
        CHECK(ran);

        // A mission whose getter computes, a kind getter outside the game, a
        // session without a map input.
        ran = false;
        fake.drill_vtable[10] = image.address(FakeWorld::kComputed);
        fake.navy_vtable[1] = reinterpret_cast<uintptr_t>(&fake_frame_body);
        FakeWorld::set(game.session, 0x1B80, uintptr_t{0});
        g_in_frame_handler = [&] {
            ran = true;
            AnvilGameUnit u{};
            u.size = sizeof(u);
            CHECK(world->unit("RUS", FakeWorld::kArmyId, &u) == ANVIL_OK && u.mission == ANVIL_GAME_MISSION_UNKNOWN);
            CHECK(world->unit("RUS", FakeWorld::kNavyId, &u) == ANVIL_ERROR_INTERNAL);
            AnvilGameUnitRef selected[4] = {};
            CHECK(world->selected_units(selected, 4, &count) == ANVIL_OK && count == 0);
        };
        frame(game.app, 1);
        CHECK(ran);
        g_in_frame_handler = nullptr;

        // Absent on a build: NOT_DECLARED.
        anvil_game::WorldLayout l;
        std::vector<uint64_t> none;
        std::vector<AnvilGameUnitRef> no_selection;
        CHECK(anvil_game::read_country_units(l, "RUS", none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_selected_units(l, no_selection) == ANVIL_ERROR_NOT_DECLARED);

        const uint8_t zero[] = {0x33, 0xC0, 0xC3}, five[] = {0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3},
                      other[] = {0x8B, 0x41, 0x08, 0xC3, 0x00, 0x00}, no_ret[] = {0xB8, 0x05, 0x00, 0x00, 0x00, 0x90};
        int64_t value = -1;
        CHECK(anvil_game::decode_constant_getter(zero, sizeof zero, value) && value == 0);
        CHECK(anvil_game::decode_constant_getter(five, sizeof five, value) && value == 5);
        CHECK(!anvil_game::decode_constant_getter(other, sizeof other, value) &&
              !anvil_game::decode_constant_getter(no_ret, sizeof no_ret, value) &&
              !anvil_game::decode_constant_getter(five, 3, value));
    });

    run("anvil-game.world: rulers, flags and variables", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && ANVIL_GAME_WORLD_HAS(world, province_variable_names));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        fake.install_rulers(game.root, image);
        game.set_session(game.session);
        frame(game.app, 1);
        AnvilGameSession session{};
        CHECK(service->session(&session) == 1);

        AnvilGameMonarch m{};
        m.size = sizeof(m);
        CHECK(world->monarch("RUS", ANVIL_GAME_MONARCH_RULER, &m) == ANVIL_ERROR_WRONG_PHASE);  // between frames
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(world->monarch("rus", ANVIL_GAME_MONARCH_RULER, &m) == ANVIL_OK && m.size == sizeof(m) &&
                  m.kind == ANVIL_GAME_MONARCH_RULER && m.id == FakeWorld::kRulerId &&
                  std::strcmp(m.country.text, "RUS") == 0 && m.flags == 0 && m.adm == 6 && m.dip == 4 && m.mil == 5 &&
                  m.claim == 0 && m.leader == FakeWorld::kLeaderId);
            CHECK(m.birth.year == 1672 && m.birth.month == 6 && m.birth.day == 9 && m.birth.session == session.number &&
                  m.birth.size == sizeof(AnvilGameDate));
            AnvilGameMonarch c{};
            c.size = sizeof(c);
            CHECK(world->monarch("RUS", ANVIL_GAME_MONARCH_CONSORT, &c) == ANVIL_OK &&
                  c.flags == ANVIL_GAME_MONARCH_FEMALE && c.leader == 0);
            // The game's placeholder heir is "none", and so is a null pointer.
            CHECK(world->monarch("RUS", ANVIL_GAME_MONARCH_HEIR, &c) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch("TUR", ANVIL_GAME_MONARCH_CONSORT, &c) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch("XXX", ANVIL_GAME_MONARCH_RULER, &c) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch("RUS", 3, &c) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->monarch(nullptr, 0, &c) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->monarch("RUS", 0, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->monarch("TUR", ANVIL_GAME_MONARCH_RULER, &c) == ANVIL_OK && c.flags == ANVIL_GAME_MONARCH_REGENCY);
            CHECK(world->monarch("TUR", ANVIL_GAME_MONARCH_HEIR, &c) == ANVIL_OK && c.claim == 75000);
            // A caller built against a shorter struct gets only that much.
            AnvilGameMonarch shorter{};
            shorter.size = offsetof(AnvilGameMonarch, birth);
            shorter.leader = 77;
            CHECK(world->monarch("RUS", 0, &shorter) == ANVIL_OK && shorter.mil == 5 && shorter.leader == 77);

            char text[32] = {};
            uint32_t length = 0;
            CHECK(world->monarch_name("RUS", ANVIL_GAME_MONARCH_RULER, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "Pyotr I") == 0 && length == 7);
            CHECK(world->monarch_name("RUS", ANVIL_GAME_MONARCH_HEIR, text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch_dynasty("RUS", ANVIL_GAME_MONARCH_CONSORT, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "Romanov") == 0);
            // A regency council shows its heir's dynasty, a queen regent her own.
            CHECK(world->monarch_dynasty("TUR", ANVIL_GAME_MONARCH_RULER, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "Osmanoglu") == 0);
            fake.monarch_vtable[0x60 / 8] = image.address(FakeWorld::kQueenRegent);
            CHECK(world->monarch_dynasty("TUR", ANVIL_GAME_MONARCH_RULER, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "Romanov") == 0);
            fake.monarch_vtable[0x60 / 8] = reinterpret_cast<uintptr_t>(&fake_frame_body);  // not a getter of the game
            CHECK(world->monarch_dynasty("TUR", ANVIL_GAME_MONARCH_RULER, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "Romanov") == 0);
            fake.monarch_vtable[0x60 / 8] = image.address(FakeWorld::kNotQueenRegent);

            std::int32_t id = -1;
            CHECK(world->flag_id("great_northern_war", &id) == ANVIL_OK && id == 2);
            CHECK(world->flag_id("Great_Northern_War", &id) == ANVIL_ERROR_NOT_FOUND &&
                  world->flag_id("", &id) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->flag_id("x", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->flag_name(3, text, sizeof text, &length) == ANVIL_OK && std::strcmp(text, "met_the_tsar") == 0);
            CHECK(world->flag_name(4, text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND &&
                  world->flag_name(-1, text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND);
            AnvilGameFlag flags[4] = {};
            uint32_t count = 0;
            CHECK(world->country_flags("RUS", flags, 4, &count) == ANVIL_OK && count == 2 && flags[0].id == 0 &&
                  flags[1].id == 2 && flags[1].date.year == 1700 && flags[1].date.month == 8 && flags[1].date.day == 19 &&
                  flags[1].date.session == session.number);
            CHECK(world->country_flags("RUS", flags, 1, &count) == ANVIL_OK && count == 2);
            CHECK(world->country_flags("TUR", nullptr, 0, &count) == ANVIL_OK && count == 0);
            CHECK(world->country_flags("XXX", flags, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            // A province's own flags and its permanent ones, each once.
            CHECK(world->province_flags(1, flags, 4, &count) == ANVIL_OK && count == 2 && flags[0].id == 1 &&
                  flags[0].date.year == 1701 && flags[1].id == 3 && flags[1].date.year == 1651);
            CHECK(world->province_flags(4, flags, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch_flags("RUS", ANVIL_GAME_MONARCH_RULER, flags, 4, &count) == ANVIL_OK && count == 1 &&
                  flags[0].id == 3 && flags[0].date.year == 1697);
            CHECK(world->monarch_flags("RUS", ANVIL_GAME_MONARCH_HEIR, flags, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->monarch_flags("RUS", 7, flags, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->global_flags(flags, 4, &count) == ANVIL_OK && count == 1 && flags[0].id == 2);

            std::int32_t value = 0;
            CHECK(world->country_variable("RUS", "treasury_plan", &value) == ANVIL_OK && value == 2500);
            CHECK(world->country_variable("RUS", fake.long_variable.c_str(), &value) == ANVIL_OK && value == -1000);
            CHECK(world->country_variable("RUS", "Treasury_Plan", &value) == ANVIL_ERROR_NOT_FOUND &&
                  world->country_variable("RUS", "", &value) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_variable("RUS", "x", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->province_variable(1, "treasury_plan", &value) == ANVIL_ERROR_NOT_FOUND);
            char names[64] = {};
            const std::string all = std::string("treasury_plan") + '\0' + fake.long_variable + '\0';
            CHECK(world->country_variable_names("RUS", names, sizeof names, &length) == ANVIL_OK &&
                  length == all.size() && std::memcmp(names, all.data(), all.size()) == 0);
            std::memset(names, 0x7F, sizeof names);
            CHECK(world->country_variable_names("RUS", names, 20, &length) == ANVIL_OK && length == all.size() &&
                  std::strcmp(names, "treasury_plan") == 0 && names[14] == 0x7F);  // whole names only
            CHECK(world->country_variable_names("RUS", nullptr, 0, &length) == ANVIL_OK && length == all.size());
            CHECK(world->province_variable_names(1, names, sizeof names, &length) == ANVIL_OK && length == 0);
            CHECK(world->country_variable_names("RUS", nullptr, 4, &length) == ANVIL_ERROR_INVALID_ARGUMENT);
            // A variable list cut short costs that answer.
            FakeWorld::set(fake.rus, 0x330 + 0x20, uint64_t{3});
            CHECK(world->country_variable("RUS", "treasury_plan", &value) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rus, 0x330 + 0x20, uint64_t{2});
        };
        frame(game.app, 1);
        CHECK(ran);
        g_in_frame_handler = nullptr;

        // Absent on a build: NOT_DECLARED.
        anvil_game::WorldLayout l;
        AnvilGameMonarch none{};
        std::vector<anvil_game::FlagEntry> no_flags;
        std::vector<anvil_game::VariableEntry> no_variables;
        std::int32_t no_id = 0;
        CHECK(anvil_game::read_monarch(l, "RUS", 0, none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_flag_id(l, "x", no_id) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_global_flags(l, no_flags) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_country_variables(l, "RUS", no_variables) == ANVIL_ERROR_NOT_DECLARED);

        const uint8_t no[] = {0x32, 0xC0, 0xC3}, yes[] = {0xB0, 0x01, 0xC3}, other[] = {0x8A, 0x41, 0x08};
        bool flag = true;
        CHECK(anvil_game::decode_bool_getter(no, sizeof no, flag) && !flag);
        CHECK(anvil_game::decode_bool_getter(yes, sizeof yes, flag) && flag);
        CHECK(!anvil_game::decode_bool_getter(other, sizeof other, flag) && !anvil_game::decode_bool_getter(no, 2, flag));
    });

    run("anvil-game.world: pending events and statistics", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && ANVIL_GAME_WORLD_HAS(world, country_statistics));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        FakeWorld::set(game.root, 0x1E60, game_tag("RUS", 0));
        frame(game.app, 1);
        AnvilGameSession session{};
        CHECK(service->session(&session) == 1);
        pending_heard.clear();
        fake.install_events(game.root);

        // The frame the entries appear in: none counts yet.
        int32_t ids[8] = {};
        uint32_t count = 99;
        g_in_frame_handler = [&] {
            CHECK(world->pending_events(nullptr, ids, 8, &count) == ANVIL_OK && count == 0);
        };
        frame(game.app, 1);
        CHECK(pending_heard.empty() && g_in_frame_handler == nullptr);
        CHECK(world->pending_events(nullptr, ids, 8, &count) == ANVIL_ERROR_WRONG_PHASE);  // between frames

        // The next frame: the player's three are told, TUR's is not.
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(world->pending_events(nullptr, ids, 8, &count) == ANVIL_OK && count == 3 && ids[0] == 5 &&
                  ids[1] == 6 && ids[2] == 8);
            CHECK(world->pending_events("rus", ids, 1, &count) == ANVIL_OK && count == 3 && ids[0] == 5);
            CHECK(world->pending_events("TUR", ids, 8, &count) == ANVIL_OK && count == 1 && ids[0] == 7);
            CHECK(world->pending_events("O00", nullptr, 0, &count) == ANVIL_OK && count == 0);
            CHECK(world->pending_events("SWE", ids, 8, &count) == ANVIL_ERROR_NOT_FOUND &&
                  world->pending_events("RU", ids, 8, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->pending_events(nullptr, nullptr, 8, &count) == ANVIL_ERROR_INVALID_ARGUMENT);

            AnvilGamePendingEvent e{};
            e.size = sizeof(e);
            CHECK(world->pending_event(5, &e) == ANVIL_OK && e.size == sizeof(e) && e.pending_id == 5 &&
                  std::strcmp(e.country.text, "RUS") == 0 && e.province == 0 && e.options == 3 && e.flags == 0 &&
                  e.auto_select.year == 1719 && e.auto_select.month == 7 && e.auto_select.day == 17 &&
                  e.auto_select.session == session.number);
            CHECK(world->pending_event(6, &e) == ANVIL_OK && std::strcmp(e.country.text, "RUS") == 0 && e.province == 1);
            CHECK(world->pending_event(7, &e) == ANVIL_OK && std::strcmp(e.country.text, "TUR") == 0 &&
                  e.flags == ANVIL_GAME_PENDING_HIDDEN && e.options == 1);
            CHECK(world->pending_event(8, &e) == ANVIL_OK && std::strcmp(e.country.text, "RUS") == 0);
            CHECK(world->pending_event(9, &e) == ANVIL_ERROR_NOT_FOUND &&
                  world->pending_event(5, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            // A struct as an older header knows it: nothing behind its size is written.
            struct Older {
                uint32_t size;
                int32_t pending_id;
                uint32_t guard;
            } older{offsetof(AnvilGamePendingEvent, country), 0, 0xA5A5A5A5u};
            CHECK(world->pending_event(5, reinterpret_cast<AnvilGamePendingEvent*>(&older)) == ANVIL_OK &&
                  older.pending_id == 5 && older.guard == 0xA5A5A5A5u);

            char text[64] = {};
            uint32_t length = 0;
            CHECK(world->pending_event_key(5, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "flavor_rus.1") == 0 && length == 12);
            CHECK(world->pending_event_title(5, text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "flavor_rus.1.t") == 0);
            CHECK(world->pending_event_key(7, text, 8, &length) == ANVIL_OK && std::strcmp(text, "an_even") == 0 &&
                  length == fake.long_event_key.size());
            CHECK(world->pending_event_title(9, text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND);

            AnvilGameStatisticPoint points[4] = {};
            CHECK(world->country_statistics("rus", ANVIL_GAME_STATISTIC_INCOME, points, 4, &count) == ANVIL_OK &&
                  count == 2 && points[0].year == 1717 && points[0].value == 0 && points[1].year == 1718 &&
                  points[1].value == 1584);
            CHECK(world->country_statistics("TUR", ANVIL_GAME_STATISTIC_INCOME, points, 1, &count) == ANVIL_OK &&
                  count == 1 && points[0].value == 2210);
            CHECK(world->country_statistics("RUS", ANVIL_GAME_STATISTIC_INCOME, nullptr, 0, &count) == ANVIL_OK &&
                  count == 2);
            CHECK(world->country_statistics("RUS", ANVIL_GAME_STATISTIC_SCORE, points, 4, &count) == ANVIL_ERROR_NOT_FOUND &&
                  world->country_statistics("SWE", ANVIL_GAME_STATISTIC_INCOME, points, 4, &count) == ANVIL_ERROR_NOT_FOUND &&
                  world->country_statistics("RUS", 4, points, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_statistics("RU", 0, points, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT);
            // Years and values that disagree cost that answer.
            FakeWorld::set(fake.series_nodes[1], 0x80, reinterpret_cast<uintptr_t>(fake.rus_years + 1));
            CHECK(world->country_statistics("RUS", ANVIL_GAME_STATISTIC_INCOME, points, 4, &count) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.series_nodes[1], 0x80, reinterpret_cast<uintptr_t>(fake.rus_years + 2));
        };
        frame(game.app, 1);
        CHECK(ran);
        CHECK((pending_heard == std::vector<std::string>{"added 5 RUS", "added 6 RUS", "added 8 RUS"}));
        frame(game.app, 1);
        CHECK(pending_heard.size() == 3);

        // The last one is chosen: told once. An AI's event for one frame and one
        // of the player's that leaves at once (hidden, or a duplicate) are never told.
        pending_heard.clear();
        FakeWorld::set(game.root, 0x1EAC, int32_t{3});
        frame(game.app, 1);
        CHECK((pending_heard == std::vector<std::string>{"removed 8 RUS"}));
        fake.set_entry(3, 9, fake.event_b, fake.scopes[4], 0);
        fake.set_entry(4, 10, fake.event_a, fake.scopes[0], 0);
        FakeWorld::set(game.root, 0x1EAC, int32_t{5});
        g_in_frame_handler = [&] {
            CHECK(world->pending_events("TUR", ids, 8, &count) == ANVIL_OK && count == 1 && ids[0] == 7);
            CHECK(world->pending_event_key(9, nullptr, 0, nullptr) == ANVIL_ERROR_NOT_FOUND);
        };
        frame(game.app, 1);
        FakeWorld::set(game.root, 0x1EAC, int32_t{3});
        frame(game.app, 1);
        CHECK(pending_heard.size() == 1 && g_in_frame_handler == nullptr);

        // Observe mode: no own country, so NULL is NOT_FOUND, and a new baseline.
        FakeWorld::set(game.root, 0x1E60, game_tag("O00", 2, 7));
        FakeWorld::set(game.root, 0x1E68, game_tag("RUS", 0));
        g_in_frame_handler = [&] {
            CHECK(world->pending_events(nullptr, ids, 8, &count) == ANVIL_ERROR_NOT_FOUND);
            CHECK(world->pending_events("RUS", ids, 8, &count) == ANVIL_OK && count == 2);
        };
        frame(game.app, 1);
        FakeWorld::set(game.root, 0x1E60, game_tag("RUS", 0));
        FakeWorld::set(game.root, 0x1E68, game_tag("---", 0, 0, 0));
        frame(game.app, 1);
        CHECK(pending_heard.size() == 1);

        // A save loaded from the game: the new list counts at once, and nothing is told.
        const auto set_session = reinterpret_cast<void (*)(void*, void*)>(image.address(0x756A30));
        fake.set_entry(2, 11, fake.event_a, fake.scopes[0], 0);
        set_session(game.root, game.session);
        g_in_frame_handler = [&] {
            CHECK(world->pending_events(nullptr, ids, 8, &count) == ANVIL_OK && count == 3 && ids[2] == 11);
        };
        frame(game.app, 1);
        CHECK(pending_heard.size() == 1 && g_in_frame_handler == nullptr);
        frame(game.app, 1);
        CHECK(pending_heard.size() == 1);

        FakeWorld::set(game.root, 0x1EAC, int32_t{0});
        frame(game.app, 1);
        pending_heard.clear();

        // Absent on a build: NOT_DECLARED.
        anvil_game::WorldLayout l;
        std::vector<anvil_game::PendingKey> keys;
        std::vector<AnvilGameStatisticPoint> points;
        AnvilGamePendingEvent none{};
        std::string text;
        CHECK(anvil_game::read_pending_keys(l, keys) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_pending(l, {}, none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_pending_text(l, {}, true, text) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_statistics(l, "RUS", 0, points) == ANVIL_ERROR_NOT_DECLARED);
    });

    run("anvil-game.world: timed modifiers and the province view", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && ANVIL_GAME_WORLD_HAS(world, province_view));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        fake.install_timed(game.session, image);
        game.set_session(game.session);
        frame(game.app, 1);
        AnvilGameSession session{};
        CHECK(service->session(&session) == 1);

        AnvilGameTimedModifier list[4] = {};
        uint32_t count = 0;
        std::int32_t province = 0;
        CHECK(world->country_timed_modifiers("RUS", list, 4, &count) == ANVIL_ERROR_WRONG_PHASE &&
              world->province_view(&province) == ANVIL_ERROR_WRONG_PHASE);  // between frames
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(world->country_timed_modifiers("rus", list, 4, &count) == ANVIL_OK && count == 3);
            CHECK(list[0].flags == ANVIL_GAME_TIMED_HIDDEN && list[0].start.year == 1715 && list[0].start.month == 1 &&
                  list[0].start.day == 1 && list[0].end.year == 1725 && list[0].end.session == session.number &&
                  list[0].end.size == sizeof(AnvilGameDate));
            CHECK(list[1].flags == (ANVIL_GAME_TIMED_RULER | ANVIL_GAME_TIMED_NO_END | ANVIL_GAME_TIMED_NO_START) &&
                  list[1].start.year == 0 && list[1].end.year == 0 && list[1].end.size == 0);
            CHECK(list[2].flags == ANVIL_GAME_TIMED_NO_END && list[2].start.year == 1719 && list[2].end.year == 0);
            CHECK(world->country_timed_modifiers("RUS", list, 1, &count) == ANVIL_OK && count == 3);
            CHECK(world->country_timed_modifiers("TUR", nullptr, 0, &count) == ANVIL_OK && count == 0);
            CHECK(world->country_timed_modifiers("XXX", list, 4, &count) == ANVIL_ERROR_NOT_FOUND &&
                  world->country_timed_modifiers(nullptr, list, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_timed_modifiers("RUS", nullptr, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT);
            char keys[96] = {};
            uint32_t length = 0;
            const std::string all = fake.timed_texts[0] + '\0' + fake.timed_texts[1] + '\0' + fake.timed_texts[0] + '\0';
            CHECK(world->country_timed_modifier_keys("RUS", keys, sizeof keys, &length) == ANVIL_OK &&
                  length == all.size() && std::memcmp(keys, all.data(), all.size()) == 0);
            std::memset(keys, 0x7F, sizeof keys);
            CHECK(world->country_timed_modifier_keys("RUS", keys, 35, &length) == ANVIL_OK && length == all.size() &&
                  std::strcmp(keys, fake.timed_texts[0].c_str()) == 0 && keys[31] == 0x7F);  // whole keys only
            CHECK(world->country_timed_modifier_keys("RUS", nullptr, 0, &length) == ANVIL_OK && length == all.size());
            CHECK(world->province_timed_modifiers(1, list, 4, &count) == ANVIL_OK && count == 1 &&
                  list[0].flags == ANVIL_GAME_TIMED_HIDDEN && list[0].end.year == 1725);
            CHECK(world->province_timed_modifier_keys(1, keys, sizeof keys, &length) == ANVIL_OK &&
                  length == fake.timed_texts[0].size() + 1 && keys == fake.timed_texts[0]);
            CHECK(world->province_timed_modifiers(2, list, 4, &count) == ANVIL_OK && count == 0);
            CHECK(world->province_timed_modifiers(4, list, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            // A broken list costs that answer.
            FakeWorld::set(fake.timed[1], 0x30, uintptr_t{0});
            CHECK(world->country_timed_modifiers("RUS", list, 4, &count) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.timed[1], 0x30, reinterpret_cast<uintptr_t>(fake.static_modifiers[1]));

            CHECK(world->province_view(&province) == ANVIL_OK && province == 1);
            CHECK(world->province_view(nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            fake.view_window[0x30 + 0xC5] = 0;  // the player closed it
            CHECK(world->province_view(&province) == ANVIL_ERROR_NOT_FOUND);
            fake.view_window[0x30 + 0xC5] = 1;
            FakeWorld::set(fake.province_view, 0x958, reinterpret_cast<uintptr_t>(fake.province(1) + 8));
            CHECK(world->province_view(&province) == ANVIL_ERROR_INTERNAL);  // not a province of the array
            FakeWorld::set(fake.province_view, 0x958, reinterpret_cast<uintptr_t>(fake.province(3)));
            CHECK(world->province_view(&province) == ANVIL_OK && province == 3);
            FakeWorld::set(fake.selection_object, 0x10, uintptr_t{0});
            CHECK(world->province_view(&province) == ANVIL_ERROR_NOT_FOUND);
            FakeWorld::set(game.session, 0x1220, uintptr_t{0});
            CHECK(world->province_view(&province) == ANVIL_ERROR_NOT_FOUND);
        };
        frame(game.app, 1);
        CHECK(ran);
        g_in_frame_handler = nullptr;

        anvil_game::WorldLayout l;
        std::vector<anvil_game::TimedEntry> none;
        std::int32_t id = 0;
        CHECK(anvil_game::read_country_timed(l, "RUS", none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_province_timed(l, 1, none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_province_view(l, id) == ANVIL_ERROR_NOT_DECLARED);
    });

    run("anvil-game.world: diplomacy", [&] {
        const auto* world = static_cast<const AnvilGameWorldService*>(
            bus.get_service(ANVIL_GAME_WORLD_SERVICE, ANVIL_GAME_WORLD_SERVICE_VERSION));
        CHECK(world != nullptr && ANVIL_GAME_WORLD_HAS(world, country_subjects) &&
              world->size == sizeof(AnvilGameWorldService));
        if (world == nullptr) return;
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        fake.install_diplomacy();
        game.set_session(game.session);
        frame(game.app, 1);
        AnvilGameSession session{};
        CHECK(service->session(&session) == 1);

        AnvilGameRelation relations[4] = {};
        uint32_t count = 99;
        AnvilGameDiplomacyStatus status{};
        status.size = sizeof(status);
        CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_ALLIANCE, relations, 4, &count) ==
                  ANVIL_ERROR_WRONG_PHASE &&
              world->diplomacy_status("RUS", "TUR", &status) == ANVIL_ERROR_WRONG_PHASE);  // between frames
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            // One alliance, the same object from either side.
            CHECK(world->country_relations("rus", ANVIL_GAME_RELATION_ALLIANCE, relations, 4, &count) == ANVIL_OK &&
                  count == 1 && std::strcmp(relations[0].first.text, "RUS") == 0 &&
                  std::strcmp(relations[0].second.text, "TUR") == 0 && relations[0].start.year == 1700 &&
                  relations[0].start.month == 1 && relations[0].start.day == 1 &&
                  relations[0].start.session == session.number && relations[0].start.size == sizeof(AnvilGameDate) &&
                  relations[0].end.size == 0 && relations[0].end.year == 0);
            CHECK(world->country_relations("TUR", ANVIL_GAME_RELATION_ALLIANCE, relations, 4, &count) == ANVIL_OK &&
                  count == 1 && std::strcmp(relations[0].first.text, "RUS") == 0);
            // Military access has a list per side.
            CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_MILITARY_ACCESS, relations, 4, &count) ==
                      ANVIL_OK &&
                  count == 1 && std::strcmp(relations[0].first.text, "RUS") == 0);
            CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_GIVING_MILITARY_ACCESS, relations, 4, &count) ==
                      ANVIL_OK &&
                  count == 0);
            CHECK(world->country_relations("TUR", ANVIL_GAME_RELATION_GIVING_MILITARY_ACCESS, relations, 4, &count) ==
                      ANVIL_OK &&
                  count == 1 && std::strcmp(relations[0].second.text, "TUR") == 0);
            CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_WARNING, relations, 4, &count) == ANVIL_OK &&
                  count == 1 && std::strcmp(relations[0].first.text, "TUR") == 0 && relations[0].start.size == 0 &&
                  relations[0].end.year == 1720 && relations[0].end.session == session.number);
            CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_ROYAL_MARRIAGE, relations, 4, &count) ==
                      ANVIL_OK &&
                  count == 0);
            CHECK(world->country_relations("RUS", ANVIL_GAME_RELATION_GUARANTEE, nullptr, 0, &count) == ANVIL_OK &&
                  count == 1);
            CHECK(world->country_relations("RUS", 8, relations, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_relations(nullptr, 0, relations, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_relations("RUS", 0, nullptr, 4, &count) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->country_relations("XXX", 0, relations, 4, &count) == ANVIL_ERROR_NOT_FOUND);
            // A relation that names neither country is not one of its own.
            FakeWorld::set(fake.relation[0], 0x18, game_tag("O00", 2, 7));
            CHECK(world->country_relations("TUR", ANVIL_GAME_RELATION_ALLIANCE, relations, 4, &count) ==
                  ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.relation[0], 0x18, game_tag("TUR", 1));

            char text[16] = {};
            uint32_t length = 0;
            CHECK(world->subject_type("RUS", "tur", text, sizeof text, &length) == ANVIL_OK &&
                  std::strcmp(text, "vassal") == 0 && length == 6);
            CHECK(world->subject_type("RUS", "TUR", text, 4, &length) == ANVIL_OK && std::strcmp(text, "vas") == 0 &&
                  length == 6);
            CHECK(world->subject_type("TUR", "RUS", text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND &&
                  world->subject_type("RUS", "XXX", text, sizeof text, &length) == ANVIL_ERROR_NOT_FOUND &&
                  world->subject_type("RUS", nullptr, text, sizeof text, &length) == ANVIL_ERROR_INVALID_ARGUMENT);

            CHECK(world->diplomacy_status("RUS", "tur", &status) == ANVIL_OK && status.size == sizeof(status) &&
                  status.opinion == -108 && status.timed_sum == -399 && status.trust == 99 && status.favors == 37590 &&
                  status.flags == ANVIL_GAME_DIPLOMACY_TRUCE);
            CHECK(world->diplomacy_status("TUR", "RUS", &status) == ANVIL_OK && status.opinion == -200 &&
                  status.timed_sum == 0 && status.trust == 50 && status.favors == 0 && status.flags == 0);
            CHECK(world->diplomacy_status("RUS", "RUS", &status) == ANVIL_ERROR_INVALID_ARGUMENT &&
                  world->diplomacy_status("RUS", "O00", &status) == ANVIL_ERROR_NOT_FOUND &&  // owns no province
                  world->diplomacy_status("O00", "RUS", &status) == ANVIL_ERROR_NOT_FOUND &&
                  world->diplomacy_status("RUS", "XXX", &status) == ANVIL_ERROR_NOT_FOUND &&
                  world->diplomacy_status("RUS", "TUR", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            // A struct as an older header knows it: nothing behind its size is written.
            struct Older {
                uint32_t size;
                int32_t opinion;
                uint32_t guard;
            } older{offsetof(AnvilGameDiplomacyStatus, timed_sum), 0, 0xA5A5A5A5u};
            CHECK(world->diplomacy_status("RUS", "TUR", reinterpret_cast<AnvilGameDiplomacyStatus*>(&older)) ==
                      ANVIL_OK &&
                  older.opinion == -108 && older.guard == 0xA5A5A5A5u);
            // The cache does not reach TUR yet: no fallback, no read past its end.
            FakeWorld::set(fake.rus, 0x2388, reinterpret_cast<uintptr_t>(fake.rus_opinions + 1));
            CHECK(world->diplomacy_status("RUS", "TUR", &status) == ANVIL_ERROR_NOT_FOUND);
            FakeWorld::set(fake.rus, 0x2388, reinterpret_cast<uintptr_t>(fake.rus_opinions + 3));
            // A status that is not this country's.
            FakeWorld::set(fake.rus_statuses[1], 0, reinterpret_cast<uintptr_t>(fake.tur + 0x14F8));
            CHECK(world->diplomacy_status("RUS", "TUR", &status) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rus_statuses[1], 0, reinterpret_cast<uintptr_t>(fake.rus + 0x14F8));
            FakeWorld::set(fake.rus_statuses[1], 0x10, game_tag("O00", 2, 7));
            CHECK(world->diplomacy_status("RUS", "TUR", &status) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rus_statuses[1], 0x10, game_tag("TUR", 1));

            AnvilGameTag tags[4] = {};
            CHECK(world->country_rivals("RUS", tags, 4, &count) == ANVIL_OK && count == 1 &&
                  std::strcmp(tags[0].text, "TUR") == 0);
            CHECK(world->country_rivals("TUR", nullptr, 0, &count) == ANVIL_OK && count == 0);
            FakeWorld::set(fake.rival_node, 0x28, reinterpret_cast<uintptr_t>(fake.rival_node));  // a loop
            CHECK(world->country_rivals("RUS", tags, 4, &count) == ANVIL_ERROR_INTERNAL);
            FakeWorld::set(fake.rival_node, 0x28, uintptr_t{0});
            AnvilGameTag overlord{};
            CHECK(world->country_overlord("TUR", &overlord) == ANVIL_OK && std::strcmp(overlord.text, "RUS") == 0);
            CHECK(world->country_overlord("RUS", &overlord) == ANVIL_ERROR_NOT_FOUND &&
                  world->country_overlord("RUS", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
            CHECK(world->country_subjects("RUS", tags, 4, &count) == ANVIL_OK && count == 1 &&
                  std::strcmp(tags[0].text, "TUR") == 0);
            CHECK(world->country_subjects("TUR", tags, 4, &count) == ANVIL_OK && count == 0);
        };
        frame(game.app, 1);
        CHECK(ran);
        g_in_frame_handler = nullptr;

        anvil_game::WorldLayout l;
        std::vector<anvil_game::RelationEntry> none;
        std::vector<AnvilGameTag> no_tags;
        AnvilGameTag tag{};
        std::string text;
        AnvilGameDiplomacyStatus nothing{};
        CHECK(anvil_game::read_relations(l, "RUS", 0, none) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_subject_type(l, "RUS", "TUR", text) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_diplomacy_status(l, "RUS", "TUR", nothing) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_rivals(l, "RUS", no_tags) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_overlord(l, "RUS", tag) == ANVIL_ERROR_NOT_DECLARED &&
              anvil_game::read_subjects(l, "RUS", no_tags) == ANVIL_ERROR_NOT_DECLARED);
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

    run("anvil-game.commands: the unit panel's commands, posted to the session", [&] {
        const auto* commands = static_cast<const AnvilGameCommandsService*>(
            bus.get_service(ANVIL_GAME_COMMANDS_SERVICE, ANVIL_GAME_COMMANDS_SERVICE_VERSION));
        CHECK(commands != nullptr && ANVIL_GAME_COMMANDS_HAS(commands, post));
        CHECK(contains(log, "anvil-game: service anvil-game.commands v1 and v2: drill yes, merge yes, split yes, event option yes"));
        CHECK(bus.get_service(ANVIL_GAME_COMMANDS_SERVICE, 1) == commands);
        if (commands == nullptr) return;
        // The game's functions, verified at load, now jump to the stand-ins.
        const auto jump = [&](uint32_t rva, const void* target) {
            image.put(rva, {0x48, 0xB8});
            image.put_pointer(rva + 2, reinterpret_cast<uintptr_t>(target));
            image.put(rva + 10, {0xFF, 0xE0});
        };
        jump(0x14CA2D0, reinterpret_cast<const void*>(&fake_command_post));
        jump(0x1A332D4, reinterpret_cast<const void*>(&fake_command_new));
        jump(0xD671A0, reinterpret_cast<const void*>(&fake_drill_command));
        jump(0xD63680, reinterpret_cast<const void*>(&fake_split_command));
        jump(0xD5FF20, reinterpret_cast<const void*>(&fake_merge_command));
        jump(0x10BC2D0, reinterpret_cast<const void*>(&fake_option_selected));
        // The idler's game (+0x330 on Steam) and its session (+0x370), with the hot-join flag at +0x7D7.
        alignas(8) static uint8_t application[0x400] = {};
        alignas(8) static uint8_t session[0x880] = {};
        FakeWorld::set(game.session, 0x330, reinterpret_cast<uintptr_t>(application));
        FakeWorld::set(application, 0x370, reinterpret_cast<uintptr_t>(session));
        // RUS again, with a second army in Moskva that does not drill.
        FakeWorld& fake = *new FakeWorld;
        fake.install(game.root, image);
        fake.install_units(game.session, image);
        alignas(8) static uint8_t army2[0x260] = {};
        alignas(8) static uint8_t node2[0x20] = {};
        std::memcpy(army2, fake.army, sizeof army2);
        constexpr uint64_t kArmy2 = 0x000003EB00000036;
        FakeWorld::set(army2, 0x20, kArmy2);
        FakeWorld::set(army2, 0x1F8, reinterpret_cast<uintptr_t>(fake.idle));
        FakeWorld::set(node2, 0, reinterpret_cast<uintptr_t>(army2));
        FakeWorld::set(fake.unit_nodes[1], 0x10, reinterpret_cast<uintptr_t>(node2));
        FakeWorld::set(fake.rus, 0xD18 + 0x10, int32_t{3});
        FakeWorld::set(game.root, 0x1E60, game_tag("RUS", 0));
        game.set_session(game.session);
        frame(game.app, 1);

        // anvil.control, as the core registers it, and the gameplay plugin it knows.
        static AnvilControlService control{};
        control.size = sizeof(control);
        control.plugin_of = fake_plugin_of;
        bus.register_service("anvil", ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION, &control);
        g_command_caller = new AnvilPlugin;
        AnvilPlugin* caller = g_command_caller;

        const AnvilGameUnitRef army = {FakeWorld::kArmyId, {"RUS"}, ANVIL_GAME_UNIT_ARMY};
        const AnvilGameUnitRef idle = {kArmy2, {"rus"}, ANVIL_GAME_UNIT_ARMY};
        const AnvilGameUnitRef navy = {FakeWorld::kNavyId, {"RUS"}, ANVIL_GAME_UNIT_NAVY};
        AnvilGameUnitRef refs[2] = {army, idle};
        AnvilGameCommand c{};
        c.size = sizeof(c);
        c.type = ANVIL_GAME_COMMAND_DRILL;
        c.flags = ANVIL_GAME_COMMAND_START;
        c.count = 2;
        c.units = refs;
        AnvilGameCommandResult out{};
        out.size = sizeof(out);
        CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_WRONG_PHASE);  // between frames
        CHECK(g_game_commands.calls.empty());

        const std::string session_text = pointer_text(session);
        bool ran = false;
        g_in_frame_handler = [&] {
            ran = true;
            CHECK(commands->post(plugin, &c, &out) == ANVIL_ERROR_UNSUPPORTED);  // anvil-game is no gameplay plugin
            // Start over both: the drilling one is skipped, the other gets the button's command.
            CHECK(commands->post(caller, &c, &out) == ANVIL_OK && out.posted == 1 && out.skipped == 1 && out.refused == 0);
            CHECK((g_game_commands.calls ==
                   std::vector<std::string>{"new 68", "drill " + pointer_text(army2) + " 36", "post " + session_text}));
            g_game_commands.calls.clear();
            // Stop: the drilling one.
            c.flags = 0;
            CHECK(commands->post(caller, &c, &out) == ANVIL_OK && out.posted == 1 && out.skipped == 1);
            CHECK(g_game_commands.calls.size() == 3 &&
                  g_game_commands.calls[1] == "drill " + pointer_text(fake.army) + " 36");
            g_game_commands.calls.clear();
            // Split the three regiments: two go.
            c.type = ANVIL_GAME_COMMAND_SPLIT;
            c.count = 1;
            CHECK(commands->post(caller, &c, &out) == ANVIL_OK && out.posted == 1 && out.skipped == 0);
            CHECK((g_game_commands.calls ==
                   std::vector<std::string>{"new 78", "split " + pointer_text(fake.army) + " 2", "post " + session_text}));
            g_game_commands.calls.clear();
            // Merge the two armies in Moskva, into the first.
            c.type = ANVIL_GAME_COMMAND_MERGE;
            c.count = 2;
            CHECK(commands->post(caller, &c, &out) == ANVIL_OK && out.posted == 1);
            CHECK((g_game_commands.calls ==
                   std::vector<std::string>{"new 68", "merge " + pointer_text(fake.army) + " " + pointer_text(army2) + " (2)",
                                            "post " + session_text}));
            g_game_commands.calls.clear();
            // Refused by the session: freed through slot 0.
            g_game_commands.allowed = false;
            CHECK(commands->post(caller, &c, &out) == ANVIL_OK && out.posted == 0 && out.refused == 1);
            CHECK(g_game_commands.calls.size() == 4 && g_game_commands.calls[3] == "free 1");
            g_game_commands.allowed = true;
            g_game_commands.calls.clear();

            // Nothing reaches the game: a navy with an army, a foreign or gone unit, a hot-join, a suspended caller.
            refs[1] = navy;
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
            refs[1] = {FakeWorld::kArmyId, {"TUR"}, ANVIL_GAME_UNIT_ARMY};
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_NOT_FOUND);
            refs[1] = {777, {"RUS"}, ANVIL_GAME_UNIT_ARMY};
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_NOT_FOUND);
            refs[1] = idle;
            session[0x7D7] = 1;
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_WRONG_PHASE);
            session[0x7D7] = 0;
            g_command_caller_suspended = 1;
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_SUSPENDED);
            g_command_caller_suspended = 0;
            // Observe mode: the player has no units of its own.
            FakeWorld::set(game.root, 0x1E60, game_tag("O00", 2, 7));
            FakeWorld::set(game.root, 0x1E68, game_tag("RUS", 0));
            CHECK(commands->post(caller, &c, &out) == ANVIL_ERROR_NOT_FOUND);
            FakeWorld::set(game.root, 0x1E60, game_tag("RUS", 0));
            CHECK(g_game_commands.calls.empty());

            // An event window in the idler's pop-ups (head at +0x78 on Steam), after another pop-up:
            // pending id 7, waiting, the buttons of options 0 (enabled) and 2 (disabled).
            alignas(8) static uint8_t other[0x10] = {};
            alignas(8) static uint8_t window[0x3E8] = {};
            alignas(8) static uint8_t nodes[2][0x20] = {};
            alignas(8) static uint8_t button0[0x130] = {}, button2[0x130] = {};
            alignas(8) static uint8_t button_nodes[2][0x20] = {};
            FakeWorld::set(window, 0, image.address(0x1D590F8));  // CEventWindow's vtable on Steam
            FakeWorld::set(window, 0x120, int32_t{7});
            window[0x124] = 1;
            FakeWorld::set_short_string(button0 + 0x38, "6053_0");
            FakeWorld::set_short_string(button2 + 0x38, "6053_2");
            FakeWorld::set(button2, 0x124, int32_t{3});
            FakeWorld::set(button_nodes[0], 0, reinterpret_cast<uintptr_t>(button0));
            FakeWorld::set(button_nodes[0], 0x10, reinterpret_cast<uintptr_t>(button_nodes[1]));
            FakeWorld::set(button_nodes[1], 0, reinterpret_cast<uintptr_t>(button2));
            FakeWorld::set(window, 0x208, reinterpret_cast<uintptr_t>(button_nodes[0]));
            FakeWorld::set(nodes[0], 0, reinterpret_cast<uintptr_t>(other));
            FakeWorld::set(nodes[0], 0x10, reinterpret_cast<uintptr_t>(nodes[1]));
            FakeWorld::set(nodes[1], 0, reinterpret_cast<uintptr_t>(window));
            FakeWorld::set(game.session, 0x78, reinterpret_cast<uintptr_t>(nodes[0]));
            FakeWorld::set(session, 0x58, uintptr_t{1});  // a server
            session[0x55] = 1;                               // single player
            AnvilGameCommand e{};
            e.size = sizeof(e);
            e.type = ANVIL_GAME_COMMAND_EVENT_OPTION;
            e.pending_event_id = 7;
            CHECK(commands->post(caller, &e, &out) == ANVIL_OK && out.posted == 1);
            CHECK((g_game_commands.calls == std::vector<std::string>{"option " + pointer_text(window) + " 0"}));
            CHECK(commands->post(caller, &e, &out) == ANVIL_OK && out.skipped == 1);  // _KeepAlive is clear now
            window[0x124] = 1;
            e.option_index = 2;
            CHECK(commands->post(caller, &e, &out) == ANVIL_ERROR_WRONG_PHASE);
            e.option_index = 1;
            CHECK(commands->post(caller, &e, &out) == ANVIL_ERROR_INVALID_ARGUMENT);
            e.option_index = 0;
            e.pending_event_id = 8;
            CHECK(commands->post(caller, &e, &out) == ANVIL_ERROR_NOT_FOUND);
            e.pending_event_id = 7;
            // Neither a dummy session nor posting allowed: refused, the window untouched.
            session[0x55] = 0;
            session[0x80D] = 0;
            CHECK(commands->post(caller, &e, &out) == ANVIL_OK && out.refused == 1 && window[0x124] == 1);
            session[0x80D] = 1;
            CHECK(commands->post(caller, &e, &out) == ANVIL_OK && out.posted == 1);
            CHECK(g_game_commands.calls.size() == 2);
            FakeWorld::set(game.session, 0x78, uintptr_t{0});
        };
        frame(game.app, 1);
        CHECK(ran);
        g_in_frame_handler = nullptr;
        for (void* p : g_game_commands.posted) std::free(p);
        g_game_commands.posted.clear();
        FakeWorld::set(game.root, 0x1E60, game_tag("TUR", 1));
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
