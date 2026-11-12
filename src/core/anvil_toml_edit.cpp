#include "anvil_toml_edit.hpp"

#include "log.hpp"

#include <toml++/toml.hpp>
#include <windows.h>

#include <algorithm>
#include <vector>

namespace anvil {
namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// "<plugin>.<module>" (ADR 0011): a plugin id, a dot and a module id
// ([a-z][a-z0-9_]*, at most 32 characters).
bool valid_module_key(std::string_view key) {
    const size_t dot = key.find('.');
    if (dot == std::string_view::npos || !valid_id(key.substr(0, dot))) return false;
    const std::string_view module = key.substr(dot + 1);
    if (module.empty() || module.size() > 32 || module.front() < 'a' || module.front() > 'z') return false;
    return std::all_of(module.begin(), module.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Byte offset of the start of 1-based `line`; npos past the end.
size_t line_start(std::string_view text, uint32_t line) {
    size_t at = 0;
    for (uint32_t i = 1; i < line; ++i) {
        at = text.find('\n', at);
        if (at == std::string_view::npos) return at;
        ++at;
    }
    return at;
}

// Past the end of a string that starts at `at` (a quote), or npos.
size_t skip_string(std::string_view text, size_t at) {
    const char quote = text[at];
    const bool basic = quote == '"';
    if (text.substr(at, 3) == std::string(3, quote)) {
        for (size_t i = at + 3; i < text.size(); ++i) {
            if (basic && text[i] == '\\') {
                ++i;
                continue;
            }
            if (text.substr(i, 3) == std::string(3, quote)) {
                // A closing delimiter may carry up to two more quotes.
                size_t end = i + 3;
                while (end < text.size() && end < i + 5 && text[end] == quote) ++end;
                return end;
            }
        }
        return std::string_view::npos;
    }
    for (size_t i = at + 1; i < text.size(); ++i) {
        if (basic && text[i] == '\\') {
            ++i;
            continue;
        }
        if (text[i] == quote) return i + 1;
        if (text[i] == '\n') return std::string_view::npos;
    }
    return std::string_view::npos;
}

// Where the id goes in the list that opens at `open`, and what to insert.
struct Insertion {
    size_t at = std::string_view::npos;
    std::string text;
};

Insertion insertion_in_list(std::string_view text, size_t open, std::string_view id) {
    size_t after_last = open + 1;
    char last = '[';  // '[': empty so far, ',': after a comma, 'v': after a value
    int depth = 1;
    for (size_t i = open + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '#') {
            const size_t end = text.find('\n', i);
            if (end == std::string_view::npos) break;
            i = end;
        } else if (c == '"' || c == '\'') {
            const size_t end = skip_string(text, i);
            if (end == std::string_view::npos) break;
            if (depth == 1) last = 'v', after_last = end;
            i = end - 1;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            if (--depth == 0) {
                Insertion out;
                out.at = after_last;
                const std::string quoted = "\"" + std::string(id) + "\"";
                out.text = last == '[' ? quoted : last == ',' ? " " + quoted : ", " + quoted;
                return out;
            }
            if (depth == 1) last = 'v', after_last = i + 1;
        } else if (c == ',') {
            if (depth == 1) last = ',', after_last = i + 1;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            if (depth == 1) last = 'v', after_last = i + 1;
        }
    }
    return {};
}

// The line of the first table header ([table] or [[array]]), 0 when none.
uint32_t first_table_line(const toml::table& table) {
    uint32_t first = 0;
    for (const auto& [key, node] : table) {
        uint32_t line = 0;
        if (const auto* t = node.as_table(); t != nullptr && !t->is_inline()) {
            line = node.source().begin.line;
            const uint32_t nested = first_table_line(*t);
            if (nested != 0 && (line == 0 || nested < line)) line = nested;
        } else if (const auto* a = node.as_array(); a != nullptr && a->is_array_of_tables()) {
            line = node.source().begin.line;
        }
        if (line != 0 && (first == 0 || line < first)) first = line;
    }
    return first;
}

// One value of a list, at the list's own level: [start, end).
struct ListItem {
    size_t start = 0;
    size_t end = 0;
};

// The values and commas of the list that opens at `open`; empty when the
// list does not end.
struct ListItems {
    std::vector<ListItem> items;
    std::vector<size_t> commas;
};

ListItems list_items(std::string_view text, size_t open) {
    ListItems out;
    int depth = 1;
    size_t nested_start = 0;
    for (size_t i = open + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '#') {
            const size_t end = text.find('\n', i);
            if (end == std::string_view::npos) break;
            i = end;
        } else if (c == '"' || c == '\'') {
            const size_t end = skip_string(text, i);
            if (end == std::string_view::npos) break;
            if (depth == 1) out.items.push_back({i, end});
            i = end - 1;
        } else if (c == '[' || c == '{') {
            if (depth++ == 1) nested_start = i;
        } else if (c == ']' || c == '}') {
            if (--depth == 0) return out;
            if (depth == 1) out.items.push_back({nested_start, i + 1});
        } else if (c == ',') {
            if (depth == 1) out.commas.push_back(i);
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && depth == 1) {
            size_t end = i + 1;
            while (end < text.size() && std::string_view(" \t\r\n,]#").find(text[end]) == std::string_view::npos) ++end;
            out.items.push_back({i, end});
            i = end - 1;
        }
    }
    return {};
}

bool blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
}

// The text without list value `index` and the comma that belongs to it: the
// one after it, or for the last value the one before it. A value that stood
// alone on its line takes the line with it.
std::string without_item(std::string_view text, const ListItems& list, size_t index) {
    const ListItem& item = list.items[index];
    size_t from = item.start;
    size_t to = item.end;
    const bool has_next = index + 1 < list.items.size();
    const auto after = std::find_if(list.commas.begin(), list.commas.end(), [&](size_t c) { return c >= item.end; });
    if (after != list.commas.end() && (!has_next || *after < list.items[index + 1].start)) {
        to = *after + 1;
        while (to < text.size() && (text[to] == ' ' || text[to] == '\t')) ++to;
    } else if (index > 0) {
        const auto before =
            std::find_if(list.commas.rbegin(), list.commas.rend(), [&](size_t c) { return c < item.start; });
        if (before != list.commas.rend() && *before > list.items[index - 1].start) from = *before;
    }
    const size_t previous_newline = text.rfind('\n', from - 1);
    const size_t first = previous_newline == std::string_view::npos ? 0 : previous_newline + 1;
    const size_t line_end = text.find('\n', to);
    if (first < from && blank(text.substr(first, from - first)) && line_end != std::string_view::npos &&
        blank(text.substr(to, line_end - to))) {
        from = first;
        to = line_end + 1;
    }
    return std::string(text.substr(0, from)) + std::string(text.substr(to));
}

// A string value without its quotes, when it is a one-line string without
// escapes (plugin ids need none).
std::optional<std::string_view> plain_string(std::string_view value) {
    if (value.size() < 2 || (value.front() != '"' && value.front() != '\'') || value.back() != value.front()) {
        return std::nullopt;
    }
    if (value.size() >= 6 && value.substr(0, 3) == std::string(3, value.front())) return std::nullopt;
    const std::string_view inner = value.substr(1, value.size() - 2);
    if (value.front() == '"' && inner.find('\\') != std::string_view::npos) return std::nullopt;
    return inner;
}

bool is_plugin_list(std::string_view key) {
    return key == "disabled_plugins" || key == "allow_ironman" || key == "allow_multiplayer" ||
           key == "enabled_modules" || key == "disabled_modules";
}

// What a list holds: plugin ids, module keys, or (the opt-ins) either.
bool fits_list(std::string_view key, std::string_view id) {
    if (key == "disabled_plugins") return valid_id(id);
    if (key == "enabled_modules" || key == "disabled_modules") return valid_module_key(id);
    return valid_id(id) || valid_module_key(id);
}

}  // namespace

std::optional<std::string> with_plugin_listed(std::string_view input, std::string_view key, std::string_view id,
                                              bool listed, std::string& error) {
    if (!is_plugin_list(key)) {
        error = "'" + std::string(key) + "' is not a list of plugin ids in anvil.toml";
        return std::nullopt;
    }
    if (!fits_list(key, id)) {
        error = "'" + std::string(id) + "' is not a " +
                (key == "enabled_modules" || key == "disabled_modules" ? "module (<plugin>.<module>)" : "plugin id");
        return std::nullopt;
    }
    const bool bom = input.substr(0, kBom.size()) == kBom;
    const std::string_view text = bom ? input.substr(kBom.size()) : input;
    toml::table before;
    try {
        before = toml::parse(text);
    } catch (const toml::parse_error& e) {
        error = "anvil.toml does not parse (line " + std::to_string(e.source().begin.line) +
                "): " + std::string(e.description());
        return std::nullopt;
    }
    const toml::node* node = before.get(key);
    const toml::array* list = node != nullptr ? node->as_array() : nullptr;
    if (node != nullptr && list == nullptr) {
        error = std::string(key) + " in anvil.toml is not a list";
        return std::nullopt;
    }
    toml::array expected;
    bool present = false;
    if (list != nullptr) {
        for (const auto& item : *list) {
            if (item.value<std::string>() == std::string(id)) {
                present = true;
            } else {
                expected.push_back(item);
            }
        }
    }
    if (present == listed) return std::string(input);  // nothing to change

    // The value starts on the key's line, after its '='.
    const size_t line = node == nullptr ? std::string_view::npos : line_start(text, node->source().begin.line);
    const size_t equals = line == std::string_view::npos ? line : text.find('=', line);
    const size_t open = equals == std::string_view::npos ? equals : text.find('[', equals);
    std::string edited;
    if (!listed) {
        // Every value that is the id goes, the last first so the offsets before it hold.
        edited = std::string(text);
        const size_t count = open == std::string_view::npos ? 0 : list_items(text, open).items.size();
        bool removed = false;
        for (size_t i = count; i-- > 0;) {
            const ListItems items = list_items(edited, open);
            if (i >= items.items.size()) break;  // earlier values keep their place
            const ListItem& item = items.items[i];
            if (plain_string(std::string_view(edited).substr(item.start, item.end - item.start)) == id) {
                edited = without_item(edited, items, i);
                removed = true;
            }
        }
        if (!removed) {
            error = "cannot find '" + std::string(id) + "' in the list of " + std::string(key) + " in anvil.toml";
            return std::nullopt;
        }
    } else if (node != nullptr) {
        const Insertion insertion =
            open == std::string_view::npos ? Insertion{} : insertion_in_list(text, open, id);
        if (insertion.at == std::string_view::npos) {
            error = "cannot find the list of " + std::string(key) + " in anvil.toml";
            return std::nullopt;
        }
        edited = std::string(text.substr(0, insertion.at)) + insertion.text + std::string(text.substr(insertion.at));
        expected.push_back(std::string(id));
    } else {
        const std::string newline = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        const std::string entry = std::string(key) + " = [\"" + std::string(id) + "\"]" + newline;
        const uint32_t table = first_table_line(before);
        const size_t at = table == 0 ? std::string_view::npos : line_start(text, table);
        if (at == std::string_view::npos) {
            edited = std::string(text);
            if (!edited.empty() && edited.back() != '\n') edited += newline;
            edited += entry;
        } else {
            edited = std::string(text.substr(0, at)) + entry + newline + std::string(text.substr(at));
        }
        expected.push_back(std::string(id));
    }
    // The edit must read back as the same file with the id added or removed.
    try {
        const toml::table after = toml::parse(edited);
        toml::table want = before;
        want.insert_or_assign(key, expected);
        if (!(after == want)) {
            error = "the edited anvil.toml would not read back as expected; not written";
            return std::nullopt;
        }
    } catch (const toml::parse_error& e) {
        error = "the edited anvil.toml would not parse: " + std::string(e.description()) + "; not written";
        return std::nullopt;
    }
    return bom ? std::string(kBom) + edited : edited;
}

std::optional<std::string> with_disabled_plugin(std::string_view text, std::string_view id, std::string& error) {
    return with_plugin_listed(text, "disabled_plugins", id, true, error);
}

bool set_plugin_listed(const std::filesystem::path& file, std::string_view key, std::string_view id, bool listed,
                       std::string& error) {
    std::string text;
    const HANDLE in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(in, &size) && size.QuadPart < (16 << 20);
        if (ok) {
            text.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty() || (ReadFile(in, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) &&
                                  read == text.size());
        }
        CloseHandle(in);
        if (!ok) {
            error = "cannot read " + narrow(file.wstring());
            return false;
        }
    } else if (const DWORD code = GetLastError(); code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
        error = "cannot open " + narrow(file.wstring()) + " (error " + std::to_string(code) + ")";
        return false;
    }
    const std::optional<std::string> edited = with_plugin_listed(text, key, id, listed, error);
    if (!edited) return false;
    if (*edited == text && (in != INVALID_HANDLE_VALUE || !listed)) return true;  // nothing to change

    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    const HANDLE out =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = "cannot write " + narrow(temporary.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(out, edited->data(), static_cast<DWORD>(edited->size()), &written, nullptr) &&
                       written == edited->size() && FlushFileBuffers(out);
    CloseHandle(out);
    if (!wrote ||
        !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + narrow(file.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool add_disabled_plugin(const std::filesystem::path& file, std::string_view id, std::string& error) {
    return set_plugin_listed(file, "disabled_plugins", id, true, error);
}

}  // namespace anvil
