#pragma once

#include "cli/game.hpp"
#include "core/crash_history.hpp"
#include "install.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

// The problem report of the Log tab (issue #186): what the bug forms ask for,
// in their order, as Markdown to paste into an issue. It stays English, as
// anvil.log does, since maintainers read it. Nothing is uploaded: the Manager
// puts it on the clipboard and shows it.

// A folder whose path is replaced by a placeholder before the report leaves
// the Manager: the game folder, EU4's user folder, the Windows user folder.
struct PrivateFolder {
    std::filesystem::path path;
    std::string placeholder;  // "<game>"
};

struct ProblemReportInput {
    std::string manager_version;
    std::optional<Installation> installation;  // nullopt: not read yet
    std::string exe_sha256;                     // of the folder's eu4.exe
    std::string build;                          // a build Anvil knows, or empty
    bool gog = false;                           // a GOG install
    std::string gog_version;                    // GOG's name for it, or empty
    const cli::GameReport* game = nullptr;      // `anvil check --game`; nullptr without an anvil folder
    std::optional<CrashSummary> newest_crash;
    std::string log_name;  // "anvil.log"
    std::string log_tail;  // its last lines (read_text_tail)
    std::vector<PrivateFolder> folders;
    std::string user_name;  // the Windows user name, also replaced where it is a path component
};

// The last lines of anvil.log the report carries at most.
inline constexpr size_t kProblemReportLogLines = 150;
inline constexpr size_t kProblemReportLogBytes = 32 << 10;

std::string problem_report(const ProblemReportInput& input);

// `text` with every private folder replaced by its placeholder (any case,
// either slash), then any other `X:\Users\<name>` by `<user folder>`, then
// `user_name` where it is a whole path component by `<user>`.
std::string redact_paths(std::string_view text, const std::vector<PrivateFolder>& folders, std::string_view user_name);

}  // namespace anvil::manager
