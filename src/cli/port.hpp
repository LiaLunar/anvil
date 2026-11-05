#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::cli {

enum class Format;

struct PortOptions {
    std::filesystem::path symbols;  // symbols.toml
    std::filesystem::path from;     // the eu4.exe of a build it declares
    std::filesystem::path to;       // the eu4.exe of the new build
    std::filesystem::path out;      // empty: nothing is written
    std::string build;              // the id of the new build
    std::string from_build;         // empty: from the hash of `from`
};

// A symbol that needs no hand: where it is in the new build.
struct PortedSymbol {
    std::string name;
    uint32_t line = 0;
    std::optional<uint32_t> old_rva;
    std::optional<uint32_t> rva;  // empty for an export
    std::optional<uint32_t> points_to;
    std::string method;
    bool bytes_changed = false;  // the declared bytes differ in the new build
    std::string review;          // the place is certain, but look at this
};

// A symbol left for a person, and why. `candidate` is a place to look at.
struct ManualSymbol {
    std::string name;
    uint32_t line = 0;
    std::optional<uint32_t> old_rva;
    std::string reason;
    std::optional<uint32_t> candidate;
};

// Exit codes as for check: 0 every symbol placed, 1 some left by hand, 2 cannot port.
struct PortReport {
    int exit_code = 0;
    std::string cannot_port;
    std::string from_build;
    std::string build;
    size_t symbols = 0;
    size_t functions = 0;  // stretches of code in the old build
    size_t functions_paired = 0;
    size_t functions_changed = 0;
    std::vector<PortedSymbol> ported;
    std::vector<ManualSymbol> manual;
    bool written = false;
};

// `anvil port` (design §16): carries symbols.toml over to a new game build.
// Reads both eu4.exe files and writes only options.out.
PortReport port_symbols(const PortOptions& options);

std::string format_port_report(const PortReport& report, const std::filesystem::path& symbols, Format format);

}  // namespace anvil::cli
