#include "settings.hpp"

#include "manifest.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

const std::set<std::string_view> kTopKeys = {"setting"};
const std::set<std::string_view> kSettingKeys = {"type",   "default", "min",         "max",   "step",
                                                 "values", "label",   "description", "group", "restart"};
const std::set<std::string_view> kOptionKeys = {"name", "label"};

struct Fail {
    std::string message;
    uint32_t line = 0;  // 0: the caller fills in the line of the key it was reading
};

uint32_t line_of(const toml::source_region& source) { return static_cast<uint32_t>(source.begin.line); }

// Setting and option names become C/C++/Rust identifiers in generated headers.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

struct Entry {
    std::string key;
    const toml::node* node;
    uint32_t line;  // of the key
};

// Table entries in the order they appear in the file (toml::table is sorted by key).
std::vector<Entry> in_file_order(const toml::table& table) {
    std::vector<std::pair<const toml::key*, const toml::node*>> entries;
    for (const auto& [key, node] : table) entries.emplace_back(&key, &node);
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        const auto& x = a.first->source().begin;
        const auto& y = b.first->source().begin;
        return x.line != y.line ? x.line < y.line : x.column < y.column;
    });
    std::vector<Entry> out;
    for (const auto& [key, node] : entries) out.push_back({std::string(key->str()), node, line_of(key->source())});
    return out;
}

// Runs `body`; a Fail without a line gets `line`.
template <typename Body> void at_line(uint32_t line, Body&& body) {
    try {
        body();
    } catch (Fail& f) {
        if (f.line == 0) f.line = line;
        throw;
    }
}

std::string get_string(const toml::node& node, std::string_view key, bool allow_empty) {
    const auto* value = node.as_string();
    if (value == nullptr) throw Fail{std::string(key) + " must be a string"};
    if (!allow_empty && value->get().empty()) throw Fail{std::string(key) + " must not be empty"};
    return value->get();
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string format_float(double value) {
    char text[64];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    std::string out(text, ec == std::errc() ? end : text);
    if (out.find_first_of(".en") == std::string::npos) out += ".0";  // TOML: a float, not an integer
    return out;
}

}  // namespace

std::string toml_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escape;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

namespace {

std::string option_list(const Setting& s) {
    std::string out;
    for (const auto& o : s.options) out += (out.empty() ? "" : ", ") + o.name;
    return out;
}

bool is_numeric(SettingType type) { return type == SettingType::Int || type == SettingType::Float; }

// A value of the setting's type from TOML. Limits are not checked here.
SettingValue value_from_node(const Setting& s, const toml::node& node, std::string_view what) {
    SettingValue v;
    const std::string where(what);
    switch (s.type) {
    case SettingType::Bool:
        if (const auto* b = node.as_boolean()) {
            v.integer = b->get() ? 1 : 0;
            return v;
        }
        throw Fail{where + " must be true or false"};
    case SettingType::Int:
        if (const auto* i = node.as_integer()) {
            v.integer = i->get();
            return v;
        }
        throw Fail{where + " must be an integer"};
    case SettingType::Float:
        if (const auto* f = node.as_floating_point()) v.real = f->get();
        else if (const auto* i = node.as_integer()) v.real = static_cast<double>(i->get());
        else throw Fail{where + " must be a number"};
        if (!std::isfinite(v.real)) throw Fail{where + " must be a finite number"};
        return v;
    case SettingType::Enum: {
        const std::string name = get_string(node, where, true);
        for (size_t i = 0; i < s.options.size(); ++i) {
            if (s.options[i].name == name) {
                v.integer = static_cast<int64_t>(i);
                return v;
            }
        }
        throw Fail{where + ": \"" + name + "\" is not one of " + option_list(s)};
    }
    case SettingType::String:
        v.text = get_string(node, where, true);
        if (v.text.find('\0') != std::string::npos) throw Fail{where + " must not contain a NUL character"};
        return v;
    case SettingType::Key: {
        const std::string text = get_string(node, where, true);
        const auto code = parse_key(text);
        if (!code) throw Fail{where + ": \"" + text + "\" is not a key binding such as \"Ctrl+Shift+F12\""};
        v.integer = *code;
        return v;
    }
    }
    throw Fail{where + ": unknown type"};
}

bool below(const Setting& s, const SettingValue& a, const SettingValue& b) {
    return s.type == SettingType::Int ? a.integer < b.integer : a.real < b.real;
}

std::vector<SettingOption> parse_options(const toml::node& node) {
    const auto* array = node.as_array();
    if (array == nullptr || array->empty()) {
        throw Fail{"values must be a non-empty array such as [{ name = \"off\", label = \"Off\" }]"};
    }
    std::vector<SettingOption> options;
    for (const auto& item : *array) {
        const uint32_t item_line = line_of(item.source());
        const auto* table = item.as_table();
        if (table == nullptr) throw Fail{"each of values must be a table such as { name = \"off\", label = \"Off\" }", item_line};
        SettingOption o;
        bool has_name = false;
        for (const auto& [key, value, line] : in_file_order(*table)) {
            at_line(line, [&] {
                if (!kOptionKeys.contains(key)) throw Fail{"values: unknown key '" + key + "'"};
                if (key == "name") {
                    o.name = get_string(*value, "values.name", false);
                    has_name = true;
                    if (!valid_name(o.name)) {
                        throw Fail{"values: name '" + o.name + "' must match [a-z][a-z0-9_]* (max 64 characters)"};
                    }
                } else {
                    o.label = get_string(*value, "values.label", false);
                }
            });
        }
        if (!has_name) throw Fail{"values: an option without 'name'", item_line};
        for (const auto& other : options) {
            if (other.name == o.name) throw Fail{"values: '" + o.name + "' is declared twice", item_line};
        }
        if (o.label.empty()) o.label = o.name;
        options.push_back(std::move(o));
    }
    return options;
}

Setting parse_setting(const std::string& name, const toml::node& node, uint32_t table_line) {
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{"must be a table"};
    Setting s;
    s.name = name;
    const std::vector<Entry> entries = in_file_order(*table);
    for (const auto& e : entries) {
        if (!kSettingKeys.contains(e.key)) throw Fail{"unknown key '" + e.key + "'", e.line};
    }
    const auto entry = [&](std::string_view key) -> const Entry* {
        for (const auto& e : entries) {
            if (e.key == key) return &e;
        }
        return nullptr;
    };
    // The type decides how the other keys read, and enum options how the default does.
    const Entry* type = entry("type");
    if (type == nullptr) throw Fail{"missing 'type' (bool, int, float, enum, string or key)", table_line};
    at_line(type->line, [&] {
        const std::string text = get_string(*type->node, "type", false);
        if (text == "bool") s.type = SettingType::Bool;
        else if (text == "int") s.type = SettingType::Int;
        else if (text == "float") s.type = SettingType::Float;
        else if (text == "enum") s.type = SettingType::Enum;
        else if (text == "string") s.type = SettingType::String;
        else if (text == "key") s.type = SettingType::Key;
        else throw Fail{"type must be bool, int, float, enum, string or key, not \"" + text + "\""};
    });
    if (const Entry* values = entry("values")) {
        at_line(values->line, [&] {
            if (s.type != SettingType::Enum) throw Fail{"values apply only to enum settings"};
            s.options = parse_options(*values->node);
        });
    } else if (s.type == SettingType::Enum) {
        throw Fail{"an enum setting needs 'values'", table_line};
    }
    bool has_default = false, has_label = false;
    for (const auto& [key, item, line] : entries) {
        at_line(line, [&] {
            if (key == "default") {
                s.default_value = value_from_node(s, *item, "default");
                has_default = true;
            } else if (key == "min" || key == "max" || key == "step") {
                if (!is_numeric(s.type)) throw Fail{key + " applies only to int and float settings"};
                const SettingValue limit = value_from_node(s, *item, key);
                if (key == "step" && !(s.type == SettingType::Int ? limit.integer > 0 : limit.real > 0)) {
                    throw Fail{"step must be greater than 0"};
                }
                (key == "min" ? s.min : key == "max" ? s.max : s.step) = limit;
            } else if (key == "label") {
                s.label = get_string(*item, key, false);
                has_label = true;
            } else if (key == "description") {
                s.description = get_string(*item, key, true);
            } else if (key == "group") {
                s.group = get_string(*item, key, true);
            } else if (key == "restart") {
                const auto* b = item->as_boolean();
                if (b == nullptr) throw Fail{"restart must be true or false"};
                s.restart = b->get();
            }
        });
    }
    if (!has_default) throw Fail{"missing 'default'", table_line};
    if (!has_label) throw Fail{"missing 'label'", table_line};
    if (s.min && s.max && below(s, *s.max, *s.min)) {
        throw Fail{"max " + setting_toml(s, *s.max) + " is below min " + setting_toml(s, *s.min), entry("max")->line};
    }
    if (const std::string problem = check_setting_value(s, s.default_value); !problem.empty()) {
        throw Fail{"default: " + problem, entry("default")->line};
    }
    return s;
}

// ---- key names ------------------------------------------------------------------

struct KeyName {
    const char* name;
    int code;
};

// Windows virtual-key codes. Letters, digits and F1-F24 are generated.
constexpr KeyName kKeys[] = {
    {"Backspace", 0x08}, {"Tab", 0x09},         {"Enter", 0x0D},        {"Pause", 0x13},
    {"CapsLock", 0x14},  {"Escape", 0x1B},      {"Space", 0x20},        {"PageUp", 0x21},
    {"PageDown", 0x22},  {"End", 0x23},         {"Home", 0x24},         {"Left", 0x25},
    {"Up", 0x26},        {"Right", 0x27},       {"Down", 0x28},         {"PrintScreen", 0x2C},
    {"Insert", 0x2D},    {"Delete", 0x2E},      {"Numpad0", 0x60},      {"Numpad1", 0x61},
    {"Numpad2", 0x62},   {"Numpad3", 0x63},     {"Numpad4", 0x64},      {"Numpad5", 0x65},
    {"Numpad6", 0x66},   {"Numpad7", 0x67},     {"Numpad8", 0x68},      {"Numpad9", 0x69},
    {"NumpadMultiply", 0x6A}, {"NumpadAdd", 0x6B}, {"NumpadSubtract", 0x6D}, {"NumpadDecimal", 0x6E},
    {"NumpadDivide", 0x6F}, {"NumLock", 0x90},  {"ScrollLock", 0x91},   {"Semicolon", 0xBA},
    {"Equals", 0xBB},    {"Comma", 0xBC},       {"Minus", 0xBD},        {"Period", 0xBE},
    {"Slash", 0xBF},     {"Backquote", 0xC0},   {"LeftBracket", 0xDB},  {"Backslash", 0xDC},
    {"RightBracket", 0xDD}, {"Quote", 0xDE},
};

std::optional<int> key_code(std::string_view name) {
    const std::string l = lower(name);
    if (l.size() == 1 && l[0] >= 'a' && l[0] <= 'z') return 0x41 + (l[0] - 'a');
    if (l.size() == 1 && l[0] >= '0' && l[0] <= '9') return 0x30 + (l[0] - '0');
    if (l.size() >= 2 && l.size() <= 3 && l[0] == 'f') {
        int n = 0;
        const auto [end, ec] = std::from_chars(l.data() + 1, l.data() + l.size(), n);
        if (ec == std::errc() && end == l.data() + l.size() && n >= 1 && n <= 24 && l[1] != '0') return 0x70 + n - 1;
    }
    for (const auto& k : kKeys) {
        if (lower(k.name) == l) return k.code;
    }
    return std::nullopt;
}

std::string code_name(int code) {
    if (code >= 0x41 && code <= 0x5A) return std::string(1, static_cast<char>(code));
    if (code >= 0x30 && code <= 0x39) return std::string(1, static_cast<char>(code));
    if (code >= 0x70 && code <= 0x87) return "F" + std::to_string(code - 0x70 + 1);
    for (const auto& k : kKeys) {
        if (k.code == code) return k.name;
    }
    return {};
}

}  // namespace

const char* setting_type_name(SettingType type) {
    switch (type) {
    case SettingType::Bool: return "bool";
    case SettingType::Int: return "int";
    case SettingType::Float: return "float";
    case SettingType::Enum: return "enum";
    case SettingType::String: return "string";
    case SettingType::Key: return "key";
    }
    return "?";
}

const Setting* SettingsFile::find(std::string_view name) const {
    for (const auto& s : settings) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::optional<int64_t> parse_key(std::string_view text) {
    if (text.empty()) return 0;
    int64_t modifiers = 0;
    while (true) {
        const size_t plus = text.find('+');
        const std::string_view part = text.substr(0, plus);
        if (plus == std::string_view::npos) {
            const auto code = key_code(part);
            if (!code) return std::nullopt;
            return modifiers | *code;
        }
        const std::string l = lower(part);
        const int64_t modifier = l == "ctrl" || l == "control" ? kKeyCtrl
                                 : l == "shift"                ? kKeyShift
                                 : l == "alt"                  ? kKeyAlt
                                                               : 0;
        if (modifier == 0 || (modifiers & modifier) != 0) return std::nullopt;
        modifiers |= modifier;
        text.remove_prefix(plus + 1);
        if (text.empty()) return std::nullopt;
    }
}

std::string key_name(int64_t code) {
    if (code <= 0 || (code & ~(kKeyShift | kKeyCtrl | kKeyAlt | 0xFF)) != 0) return {};
    const std::string key = code_name(static_cast<int>(code & 0xFF));
    if (key.empty()) return {};
    std::string out;
    if (code & kKeyCtrl) out += "Ctrl+";
    if (code & kKeyShift) out += "Shift+";
    if (code & kKeyAlt) out += "Alt+";
    return out + key;
}

std::string check_setting_value(const Setting& s, const SettingValue& v) {
    switch (s.type) {
    case SettingType::Bool:
        return v.integer == 0 || v.integer == 1 ? std::string() : "a bool is 0 or 1";
    case SettingType::Int:
    case SettingType::Float:
        if (s.type == SettingType::Float && !std::isfinite(v.real)) return "not a finite number";
        if (s.min && below(s, v, *s.min)) return setting_toml(s, v) + " is below the minimum " + setting_toml(s, *s.min);
        if (s.max && below(s, *s.max, v)) return setting_toml(s, v) + " is above the maximum " + setting_toml(s, *s.max);
        return {};
    case SettingType::Enum:
        return v.integer >= 0 && static_cast<size_t>(v.integer) < s.options.size()
                   ? std::string()
                   : "option " + std::to_string(v.integer) + " does not exist";
    case SettingType::String:
        return v.text.find('\0') == std::string::npos ? std::string() : "a NUL character";
    case SettingType::Key:
        return v.integer == 0 || !key_name(v.integer).empty() ? std::string()
                                                              : "key code " + std::to_string(v.integer) + " is not a key binding";
    }
    return "unknown type";
}

std::optional<SettingValue> parse_setting_text(const Setting& s, std::string_view text, std::string& error) {
    SettingValue v;
    switch (s.type) {
    case SettingType::Bool: {
        const std::string l = lower(text);
        if (l == "true" || l == "1") v.integer = 1;
        else if (l == "false" || l == "0") v.integer = 0;
        else error = "\"" + std::string(text) + "\" is not true or false";
        break;
    }
    case SettingType::Int: {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v.integer);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            error = "\"" + std::string(text) + "\" is not an integer";
        }
        break;
    }
    case SettingType::Float: {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v.real);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            error = "\"" + std::string(text) + "\" is not a number";
        }
        break;
    }
    case SettingType::Enum: {
        const auto it = std::find_if(s.options.begin(), s.options.end(), [&](const auto& o) { return o.name == text; });
        if (it == s.options.end()) error = "\"" + std::string(text) + "\" is not one of " + option_list(s);
        else v.integer = it - s.options.begin();
        break;
    }
    case SettingType::String:
        v.text = text;
        break;
    case SettingType::Key: {
        const auto code = parse_key(text);
        if (!code) error = "\"" + std::string(text) + "\" is not a key binding such as \"Ctrl+Shift+F12\"";
        else v.integer = *code;
        break;
    }
    }
    if (error.empty()) error = check_setting_value(s, v);
    if (!error.empty()) return std::nullopt;
    return v;
}

std::string setting_toml(const Setting& s, const SettingValue& v) {
    switch (s.type) {
    case SettingType::Bool: return v.integer != 0 ? "true" : "false";
    case SettingType::Int: return std::to_string(v.integer);
    case SettingType::Float: return format_float(v.real);
    case SettingType::Enum:
        return v.integer >= 0 && static_cast<size_t>(v.integer) < s.options.size()
                   ? toml_string(s.options[static_cast<size_t>(v.integer)].name)
                   : std::to_string(v.integer);
    case SettingType::String: return toml_string(v.text);
    case SettingType::Key: return toml_string(key_name(v.integer));
    }
    return {};
}

SettingsFileResult parse_settings_text(std::string_view text) {
    SettingsFileResult result;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "settings.toml: " << e.description() << " (line " << e.source().begin.line << ")";
        result.error = message.str();
        result.line = line_of(e.source());
        return result;
    }
    SettingsFile file;
    std::string where;
    try {
        for (const auto& [key, node, line] : in_file_order(table)) {
            if (!kTopKeys.contains(key)) throw Fail{"unknown key '" + key + "'", line};
        }
        if (const auto* node = table.get("setting")) {
            const auto* settings = node->as_table();
            if (settings == nullptr) {
                throw Fail{"setting must be a table of [setting.<name>] tables", line_of(node->source())};
            }
            for (const auto& [name, item, line] : in_file_order(*settings)) {
                where = "setting " + name + ": ";
                Setting s;
                at_line(line, [&] {
                    if (!valid_name(name)) throw Fail{"the name must match [a-z][a-z0-9_]* (max 64 characters)"};
                    s = parse_setting(name, *item, line);
                });
                s.id = static_cast<uint32_t>(file.settings.size());
                s.line = line;
                file.settings.push_back(std::move(s));
            }
        }
    } catch (const Fail& f) {
        result.error = "settings.toml: " + where + f.message;
        result.line = f.line;
        return result;
    }
    result.file = std::move(file);
    return result;
}

namespace {

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

bool read_text(const std::filesystem::path& file, std::string& text) {
    std::error_code size_error;
    if (const std::uintmax_t size = std::filesystem::file_size(file, size_error);
        !size_error && size > kMaxPluginTomlBytes) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // saved by Notepad
    return true;
}

}  // namespace

SettingsFileResult load_settings(const std::filesystem::path& file) {
    std::string text;
    if (!read_text(file, text)) {
        SettingsFileResult result;
        result.error = "cannot read " + shown(file.filename());
        return result;
    }
    return parse_settings_text(text);
}

PlayerSettings parse_player_settings(const SettingsFile& schema, std::string_view text) {
    PlayerSettings out;
    out.values.resize(schema.settings.size());
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "line " << e.source().begin.line << ": " << e.description()
                << "; the file is ignored and every setting has its default";
        out.warnings.push_back(message.str());
        return out;
    }
    for (const auto& [name, node, line] : in_file_order(table)) {
        const std::string at = "line " + std::to_string(line) + ": ";
        const Setting* s = schema.find(name);
        if (s == nullptr) {
            out.warnings.push_back(at + "'" + name + "' is not a setting of this plugin; ignored");
            continue;
        }
        const std::string fallback = "; the default " + setting_toml(*s, s->default_value) + " applies";
        try {
            const SettingValue v = value_from_node(*s, *node, name);
            if (const std::string problem = check_setting_value(*s, v); !problem.empty()) {
                out.warnings.push_back(at + name + ": " + problem + fallback);
                continue;
            }
            out.values[s->id] = v;
        } catch (const Fail& f) {
            out.warnings.push_back(at + f.message + fallback);
        }
    }
    return out;
}

PlayerSettings load_player_settings(const SettingsFile& schema, const std::filesystem::path& file) {
    std::string text;
    if (!read_text(file, text)) {
        PlayerSettings out;
        out.values.resize(schema.settings.size());
        return out;
    }
    return parse_player_settings(schema, text);
}

std::string player_settings_text(const SettingsFile& schema, const std::vector<SettingValue>& values) {
    std::string body;
    for (const auto& s : schema.settings) {
        if (s.id >= values.size() || values[s.id] == s.default_value) continue;
        body += s.name + " = " + setting_toml(s, values[s.id]) + "\n";
    }
    if (body.empty()) return {};
    return "# The player's values of this plugin's settings that differ from the defaults (settings.toml in\n"
           "# the plugin folder). Anvil rewrites this file when a value changes; edit it while the game is closed.\n" +
           body;
}

bool write_player_settings(const SettingsFile& schema, const std::vector<SettingValue>& values,
                           const std::filesystem::path& file, std::string& error) {
    const std::string text = player_settings_text(schema, values);
    std::error_code ec;
    if (text.empty()) {
        std::filesystem::remove(file, ec);
        if (ec) error = "cannot remove " + shown(file) + ": " + ec.message();
        return !ec;
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) {
            error = "cannot write " + shown(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, ec);  // replaces the file in one step
    if (ec) {
        error = "cannot replace " + shown(file) + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::string setting_environment_name(std::string_view plugin_id, std::string_view setting_name) {
    std::string out = "ANVIL_";
    for (const std::string_view part : {plugin_id, std::string_view("_"), setting_name}) {
        for (const char c : part) {
            out.push_back(c == '-' ? '_' : c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
    }
    return out;
}

}  // namespace anvil
