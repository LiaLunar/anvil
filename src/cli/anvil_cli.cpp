// anvil.exe: the command-line tool for plugin authors (design §16). It links
// anvil_core, so it applies the same manifest, symbols.toml and verification
// rules as the loader in version.dll.
//
// Exit codes: 0 OK, 1 problems found, 2 cannot check (also usage errors).
#include "cli/check.hpp"
#include "cli/game.hpp"
#include "cli/gen.hpp"
#include "cli/port.hpp"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <windows.h>

namespace {

using namespace anvil::cli;

void print_usage(std::FILE* out) {
    std::fputs("Usage: anvil <command> [options]\n"
               "       anvil --version\n"
               "\n"
               "Tools for Anvil plugin authors.\n"
               "\n"
               "Commands:\n"
               "  gen [--lang c|cpp|rust] [--settings <settings.toml>] <symbols.toml|settings.toml>... <out-dir>\n"
               "      write symbols.gen.h (C) and symbols.gen.hpp (C++) from symbols.toml, and\n"
               "      settings.gen.h and settings.gen.hpp from settings.toml (--settings, a file named\n"
               "      *settings.toml or one with [setting.<name>] tables); both languages without --lang.\n"
               "      --lang rust writes symbols.gen.rs and settings.gen.rs only.\n"
               "      A file whose content would not change is not rewritten.\n"
               "  check <plugin-dir> [--exe <eu4.exe>] [--format text|json|github]\n"
               "      check plugin.toml, symbols.toml and settings.toml against their schemas. With\n"
               "      --exe, also the game build, every symbol (bytes, memory, pointers, exports, values) and\n"
               "      instruction boundaries, and write symbols.lock; without it, check that\n"
               "      symbols.lock matches symbols.toml.\n"
               "  check --game <EU4 folder> [--format text|json|github] [--user-dir <folder>]\n"
               "      check every plugin in <folder>/anvil/plugins against <folder>/eu4.exe (nothing is\n"
               "      written), then resolve them as the game does at start-up: the load order, what is\n"
               "      skipped and why (anvil.toml, API, build, conflicts, dependencies), and warnings\n"
               "      for plugins whose code ranges overlap. Plugins in the mods that dlc_load.json in\n"
               "      EU4's user folder enables (--user-dir, or the folder the game uses) are listed as\n"
               "      approved, update pending, pending, in conflict, shadowed or invalid; the approved\n"
               "      ones take part in the resolution.\n"
               "  port <symbols.toml> --from <old eu4.exe> --to <new eu4.exe> --build <new id> [--out <file>]\n"
               "       [--from-build <id>] [--format text|json|github]\n"
               "      place every symbol in a new build of the game: reads each symbol's bytes from the old\n"
               "      eu4.exe, compares the code of both builds (relative addresses aside) and writes the new\n"
               "      build's tables to --out, keeping the rest of the file. Reads both eu4.exe files, never\n"
               "      guesses: what has no certain place stays by hand, with the reason. Then run anvil check.\n"
               "\n"
               "Options:\n"
               "  -h, --help    show this help\n"
               "  --version     show the Anvil version\n"
               "\n"
               "Exit codes: 0 OK, 1 problems found, 2 cannot check.\n",
               out);
}

int usage_error(const std::string& message) {
    std::fprintf(stderr, "anvil: %s\n\n", message.c_str());
    print_usage(stderr);
    return kExitCannotCheck;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// "--name value" or "--name=value"; advances `i` past what it used.
std::optional<std::wstring> option_value(const std::vector<std::wstring>& args, size_t& i, std::wstring_view name) {
    const std::wstring& arg = args[i];
    if (arg == name) {
        if (i + 1 >= args.size()) return std::nullopt;
        return args[++i];
    }
    if (arg.size() > name.size() && arg.starts_with(name) && arg[name.size()] == L'=') return arg.substr(name.size() + 1);
    return std::nullopt;
}

bool is_option(const std::wstring& arg, std::wstring_view name) {
    return arg == name || (arg.starts_with(name) && arg.size() > name.size() && arg[name.size()] == L'=');
}

// Writes `content` unless the file already holds it (keeps build systems from
// rebuilding plugins whose symbols did not change).
bool write_if_changed(const std::filesystem::path& path, const std::string& content, bool& written) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream existing;
        existing << in.rdbuf();
        if (existing.str() == content) {
            written = false;
            return true;
        }
    }
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    written = true;
    return static_cast<bool>(out);
}

// A settings.toml for gen: its name ends in "settings.toml", or its top level
// holds [setting.<name>] tables (symbols.toml has `build` and `symbol`).
bool is_settings_file(const std::filesystem::path& file) {
    std::wstring name = file.filename().wstring();
    for (wchar_t& ch : name) ch = static_cast<wchar_t>(towlower(ch));
    if (name.ends_with(L"settings.toml")) return true;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const size_t start = line.find_first_not_of(" \t\xEF\xBB\xBF");
        if (start != std::string::npos && line.compare(start, 9, "[setting.") == 0) return true;
    }
    return false;
}

int run_gen(const std::vector<std::wstring>& args) {
    bool c = true, cpp = true, rust = false;
    std::vector<std::wstring> positional;
    std::vector<std::wstring> settings_files;  // --settings: whatever the name or content
    for (size_t i = 2; i < args.size(); ++i) {
        if (is_option(args[i], L"--lang")) {
            const auto lang = option_value(args, i, L"--lang");
            if (lang == L"c") cpp = false;
            else if (lang == L"cpp") c = false;
            else if (lang == L"rust") c = cpp = false, rust = true;
            else return usage_error("gen: --lang must be c, cpp or rust");
        } else if (is_option(args[i], L"--settings")) {
            const auto file = option_value(args, i, L"--settings");
            if (!file || file->empty()) return usage_error("gen: --settings needs the path of settings.toml");
            settings_files.push_back(*file);
        } else if (args[i].starts_with(L"--")) {
            return usage_error("gen: unknown option " + utf8(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.empty() || positional.size() + settings_files.size() < 2) {
        return usage_error("gen needs <symbols.toml|settings.toml>... and <out-dir>");
    }
    const std::filesystem::path out_dir(positional.back());
    positional.pop_back();
    std::vector<std::pair<std::filesystem::path, bool>> inputs;  // the file, and whether it is a settings file
    for (const auto& file : positional) inputs.emplace_back(file, is_settings_file(file));
    for (const auto& file : settings_files) inputs.emplace_back(file, true);

    // Every input first, so that nothing is written when one of them fails.
    struct Output {
        std::string stem;  // "symbols" or "settings"
        GeneratedHeaders headers;
    };
    std::vector<Output> outputs;
    for (const auto& [source, settings] : inputs) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) {
            std::fprintf(stderr, "anvil gen: cannot read %s\n", shown(source).c_str());
            return kExitCannotCheck;
        }
        const auto name_u8 = source.filename().u8string();
        const std::string name(name_u8.begin(), name_u8.end());
        const char* prefix = settings ? "settings.toml: " : "symbols.toml: ";
        auto report = [&](std::string message, uint32_t line) {
            if (message.starts_with(prefix)) message.erase(0, std::strlen(prefix));
            std::printf("%s%s: error: %s\n", shown(source).c_str(), line != 0 ? (":" + std::to_string(line)).c_str() : "",
                        message.c_str());
            return kExitProblems;
        };
        Output out{settings ? "settings" : "symbols", {}};
        if (settings) {
            const anvil::SettingsFileResult parsed = anvil::load_settings(source);
            if (!parsed.file) return report(parsed.error, parsed.line);
            out.headers = generate_settings_headers(*parsed.file, name, c, cpp, rust);
        } else {
            const anvil::SymbolFileResult parsed = anvil::load_symbols(source);
            if (!parsed.file) return report(parsed.error, parsed.line);
            out.headers = generate_headers(*parsed.file, name, c, cpp, rust);
        }
        if (!out.headers.error.empty()) return report(out.headers.error, out.headers.line);
        for (const auto& warning : out.headers.warnings) {
            std::printf("%s%s: warning: %s\n", shown(source).c_str(),
                        warning.line != 0 ? (":" + std::to_string(warning.line)).c_str() : "", warning.message.c_str());
        }
        for (const auto& other : outputs) {
            if (other.stem == out.stem) return usage_error("gen: more than one " + out.stem + " file");
        }
        outputs.push_back(std::move(out));
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    for (const auto& output : outputs) {
        for (const auto& [extension, content, wanted] : {std::tuple{".gen.h", &output.headers.c, c},
                                                         std::tuple{".gen.hpp", &output.headers.cpp, cpp},
                                                         std::tuple{".gen.rs", &output.headers.rust, rust}}) {
            if (!wanted) continue;
            const std::filesystem::path path = out_dir / (output.stem + extension);
            bool written = false;
            if (!write_if_changed(path, *content, written)) {
                std::fprintf(stderr, "anvil gen: cannot write %s\n", shown(path).c_str());
                return kExitCannotCheck;
            }
            std::printf("%s: %s\n", shown(path).c_str(), written ? "written" : "unchanged");
        }
    }
    return kExitOk;
}

int run_check(const std::vector<std::wstring>& args) {
    CheckOptions options;
    Format format = Format::Text;
    std::optional<std::wstring> game;
    std::optional<std::wstring> user_dir;
    std::vector<std::wstring> positional;
    for (size_t i = 2; i < args.size(); ++i) {
        if (is_option(args[i], L"--game")) {
            game = option_value(args, i, L"--game");
            if (!game || game->empty()) return usage_error("check: --game needs the EU4 folder");
        } else if (is_option(args[i], L"--user-dir")) {
            user_dir = option_value(args, i, L"--user-dir");
            if (!user_dir || user_dir->empty()) return usage_error("check: --user-dir needs EU4's user folder");
        } else if (is_option(args[i], L"--exe")) {
            const auto exe = option_value(args, i, L"--exe");
            if (!exe || exe->empty()) return usage_error("check: --exe needs the path of eu4.exe");
            options.exe = *exe;
        } else if (is_option(args[i], L"--format")) {
            const auto text = option_value(args, i, L"--format");
            const auto parsed = text ? parse_format(utf8(*text)) : std::nullopt;
            if (!parsed) return usage_error("check: --format must be text, json or github");
            format = *parsed;
        } else if (args[i].starts_with(L"--")) {
            return usage_error("check: unknown option " + utf8(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (user_dir && !game) return usage_error("check: --user-dir goes with --game");
    if (game) {
        if (!positional.empty() || !options.exe.empty()) {
            return usage_error("check: --game takes the EU4 folder only (not a <plugin-dir> or --exe)");
        }
        GameOptions game_options;
        game_options.game_dir = *game;
        if (user_dir) game_options.user_dir = *user_dir;
        const GameReport report = check_game(game_options);
        const std::string text = format_game_report(report, format);
        std::fputs(text.c_str(), format == Format::Text && report.exit_code == kExitCannotCheck && report.plugins.empty()
                                     ? stderr
                                     : stdout);
        return report.exit_code;
    }
    if (positional.size() != 1) return usage_error("check needs one <plugin-dir>");
    options.plugin_dir = positional[0];

    const CheckReport report = check_plugin(options);
    const std::string text = format_report(report, format);
    // Text: "cannot check" goes to stderr like usage errors; json and github stay on stdout for tools.
    std::fputs(text.c_str(), format == Format::Text && report.exit_code == kExitCannotCheck ? stderr : stdout);
    return report.exit_code;
}

int run_port(const std::vector<std::wstring>& args) {
    PortOptions options;
    Format format = Format::Text;
    std::vector<std::wstring> positional;
    for (size_t i = 2; i < args.size(); ++i) {
        const auto path_option = [&](std::wstring_view name, std::filesystem::path& out) {
            const auto value = option_value(args, i, name);
            if (value) out = *value;
            return value.has_value() && !value->empty();
        };
        const auto text_option = [&](std::wstring_view name, std::string& out) {
            const auto value = option_value(args, i, name);
            if (value) out = utf8(*value);
            return value.has_value() && !value->empty();
        };
        if (is_option(args[i], L"--from-build")) {
            if (!text_option(L"--from-build", options.from_build)) return usage_error("port: --from-build needs a build id");
        } else if (is_option(args[i], L"--from")) {
            if (!path_option(L"--from", options.from)) return usage_error("port: --from needs the old eu4.exe");
        } else if (is_option(args[i], L"--to")) {
            if (!path_option(L"--to", options.to)) return usage_error("port: --to needs the new eu4.exe");
        } else if (is_option(args[i], L"--build")) {
            if (!text_option(L"--build", options.build)) return usage_error("port: --build needs the id of the new build");
        } else if (is_option(args[i], L"--out")) {
            if (!path_option(L"--out", options.out)) return usage_error("port: --out needs a file");
        } else if (is_option(args[i], L"--format")) {
            const auto text = option_value(args, i, L"--format");
            const auto parsed = text ? parse_format(utf8(*text)) : std::nullopt;
            if (!parsed) return usage_error("port: --format must be text, json or github");
            format = *parsed;
        } else if (args[i].starts_with(L"--")) {
            return usage_error("port: unknown option " + utf8(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 1) return usage_error("port needs one <symbols.toml>");
    if (options.from.empty() || options.to.empty() || options.build.empty()) {
        return usage_error("port needs --from, --to and --build");
    }
    options.symbols = positional[0];
    const PortReport report = port_symbols(options);
    std::fputs(format_port_report(report, options.symbols, format).c_str(), stdout);
    return report.exit_code;
}

// UTF-8 output, so paths and messages survive the console and pipes. The code
// page belongs to the console, which outlives this process: restore it.
class Utf8Console {
public:
    Utf8Console() : previous_(GetConsoleOutputCP()) {
        if (previous_ != 0) SetConsoleOutputCP(CP_UTF8);
    }
    ~Utf8Console() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (previous_ != 0) SetConsoleOutputCP(previous_);
    }
    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;

private:
    UINT previous_;
};

int run(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        print_usage(stderr);
        return kExitCannotCheck;
    }
    const std::wstring& command = args[1];
    if (command == L"-h" || command == L"--help") {
        print_usage(stdout);
        return kExitOk;
    }
    if (command == L"--version") {
        std::printf("anvil %s\n", ANVIL_VERSION);
        return kExitOk;
    }
    if (command == L"gen") return run_gen(args);
    if (command == L"check") return run_check(args);
    if (command == L"port") return run_port(args);
    std::fprintf(stderr, "anvil: unknown command '%s'\n\n", utf8(command).c_str());
    print_usage(stderr);
    return kExitCannotCheck;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const Utf8Console console;
    return run(std::vector<std::wstring>(argv, argv + argc));
}
