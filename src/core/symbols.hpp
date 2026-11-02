#pragma once

#include "i18n/message.hpp"
#include "pe_image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anvil {

// What a symbol is (design §5.2): a function (`signature`), a pointer slot
// (`points_to`), data (`type`) or a site (anything else).
enum class SymbolKind { Function, Slot, Data, Site };

const char* symbol_kind_name(SymbolKind kind);

// A constant taken from a symbol's verified bytes (`values.<name>`).
struct SymbolValue {
    std::string name;
    uint32_t at = 0;            // offset inside the symbol's bytes
    uint32_t size = 0;          // 1, 2, 4 or 8: little-endian unsigned; 0 with rip_relative
    uint32_t rip_relative = 0;  // > 0: disp32 at `at`, the value is the RVA rva + rip_relative + disp32
    std::string type;           // optional, for generated headers
};

struct Symbol {
    uint32_t id = 0;  // declaration order in the file: the plugin's AnvilSymbol
    std::string name;
    uint32_t line = 0;  // of its [symbol.<name>] table in symbols.toml
    SymbolKind kind = SymbolKind::Site;
    std::optional<uint32_t> rva;  // exactly one of rva and export_name
    std::string export_name;
    std::vector<uint8_t> bytes;  // expected bytes; may be empty
    std::vector<uint8_t> mask;   // 0xFF = compare, 0x00 = any byte ("??")
    std::optional<MemoryKind> memory;
    std::optional<uint32_t> points_to;  // RVA the 8-byte pointer at the symbol holds
    std::string type;
    std::string signature;  // checked for shape only; typed by `anvil gen`
    std::string resume;     // another symbol; empty = the end of the block
    std::vector<SymbolValue> values;
    std::string note;
};

struct SymbolFile {
    std::string build;  // the game build these symbols are for
    uint32_t build_line = 0;  // of `build` in symbols.toml
    std::vector<Symbol> symbols;  // in declaration order (index == id)

    const Symbol* find(std::string_view name) const;
};

struct SymbolFileResult {
    std::optional<SymbolFile> file;
    std::string error;  // set when file is empty
    uint32_t line = 0;  // the line of symbols.toml the error is about; 0 = the whole file
};

// Parses symbols.toml (design §5.2). Unknown keys, values of the wrong type
// and inconsistent declarations are errors; the image is not consulted.
SymbolFileResult parse_symbols_text(std::string_view text);
SymbolFileResult load_symbols(const std::filesystem::path& file);

struct VerifiedSymbol {
    uint32_t rva = 0;  // resolved (exports) or declared
    std::vector<std::pair<std::string, uint64_t>> values;  // in declaration order
};

struct SymbolVerification {
    std::vector<VerifiedSymbol> symbols;  // index == id; complete only when ok()
    std::vector<std::string> problems;    // one line per failing symbol
    std::vector<uint32_t> failed;         // the id of the symbol each problem is about

    bool ok() const { return problems.empty(); }
};

// Compares every symbol with the image (design §7): resolves exports, checks
// (actual & mask) == (expected & mask), the memory kind against the section
// flags and `points_to` against the stored pointer, and computes `values`.
// Does not compare `file.build` with the image's build; the caller does.
SymbolVerification verify_symbols(const SymbolFile& file, const PeImage& image);

// Why a plugin whose symbols do not verify is skipped: the first problem, and
// how many more there are.
i18n::Message symbol_mismatch(const std::vector<std::string>& problems);

}  // namespace anvil
