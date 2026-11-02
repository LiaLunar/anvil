#include "game.hpp"

#include "core/crash_history.hpp"
#include "core/game_build.hpp"
#include "core/log.hpp"
#include "zip.hpp"
#include "i18n/translate.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

class VdfReader {
public:
    explicit VdfReader(std::string_view text) : text_(text) {}

    bool read_block(VdfNode& node, bool top, std::string& error) {
        for (;;) {
            skip_space();
            if (at_ == text_.size()) {
                if (top) return true;
                error = "a block is not closed";
                return false;
            }
            if (text_[at_] == '}') {
                if (top) {
                    error = "'}' without a block at line " + std::to_string(line_);
                    return false;
                }
                ++at_;
                return true;
            }
            std::string key;
            if (!read_string(key, error)) return false;
            skip_space();
            VdfNode child;
            if (at_ < text_.size() && text_[at_] == '{') {
                ++at_;
                if (++depth_ > 32) {
                    error = "blocks nested too deep";
                    return false;
                }
                if (!read_block(child, false, error)) return false;
                --depth_;
            } else if (!read_string(child.value, error)) {
                return false;
            }
            node.children.emplace_back(std::move(key), std::move(child));
        }
    }

private:
    void skip_space() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == '\n') {
                ++line_;
                ++at_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++at_;
            } else if (text_.substr(at_, 2) == "//") {
                while (at_ < text_.size() && text_[at_] != '\n') ++at_;
            } else {
                break;
            }
        }
        // A UTF-8 BOM at the start.
        if (at_ == 0 && text_.substr(0, 3) == "\xEF\xBB\xBF") {
            at_ = 3;
            skip_space();
        }
    }

    bool read_string(std::string& out, std::string& error) {
        if (at_ >= text_.size()) {
            error = "a value is missing at the end";
            return false;
        }
        if (text_[at_] != '"') {
            // Unquoted tokens run to the next space or brace.
            const size_t start = at_;
            while (at_ < text_.size() && std::string_view(" \t\r\n{}\"").find(text_[at_]) == std::string_view::npos) ++at_;
            if (at_ == start) {
                error = "unexpected '" + std::string(1, text_[at_]) + "' at line " + std::to_string(line_);
                return false;
            }
            out = std::string(text_.substr(start, at_ - start));
            return true;
        }
        ++at_;
        while (at_ < text_.size() && text_[at_] != '"') {
            char c = text_[at_++];
            if (c == '\n') ++line_;
            if (c == '\\' && at_ < text_.size()) {
                const char e = text_[at_++];
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            out += c;
        }
        if (at_ >= text_.size()) {
            error = "a string is not closed";
            return false;
        }
        ++at_;
        return true;
    }

    std::string_view text_;
    size_t at_ = 0;
    uint32_t line_ = 1;
    int depth_ = 0;
};

std::string read_small(const fs::path& file) { return read_text_file(file, 4 << 20).value_or(std::string()); }

fs::path from_utf8(std::string_view text) { return fs::path(widen(text)); }

std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view) {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | view, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

struct FileId {
    DWORD volume = 0;
    DWORD high = 0;
    DWORD low = 0;
};

std::optional<FileId> file_id(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) != 0;
    CloseHandle(file);
    if (!ok) return std::nullopt;
    return FileId{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
}

}  // namespace

const VdfNode* VdfNode::child(std::string_view key) const {
    for (const auto& [name, node] : children) {
        if (iequals(name, key)) return &node;
    }
    return nullptr;
}

std::optional<VdfNode> parse_vdf(std::string_view text, std::string& error) {
    VdfNode root;
    VdfReader reader(text);
    if (!reader.read_block(root, true, error)) return std::nullopt;
    return root;
}

std::vector<fs::path> steam_libraries(const VdfNode& libraryfolders) {
    std::vector<fs::path> with_game;
    std::vector<fs::path> others;
    const VdfNode* top = libraryfolders.child("libraryfolders");
    if (top == nullptr) return {};
    for (const auto& [key, node] : top->children) {
        if (!std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }) || key.empty()) continue;
        if (node.children.empty()) {
            if (!node.value.empty()) others.push_back(from_utf8(node.value));  // the old format
            continue;
        }
        const VdfNode* path = node.child("path");
        if (path == nullptr || path->value.empty()) continue;
        const VdfNode* apps = node.child("apps");
        (apps != nullptr && apps->child(kSteamAppId) != nullptr ? with_game : others).push_back(from_utf8(path->value));
    }
    with_game.insert(with_game.end(), others.begin(), others.end());
    return with_game;
}

std::vector<fs::path> find_steam_games(const fs::path& steam_root) {
    std::vector<fs::path> libraries;
    std::string error;
    for (const fs::path& vdf : {steam_root / "steamapps" / "libraryfolders.vdf", steam_root / "config" / "libraryfolders.vdf"}) {
        const std::string text = read_small(vdf);
        if (text.empty()) continue;
        if (const auto node = parse_vdf(text, error)) {
            libraries = steam_libraries(*node);
            if (!libraries.empty()) break;
        }
    }
    libraries.push_back(steam_root);  // Steam's own library, listed or not

    std::vector<fs::path> games;
    for (const fs::path& library : libraries) {
        const fs::path steamapps = library / "steamapps";
        std::string installdir = "Europa Universalis IV";
        if (const auto manifest = parse_vdf(read_small(steamapps / ("appmanifest_" + std::string(kSteamAppId) + ".acf")), error)) {
            const VdfNode* state = manifest->child("AppState");
            const VdfNode* dir = state != nullptr ? state->child("installdir") : nullptr;
            if (dir != nullptr && !dir->value.empty() && unsafe_path_reason(dir->value).empty() &&
                dir->value.find_first_of("/\\") == std::string::npos) installdir = dir->value;
        }
        const fs::path game = (steamapps / "common" / from_utf8(installdir)).lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(game / "eu4.exe", ec)) continue;
        if (std::none_of(games.begin(), games.end(), [&](const fs::path& known) { return same_file(known, game); })) {
            games.push_back(game);
        }
    }
    return games;
}

fs::path steam_root_from_registry() {
    std::wstring path = registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    if (path.empty()) path = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", RRF_SUBKEY_WOW6432KEY);
    if (path.empty()) path = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", RRF_SUBKEY_WOW6464KEY);
    return path.empty() ? fs::path() : fs::path(path).make_preferred().lexically_normal();
}

GameFolder inspect_game_folder(const fs::path& dir) {
    GameFolder out;
    out.dir = dir;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        out.error = i18n::tr("the folder does not exist");
        return out;
    }
    out.has_exe = fs::is_regular_file(dir / "eu4.exe", ec);
    if (!out.has_exe) {
        out.error = i18n::tr("there is no eu4.exe in this folder");
        return out;
    }
    out.exe_sha256 = sha256_file(dir / "eu4.exe");
    if (out.exe_sha256.empty()) {
        out.error = i18n::tr("eu4.exe cannot be read");
        return out;
    }
    if (const KnownBuild* build = find_known_build_by_sha256(out.exe_sha256)) out.build = build->id;
    return out;
}

bool same_file(const fs::path& a, const fs::path& b) {
    const auto x = file_id(a);
    const auto y = file_id(b);
    return x && y && x->volume == y->volume && x->high == y->high && x->low == y->low;
}

bool game_running(const fs::path& game_dir) {
    const fs::path exe = game_dir / "eu4.exe";
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"eu4.exe") != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) {
            running = true;  // cannot tell which game it is
            continue;
        }
        wchar_t path[MAX_PATH * 4] = {};
        DWORD size = static_cast<DWORD>(std::size(path));
        const bool known = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
        CloseHandle(process);
        running = !known || same_file(path, exe);
    }
    CloseHandle(snapshot);
    return running;
}

bool runs_from(const fs::path& exe, const fs::path& game_dir) {
    return same_file(exe.parent_path(), game_dir);
}

}  // namespace anvil::manager
