#include "symbols.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

constexpr size_t kMaxSymbolBytes = 65536;

const std::set<std::string_view> kTopKeys = {"build", "symbol"};
const std::set<std::string_view> kSymbolKeys = {"rva",  "export",    "bytes",  "memory", "points_to",
                                                "type", "signature", "resume", "values", "note"};
const std::set<std::string_view> kValueKeys = {"at", "size", "rip_relative", "type"};

struct Fail {
    std::string message;
    uint32_t line = 0;  // 0: the caller fills in the line of the key it was reading
};

uint32_t line_of(const toml::source_region& source) { return static_cast<uint32_t>(source.begin.line); }

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

// Symbol and value names become C/C++/Rust identifiers in generated headers.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// "<return type> (<parameters>)": printable ASCII, balanced parentheses and a
// non-empty return type before the final parameter list. The types themselves
// are the generated header's business.
bool valid_signature(std::string_view text) {
    const std::string_view s = trim(text);
    if (s.empty() || s.back() != ')') return false;
    int depth = 0;
    for (char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
        if (c == '(') ++depth;
        if (c == ')' && --depth < 0) return false;
    }
    if (depth != 0) return false;
    size_t open = std::string_view::npos;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == ')') ++depth;
        if (s[i] == '(' && --depth == 0) {
            open = i;
            break;
        }
    }
    return open != std::string_view::npos && !trim(s.substr(0, open)).empty();
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "48 8B ?? 24": hex pairs or "??" (any byte), whitespace ignored, at least one fixed byte.
void parse_bytes(std::string_view text, Symbol& s) {
    std::string digits;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2 != 0) throw Fail{"bytes must be pairs of hex digits or \"??\""};
    if (digits.size() / 2 > kMaxSymbolBytes) throw Fail{"bytes longer than " + std::to_string(kMaxSymbolBytes) + " bytes"};
    bool fixed = false;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (digits[i] == '?' && digits[i + 1] == '?') {
            s.bytes.push_back(0);
            s.mask.push_back(0);
            continue;
        }
        const int high = hex_digit(digits[i]), low = hex_digit(digits[i + 1]);
        if (high < 0 || low < 0) throw Fail{"bytes: '" + digits.substr(i, 2) + "' is not a hex byte or \"??\""};
        s.bytes.push_back(static_cast<uint8_t>(high * 16 + low));
        s.mask.push_back(0xFF);
        fixed = true;
    }
    if (!fixed) throw Fail{"bytes must contain at least one byte that is not \"??\""};
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

uint32_t get_u32(const toml::node& node, std::string_view key) {
    const auto* value = node.as_integer();
    if (value == nullptr || value->get() < 0 || value->get() > 0xFFFFFFFFll) {
        throw Fail{std::string(key) + " must be an integer from 0 to 0xFFFFFFFF"};
    }
    return static_cast<uint32_t>(value->get());
}

std::string get_string(const toml::node& node, std::string_view key, bool allow_empty = false) {
    const auto* value = node.as_string();
    if (value == nullptr) throw Fail{std::string(key) + " must be a string"};
    if (!allow_empty && value->get().empty()) throw Fail{std::string(key) + " must not be empty"};
    return value->get();
}

SymbolValue parse_value(const std::string& name, const toml::node& node, const Symbol& s) {
    if (!valid_name(name)) throw Fail{"value name '" + name + "' must match [a-z][a-z0-9_]* (max 64 characters)"};
    const std::string where = "values." + name;
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{where + " must be a table such as { at = 3, size = 4 }"};
    SymbolValue v;
    v.name = name;
    bool has_at = false;
    for (const auto& [key, item, line] : in_file_order(*table)) {
        if (!kValueKeys.contains(key)) throw Fail{where + ": unknown key '" + key + "'", line};
        if (key == "at") {
            v.at = get_u32(*item, where + ".at");
            has_at = true;
        } else if (key == "size") {
            v.size = get_u32(*item, where + ".size");
            if (v.size != 1 && v.size != 2 && v.size != 4 && v.size != 8) throw Fail{where + ".size must be 1, 2, 4 or 8"};
        } else if (key == "rip_relative") {
            v.rip_relative = get_u32(*item, where + ".rip_relative");
        } else if (key == "type") {
            v.type = get_string(*item, where + ".type");
        }
    }
    if (!has_at) throw Fail{where + ": missing 'at'"};
    if ((v.size != 0) == (v.rip_relative != 0)) throw Fail{where + ": needs exactly one of 'size' and 'rip_relative'"};
    if (s.bytes.empty()) throw Fail{where + ": values are read from 'bytes', which the symbol does not declare"};
    const uint32_t span = v.rip_relative != 0 ? 4 : v.size;
    if (v.at > s.bytes.size() || span > s.bytes.size() - v.at) {
        throw Fail{where + ": bytes +" + hex(v.at) + ".." + hex(static_cast<uint64_t>(v.at) + span) +
                   " lie outside the declared " + std::to_string(s.bytes.size()) + " bytes"};
    }
    for (uint32_t i = v.at; i < v.at + span; ++i) {
        if (s.mask[i] != 0xFF) throw Fail{where + ": byte +" + hex(i) + " is \"??\"; values must lie on fixed bytes"};
    }
    if (v.rip_relative != 0 && v.rip_relative < v.at + 4) {
        throw Fail{where + ": rip_relative (the end of the instruction) must be at least at + 4"};
    }
    return v;
}

// One key of a [symbol.<name>] table.
void parse_symbol_key(Symbol& s, const toml::table*& values, const std::string& key, const toml::node& item) {
    if (!kSymbolKeys.contains(key)) throw Fail{"unknown key '" + key + "'"};
    if (key == "rva") {
        s.rva = get_u32(item, key);
        if (*s.rva == 0) throw Fail{"rva must not be 0"};
    } else if (key == "export") {
        s.export_name = get_string(item, key);
    } else if (key == "bytes") {
        parse_bytes(get_string(item, key), s);
    } else if (key == "memory") {
        const std::string memory = get_string(item, key);
        if (memory == "code") s.memory = MemoryKind::Code;
        else if (memory == "read") s.memory = MemoryKind::Read;
        else if (memory == "write") s.memory = MemoryKind::Write;
        else throw Fail{"memory must be \"code\", \"read\" or \"write\", not \"" + memory + "\""};
    } else if (key == "points_to") {
        s.points_to = get_u32(item, key);
    } else if (key == "type") {
        s.type = get_string(item, key);
    } else if (key == "signature") {
        s.signature = get_string(item, key);
        if (!valid_signature(s.signature)) {
            throw Fail{"signature \"" + s.signature + "\" must look like \"<return type> (<parameters>)\""};
        }
    } else if (key == "resume") {
        s.resume = get_string(item, key);
    } else if (key == "values") {
        values = item.as_table();
        if (values == nullptr) throw Fail{"values must be a table"};
    } else if (key == "note") {
        s.note = get_string(item, key, true);
    }
}

Symbol parse_symbol(const std::string& name, const toml::node& node) {
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{"must be a table"};
    Symbol s;
    s.name = name;
    const toml::table* values = nullptr;
    for (const auto& [key, item, line] : in_file_order(*table)) {
        at_line(line, [&] { parse_symbol_key(s, values, key, *item); });
    }
    if (s.rva.has_value() == !s.export_name.empty()) throw Fail{"needs exactly one of 'rva' and 'export'"};
    const int kinds = !s.signature.empty() + s.points_to.has_value() + !s.type.empty();
    if (kinds > 1) throw Fail{"declares more than one of 'signature' (function), 'points_to' (slot) and 'type' (data)"};
    s.kind = !s.signature.empty() ? SymbolKind::Function
             : s.points_to       ? SymbolKind::Slot
             : !s.type.empty()   ? SymbolKind::Data
                                 : SymbolKind::Site;
    if (s.rva) {
        if (!s.memory) throw Fail{"a symbol at an rva needs 'memory' (code, read or write)"};
        if (s.bytes.empty() && !s.points_to && s.kind != SymbolKind::Data) {
            throw Fail{"a symbol at an rva needs 'bytes' or 'points_to' to be verified"};
        }
    }
    if (values != nullptr) {
        for (const auto& [value_name, item, line] : in_file_order(*values)) {
            at_line(line, [&] { s.values.push_back(parse_value(value_name, *item, s)); });
        }
    }
    if (!s.resume.empty() && s.bytes.empty()) throw Fail{"resume needs 'bytes': the block it ends"};
    return s;
}

}  // namespace

const char* symbol_kind_name(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Function: return "function";
    case SymbolKind::Slot: return "slot";
    case SymbolKind::Data: return "data";
    case SymbolKind::Site: return "site";
    }
    return "?";
}

const Symbol* SymbolFile::find(std::string_view name) const {
    for (const auto& s : symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

SymbolFileResult parse_symbols_text(std::string_view text) {
    SymbolFileResult result;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "symbols.toml: " << e.description() << " (line " << e.source().begin.line << ")";
        result.error = message.str();
        result.line = line_of(e.source());
        return result;
    }
    SymbolFile file;
    std::string where;
    try {
        for (const auto& [key, node, line] : in_file_order(table)) {
            if (!kTopKeys.contains(key)) throw Fail{"unknown key '" + key + "'", line};
        }
        if (const auto* build = table.get("build")) {
            file.build_line = line_of(build->source());
            at_line(file.build_line, [&] { file.build = get_string(*build, "build"); });
        } else {
            throw Fail{"missing 'build'"};
        }
        if (const auto* node = table.get("symbol")) {
            const auto* symbols = node->as_table();
            if (symbols == nullptr) throw Fail{"symbol must be a table of [symbol.<name>] tables", line_of(node->source())};
            for (const auto& [name, item, line] : in_file_order(*symbols)) {
                where = "symbol " + name + ": ";
                Symbol s;
                at_line(line, [&] {
                    if (!valid_name(name)) throw Fail{"the name must match [a-z][a-z0-9_]* (max 64 characters)"};
                    s = parse_symbol(name, *item);
                });
                s.id = static_cast<uint32_t>(file.symbols.size());
                s.line = line;
                file.symbols.push_back(std::move(s));
            }
        }
        for (const auto& s : file.symbols) {
            where = "symbol " + s.name + ": ";
            if (s.resume.empty()) continue;
            if (s.resume == s.name) throw Fail{"resume names the symbol itself", s.line};
            if (file.find(s.resume) == nullptr) throw Fail{"resume names '" + s.resume + "', which is not declared", s.line};
        }
    } catch (const Fail& f) {
        result.error = "symbols.toml: " + where + f.message;
        result.line = f.line;
        return result;
    }
    result.file = std::move(file);
    return result;
}

SymbolFileResult load_symbols(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        SymbolFileResult result;
        result.error = "cannot read " + file.filename().string();
        return result;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // saved by Notepad
    return parse_symbols_text(text);
}

i18n::Message symbol_mismatch(const std::vector<std::string>& problems) {
    if (problems.size() == 1) return i18n::Message("symbol_mismatch", {problems.front()});
    return i18n::Message("symbol_mismatch_more", {problems.front(), std::to_string(problems.size() - 1)});
}

SymbolVerification verify_symbols(const SymbolFile& file, const PeImage& image) {
    SymbolVerification out;
    const ImageReader& reader = image.reader();
    for (const auto& s : file.symbols) {
        out.symbols.emplace_back();
        std::string where = "symbol " + s.name;
        auto fail = [&](std::string problem) {
            out.problems.push_back(std::move(problem));
            out.failed.push_back(s.id);
        };
        uint32_t rva = 0;
        if (s.rva) {
            rva = *s.rva;
            where += " (rva " + hex(rva) + ")";
        } else {
            std::string error;
            const auto found = image.find_export(s.export_name, error);
            if (!found) {
                fail(where + ": " + (error.empty() ? "export " + s.export_name + " not found in the image" : error));
                continue;
            }
            rva = *found;
            where += " (export " + s.export_name + ", rva " + hex(rva) + ")";
        }
        const size_t length = std::max<size_t>({s.bytes.size(), s.points_to ? 8u : 0u, 1u});
        const Section* section = image.section_of(rva, static_cast<uint32_t>(length));
        if (section == nullptr) {
            fail(where + ": " + std::to_string(length) + " B do not lie inside one section of the image");
            continue;
        }
        if (s.memory && section->kind != *s.memory) {
            fail(where + ": declared " + memory_kind_name(*s.memory) + " memory, section " + section->name + " is " +
                 memory_kind_name(section->kind));
            continue;
        }
        if (!s.bytes.empty()) {
            std::vector<uint8_t> actual(s.bytes.size());
            if (!reader.read(rva, actual.data(), actual.size())) {
                fail(where + ": the bytes cannot be read");
                continue;
            }
            bool same = true;
            for (size_t i = 0; i < actual.size() && same; ++i) {
                if ((actual[i] & s.mask[i]) == (s.bytes[i] & s.mask[i])) continue;
                char text[96];
                std::snprintf(text, sizeof(text), ": byte +0x%zX (rva 0x%llX) expected 0x%02X, found 0x%02X", i,
                              static_cast<unsigned long long>(rva) + i, s.bytes[i], actual[i]);
                fail(where + text);
                same = false;
            }
            if (!same) continue;
        }
        if (s.points_to) {
            uint64_t pointer = 0;
            if (!reader.read(rva, &pointer, sizeof(pointer))) {
                fail(where + ": the pointer cannot be read");
                continue;
            }
            if (pointer != reader.base() + *s.points_to) {
                const bool inside = pointer >= reader.base() && pointer - reader.base() < image.size_of_image();
                fail(where + ": points to " +
                     (inside ? "rva " + hex(pointer - reader.base()) : hex(pointer) + " (outside the image)") +
                     ", expected rva " + hex(*s.points_to));
                continue;
            }
        }
        // Values lie on fixed bytes that now match, so the declared bytes are the image's.
        VerifiedSymbol verified;
        verified.rva = rva;
        bool values_ok = true;
        for (const auto& v : s.values) {
            uint64_t raw = 0;
            std::memcpy(&raw, s.bytes.data() + v.at, v.rip_relative != 0 ? 4 : v.size);
            if (v.rip_relative == 0) {
                verified.values.emplace_back(v.name, raw);
                continue;
            }
            const int64_t target = static_cast<int64_t>(rva) + v.rip_relative + static_cast<int32_t>(static_cast<uint32_t>(raw));
            if (target < 0 || target >= static_cast<int64_t>(image.size_of_image())) {
                fail(where + ": value " + v.name + " points outside the image");
                values_ok = false;
                break;
            }
            verified.values.emplace_back(v.name, static_cast<uint64_t>(target));
        }
        if (values_ok) out.symbols.back() = std::move(verified);
    }
    return out;
}

}  // namespace anvil
