#pragma once

#include "core/manifest.hpp"
#include "i18n/message.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::cli {

// Exit codes (design §16).
inline constexpr int kExitOk = 0;
inline constexpr int kExitProblems = 1;
inline constexpr int kExitCannotCheck = 2;

enum class Format { Text, Json, Github };

std::optional<Format> parse_format(std::string_view text);

// A problem in one of the plugin's files. `file` is the path as the author
// passed it (the plugin folder joined with the file name), `line` is 1-based
// or 0 for the whole file.
struct Problem {
    std::string file;
    uint32_t line = 0;
    std::string message;
};

struct CheckOptions {
    std::filesystem::path plugin_dir;
    std::filesystem::path exe;  // empty: no --exe, check symbols.lock instead
    // With an exe: write symbols.lock. check --game leaves the installed plugins alone.
    bool write_lock = true;
    // The build id of an eu4.exe by its SHA-256 (upper-case hex), or nullptr
    // when the build is unknown. Empty: the table compiled into Anvil.
    std::function<const char*(std::string_view sha256)> build_of;
};

// A verified `memory = "code"` symbol with declared bytes: [rva, rva + size).
struct CodeRange {
    std::string symbol;
    uint32_t line = 0;  // of the symbol in symbols.toml
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct CheckReport {
    int exit_code = kExitOk;
    std::string cannot_check;  // why, with kExitCannotCheck
    std::string plugin;        // the plugin id, once plugin.toml parsed
    std::optional<Manifest> manifest;
    // Why the loader skips the plugin on its own account (a broken plugin.toml or
    // symbols.toml, a build or symbol that does not match the exe), in the
    // loader's words; empty = it passes. Only problems the loader acts on count:
    // instruction boundaries are checked by anvil only.
    std::string skip_reason;
    i18n::Message skip_why;  // skip_reason by code, for the player's language (#158)
    std::vector<CodeRange> code_ranges;  // with an exe, in declaration order
    bool has_symbols = false;
    std::string build;         // symbols.toml `build`
    size_t symbols = 0;
    bool has_settings = false;  // plugin.toml names a settings.toml that could be read
    size_t settings = 0;        // declared settings, once settings.toml parsed
    bool with_exe = false;
    std::string exe_name;      // the --exe file name
    std::string exe_sha256;
    std::string exe_build;     // empty: not a known build
    std::string lock;          // symbols.lock: "written", "unchanged", "matches" or empty
    std::vector<Problem> problems;
};

// `anvil check <plugin-dir> [--exe <eu4.exe>]` (design §16): the strict
// plugin.toml, symbols.toml and settings.toml schemas (the loader's parsers)
// and the names `anvil gen` refuses (generate_headers, generate_settings_headers,
// every language); with an exe the
// build, every symbol (verify_symbols) and instruction boundaries, then
// writes symbols.lock; without one, that symbols.lock matches symbols.toml.
CheckReport check_plugin(const CheckOptions& options);

std::string format_report(const CheckReport& report, Format format);

// The report as a JSON object, each line prefixed with `pad` (not the first).
// With a `folder` it is one entry of anvil check --game: it names the folder
// and, if given, its `status`, and leaves out the tool.
std::string report_json(const CheckReport& report, const std::string& pad = {}, const std::string& folder = {},
                        const std::string& status = {});

// One line per problem as `file:line: <level>: message` or a GitHub workflow command.
std::string problem_lines(const std::vector<Problem>& problems, Format format, const char* level);
// "<plugin>: 3 symbol(s) for <build>, ...: OK"
std::string report_summary(const CheckReport& report);

std::string tool_name();  // "anvil <version>"
std::string json_string(std::string_view text);
// GitHub workflow commands: data escapes %, CR and LF; properties also : and ,.
std::string github_escape(std::string_view text, bool property);

// symbols.lock (design §16), next to plugin.toml.
struct SymbolsLock {
    std::string tool;            // "anvil <version>" that wrote it
    std::string symbols_sha256;  // symbols_digest() of symbols.toml
    std::string exe_sha256;      // of the eu4.exe it was checked against
    std::string build;           // that eu4.exe's build id, or "unknown"
    bool ok = false;             // result = "ok" | "problems"
};

inline constexpr const char* kLockFile = "symbols.lock";

// SHA-256 of symbols.toml with a UTF-8 BOM removed and CRLF read as LF, so a
// checkout with other line endings (git autocrlf) gives the same digest.
std::string symbols_digest(std::string_view text);
std::string lock_text(const SymbolsLock& lock);

struct LockResult {
    std::optional<SymbolsLock> lock;
    std::string error;
    uint32_t line = 0;
    uint32_t symbols_line = 0;  // of symbols_sha256
    uint32_t result_line = 0;   // of result
};
LockResult parse_lock(std::string_view text);

}  // namespace anvil::cli
