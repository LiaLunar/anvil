#include "playset.hpp"

#include "game_build.hpp"
#include "json_scan.hpp"
#include "log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace anvil {

namespace fs = std::filesystem;

namespace {

// At most 1 MB: dlc_load.json and descriptors are small.
std::optional<std::string> read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text;
    char buffer[16384];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        text.append(buffer, static_cast<size_t>(in.gcount()));
        if (text.size() > (1u << 20)) return std::nullopt;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
}

fs::path from_utf8(std::string_view text) { return fs::path(widen(text)); }

std::string trimmed(std::string_view text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

bool equals_ascii_ci(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// <...>/content/236850/<digits>: a Steam Workshop item of EU4.
std::string workshop_id_of(const fs::path& folder) {
    const fs::path normal = folder.lexically_normal();
    fs::path item = normal.filename();
    fs::path parent = normal.parent_path();
    if (item.empty()) {  // a trailing separator
        item = parent.filename();
        parent = parent.parent_path();
    }
    const std::wstring id = item.wstring();
    if (id.empty() || id.find_first_not_of(L"0123456789") != std::wstring::npos) return {};
    if (!equals_ascii_ci(parent.filename().wstring(), L"236850")) return {};
    if (!equals_ascii_ci(parent.parent_path().filename().wstring(), L"content")) return {};
    return narrow(id);
}

// Paradox script tokens: = { } "quoted" and bare words; # starts a comment.
class ScriptTokens {
public:
    explicit ScriptTokens(std::string_view text) : text_(text) {}

    // false at the end. `quoted` tells a "{" string from the brace.
    bool next(std::string& token, bool& quoted) {
        quoted = false;
        for (;;) {
            while (at_ < text_.size() && std::string_view(" \t\r\n").find(text_[at_]) != std::string_view::npos) ++at_;
            if (at_ < text_.size() && text_[at_] == '#') {
                while (at_ < text_.size() && text_[at_] != '\n') ++at_;
                continue;
            }
            break;
        }
        if (at_ >= text_.size()) return false;
        const char c = text_[at_];
        if (c == '=' || c == '{' || c == '}') {
            token.assign(1, c);
            ++at_;
            return true;
        }
        token.clear();
        if (c == '"') {
            quoted = true;
            ++at_;
            while (at_ < text_.size() && text_[at_] != '"') {
                if (text_[at_] == '\\' && at_ + 1 < text_.size() && (text_[at_ + 1] == '"' || text_[at_ + 1] == '\\')) ++at_;
                token += text_[at_++];
            }
            if (at_ < text_.size()) ++at_;  // the closing quote
            return true;
        }
        while (at_ < text_.size() && std::string_view(" \t\r\n={}#\"").find(text_[at_]) == std::string_view::npos) {
            token += text_[at_++];
        }
        return true;
    }

private:
    std::string_view text_;
    size_t at_ = 0;
};

}  // namespace

fs::path eu4_user_dir(const fs::path& game_dir) {
    if (const auto text = read_text(game_dir / "userdir.txt")) {
        const std::string named = trimmed(*text);
        if (!named.empty()) return from_utf8(named);
    }
    // eu4.exe imports SHGetFolderPathW, so shell32.dll is loaded in the game
    // already; version.dll looks it up instead of importing it.
    using GetFolderPath = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
    HMODULE shell32 = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shell32 == nullptr) return {};
    const auto get = reinterpret_cast<GetFolderPath>(GetProcAddress(shell32, "SHGetFolderPathW"));
    wchar_t documents[MAX_PATH] = {};
    const bool ok = get != nullptr && SUCCEEDED(get(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents));
    FreeLibrary(shell32);
    if (!ok || documents[0] == 0) return {};
    return fs::path(documents) / L"Paradox Interactive" /
           (is_gog_folder(game_dir) ? L"Europa Universalis IV GOG" : L"Europa Universalis IV");
}

std::optional<fs::path> user_dir_argument(std::wstring_view line) {
    // Past the program's name, whose path may hold the same text.
    size_t at = line.starts_with(L'"') ? line.find(L'"', 1) : line.find(L' ');
    if (at == std::wstring_view::npos) return std::nullopt;
    constexpr std::wstring_view key = L"-userdir=";
    for (at = line.find(key, at + 1); at != std::wstring_view::npos; at = line.find(key, at + 1)) {
        const wchar_t before = line[at - 1];
        if (before != L' ' && before != L'\t' && before != L'"') continue;
        std::wstring_view value = line.substr(at + key.size());
        size_t end = std::wstring_view::npos;
        if (before == L'"') {
            end = value.find(L'"');
        } else {
            for (const std::wstring_view stop : {L" -", L" +", L" /", L" continue="}) end = std::min(end, value.find(stop));
        }
        value = value.substr(0, end);
        while (!value.empty() && value.back() == L' ') value.remove_suffix(1);
        if (value.empty()) return std::nullopt;
        return fs::path(value);
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> parse_dlc_load(std::string_view json, std::string& error) {
    if (json.starts_with("\xEF\xBB\xBF")) json.remove_prefix(3);
    JsonScanner scan(json);
    std::vector<std::string> mods;
    if (!scan.eat('{')) {
        error = "not a JSON object";
        return std::nullopt;
    }
    if (!scan.peek('}')) {
        do {
            const auto key = scan.string();
            if (!key || !scan.eat(':')) {
                error = "not a JSON object";
                return std::nullopt;
            }
            if (*key != "enabled_mods") {
                if (!scan.skip_value()) {
                    error = "not valid JSON";
                    return std::nullopt;
                }
                continue;
            }
            if (!scan.eat('[')) {
                error = "enabled_mods is not an array";
                return std::nullopt;
            }
            mods.clear();
            if (!scan.eat(']')) {
                do {
                    const auto mod = scan.string();
                    if (!mod) {
                        error = "enabled_mods holds something other than strings";
                        return std::nullopt;
                    }
                    mods.push_back(*mod);
                } while (scan.eat(','));
                if (!scan.eat(']')) {
                    error = "enabled_mods is not an array of strings";
                    return std::nullopt;
                }
            }
        } while (scan.eat(','));
    }
    if (!scan.eat('}') || !scan.at_end()) {
        error = "not valid JSON";
        return std::nullopt;
    }
    return mods;
}

PlaysetMod parse_mod_descriptor(std::string_view text) {
    struct Token {
        std::string text;
        bool quoted = false;
    };
    std::vector<Token> list;
    ScriptTokens tokens(text);
    for (Token t; tokens.next(t.text, t.quoted);) list.push_back(t);
    const auto is = [&](size_t i, const char* what) { return i < list.size() && !list[i].quoted && list[i].text == what; };

    PlaysetMod mod;
    std::string path, archive;
    int depth = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (is(i, "{")) {
            ++depth;
        } else if (is(i, "}")) {
            if (depth > 0) --depth;
        } else if (depth == 0 && is(i + 1, "=") && i + 2 < list.size() && !is(i + 2, "{") && !is(i + 2, "}")) {
            const std::string& key = list[i].text;
            const std::string& value = list[i + 2].text;
            if (key == "name") mod.name = value;
            else if (key == "path") path = value;
            else if (key == "archive") archive = value;
            i += 2;
        }
    }
    if (!path.empty()) {
        mod.folder = from_utf8(path);
    } else if (!archive.empty()) {
        mod.error = "a zipped mod (archive = \"" + archive + "\"); plugins load only from a mod folder";
    } else {
        mod.error = "the descriptor has no path";
    }
    return mod;
}

Playset read_playset(const fs::path& user_dir) {
    Playset playset;
    playset.user_dir = user_dir;
    if (user_dir.empty()) return playset;
    const fs::path file = user_dir / "dlc_load.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return playset;
    playset.found = true;
    const auto text = read_text(file);
    if (!text) {
        playset.error = "cannot read " + narrow(file.wstring());
        return playset;
    }
    std::string error;
    const auto descriptors = parse_dlc_load(*text, error);
    if (!descriptors) {
        playset.error = narrow(file.wstring()) + ": " + error;
        return playset;
    }
    for (const std::string& descriptor : *descriptors) {
        const fs::path descriptor_file = (user_dir / from_utf8(descriptor)).lexically_normal();
        const auto content = read_text(descriptor_file);
        PlaysetMod mod;
        if (!content) {
            mod.error = "cannot read the descriptor " + narrow(descriptor_file.wstring());
        } else {
            mod = parse_mod_descriptor(*content);
            if (!mod.folder.empty()) {
                mod.folder = (mod.folder.is_absolute() ? mod.folder : user_dir / mod.folder).lexically_normal();
                mod.workshop_id = workshop_id_of(mod.folder);
            }
        }
        mod.descriptor = descriptor;
        if (mod.name.empty()) mod.name = narrow(descriptor_file.filename().wstring());
        playset.mods.push_back(std::move(mod));
    }
    return playset;
}

}  // namespace anvil
