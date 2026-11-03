#include "problem_report.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace anvil::manager {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

char fold(char c) {
    if (c == '/') return '\\';
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// For matching paths: ASCII lower case, one kind of slash. Same length as `text`.
std::string folded(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), fold);
    return out;
}

bool is_separator(char c) { return c == '\\' || c == '/'; }

// Replaces every match of `needle` (folded) in `text` that `accept` allows.
template <typename Accept>
std::string replace_folded(std::string_view text, std::string_view needle, std::string_view with, Accept accept) {
    if (needle.empty()) return std::string(text);
    const std::string key = folded(needle);
    const std::string hay = folded(text);
    std::string out;
    size_t from = 0;
    for (size_t at = hay.find(key); at != std::string::npos; at = hay.find(key, at)) {
        if (!accept(at, at + key.size())) {
            ++at;
            continue;
        }
        out.append(text.substr(from, at - from));
        out.append(with);
        from = at = at + key.size();
    }
    out.append(text.substr(from));
    return out;
}

// One cell of a Markdown table.
std::string cell(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\r' || c == '\n') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out.empty() ? "-" : out;
}

// A fence longer than every run of backticks in `text`.
std::string fence_for(std::string_view text) {
    size_t longest = 0, run = 0;
    for (const char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return std::string(std::max<size_t>(3, longest + 1), '`');
}

std::string installed_anvil(const std::optional<Installation>& installation) {
    if (!installation) return "not read";
    switch (installation->dll) {
    case Installation::Dll::Anvil:
        return installation->dll_version.empty() ? "installed (version unknown)" : installation->dll_version;
    case Installation::Dll::Foreign:
        return "another version.dll (not Anvil)";
    case Installation::Dll::Missing:
        break;
    }
    return "not installed";
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

void plugin_rows(std::string& out, const cli::GameReport& game) {
    for (const cli::GamePlugin& p : game.plugins) {
        const auto& manifest = p.report.manifest;
        std::string status = p.status.empty() ? "skipped" : p.status;
        if (p.status == "loaded") {
            const auto loaded = std::find_if(game.load_order.begin(), game.load_order.end(),
                                             [&](const cli::LoadedPlugin& l) { return l.folder == p.folder; });
            if (loaded != game.load_order.end() && !loaded->phase.empty()) status += " (" + loaded->phase + ")";
        } else if (p.status != "disabled") {
            const auto skipped = std::find_if(game.skipped.begin(), game.skipped.end(),
                                              [&](const cli::SkippedPlugin& s) { return s.folder == p.folder; });
            if (skipped != game.skipped.end() && !skipped->reason.empty()) {
                status += ": " + skipped->reason;
            } else if (!p.report.skip_reason.empty()) {
                status += ": " + p.report.skip_reason;
            }
        }
        out += "| " + cell(manifest ? manifest->id : p.folder) + " | " + cell(manifest ? manifest->version.to_string() : "") +
               " | " + cell("anvil/plugins/" + p.folder) + " | " + cell(status) + " |\n";
    }
    for (const cli::GameModPlugin& p : game.mod_plugins) {
        const std::string from = "mod '" + p.mod + "' (" + (p.workshop_id.empty() ? "local mod" : "Workshop " + p.workshop_id) + ")";
        std::string status = p.status.empty() ? "skipped" : p.status;
        if (!p.state.empty() && p.state != "approved") status += ", " + p.state;
        const std::string& reason = !p.skip_reason.empty() ? p.skip_reason : p.reason;
        if (p.status != "loaded" && !reason.empty()) status += ": " + reason;
        out += "| " + cell(p.id) + " | " + cell(p.version) + " | " + cell(from) + " | " + cell(status) + " |\n";
    }
}

}  // namespace

std::string redact_paths(std::string_view text, const std::vector<PrivateFolder>& folders, std::string_view user_name) {
    // Longer folders first, so the game folder inside the user folder keeps its own name.
    std::vector<const PrivateFolder*> order;
    for (const PrivateFolder& f : folders) {
        if (!f.path.empty()) order.push_back(&f);
    }
    std::sort(order.begin(), order.end(),
              [](const PrivateFolder* a, const PrivateFolder* b) { return a->path.native().size() > b->path.native().size(); });

    std::string out(text);
    for (const PrivateFolder* f : order) {
        std::string path = utf8(f->path);
        while (path.size() > 3 && is_separator(path.back())) path.pop_back();
        const std::string& current = out;
        out = replace_folded(current, path, f->placeholder, [&](size_t, size_t end) {
            // A whole folder: not the start of a longer name.
            if (end == current.size()) return true;
            const unsigned char next = static_cast<unsigned char>(current[end]);
            return !(std::isalnum(next) || next == '_' || next == '-' || next == '.' || next >= 0x80);
        });
    }

    static const std::regex users(R"(([A-Za-z]:[\\/]Users[\\/])[^\\/\r\n]+(?=[\\/]))", std::regex::icase);
    out = std::regex_replace(out, users, "<user folder>");

    if (user_name.size() >= 2) {
        const std::string& current = out;
        out = replace_folded(current, user_name, "<user>", [&](size_t begin, size_t end) {
            const bool after_separator = begin > 0 && is_separator(current[begin - 1]);
            const bool before_end = end == current.size() || is_separator(current[end]) || current[end] == '"' ||
                                    current[end] == '\'' || current[end] == ')' || std::isspace(static_cast<unsigned char>(current[end]));
            return after_separator && before_end;
        });
    }
    return out;
}

std::string problem_report(const ProblemReportInput& input) {
    std::string out;
    out += "### Anvil problem report\n\n";
    out += "Copied from Anvil Manager " + input.manager_version +
           ". Paths are shortened: `<game>` is the game folder, `<eu4 user folder>` EU4's user folder, `<user folder>` the "
           "Windows user folder.\n\n";

    out += "| | |\n| --- | --- |\n";
    out += "| Anvil in the game folder | " + cell(installed_anvil(input.installation)) + " |\n";
    out += "| Anvil Manager | " + cell(input.manager_version) + " |\n";
    out += "| Game build | " + cell(input.build.empty() ? "unknown to this Anvil" : "`" + input.build + "`") + " |\n";
    out += "| eu4.exe SHA-256 | " + cell(input.exe_sha256.empty() ? "" : "`" + input.exe_sha256 + "`") + " |\n";
    if (const cli::GameReport* game = input.game) {
        std::string toml = game->enabled ? "plugins enabled" : "all plugins disabled (`enabled = false`)";
        if (!game->disabled_plugins.empty()) toml += "; disabled_plugins: " + join(game->disabled_plugins);
        out += "| anvil.toml | " + cell(toml) + " |\n";
        out += "| Playset | " +
               cell(game->playset_found ? std::to_string(game->enabled_mods) + " enabled mod(s)" : "dlc_load.json not found") +
               " |\n";
    } else {
        out += "| anvil.toml | no anvil folder |\n";
    }
    if (input.newest_crash) {
        const CrashSummary& c = *input.newest_crash;
        std::string crash = c.name;
        if (!c.time.empty()) crash += " (" + c.time + ")";
        if (!c.summary.empty()) crash += ": " + c.summary;
        if (c.before_game_ready) crash += ", before the game was ready";
        out += "| Newest crash report | " + cell(crash) + " |\n";
    } else {
        out += "| Newest crash report | none |\n";
    }

    if (const cli::GameReport* game = input.game) {
        out += "\n#### Plugins at the next start\n\nAs the Manager's Plugins tab sees them; the log below is from the last start.\n\n";
        if (!game->cannot_check.empty()) out += "Cannot check the folder: " + game->cannot_check + "\n\n";
        if (game->plugins.empty() && game->mod_plugins.empty()) {
            out += "No plugins.\n";
        } else {
            out += "| Plugin | Version | From | Status |\n| --- | --- | --- | --- |\n";
            plugin_rows(out, *game);
        }
        std::vector<std::string> notes;
        for (const cli::Problem& p : game->problems) notes.push_back("anvil.toml: " + p.message);
        for (const cli::OverlapWarning& w : game->warnings) notes.push_back(w.message);
        for (const std::string& w : game->mod_warnings) notes.push_back(w);
        if (!notes.empty()) {
            out += "\n";
            for (const std::string& note : notes) out += "- " + note + "\n";
        }
    }

    out += "\n#### " + input.log_name + " (last lines)\n\n";
    if (input.log_tail.empty()) {
        out += input.log_name + " is empty or missing: Anvil writes it when the game starts.\n";
    } else {
        const std::string fence = fence_for(input.log_tail);
        out += fence + "text\n" + input.log_tail;
        if (input.log_tail.back() != '\n') out += "\n";
        out += fence + "\n";
    }
    return redact_paths(out, input.folders, input.user_name);
}

}  // namespace anvil::manager
