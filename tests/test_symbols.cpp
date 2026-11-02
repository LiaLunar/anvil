// Unit tests for symbols.toml: parsing, and verification against a small
// synthetic PE image, both as a file and mapped in memory. Usage: anvil_symbols_tests
#include "core/changes.hpp"
#include "core/plugin_symbols.hpp"
#include "core/pe_image.hpp"
#include "core/symbol_check.hpp"
#include "core/symbols.hpp"
#include "test_image.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

void run(const char* name, const std::function<void()>& body) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

using namespace anvil;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The image mapped in this process; the pointer slot is relocated to the
// actual base, as the Windows loader would do.
struct MappedImage {
    uint8_t* base;
    explicit MappedImage(const TestImage& image) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, TestImage::kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memcpy(base, image.mapped.data(), TestImage::kSize);
        const uint64_t slot = reinterpret_cast<uint64_t>(base) + 0x1000;
        std::memcpy(base + 0x2000, &slot, sizeof(slot));
    }
    ~MappedImage() { VirtualFree(base, 0, MEM_RELEASE); }
    uintptr_t address() const { return reinterpret_cast<uintptr_t>(base); }
};

const char* kGoodSymbols = R"toml(
build = "test-build"

[symbol.speed_object]
rva = 0x1000
bytes = "48 8B ?? 18 04 00 00 C3"
memory = "code"
signature = "void* (void* session)"

[symbol.root_and_date]
rva = 0x1010
bytes = "48 8B 05 F1 1F 00 00 8B 98 D0 1D 00 00"
memory = "code"
values.root        = { at = 3, rip_relative = 7, type = "Root" }
values.date_offset = { at = 9, size = 4 }
values.opcode      = { at = 0, size = 1 }
values.modrm       = { at = 7, size = 2 }
values.head        = { at = 0, size = 8 }

[symbol.tick_slot]
rva = 0x2000
memory = "read"
points_to = 0x1000

[symbol.exported]
export = "Exported_fn"
bytes = "40 53"
memory = "code"

[symbol.root]
rva = 0x3008
memory = "write"
type = "Root"

[symbol.bss_tail]
rva = 0x3500
bytes = "00 00 ?? 00"
memory = "write"
)toml";

SymbolFile symbols(const std::string& text) {
    auto r = parse_symbols_text(text);
    if (!r.file) std::printf("  symbols error: %s\n", r.error.c_str());
    return r.file ? *r.file : SymbolFile{};
}

std::string parse_error(const std::string& text) {
    auto r = parse_symbols_text(text);
    return r.file ? std::string() : r.error;
}

// One symbol table appended to a file that names the build.
std::string with_symbol(const std::string& body) { return "build = \"test-build\"\n[symbol.s]\n" + body; }

std::vector<std::string> verify(const std::string& text, const ImageReader& reader) {
    std::string error;
    const auto image = PeImage::parse(reader, error);
    if (!image) {
        std::printf("  image error: %s\n", error.c_str());
        return {"no image"};
    }
    const SymbolVerification v = verify_symbols(symbols(text), *image);
    for (const auto& p : v.problems) std::printf("  (problem) %s\n", p.c_str());
    return v.problems;
}

uint64_t value_of(const VerifiedSymbol& s, const std::string& name) {
    for (const auto& [n, v] : s.values) {
        if (n == name) return v;
    }
    return ~0ull;
}

}  // namespace

int main() {
    const TestImage test_image;
    std::string error;
    const auto file_reader = FileImageReader::from_bytes(test_image.file(), error);
    if (!file_reader) {
        std::printf("cannot build the test image: %s\n", error.c_str());
        return 1;
    }

    run("symbols: the design example parses, ids follow declaration order", [] {
        const SymbolFile f = symbols(R"toml(
build = "eu4-1.37.5-steam-win64"

[symbol.speed_object]                         # a function
rva = 0x8024D0
bytes = "48 8B 81 18 04 00 00 C3"             # hex pairs, ?? = any byte
memory = "code"                               # code | read | write
signature = "void* (void* session)"           # typed symbol in C/C++/Rust
note = "session vtable slot 0x100: returns the game-speed object"

[symbol.person_name_block]                    # a block for replace_block (Unicode Patch's person_name_hook)
rva = 0xA4B48C
bytes = "48 8D 55 B8 48 83 7D D0 10 48 0F 43 55 B8 4C 8B 45 C8 48 8B CB E8 4A 7E 64 FF"
memory = "code"
# resume = "<symbol>"                         # optional; default: the end of the block (0xA4B4A6 here)

[symbol.frontend_tick_slot]                   # a pointer slot
rva = 0x1D5A8E8
memory = "read"
points_to = 0x10D2E80

[symbol.root_and_date]                        # values taken from verified bytes
rva = 0x14321FC
bytes = "48 8B 05 75 DC F0 00 8B 98 D0 1D 00 00"
memory = "code"
values.root        = { at = 3, rip_relative = 7, type = "Root" }   # -> sym::root, Data<Root> at 0x233FE78
values.date_offset = { at = 9, size = 4 }                          # -> sym::root_and_date.date_offset = 0x1DD0

[symbol.physfs_get_write_dir]                 # by export name instead of RVA
export = "PHYSFS_getWriteDir"
signature = "const char* ()"
)toml");
        CHECK(f.build == "eu4-1.37.5-steam-win64");
        CHECK(f.symbols.size() == 5);
        if (f.symbols.size() != 5) return;
        const char* order[] = {"speed_object", "person_name_block", "frontend_tick_slot", "root_and_date",
                               "physfs_get_write_dir"};
        for (uint32_t i = 0; i < 5; ++i) {
            CHECK(f.symbols[i].name == order[i]);
            CHECK(f.symbols[i].id == i);
        }
        CHECK(f.symbols[0].kind == SymbolKind::Function);
        CHECK(f.symbols[0].note == "session vtable slot 0x100: returns the game-speed object");
        CHECK(f.symbols[1].kind == SymbolKind::Site);
        CHECK(f.symbols[1].bytes.size() == 26);
        CHECK(f.symbols[2].kind == SymbolKind::Slot);
        CHECK(f.symbols[2].points_to == 0x10D2E80u);
        CHECK(f.symbols[2].memory == MemoryKind::Read);
        CHECK(f.symbols[3].values.size() == 2);
        CHECK(f.symbols[3].values[0].name == "root" && f.symbols[3].values[0].rip_relative == 7);
        CHECK(f.symbols[3].values[0].type == "Root");
        CHECK(f.symbols[3].values[1].name == "date_offset" && f.symbols[3].values[1].size == 4);
        CHECK(f.symbols[4].export_name == "PHYSFS_getWriteDir" && !f.symbols[4].rva);
        CHECK(f.find("root_and_date") == &f.symbols[3]);
        CHECK(f.find("nope") == nullptr);
    });

    run("symbols: declaration order, not key order, gives ids", [] {
        const SymbolFile f = symbols("build = \"b\"\n"
                                     "[symbol.zeta]\nrva = 1\nbytes = \"C3\"\nmemory = \"code\"\n"
                                     "[symbol.alpha]\nrva = 2\nbytes = \"C3\"\nmemory = \"code\"\n"
                                     "[symbol.mid]\nrva = 3\nbytes = \"C3\"\nmemory = \"code\"\n");
        CHECK(f.symbols.size() == 3);
        if (f.symbols.size() != 3) return;
        CHECK(f.symbols[0].name == "zeta" && f.symbols[1].name == "alpha" && f.symbols[2].name == "mid");
    });

    run("symbols: bytes with ?? build a mask", [] {
        const SymbolFile f = symbols(with_symbol("rva = 0x10\nbytes = \"48??8B\"\nmemory = \"code\"\n"));
        CHECK(f.symbols.size() == 1);
        if (f.symbols.empty()) return;
        CHECK((f.symbols[0].bytes == std::vector<uint8_t>{0x48, 0x00, 0x8B}));
        CHECK((f.symbols[0].mask == std::vector<uint8_t>{0xFF, 0x00, 0xFF}));
    });

    run("symbols: resume names another symbol", [] {
        const SymbolFile f = symbols("build = \"b\"\n"
                                     "[symbol.block]\nrva = 0x10\nbytes = \"90 90\"\nmemory = \"code\"\nresume = \"after\"\n"
                                     "[symbol.after]\nrva = 0x20\nbytes = \"C3\"\nmemory = \"code\"\n");
        CHECK(f.symbols.size() == 2 && f.symbols[0].resume == "after");
    });

    run("symbols: schema errors", [] {
        const std::string site = "rva = 0x1000\nbytes = \"C3\"\nmemory = \"code\"\n";
        CHECK(contains(parse_error("build = \"b\"\nextra = 1\n"), "unknown key 'extra'"));
        CHECK(contains(parse_error(with_symbol(site + "offset = 4\n")), "symbol s: unknown key 'offset'"));
        CHECK(contains(parse_error(with_symbol(site + "values.v = { at = 0, size = 1, sign = true }\n")),
                       "values.v: unknown key 'sign'"));
        CHECK(contains(parse_error("[symbol.s]\n" + site), "missing 'build'"));
        CHECK(contains(parse_error("build = 3\n"), "build must be a string"));
        CHECK(contains(parse_error("build = \"b\"\n[symbol.Bad_Name]\n" + site), "must match [a-z][a-z0-9_]*"));
        CHECK(contains(parse_error(with_symbol(site + "export = \"F\"\n")), "exactly one of 'rva' and 'export'"));
        CHECK(contains(parse_error(with_symbol("bytes = \"C3\"\nmemory = \"code\"\n")), "exactly one of 'rva' and 'export'"));
        CHECK(contains(parse_error(with_symbol("rva = 0\nbytes = \"C3\"\nmemory = \"code\"\n")), "rva must not be 0"));
        CHECK(contains(parse_error(with_symbol("rva = 0x100000000\nbytes = \"C3\"\nmemory = \"code\"\n")),
                       "rva must be an integer"));
        CHECK(contains(parse_error(with_symbol("rva = \"0x10\"\nbytes = \"C3\"\nmemory = \"code\"\n")),
                       "rva must be an integer"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"?? ??\"\nmemory = \"code\"\n")), "at least one byte"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"48 8\"\nmemory = \"code\"\n")), "pairs of hex digits"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"48 ZZ\"\nmemory = \"code\"\n")), "'ZZ' is not a hex byte"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"4? 00\"\nmemory = \"code\"\n")), "'4?' is not a hex byte"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"C3\"\nmemory = \"exec\"\n")),
                       "memory must be \"code\", \"read\" or \"write\""));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nbytes = \"C3\"\n")), "needs 'memory'"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nmemory = \"code\"\n")), "needs 'bytes' or 'points_to'"));
        CHECK(contains(parse_error(with_symbol("rva = 0x10\nmemory = \"read\"\npoints_to = 0x20\nsignature = \"void ()\"\n")),
                       "more than one of"));
        CHECK(contains(parse_error(with_symbol(site + "signature = \"void*\"\n")), "must look like"));
        CHECK(contains(parse_error(with_symbol(site + "signature = \"void (int\"\n")), "must look like"));
        CHECK(contains(parse_error(with_symbol(site + "signature = \"(int)\"\n")), "must look like"));
        CHECK(parse_error(with_symbol(site + "signature = \"void (*)(int) (int (*cb)(void*), size_t)\"\n")).empty());
        CHECK(contains(parse_error(with_symbol(site + "resume = \"nowhere\"\n")), "'nowhere', which is not declared"));
        CHECK(contains(parse_error(with_symbol(site + "resume = \"s\"\n")), "resume names the symbol itself"));
        CHECK(contains(parse_error(with_symbol("export = \"F\"\nresume = \"s\"\n")), "resume needs 'bytes'"));
        CHECK(contains(parse_error(with_symbol(site + "note = 3\n")), "note must be a string"));
        CHECK(contains(parse_error(with_symbol(site + "values = 3\n")), "values must be a table"));
        CHECK(contains(parse_error("build = \"b\"\nsymbol = 3\n"), "symbol must be a table"));
        CHECK(contains(parse_error("build = \"b\"\nsymbol = [1]\n"), "symbol must be a table"));
        CHECK(contains(parse_error("build = \"b\"\n[symbol.s\n"), "symbols.toml:"));
    });

    run("symbols: values must lie on fixed bytes", [] {
        const std::string load = "rva = 0x1010\nmemory = \"code\"\nbytes = \"48 8B 05 ?? ?? ?? ?? 8B 98 D0 1D 00 00\"\n";
        CHECK(contains(parse_error(with_symbol(load + "values.root = { at = 3, rip_relative = 7 }\n")),
                       "byte +0x3 is \"??\"; values must lie on fixed bytes"));
        CHECK(contains(parse_error(with_symbol(load + "values.part = { at = 5, size = 4 }\n")), "values must lie on fixed bytes"));
        CHECK(parse_error(with_symbol(load + "values.date_offset = { at = 9, size = 4 }\n")).empty());
        CHECK(contains(parse_error(with_symbol(load + "values.x = { at = 10, size = 4 }\n")), "lie outside the declared 13 bytes"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = { at = 13, size = 1 }\n")), "lie outside"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = { at = 9, size = 3 }\n")), "size must be 1, 2, 4 or 8"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = { at = 9 }\n")), "exactly one of 'size' and 'rip_relative'"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = { at = 9, size = 4, rip_relative = 13 }\n")),
                       "exactly one of 'size' and 'rip_relative'"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = { size = 4 }\n")), "missing 'at'"));
        CHECK(contains(parse_error(with_symbol("rva = 0x1010\nmemory = \"code\"\nbytes = \"48 8B 05 F1 1F 00 00\"\n"
                                               "values.x = { at = 3, rip_relative = 6 }\n")),
                       "at least at + 4"));
        CHECK(contains(parse_error(with_symbol("rva = 0x2000\nmemory = \"read\"\npoints_to = 0x1000\n"
                                               "values.x = { at = 0, size = 8 }\n")),
                       "does not declare"));
        CHECK(contains(parse_error(with_symbol(load + "values.Root = { at = 9, size = 4 }\n")), "value name 'Root'"));
        CHECK(contains(parse_error(with_symbol(load + "values.x = 4\n")), "values.x must be a table"));
    });

    run("image: the file reader maps sections and zero-fills past raw data", [&] {
        uint8_t bytes[4] = {1, 1, 1, 1};
        CHECK(file_reader->read(0x3500, bytes, 4));
        CHECK(bytes[0] == 0 && bytes[3] == 0);
        uint8_t code[8] = {};
        CHECK(file_reader->read(0x1000, code, 8) && code[0] == 0x48 && code[7] == 0xC3);
        CHECK(!file_reader->read(TestImage::kSize - 1, bytes, 2));
        CHECK(file_reader->base() == TestImage::kPreferredBase);
        std::string e;
        CHECK(!FileImageReader::from_bytes(std::vector<uint8_t>(64), e) && !e.empty());
        auto truncated = test_image.file();
        truncated.resize(0x500);  // .data's raw bytes are gone
        CHECK(!FileImageReader::from_bytes(truncated, e) && contains(e, "past the end of the file"));
    });

    run("image: sections, kinds and exports", [&] {
        std::string e;
        const auto image = PeImage::parse(*file_reader, e);
        CHECK(image.has_value());
        if (!image) return;
        CHECK(image->sections().size() == 3);
        CHECK(image->section_of(0x1000, 8) != nullptr && image->section_of(0x1000, 8)->kind == MemoryKind::Code);
        CHECK(image->section_of(0x2000, 8)->kind == MemoryKind::Read);
        CHECK(image->section_of(0x3FF8, 8)->kind == MemoryKind::Write);
        CHECK(image->section_of(0x10FC, 8) == nullptr);  // crosses the end of .text
        CHECK(image->section_of(0x100, 1) == nullptr);   // headers
        CHECK(image->find_export("Exported_fn", e) == 0x1040u && e.empty());
        CHECK(!image->find_export("Exported", e) && e.empty());
        CHECK(!image->find_export("Exported_fn2", e) && e.empty());
        CHECK(!image->find_export("Forwarded_fn", e) && contains(e, "forwarded"));
    });

    run("verify: every kind passes on the file image, values computed", [&] {
        std::string e;
        const auto image = PeImage::parse(*file_reader, e);
        const SymbolVerification v = verify_symbols(symbols(kGoodSymbols), *image);
        for (const auto& p : v.problems) std::printf("  (problem) %s\n", p.c_str());
        CHECK(v.ok());
        CHECK(v.symbols.size() == 6);
        if (v.symbols.size() != 6) return;
        CHECK(v.symbols[0].rva == 0x1000);
        CHECK(value_of(v.symbols[1], "root") == 0x3008);
        CHECK(value_of(v.symbols[1], "date_offset") == 0x1DD0);
        CHECK(value_of(v.symbols[1], "opcode") == 0x48);
        CHECK(value_of(v.symbols[1], "modrm") == 0x988B);
        CHECK(value_of(v.symbols[1], "head") == 0x8B00001FF1058B48ull);
        CHECK(v.symbols[3].rva == 0x1040);  // resolved export
    });

    run("verify: mismatches name the symbol, the rva and the first differing byte", [&] {
        auto problems = verify(with_symbol("rva = 0x1000\nbytes = \"48 8B 80 18 04 00 00 C4\"\nmemory = \"code\"\n"),
                               *file_reader);
        CHECK(problems.size() == 1);
        if (!problems.empty()) {
            CHECK(contains(problems[0], "symbol s (rva 0x1000): byte +0x2 (rva 0x1002) expected 0x80, found 0x81"));
        }
        // ?? accepts any byte
        CHECK(verify(with_symbol("rva = 0x1000\nbytes = \"48 8B ?? ?? 04\"\nmemory = \"code\"\n"), *file_reader).empty());
    });

    run("verify: memory kind against the section flags", [&] {
        auto problems = verify(with_symbol("rva = 0x1000\nbytes = \"48\"\nmemory = \"read\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "declared read memory, section .text is code"));
        problems = verify(with_symbol("rva = 0x2000\nmemory = \"write\"\npoints_to = 0x1000\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "declared write memory, section .rdata is read"));
        problems = verify(with_symbol("rva = 0x3008\nmemory = \"code\"\ntype = \"Root\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "declared code memory, section .data is write"));
        problems = verify(with_symbol("rva = 0x10FC\nbytes = \"00 00 00 00 00 00 00 00\"\nmemory = \"code\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "do not lie inside one section"));
        problems = verify(with_symbol("rva = 0x5000\nbytes = \"00\"\nmemory = \"code\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "do not lie inside one section"));
    });

    run("verify: points_to compares the stored pointer minus the image base", [&] {
        auto problems = verify(with_symbol("rva = 0x2000\nmemory = \"read\"\npoints_to = 0x1008\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "points to rva 0x1000, expected rva 0x1008"));
        problems = verify(with_symbol("rva = 0x2100\nmemory = \"read\"\npoints_to = 0x1000\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "(outside the image)"));
    });

    run("verify: exports", [&] {
        CHECK(verify(with_symbol("export = \"Exported_fn\"\nsignature = \"void ()\"\n"), *file_reader).empty());
        auto problems = verify(with_symbol("export = \"Exported_fn\"\nbytes = \"40 54\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "symbol s (export Exported_fn, rva 0x1040): byte +0x1"));
        problems = verify(with_symbol("export = \"Exported_fn\"\nmemory = \"read\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "declared read memory"));
        problems = verify(with_symbol("export = \"Missing_fn\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "export Missing_fn not found"));
        problems = verify(with_symbol("export = \"Forwarded_fn\"\n"), *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "forwarded"));
    });

    run("verify: a RIP-relative value outside the image fails", [&] {
        auto problems = verify(with_symbol("rva = 0x1020\nbytes = \"48 8B 05 00 00 00 10\"\nmemory = \"code\"\n"
                                           "values.far = { at = 3, rip_relative = 7 }\n"),
                               *file_reader);
        CHECK(problems.size() == 1 && contains(problems[0], "value far points outside the image"));
    });

    run("verify: every failing symbol is reported, ids stay aligned", [&] {
        std::string e;
        const auto image = PeImage::parse(*file_reader, e);
        const SymbolVerification v = verify_symbols(
            symbols("build = \"b\"\n"
                    "[symbol.a]\nexport = \"Missing_fn\"\n"
                    "[symbol.b]\nrva = 0x1000\nbytes = \"00\"\nmemory = \"code\"\n"
                    "[symbol.c]\nexport = \"Exported_fn\"\n"),
            *image);
        CHECK(v.problems.size() == 2);
        CHECK(v.symbols.size() == 3 && v.symbols[2].rva == 0x1040);
    });

    run("verify: the same rules on the image mapped in memory", [&] {
        const MappedImage mapped(test_image);
        const MemoryImageReader reader(mapped.address(), TestImage::kSize);
        CHECK(reader.base() == mapped.address());
        CHECK(verify(kGoodSymbols, reader).empty());
        uint8_t byte = 0;
        CHECK(!reader.read(TestImage::kSize, &byte, 1));
        CHECK(!reader.read(0xFFFFFFFFu, &byte, 2));
    });

    run("loader: bytes another plugin changed are compared in their original form", [&] {
        const MappedImage mapped(test_image);
        ChangeRegistry patches(mapped.address(), TestImage::kSize);
        const uint8_t original[] = {0x48, 0x8B, 0x81, 0x18, 0x04, 0x00, 0x00, 0xC3};
        const uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const ChangeTarget at{mapped.address() + 0x1000, true, original, nullptr, sizeof(original)};
        CHECK(patches.patch_bytes(patches.owner("other"), at, nops, sizeof(nops)) == ChangeStatus::Ok);
        CHECK(mapped.base[0x1000] == 0x90);

        const MemoryImageReader live(mapped.address(), TestImage::kSize);
        const auto live_problems = verify(kGoodSymbols, live);
        CHECK(live_problems.size() == 1 && contains(live_problems[0], "symbol speed_object"));
        const OriginalImageReader shipped(patches, mapped.address());
        CHECK(verify(kGoodSymbols, shipped).empty());
        uint8_t copy[10] = {};
        CHECK(patches.read_original(mapped.address() + 0xFFF, copy, sizeof(copy)));
        CHECK(copy[1] == 0x48 && copy[8] == 0xC3);
        CHECK(!patches.read_original(mapped.address() + TestImage::kSize - 4, copy, 8));
    });

    run("loader: check_plugin_symbols applies the build of the file", [&] {
        const MappedImage mapped(test_image);
        ChangeRegistry patches(mapped.address(), TestImage::kSize);
        const auto folder = std::filesystem::temp_directory_path() /
                            ("anvil-symbols-tests-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);
        const auto file = folder / "symbols.toml";
        {
            std::ofstream out(file, std::ios::binary);
            out << "\xEF\xBB\xBF" << kGoodSymbols;  // with a UTF-8 BOM
        }
        SymbolVerification verified;
        CHECK(check_plugin_symbols(file, "test-build", patches, mapped.address(), &verified).empty());
        CHECK(verified.symbols.size() == 6 && value_of(verified.symbols[1], "root") == 0x3008);
        CHECK(contains(english(check_plugin_symbols(file, nullptr, patches, mapped.address())), "game build unknown"));
        CHECK(contains(english(check_plugin_symbols(file, "eu4-9.9.9", patches, mapped.address())),
                       "symbols.toml is for build test-build, the game is eu4-9.9.9"));
        {
            std::ofstream out(file, std::ios::binary);
            out << "build = \"test-build\"\n[symbol.s]\nrva = 0x1000\nbytes = \"00\"\nmemory = \"code\"\n"
                   "[symbol.t]\nrva = 0x1000\nbytes = \"01\"\nmemory = \"code\"\n";
        }
        const std::string reason = english(check_plugin_symbols(file, "test-build", patches, mapped.address()));
        CHECK(contains(reason, "symbol s (rva 0x1000): byte +0x0") && contains(reason, "and 1 more"));
        CHECK(contains(english(check_plugin_symbols(folder / "missing.toml", "test-build", patches, mapped.address())),
                       "cannot read missing.toml"));
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    });

    run("loader: verified symbols per plugin and changes on symbols", [&] {
        MappedImage mapped(test_image);
        DWORD old = 0;
        CHECK(VirtualProtect(mapped.base + 0x1000, 0x1000, PAGE_EXECUTE_READWRITE, &old));
        ChangeRegistry changes(mapped.address(), TestImage::kSize);
        const OwnerId owner = changes.add_owner("p");
        const SymbolFile file = symbols(kGoodSymbols);
        const OriginalImageReader reader(changes, mapped.address());
        std::string error;
        const auto image = PeImage::parse(reader, error);
        CHECK(image.has_value());
        if (!image) return;
        const SymbolVerification verified = verify_symbols(file, *image);
        CHECK(verified.ok());
        const PluginSymbols syms(file, verified, mapped.address());
        CHECK(syms.size() == 6 && syms.get(6) == nullptr);
        CHECK(syms.get(0)->declaration.name == "speed_object" && syms.get(0)->address == mapped.address() + 0x1000);
        CHECK(syms.address_of(3) == mapped.base + 0x1040);  // resolved export
        CHECK(syms.address_of(99) == nullptr);
        CHECK(syms.value(1, "root") == 0x3008u && syms.value(1, "date_offset") == 0x1DD0u);
        CHECK(!syms.value(1, "missing") && !syms.value(42, "root"));

        void* next = nullptr;
        auto* detour = reinterpret_cast<void*>(+[](void*) -> void* { return nullptr; });
        CHECK(hook_function(changes, owner, syms, 99, detour, &next) == ChangeStatus::NotDeclared);
        CHECK(hook_function(changes, owner, syms, 2, detour, &next) == ChangeStatus::WrongKind);  // a slot
        CHECK(redirect_call(changes, owner, syms, 0, detour, &next) == ChangeStatus::WrongKind);  // a function
        CHECK(redirect_call(changes, owner, syms, 1, detour, &next) == ChangeStatus::WrongKind);  // not call rel32
        const uint8_t nops[2] = {0x90, 0x90};
        CHECK(patch_bytes(changes, owner, syms, 2, nops, 2) == ChangeStatus::WrongKind);       // swap_pointer
        // A slot in read-only memory: the pointer must still be its points_to.
        CHECK(VirtualProtect(mapped.base + 0x2000, 0x1000, PAGE_READONLY, &old));
        void* previous = nullptr;
        int replacement = 0;
        CHECK(swap_pointer(changes, owner, syms, 99, &replacement, &previous) == ChangeStatus::NotDeclared);
        CHECK(swap_pointer(changes, owner, syms, 0, &replacement, &previous) == ChangeStatus::WrongKind);
        ChangeId swapped = 0;
        CHECK(swap_pointer(changes, owner, syms, 2, &replacement, &previous, &swapped) == ChangeStatus::Ok);
        CHECK(previous == mapped.base + 0x1000 && swapped != 0);
        CHECK(*reinterpret_cast<void**>(mapped.base + 0x2000) == &replacement);
        CHECK(changes.remove(owner, swapped) == ChangeStatus::Ok);
        CHECK(*reinterpret_cast<void**>(mapped.base + 0x2000) == mapped.base + 0x1000);
        CHECK(VirtualProtect(mapped.base + 0x2000, 0x1000, PAGE_READWRITE, &old));
        void* const moved = mapped.base + 0x1008;
        std::memcpy(mapped.base + 0x2000, &moved, sizeof(moved));
        CHECK(swap_pointer(changes, owner, syms, 2, &replacement, &previous) == ChangeStatus::BytesMismatch);
        void* const back = mapped.base + 0x1000;
        std::memcpy(mapped.base + 0x2000, &back, sizeof(back));
        CHECK(patch_bytes(changes, owner, syms, 4, nops, 2) == ChangeStatus::InvalidArgument);  // no declared bytes
        CHECK(patch_bytes(changes, owner, syms, 1, nops, 14) == ChangeStatus::InvalidArgument);  // past the bytes
        ChangeId patched = 0;
        CHECK(patch_bytes(changes, owner, syms, 1, nops, 2, &patched) == ChangeStatus::Ok);
        CHECK(mapped.base[0x1010] == 0x90 && patched != 0);
        // A function symbol: mov rax, [rcx+0x418]; ret.
        CHECK(hook_function(changes, owner, syms, 0, detour, &next) == ChangeStatus::Ok);
        CHECK(next != nullptr && mapped.base[0x1000] != 0x48);
        using Speed = void* (*)(void*);
        struct Session {
            uint8_t pad[0x418];
            void* speed;
        } session{{}, &session};
        CHECK(reinterpret_cast<Speed>(next)(&session) == &session);  // the original through the chain
        CHECK(reinterpret_cast<Speed>(mapped.base + 0x1000)(&session) == nullptr);
        CHECK(changes.rollback(owner) == 2 && mapped.base[0x1000] == 0x48 && mapped.base[0x1010] == 0x48);
    });

    run("loader: mid hooks and block replacements on symbols", [&] {
        MappedImage mapped(test_image);
        DWORD old = 0;
        CHECK(VirtualProtect(mapped.base + 0x1000, 0x1000, PAGE_EXECUTE_READWRITE, &old));
        ChangeRegistry changes(mapped.address(), TestImage::kSize);
        const OwnerId owner = changes.add_owner("p");
        const SymbolFile file = symbols(R"toml(
build = "test-build"

[symbol.block]
rva = 0x1010
bytes = "48 8B 05 F1 1F 00 00 8B 98 D0 1D 00 00"
memory = "code"
resume = "speed_object"

[symbol.speed_object]
rva = 0x1000
bytes = "48 8B 81 18 04 00 00 C3"
memory = "code"
signature = "void* (void* session)"

[symbol.exported]
export = "Exported_fn"
bytes = "40 53"
memory = "code"
signature = "void ()"

[symbol.tick_slot]
rva = 0x2000
memory = "read"
points_to = 0x1000

[symbol.bss_tail]
rva = 0x3500
bytes = "00 00 00 00 00 00"
memory = "write"
)toml");
        const OriginalImageReader reader(changes, mapped.address());
        std::string error;
        const auto image = PeImage::parse(reader, error);
        CHECK(image.has_value());
        if (!image) return;
        const SymbolVerification verified = verify_symbols(file, *image);
        CHECK(verified.ok());
        const PluginSymbols syms(file, verified, mapped.address());
        CHECK(syms.find("speed_object") == syms.get(1) && syms.find("nothing") == nullptr);

        const AnvilCpuCallback callback = [](AnvilCpuContext* cpu, void*) { cpu->rcx = cpu->rdx; };
        void* next = nullptr;
        auto* detour = reinterpret_cast<void*>(+[](void*) -> void* { return nullptr; });
        // Fewer than the 5 declared bytes of the jump: unsafe for every hook.
        CHECK(mid_hook(changes, owner, syms, 2, callback, nullptr) == ChangeStatus::UnsafeSite);
        CHECK(replace_block(changes, owner, syms, 2, callback, nullptr) == ChangeStatus::UnsafeSite);
        CHECK(hook_function(changes, owner, syms, 2, detour, &next) == ChangeStatus::UnsafeSite);
        CHECK(hook_function(changes, owner, syms, 0, detour, &next) == ChangeStatus::WrongKind);  // not a function
        CHECK(mid_hook(changes, owner, syms, 3, callback, nullptr) == ChangeStatus::WrongKind);   // a slot
        CHECK(mid_hook(changes, owner, syms, 4, callback, nullptr) == ChangeStatus::WrongKind);   // not code
        CHECK(replace_block(changes, owner, syms, 4, callback, nullptr) == ChangeStatus::WrongKind);
        CHECK(mid_hook(changes, owner, syms, 9, callback, nullptr) == ChangeStatus::NotDeclared);
        CHECK(replace_block(changes, owner, syms, 9, callback, nullptr) == ChangeStatus::NotDeclared);

        // A mid hook on a function symbol: rcx (the session) taken from rdx.
        ChangeId mid = 0;
        CHECK(mid_hook(changes, owner, syms, 1, callback, nullptr, &mid) == ChangeStatus::Ok);
        struct Session {
            uint8_t pad[0x418];
            void* speed;
        } first{{}, &first}, second{{}, &second};
        using Speed = void* (*)(void*, void*);
        CHECK(reinterpret_cast<Speed>(mapped.base + 0x1000)(&first, &second) == &second);
        CHECK(changes.remove(owner, mid) == ChangeStatus::Ok);
        CHECK(reinterpret_cast<Speed>(mapped.base + 0x1000)(&first, &second) == &first);

        // The block is the declared bytes; it resumes at its `resume` symbol.
        ChangeId block = 0;
        CHECK(replace_block(changes, owner, syms, 0, callback, nullptr, &block) == ChangeStatus::Ok);
        CHECK(block != 0 && mapped.base[0x1010] == 0xE9);
        CHECK(mid_hook(changes, owner, syms, 1, callback, nullptr) == ChangeStatus::Ok);  // outside the block
        CHECK(changes.describe(owner) ==
              (std::vector<std::string>{"block replacement rva 0x1010 (13 B)", "mid hook rva 0x1000"}));
        CHECK(target_of(*syms.get(0)).symbol == std::string("block"));  // named in the call counters
        CHECK(changes.rollback(owner) == 2 && mapped.base[0x1010] == 0x48 && mapped.base[0x1000] == 0x48);
    });

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
