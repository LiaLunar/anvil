#include "cli/check.hpp"

#include "cli/gen.hpp"
#include "core/game_build.hpp"
#include "core/instructions.hpp"
#include "core/manifest.hpp"
#include "core/pe_image.hpp"
#include "core/settings.hpp"
#include "core/symbols.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

namespace anvil::cli {
namespace {

void set_skip(CheckReport& report, i18n::Message why) {
    report.skip_reason = i18n::english(why);
    report.skip_why = std::move(why);
}

// symbols.lock: format 1 for a symbols.toml with `build`, 2 for one with `builds`.
const std::set<std::string_view> kLockKeys = {"format", "tool", "symbols_sha256", "exe_sha256", "build", "result"};
const std::set<std::string_view> kLockKeys2 = {"format", "tool", "symbols_sha256", "check"};
const std::set<std::string_view> kLockCheckKeys = {"build", "exe_sha256", "result"};

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

bool read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.lexically_normal().generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// The path of a plugin file as the author passed the folder ("." gives "plugin.toml").
std::string shown_path(const std::filesystem::path& dir, const std::filesystem::path& name) { return shown(dir / name); }

std::filesystem::path utf8_path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

// Removes the prefix the parsers give their errors: the file is shown separately.
std::string without_prefix(std::string message, std::string_view prefix) {
    if (message.starts_with(prefix)) message.erase(0, prefix.size());
    return message;
}

const char* known_build(std::string_view sha256) {
    const KnownBuild* build = find_known_build_by_sha256(sha256);
    return build != nullptr ? build->id : nullptr;
}

// A problem of a symbol in the selected build: at its table for that build, if it has one.
uint32_t line_in_build(const Symbol& s) { return s.placement_line != 0 ? s.placement_line : s.line; }

// Compares the plugin's symbols with an eu4.exe file and writes symbols.lock.
// The file's symbols are those of the exe's build, if it declares it, else of its first.
void check_against_exe(const CheckOptions& options, SymbolFile file, const std::string& symbols_text,
                       const std::string& symbols_path, CheckReport& report) {
    auto cannot = [&](const std::string& why) {
        report.exit_code = kExitCannotCheck;
        report.cannot_check = why;
    };
    std::string error;
    const auto reader = FileImageReader::load(options.exe, error);
    if (!reader) return cannot("cannot read " + report.exe_name + ": " + error);
    const auto image = PeImage::parse(*reader, error);
    if (!image) return cannot(report.exe_name + ": " + error);
    report.exe_sha256 = sha256_file(options.exe);
    if (report.exe_sha256.empty()) return cannot("cannot hash " + report.exe_name);

    const auto& build_of = options.build_of ? options.build_of : known_build;
    const size_t before = report.problems.size();
    const bool several = file.builds_listed;
    bool declared = false;  // the exe's build is one of the file's
    if (const char* id = build_of(report.exe_sha256)) {
        report.exe_build = id;
        declared = select_build(file, id);
        if (!declared) {
            report.problems.push_back({symbols_path, file.build_line,
                                       "symbols.toml is for " + file.build_list() + ", " + report.exe_name + " is " +
                                           report.exe_build});
            set_skip(report, build_mismatch(file, report.exe_build));
        }
    } else {
        set_skip(report, unknown_build_reason(options.exe.parent_path()));
        report.problems.push_back({symbols_path, file.build_line,
                                   report.exe_name + " (SHA-256 " + report.exe_sha256 +
                                       ") is not a game build this Anvil knows; symbols.toml declares " + file.build_list()});
    }
    report.build = file.build;

    const SymbolVerification verification = verify_symbols(file, *image);
    std::set<uint32_t> failed;
    for (size_t i = 0; i < verification.problems.size(); ++i) {
        const Symbol& s = file.symbols[verification.failed[i]];
        failed.insert(s.id);
        report.problems.push_back({symbols_path, line_in_build(s),
                                   (several ? "in build " + file.build + ": " : std::string()) + verification.problems[i]});
    }
    // The loader checks the build first and gives the first failing symbol.
    if (report.skip_reason.empty() && !verification.problems.empty()) set_skip(report, symbol_mismatch(verification.problems));
    for (const auto& s : file.symbols) {
        if (failed.contains(s.id) || s.memory != MemoryKind::Code || s.bytes.empty()) continue;
        report.code_ranges.push_back(
            {s.name, line_in_build(s), verification.symbols[s.id].rva, static_cast<uint32_t>(s.bytes.size())});
    }
    for (const auto& s : file.symbols) {
        if (s.absent) report.absent.push_back(s.name);
    }
    // Instruction boundaries (design §16) of the symbols that verified.
    for (const auto& s : file.symbols) {
        if (failed.contains(s.id) || s.absent) continue;
        const uint32_t rva = verification.symbols[s.id].rva;
        const Section* section = image->section_of(rva, 1);
        if (section == nullptr) continue;
        const std::string where =
            (several ? "in build " + file.build + ": " : std::string()) + "symbol " + s.name + " (rva " + hex(rva) + "): ";
        if (has_instruction_boundaries(s, section->kind)) {
            const std::string problem = check_instruction_boundaries(s, rva, *reader);
            if (!problem.empty()) report.problems.push_back({symbols_path, line_in_build(s), where + problem});
        }
        // A block replacement continues at `resume`: it must be code.
        if (!s.resume.empty()) {
            const Symbol* target = file.find(s.resume);
            if (failed.contains(target->id)) continue;
            const Section* resume = image->section_of(verification.symbols[target->id].rva, 1);
            if (resume != nullptr && resume->kind != MemoryKind::Code) {
                report.problems.push_back({symbols_path, line_in_build(s),
                                           where + "resume names " + s.resume + ", which is not in code (section " +
                                               resume->name + ")"});
            }
        }
    }

    if (!options.write_lock) return;
    const std::filesystem::path lock_path = options.plugin_dir / kLockFile;
    std::string existing;
    const bool has_existing = read_file(lock_path, existing);
    SymbolsLock lock;
    lock.format = file.builds_listed ? 2 : 1;
    lock.tool = tool_name();
    lock.symbols_sha256 = symbols_digest(symbols_text);
    const LockCheck check{report.exe_sha256, report.exe_build.empty() ? "unknown" : report.exe_build,
                          report.problems.size() == before};
    if (lock.format == 1) {
        lock.checks.push_back(check);
    } else {
        // A check of a build symbols.toml does not declare belongs to no build: the lock is left alone.
        if (!declared) return;
        // The other builds' checks stay while symbols.toml is the same.
        std::optional<SymbolsLock> previous;
        if (has_existing) previous = parse_lock(existing).lock;
        for (const auto& build : file.builds) {
            if (build == check.build) {
                lock.checks.push_back(check);
                continue;
            }
            if (!previous || previous->format != 2 || previous->symbols_sha256 != lock.symbols_sha256) continue;
            for (const auto& c : previous->checks) {
                if (c.build == build) lock.checks.push_back(c);
            }
        }
    }
    const std::string text = lock_text(lock);
    if (has_existing && existing == text) {
        report.lock = "unchanged";
        return;
    }
    std::ofstream out(lock_path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) return cannot("cannot write " + shown_path(options.plugin_dir, kLockFile));
    report.lock = "written";
}

// Without the game: symbols.lock must describe this symbols.toml and a clean
// check, of every build when it lists `builds`.
void check_lock(const CheckOptions& options, const SymbolFile& file, const std::string& symbols_text,
                const std::string& symbols_path, CheckReport& report) {
    const std::string lock_path = shown_path(options.plugin_dir, kLockFile);
    std::string text;
    if (!read_file(options.plugin_dir / kLockFile, text)) {
        report.problems.push_back({symbols_path, 0,
                                   std::string(kLockFile) + " is missing: run anvil check --exe <eu4.exe> and commit it"});
        return;
    }
    const LockResult parsed = parse_lock(text);
    if (!parsed.lock) {
        report.problems.push_back({lock_path, parsed.line, parsed.error});
        return;
    }
    if (parsed.lock->symbols_sha256 != symbols_digest(symbols_text)) {
        report.problems.push_back({lock_path, parsed.symbols_line,
                                   "stale: symbols.toml changed after the last anvil check --exe; run it again and commit " +
                                       std::string(kLockFile)});
        return;
    }
    for (const LockCheck& c : parsed.lock->checks) {
        if (c.ok) continue;
        report.problems.push_back({lock_path, c.result_line,
                                   "the last anvil check --exe (eu4.exe " + c.build +
                                       ") found problems; fix symbols.toml and run it again"});
        return;
    }
    bool complete = true;
    for (const auto& build : file.builds) {
        if (!file.builds_listed) break;
        const auto& checks = parsed.lock->checks;
        if (std::any_of(checks.begin(), checks.end(), [&](const LockCheck& c) { return c.build == build; })) continue;
        report.problems.push_back({lock_path, 0,
                                   "no check of build " + build + ": run anvil check --exe with its eu4.exe and commit " +
                                       std::string(kLockFile)});
        complete = false;
    }
    if (complete) report.lock = "matches";
}

}  // namespace

std::string tool_name() { return "anvil " ANVIL_VERSION; }

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(c));
                out += escaped;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

// GitHub workflow commands: data escapes %, CR and LF; properties also : and ,.
std::string github_escape(std::string_view text, bool property) {
    std::string out;
    for (char c : text) {
        if (c == '%') out += "%25";
        else if (c == '\r') out += "%0D";
        else if (c == '\n') out += "%0A";
        else if (property && c == ':') out += "%3A";
        else if (property && c == ',') out += "%2C";
        else out.push_back(c);
    }
    return out;
}

std::optional<Format> parse_format(std::string_view text) {
    if (text == "text") return Format::Text;
    if (text == "json") return Format::Json;
    if (text == "github") return Format::Github;
    return std::nullopt;
}

std::string symbols_digest(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        normalized.push_back(text[i]);
    }
    return sha256_bytes(normalized.data(), normalized.size());
}

std::string lock_text(const SymbolsLock& lock) {
    std::string out = "# Written by anvil check --exe; anvil check without the game fails when it does not match\n"
                      "# symbols.toml. Commit it with symbols.toml.\n";
    out += "format = " + std::to_string(lock.format) + "\n";
    out += "tool = " + json_string(lock.tool) + "\n";
    out += "symbols_sha256 = " + json_string(lock.symbols_sha256) + "\n";
    for (const LockCheck& c : lock.checks) {
        if (lock.format != 1) out += "\n[[check]]\n";
        if (lock.format == 1) out += "exe_sha256 = " + json_string(c.exe_sha256) + "\n";
        out += "build = " + json_string(c.build) + "\n";
        if (lock.format != 1) out += "exe_sha256 = " + json_string(c.exe_sha256) + "\n";
        out += std::string("result = ") + (c.ok ? "\"ok\"" : "\"problems\"") + "\n";
    }
    return out;
}

LockResult parse_lock(std::string_view text) {
    LockResult result;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        result.error = std::string(e.description());
        result.line = static_cast<uint32_t>(e.source().begin.line);
        return result;
    }
    auto fail = [&](const std::string& message, const toml::node* node) {
        result.error = message;
        result.line = node != nullptr ? static_cast<uint32_t>(node->source().begin.line) : 0;
        return result;
    };
    const auto format = table["format"].value_exact<int64_t>();
    const std::set<std::string_view>& keys = format == 2 ? kLockKeys2 : kLockKeys;
    for (const auto& [key, node] : table) {
        if (!keys.contains(key.str())) return fail("unknown key '" + std::string(key.str()) + "'", &node);
    }
    if (!format) return fail("missing 'format'", nullptr);
    if (*format != 1 && *format != 2) {
        return fail("format " + std::to_string(*format) + " is not supported by " + tool_name() + "; update anvil",
                    table.get("format"));
    }
    SymbolsLock lock;
    lock.format = static_cast<int>(*format);
    for (const auto& [key, out] : {std::pair{"tool", &lock.tool}, std::pair{"symbols_sha256", &lock.symbols_sha256}}) {
        const auto value = table[key].value_exact<std::string>();
        if (!value) return fail(std::string("missing or invalid '") + key + "'", table.get(key));
        *out = *value;
    }
    // A check: the top of a format 1 lock, or one [[check]] table of format 2.
    auto read_check = [&](const toml::table& t) -> bool {
        LockCheck c;
        for (const auto& [key, out] : {std::pair{"exe_sha256", &c.exe_sha256}, std::pair{"build", &c.build}}) {
            const auto value = t[key].value_exact<std::string>();
            if (!value) {
                fail(std::string("missing or invalid '") + key + "'", t.get(key) != nullptr ? t.get(key) : &t);
                return false;
            }
            *out = *value;
        }
        const auto outcome = t["result"].value_exact<std::string>();
        if (!outcome || (*outcome != "ok" && *outcome != "problems")) {
            fail("result must be \"ok\" or \"problems\"", t.get("result") != nullptr ? t.get("result") : &t);
            return false;
        }
        c.ok = *outcome == "ok";
        c.result_line = static_cast<uint32_t>(t.get("result")->source().begin.line);
        lock.checks.push_back(std::move(c));
        return true;
    };
    if (lock.format == 1) {
        if (!read_check(table)) return result;
    } else if (const auto* checks = table.get("check")) {
        const auto* list = checks->as_array();
        if (list == nullptr || !list->is_array_of_tables()) return fail("check must be [[check]] tables", checks);
        for (const auto& item : *list) {
            const auto& t = *item.as_table();
            for (const auto& [key, node] : t) {
                if (!kLockCheckKeys.contains(key.str())) return fail("unknown key 'check." + std::string(key.str()) + "'", &node);
            }
            if (!read_check(t)) return result;
        }
    }
    result.symbols_line = static_cast<uint32_t>(table.get("symbols_sha256")->source().begin.line);
    result.lock = std::move(lock);
    return result;
}

CheckReport check_plugin(const CheckOptions& options) {
    CheckReport report;
    report.with_exe = !options.exe.empty();
    if (report.with_exe) {
        const auto name = options.exe.filename().u8string();
        report.exe_name.assign(name.begin(), name.end());
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.plugin_dir / "plugin.toml", ec)) {
        report.exit_code = kExitCannotCheck;
        report.cannot_check = "no plugin.toml in " + shown(options.plugin_dir);
        return report;
    }
    if (report.with_exe && !std::filesystem::is_regular_file(options.exe, ec)) {
        report.exit_code = kExitCannotCheck;
        report.cannot_check = "--exe: " + report.exe_name + " is not a file";
        return report;
    }
    const std::string manifest_path = shown_path(options.plugin_dir, "plugin.toml");

    const ManifestResult manifest = load_manifest(options.plugin_dir);
    if (!manifest.manifest) {
        report.problems.push_back({manifest_path, manifest.line, without_prefix(manifest.error, "plugin.toml: ")});
        set_skip(report, manifest.error);
        report.exit_code = kExitProblems;
        return report;
    }
    report.manifest = manifest.manifest;
    const Manifest& m = *manifest.manifest;
    report.plugin = m.id;
    report.has_symbols = !m.symbols.empty();
    if (report.has_symbols) {
        const std::filesystem::path relative = utf8_path(m.symbols);
        const std::string symbols_path = shown_path(options.plugin_dir, relative);
        std::string text;
        if (!read_file(options.plugin_dir / relative, text)) {
            report.exit_code = kExitCannotCheck;
            report.cannot_check = "cannot read " + symbols_path;
            set_skip(report, i18n::Message("cannot_read", {symbols_path}));
            return report;
        }
        if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // saved by Notepad, as load_symbols allows
        const SymbolFileResult symbols = parse_symbols_text(text);
        if (!symbols.file) {
            report.problems.push_back({symbols_path, symbols.line, without_prefix(symbols.error, "symbols.toml: ")});
            set_skip(report, symbols.error);
            report.exit_code = kExitProblems;
            return report;
        }
        report.build = symbols.file->build;
        report.builds = symbols.file->builds;
        report.symbols = symbols.file->symbols.size();
        // Names `anvil gen` refuses, in any language it writes, fail the build later.
        const GeneratedHeaders headers = generate_headers(*symbols.file, relative.generic_string(), true, true, true);
        if (!headers.error.empty()) {
            report.problems.push_back({symbols_path, headers.line, without_prefix(headers.error, "symbols.toml: ")});
        }
        if (report.with_exe) {
            check_against_exe(options, *symbols.file, text, symbols_path, report);
            if (report.exit_code == kExitCannotCheck) return report;
        } else {
            check_lock(options, *symbols.file, text, symbols_path, report);
        }
    }
    // settings.toml: the loader skips a plugin whose declarations do not parse.
    if (!m.settings.empty()) {
        const std::filesystem::path relative = utf8_path(m.settings);
        const std::string settings_path = shown_path(options.plugin_dir, relative);
        std::string text;
        if (!read_file(options.plugin_dir / relative, text)) {
            report.exit_code = kExitCannotCheck;
            report.cannot_check = "cannot read " + settings_path;
            if (report.skip_reason.empty()) set_skip(report, i18n::Message("cannot_read", {settings_path}));
            return report;
        }
        if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // saved by Notepad, as load_settings allows
        const SettingsFileResult settings = parse_settings_text(text);
        report.has_settings = true;
        if (settings.file) {
            report.settings = settings.file->settings.size();
            const GeneratedHeaders headers =
                generate_settings_headers(*settings.file, relative.generic_string(), true, true, true);
            if (!headers.error.empty()) {
                report.problems.push_back({settings_path, headers.line, without_prefix(headers.error, "settings.toml: ")});
            }
        } else {
            report.problems.push_back({settings_path, settings.line, without_prefix(settings.error, "settings.toml: ")});
            if (report.skip_reason.empty()) set_skip(report, settings.error);
        }
    }
    // In file order, so the output reads like a compiler's.
    std::stable_sort(report.problems.begin(), report.problems.end(),
                     [](const Problem& a, const Problem& b) { return std::tie(a.file, a.line) < std::tie(b.file, b.line); });
    report.exit_code = report.problems.empty() ? kExitOk : kExitProblems;
    return report;
}

std::string report_json(const CheckReport& report, const std::string& pad, const std::string& folder,
                        const std::string& status) {
    const bool nested = !folder.empty();
    const std::string in = pad + "  ";  // members of the object
    const char* result = report.exit_code == kExitCannotCheck ? "cannot-check"
                         : report.problems.empty()              ? "ok"
                                                                : "problems";
    std::string out = "{\n";
    if (nested) {
        out += in + "\"folder\": " + json_string(folder) + ",\n";
        if (!status.empty()) out += in + "\"status\": " + json_string(status) + ",\n";
    } else {
        out += in + "\"tool\": " + json_string(tool_name()) + ",\n";
    }
    out += in + "\"result\": \"" + result + "\",\n";
    out += in + "\"exit_code\": " + std::to_string(report.exit_code) + ",\n";
    if (!report.cannot_check.empty()) out += in + "\"cannot_check\": " + json_string(report.cannot_check) + ",\n";
    out += in + "\"plugin\": " + (report.plugin.empty() ? std::string("null") : json_string(report.plugin)) + ",\n";
    if (report.has_symbols) {
        out += in + "\"build\": " + json_string(report.build) + ",\n";
        out += in + "\"builds\": [";
        for (size_t i = 0; i < report.builds.size(); ++i) out += (i == 0 ? "" : ", ") + json_string(report.builds[i]);
        out += "],\n";
        out += in + "\"symbols\": " + std::to_string(report.symbols) + ",\n";
        if (report.with_exe) {
            out += in + "\"absent\": [";
            for (size_t i = 0; i < report.absent.size(); ++i) out += (i == 0 ? "" : ", ") + json_string(report.absent[i]);
            out += "],\n";
        }
    }
    if (report.has_settings) out += in + "\"settings\": " + std::to_string(report.settings) + ",\n";
    if (report.with_exe && !report.exe_sha256.empty()) {
        out += in + "\"exe\": {\"sha256\": " + json_string(report.exe_sha256) + ", \"build\": " +
               (report.exe_build.empty() ? std::string("null") : json_string(report.exe_build)) + "},\n";
    }
    if (!report.lock.empty()) out += in + "\"lock\": " + json_string(report.lock) + ",\n";
    out += in + "\"problems\": [";
    for (size_t i = 0; i < report.problems.size(); ++i) {
        const Problem& p = report.problems[i];
        out += std::string(i == 0 ? "\n" : ",\n") + in + "  {\"file\": " + json_string(p.file) +
               ", \"line\": " + std::to_string(p.line) + ", \"message\": " + json_string(p.message) + "}";
    }
    out += report.problems.empty() ? "]\n" + pad + "}" : "\n" + in + "]\n" + pad + "}";
    return out;
}

std::string problem_lines(const std::vector<Problem>& problems, Format format, const char* level) {
    std::string out;
    for (const Problem& p : problems) {
        if (format == Format::Github) {
            out += std::string("::") + level + " file=" + github_escape(p.file, true);
            if (p.line != 0) out += ",line=" + std::to_string(p.line);
            out += ",title=anvil check::" + github_escape(p.message, false) + "\n";
        } else {
            out += p.file + (p.line != 0 ? ":" + std::to_string(p.line) : "") + ": " + level + ": " + p.message + "\n";
        }
    }
    return out;
}

std::string report_summary(const CheckReport& report) {
    std::string summary = report.plugin.empty() ? std::string("plugin") : report.plugin;
    if (!report.has_symbols && !report.plugin.empty()) {
        summary += ": no symbols.toml (runs on every game build)";
    } else if (report.has_symbols) {
        std::string builds;
        for (const auto& b : report.builds) builds += (builds.empty() ? "" : ", ") + b;
        summary += ": " + std::to_string(report.symbols) + " symbol(s) for " + (builds.empty() ? report.build : builds);
        if (report.with_exe) {
            summary += ", checked against " + report.exe_name + " (" +
                       (report.exe_build.empty() ? std::string("unknown build") : report.exe_build) + ")";
            if (!report.absent.empty()) {
                std::string names;
                for (const auto& n : report.absent) names += (names.empty() ? "" : ", ") + n;
                summary += "; absent on " + report.build + ": " + names;
            }
        }
        if (!report.lock.empty()) summary += "; " + std::string(kLockFile) + " " + report.lock;
    }
    if (report.has_settings) summary += "; " + std::to_string(report.settings) + " setting(s)";
    summary += report.problems.empty() ? ": OK" : ": " + std::to_string(report.problems.size()) + " problem(s)";
    return summary;
}

std::string format_report(const CheckReport& report, Format format) {
    if (format == Format::Json) return report_json(report) + "\n";

    std::string out = problem_lines(report.problems, format, "error");
    if (report.exit_code == kExitCannotCheck) {
        out += format == Format::Github ? "::error title=anvil check::" + github_escape(report.cannot_check, false) + "\n"
                                        : "error: cannot check: " + report.cannot_check + "\n";
        return out;
    }
    return out + report_summary(report) + "\n";
}

}  // namespace anvil::cli
