#include "playset.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tally.hpp"

namespace script_errors {

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// key="value" in a .mod file.
std::string value_of(std::string_view text, std::string_view key) {
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
        if (at != 0 && !std::isspace(static_cast<unsigned char>(text[at - 1]))) continue;
        const std::size_t q = text.find('"', at + key.size());
        const std::size_t eq = text.find('=', at + key.size());
        if (q == std::string_view::npos || eq == std::string_view::npos || eq > q) continue;
        const std::size_t end = text.find('"', q + 1);
        if (end == std::string_view::npos) return {};
        return std::string(text.substr(q + 1, end - q - 1));
    }
    return {};
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool word_at(std::string_view line, std::size_t at, std::size_t size) {
    if (at > 0 && word_char(line[at - 1])) return false;
    return at + size >= line.size() || !word_char(line[at + size]);
}

}  // namespace

std::filesystem::path path_of(std::string_view text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
    return std::filesystem::path(w);
}

std::string utf8_of(const std::filesystem::path& path) {
    const std::wstring& w = path.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path user_dir() {
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    const std::size_t at = line.find(key);
    if (at != std::wstring::npos) {
        std::size_t from = at + key.size();
        const bool quoted = from < line.size() && line[from] == L'"';
        if (quoted) ++from;
        std::size_t end = line.find(quoted ? L'"' : L' ', from);
        if (end == std::wstring::npos) end = line.size();
        return std::filesystem::path(line.substr(from, end - from));
    }
    PWSTR docs = nullptr;
    std::filesystem::path out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        out = std::filesystem::path(docs) / L"Paradox Interactive" / L"Europa Universalis IV";
    }
    CoTaskMemFree(docs);
    return out;
}

std::filesystem::path game_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::vector<Layer> playset_layers(const std::filesystem::path& dlc_load, const std::filesystem::path& user,
                                  const std::filesystem::path& game) {
    std::vector<Layer> layers;
    if (!game.empty()) layers.push_back({"game", game});
    const std::string json = read_file(dlc_load);
    const std::size_t list = json.find("\"enabled_mods\"");
    if (list == std::string::npos) return layers;
    const std::size_t open = json.find('[', list);
    const std::size_t close = open == std::string::npos ? std::string::npos : json.find(']', open);
    if (close == std::string::npos) return layers;
    const std::string_view items(json.data() + open + 1, close - open - 1);
    for (std::size_t q = items.find('"'); q != std::string_view::npos; q = items.find('"', q + 1)) {
        const std::size_t end = items.find('"', q + 1);
        if (end == std::string_view::npos) break;
        const std::string mod_file(items.substr(q + 1, end - q - 1));
        q = end;
        const std::string mod = read_file(user / path_of(mod_file));
        const std::string name = value_of(mod, "name");
        const std::string path = value_of(mod, "path");
        if (path.empty()) continue;
        std::filesystem::path p = path_of(path);
        if (p.is_relative()) p = user / p;
        layers.push_back({name.empty() ? mod_file : log_throttle::utf8_text(name), p});
    }
    return layers;
}

bool resolve(const std::vector<Layer>& layers, std::string_view file, std::string& path, std::string& layer) {
    if (file.empty()) return false;
    const std::filesystem::path rel = path_of(file);
    if (rel.is_absolute()) return false;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(it->path / rel, ec)) {
            path = utf8_of(it->path / rel);
            layer = it->name;
            return true;
        }
    }
    return false;
}

Search search(const std::vector<Layer>& layers, std::string_view token) {
    Search out;
    if (token.empty()) return out;
    static const wchar_t* const kFolders[] = {L"events", L"common", L"decisions", L"missions", L"history/countries"};
    std::vector<Hit> uses;
    for (const Layer& layer : layers) {
        for (const wchar_t* folder : kFolders) {
            std::error_code ec;
            const std::filesystem::path dir = layer.path / folder;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != L".txt") continue;
                const std::string text = read_file(it->path());
                if (text.find(token) == std::string::npos) continue;
                bool counted_file = false;
                int number = 0;
                for (std::size_t start = 0; start < text.size();) {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    ++number;
                    const std::string_view line(text.data() + start, end - start);
                    start = end + 1;
                    const std::string_view code = line.substr(0, line.find('#'));
                    for (std::size_t at = code.find(token); at != std::string_view::npos;
                         at = code.find(token, at + 1)) {
                        if (!word_at(code, at, token.size())) continue;
                        ++out.total;
                        if (!counted_file) {
                            ++out.files;
                            counted_file = true;
                        }
                        Hit h;
                        h.abs = utf8_of(it->path());
                        std::string rel = utf8_of(std::filesystem::relative(it->path(), layer.path, ec));
                        std::replace(rel.begin(), rel.end(), '\\', '/');
                        h.rel = rel;
                        h.line = number;
                        h.defines = code.find("event_target_as") != std::string_view::npos;
                        if (h.defines) {
                            out.hits.push_back(std::move(h));
                        } else if (uses.size() < 8) {
                            uses.push_back(std::move(h));
                        }
                        break;
                    }
                }
            }
        }
    }
    if (out.hits.size() > 6) out.hits.resize(6);
    for (Hit& h : uses) {
        if (out.hits.size() >= 10) break;
        out.hits.push_back(std::move(h));
    }
    return out;
}

std::string vscode_uri(std::string_view utf8_path, int line) {
    std::string out = "vscode://file/";
    for (const char ch : utf8_path) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\\') {
            out += '/';
        } else if (std::isalnum(c) || c == '/' || c == ':' || c == '.' || c == '_' || c == '-') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    if (line > 0) out += ":" + std::to_string(line);
    return out;
}

}  // namespace script_errors
