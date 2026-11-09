// PROTOTYPE (eu4-engine-lab#11), throwaway. See model.hpp.
#include "model.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace script_errors {

namespace {

constexpr std::string_view kNotice = "[anvil-log-throttle] logged ";
constexpr std::string_view kReport = "[anvil-log-throttle] repeated ";

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// "X: rest" after the throttle's own words: the game's message.
std::string_view after_colon_space(std::string_view s) {
    const std::size_t at = s.find(": ");
    return at == std::string_view::npos ? std::string_view{} : s.substr(at + 2);
}

std::string quoted_after(std::string_view text, std::string_view key) {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) return {};
    const std::size_t from = at + key.size();
    const std::size_t end = text.find('"', from);
    return end == std::string_view::npos ? std::string{} : std::string(text.substr(from, end - from));
}

void classify(Message& m) {
    const std::string& t = m.text;
    std::string_view src = m.source;
    // "file: interface/x.gui line: 12" or 'File: "interface/x.gui", ... near line: 12'
    static const std::regex quoted(R"re(file:\s*"([^"]+)")re", std::regex::icase);
    static const std::regex bare(R"(file:\s*(\S+))", std::regex::icase);
    static const std::regex number(R"(line:\s*(\d+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(t, match, quoted) || std::regex_search(t, match, bare)) {
        m.file = match[1].str();
        std::smatch n;
        if (std::regex_search(t, n, number)) m.line = std::stoi(n[1].str());
    }
    if (starts_with(t, "Script error!")) {
        m.kind = Kind::script;
        m.token = quoted_after(t, "Script Object Token: \"");
        const std::size_t e = t.find("Error: ");
        if (e != std::string::npos) m.detail = t.substr(e + 7);
    } else if (starts_with(t, "Undefined event target")) {
        m.kind = Kind::target;
        const std::size_t c = t.find(": ");
        if (c != std::string::npos) m.token = t.substr(c + 2);
        m.detail = t.find("tooltip") != std::string::npos ? "in a tooltip" : "in script";
    } else if (starts_with(t, "Unknown country '")) {
        m.kind = Kind::script;
        const std::size_t end = t.find('\'', 17);
        if (end != std::string::npos) m.token = t.substr(17, end - 17);
        m.detail = t;
    } else if (starts_with(t, "Could not open file: ") || starts_with(t, "Invalid file when updating checksum: ")) {
        m.kind = Kind::files;
        std::string p = std::string(after_colon_space(t));
        const std::size_t comma = p.find(", error:");
        if (comma != std::string::npos) p = p.substr(0, comma);
        m.file = p;
        m.line = 0;
    } else if (src.find("windowtype") != std::string_view::npos || src.find("guitype") != std::string_view::npos ||
               src.find("spritetype") != std::string_view::npos ||
               (!m.file.empty() && (m.file.ends_with(".gui") || m.file.ends_with(".gfx")))) {
        m.kind = Kind::gui;
    } else if (!m.file.empty()) {
        m.kind = Kind::load;
    } else if (src.find("localize") != std::string_view::npos || src.find("ck2text") != std::string_view::npos ||
               t.find("ocaliz") != std::string::npos || t.find("ocalis") != std::string::npos) {
        m.kind = Kind::loc;
        m.token = quoted_after(t, "Key: \"");
    } else if (src.find("bitmapfont") != std::string_view::npos || src.find("texture") != std::string_view::npos ||
               t.find("Texture ") != std::string::npos || t.find("building entity") != std::string::npos) {
        m.kind = Kind::gfx;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string value_of(std::string_view text, std::string_view key) {
    // key="value" in a .mod file
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
        if (at != 0 && !std::isspace(static_cast<unsigned char>(text[at - 1]))) continue;
        std::size_t q = text.find('"', at + key.size());
        const std::size_t eq = text.find('=', at + key.size());
        if (q == std::string_view::npos || eq == std::string_view::npos || eq > q) continue;
        const std::size_t end = text.find('"', q + 1);
        if (end == std::string_view::npos) return {};
        return std::string(text.substr(q + 1, end - q - 1));
    }
    return {};
}

bool word_at(std::string_view line, std::size_t at, std::size_t size) {
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    if (at > 0 && word(line[at - 1])) return false;
    if (at + size < line.size() && word(line[at + size])) return false;
    return true;
}

}  // namespace

const char* kind_label(Kind kind) {
    switch (kind) {
        case Kind::script: return "Script: triggers and effects";
        case Kind::target: return "Undefined event targets";
        case Kind::files: return "Files the game could not open";
        case Kind::gui: return "Interface (.gui, .gfx)";
        case Kind::load: return "Load errors with file and line";
        case Kind::loc: return "Localisation";
        case Kind::gfx: return "Graphics and fonts";
        case Kind::other: return "Other";
    }
    return "Other";
}

std::string utf8_from_1252(std::string_view text) {
    bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii || text.empty()) return std::string(text);
    const int wide = MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), w.data(), wide);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), n, nullptr, nullptr);
    // The overlay shows no control characters well.
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) c = '?';
    }
    return out;
}

std::filesystem::path path_from_utf8(std::string_view text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
    return std::filesystem::path(w);
}

std::string utf8_from_path(const std::filesystem::path& path) {
    const std::wstring& w = path.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string grouped(std::uint64_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out;
}

std::string short_label(const Message& m) {
    auto cut = [](std::string s, std::size_t max) { return s.size() <= max ? s : s.substr(0, max - 3) + "..."; };
    std::string detail = m.detail;
    const std::string wrong = "Trigger condition set on the wrong scope: ";
    if (starts_with(detail, wrong)) detail = "wrong scope (" + detail.substr(wrong.size()) + ")";
    switch (m.kind) {
        case Kind::script:
            if (starts_with(m.text, "Unknown country")) return cut("unknown tag " + m.token + " in a tag trigger", 64);
            return cut(m.token + ": " + detail, 64);
        case Kind::target: return cut("event_target:" + m.token + " (" + m.detail + ")", 64);
        case Kind::files: return cut((starts_with(m.text, "Could") ? "missing: " : "checksum: ") + m.file, 64);
        default: return cut(m.text, 64);
    }
}

std::string vscode_uri(const std::filesystem::path& file, int line) {
    std::string p = utf8_from_path(file);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out = "vscode://file/";
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '.' || c == '_' || c == '-') {
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

void Log::reset() { *this = Log{}; }

void Log::feed(std::string_view bytes) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\n') continue;
        if (!carry_.empty()) {
            carry_.append(bytes.substr(start, i - start));
            line(carry_);
            carry_.clear();
        } else {
            line(bytes.substr(start, i - start));
        }
        start = i + 1;
    }
    carry_.append(bytes.substr(start));
}

void Log::line(std::string_view raw) {
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.remove_suffix(1);
    if (raw.empty()) return;
    ++lines_;
    std::string source;
    std::string_view rest = raw;
    if (rest.front() == '[') {
        const std::size_t end = rest.find("]: ");
        if (end != std::string_view::npos) {
            source = std::string(rest.substr(1, end - 1));
            rest = rest.substr(end + 3);
        }
    }
    std::uint64_t add_lines = 1;
    std::uint64_t add_repeats = 0;
    bool notice = false;
    if (starts_with(rest, kNotice)) {
        // "logged 10 times; further repeats are counted and reported every 60 s: <text>"
        rest = after_colon_space(rest);
        add_lines = 0;
        notice = true;
        ++notices_;
    } else if (starts_with(rest, kReport)) {
        // "repeated N more times in the last S s: <text>"
        add_repeats = std::strtoull(std::string(rest.substr(kReport.size(), 20)).c_str(), nullptr, 10);
        rest = after_colon_space(rest);
        add_lines = 0;
        ++reports_;
    }
    std::string text = utf8_from_1252(rest);
    std::string key = source + "|" + text;
    auto [it, fresh] = index_.try_emplace(std::move(key), messages_.size());
    if (fresh) {
        Message& m = messages_.emplace_back();
        m.source = source;
        m.text = std::move(text);
        m.order = it->second;
        classify(m);
    }
    Message& m = messages_[it->second];
    m.lines += add_lines;
    m.repeats += add_repeats;
    if (notice || add_repeats != 0) m.throttled = true;
}

std::vector<Root> playset_roots(const std::filesystem::path& dlc_load, const std::filesystem::path& user_dir,
                                const std::filesystem::path& game_dir) {
    std::vector<Root> roots;
    if (!game_dir.empty()) roots.push_back({"game", game_dir});
    const std::string json = read_file(dlc_load);
    const std::size_t list = json.find("\"enabled_mods\"");
    if (list == std::string::npos) return roots;
    const std::size_t open = json.find('[', list);
    const std::size_t close = json.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return roots;
    std::string_view items(json.data() + open + 1, close - open - 1);
    for (std::size_t q = items.find('"'); q != std::string_view::npos; q = items.find('"', q + 1)) {
        const std::size_t end = items.find('"', q + 1);
        if (end == std::string_view::npos) break;
        const std::string mod_file(items.substr(q + 1, end - q - 1));
        q = end;
        const std::string mod = read_file(user_dir / path_from_utf8(mod_file));
        std::string name = value_of(mod, "name");
        std::string path = value_of(mod, "path");
        if (path.empty()) continue;
        std::filesystem::path p = path_from_utf8(path);
        if (p.is_relative()) p = user_dir / p;
        roots.push_back({name.empty() ? mod_file : name, p});
    }
    return roots;
}

void resolve(Message& m, const std::vector<Root>& roots) {
    m.resolve_tried = true;
    if (m.file.empty()) return;
    const std::filesystem::path rel = path_from_utf8(m.file);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(it->path / rel, ec)) {
            m.resolved = it->path / rel;
            m.resolved_layer = it->name;
            return;
        }
    }
}

void search(const std::vector<Root>& roots, const Message& m, std::vector<Hit>& hits, std::size_t& total,
            std::size_t& files) {
    total = 0;
    files = 0;
    hits.clear();
    if (m.token.empty()) return;
    static const char* kFolders[] = {"events", "common", "decisions", "missions", "history/countries"};
    std::vector<Hit> uses;
    for (const Root& root : roots) {
        for (const char* folder : kFolders) {
            std::error_code ec;
            const std::filesystem::path dir = root.path / folder;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".txt") continue;
                const std::string text = read_file(it->path());
                if (text.find(m.token) == std::string::npos) continue;
                ++files;
                int number = 0;
                std::size_t start = 0;
                while (start < text.size()) {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    ++number;
                    std::string_view line(text.data() + start, end - start);
                    const std::size_t comment = line.find('#');
                    const std::string_view code = comment == std::string_view::npos ? line : line.substr(0, comment);
                    for (std::size_t at = code.find(m.token); at != std::string_view::npos;
                         at = code.find(m.token, at + 1)) {
                        if (!word_at(code, at, m.token.size())) continue;
                        ++total;
                        Hit h;
                        h.layer = root.name;
                        h.abs = it->path();
                        h.rel = utf8_from_path(std::filesystem::relative(it->path(), root.path, ec));
                        std::replace(h.rel.begin(), h.rel.end(), '\\', '/');
                        h.line = number;
                        std::string_view trimmed = code;
                        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
                            trimmed.remove_prefix(1);
                        h.text = utf8_from_1252(trimmed.substr(0, 80));
                        h.defines = code.find("event_target_as") != std::string_view::npos;
                        if (h.defines) {
                            hits.push_back(std::move(h));
                        } else if (uses.size() < 8) {
                            uses.push_back(std::move(h));
                        }
                        break;
                    }
                    start = end + 1;
                }
            }
        }
    }
    if (hits.size() > 6) hits.resize(6);
    for (Hit& h : uses) {
        if (hits.size() >= 10) break;
        hits.push_back(std::move(h));
    }
}

}  // namespace script_errors
