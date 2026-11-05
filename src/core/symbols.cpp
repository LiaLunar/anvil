#include "symbols.hpp"

#include "manifest.hpp"

#include "game_build.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

constexpr size_t kMaxSymbolBytes = 65536;

const std::set<std::string_view> kTopKeys = {"build", "builds", "symbol"};
const std::set<std::string_view> kSymbolKeys = {"rva",  "export",    "bytes",  "sha256", "size",  "any",    "memory",
                                                "points_to", "type", "signature", "resume", "values", "note", "build",
                                                "absent"};
// The keys a symbol may give per build (`build."<id>"`) in a file with `builds`.
const std::set<std::string_view> kPlacementKeys = {"rva", "bytes", "sha256", "size", "any", "points_to"};
const std::set<std::string_view> kValueKeys = {"at", "size", "rip_relative", "type", "is"};

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
void parse_bytes(std::string_view text, SymbolPlacement& s) {
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

// The raw keys of a digest, before finish_digest checks them together.
struct DigestKeys {
    std::string sha256;
    std::optional<uint32_t> size;
    const toml::array* any = nullptr;
};

// `sha256`, `size` and `any` of one placement: bytes the same length as `bytes`
// would have, zero until they verify, and the mask of `any`.
void finish_digest(SymbolPlacement& p, const DigestKeys& keys) {
    if (keys.sha256.empty()) {
        if (keys.size || keys.any != nullptr) throw Fail{"size and any go with sha256"};
        return;
    }
    if (!p.bytes.empty()) throw Fail{"bytes and sha256 exclude each other"};
    if (!keys.size) throw Fail{"sha256 needs 'size': how many bytes it covers"};
    if (*keys.size == 0 || *keys.size > kMaxSymbolBytes) {
        throw Fail{"size must be 1 to " + std::to_string(kMaxSymbolBytes)};
    }
    if (keys.sha256.size() != 64 || !std::all_of(keys.sha256.begin(), keys.sha256.end(), [](char c) { return hex_digit(c) >= 0; })) {
        throw Fail{"sha256 must be 64 hex digits"};
    }
    p.sha256 = keys.sha256;
    std::transform(p.sha256.begin(), p.sha256.end(), p.sha256.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    p.bytes.assign(*keys.size, 0);
    p.mask.assign(*keys.size, 0xFF);
    if (keys.any == nullptr) return;
    for (const auto& item : *keys.any) {
        const auto offset = item.value_exact<int64_t>();
        if (!offset || *offset < 0 || *offset >= static_cast<int64_t>(*keys.size)) {
            throw Fail{"any: offsets must lie inside the " + std::to_string(*keys.size) + " bytes"};
        }
        p.mask[static_cast<size_t>(*offset)] = 0;
    }
    if (std::none_of(p.mask.begin(), p.mask.end(), [](uint8_t m) { return m != 0; })) {
        throw Fail{"sha256 must cover at least one byte that any does not skip"};
    }
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

// The structure of values.<name>; check_value checks it against each build's bytes.
SymbolValue parse_value(const std::string& name, const toml::node& node) {
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
        } else if (key == "is") {
            const auto is = item->value_exact<int64_t>();
            if (!is || *is < 0) throw Fail{where + ".is must be a non-negative integer"};
            v.is = static_cast<uint64_t>(*is);
        }
    }
    if (!has_at) throw Fail{where + ": missing 'at'"};
    if ((v.size != 0) == (v.rip_relative != 0)) throw Fail{where + ": needs exactly one of 'size' and 'rip_relative'"};
    return v;
}

void check_value(const SymbolValue& v, const SymbolPlacement& s) {
    const std::string where = "values." + v.name;
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
}

// rva, bytes or points_to: shared by every build or in one build's table.
void parse_placement_key(SymbolPlacement& p, DigestKeys& digest, const std::string& key, const toml::node& item) {
    if (key == "sha256") {
        digest.sha256 = get_string(item, key);
    } else if (key == "size") {
        digest.size = get_u32(item, key);
    } else if (key == "any") {
        digest.any = item.as_array();
        if (digest.any == nullptr) throw Fail{"any must be a list of offsets"};
    } else if (key == "rva") {
        p.rva = get_u32(item, key);
        if (*p.rva == 0) throw Fail{"rva must not be 0"};
    } else if (key == "bytes") {
        parse_bytes(get_string(item, key), p);
    } else if (key == "points_to") {
        p.points_to = get_u32(item, key);
    }
}

// What a [symbol.<name>] table holds besides the Symbol's own fields.
struct SymbolTables {
    SymbolPlacement shared;               // the placement keys given for every build
    DigestKeys digest;                    // its sha256, size and any
    const toml::table* values = nullptr;
    const toml::table* builds = nullptr;  // the build."<id>" tables
    uint32_t builds_line = 0;
    std::vector<std::string> absent;  // the builds the symbol is not in
    uint32_t absent_line = 0;
};

// One key of a [symbol.<name>] table.
void parse_symbol_key(Symbol& s, SymbolTables& t, const std::string& key, const toml::node& item, uint32_t line) {
    if (!kSymbolKeys.contains(key)) throw Fail{"unknown key '" + key + "'"};
    if (kPlacementKeys.contains(key)) {
        parse_placement_key(t.shared, t.digest, key, item);
    } else if (key == "export") {
        s.export_name = get_string(item, key);
    } else if (key == "build") {
        t.builds = item.as_table();
        t.builds_line = line;
        if (t.builds == nullptr) throw Fail{"build must be a table of build.\"<id>\" tables"};
    } else if (key == "memory") {
        const std::string memory = get_string(item, key);
        if (memory == "code") s.memory = MemoryKind::Code;
        else if (memory == "read") s.memory = MemoryKind::Read;
        else if (memory == "write") s.memory = MemoryKind::Write;
        else throw Fail{"memory must be \"code\", \"read\" or \"write\", not \"" + memory + "\""};
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
        t.values = item.as_table();
        if (t.values == nullptr) throw Fail{"values must be a table"};
    } else if (key == "note") {
        s.note = get_string(item, key, true);
    } else if (key == "absent") {
        t.absent_line = line;
        const auto* list = item.as_array();
        if (list == nullptr || list->empty()) throw Fail{"absent must be a list of build ids such as [\"eu4-…\"]"};
        for (const auto& id : *list) {
            const std::string build = get_string(id, "absent[]");
            if (std::find(t.absent.begin(), t.absent.end(), build) != t.absent.end()) throw Fail{"absent lists " + build + " twice"};
            t.absent.push_back(build);
        }
    }
}

// The shared placement keys with those of build `id`'s table, if the symbol has one.
SymbolPlacement placement_for(const std::string& id, const SymbolTables& t) {
    SymbolPlacement p = t.shared;
    DigestKeys digest = t.digest;
    p.build = id;
    const toml::node* node = t.builds != nullptr ? t.builds->get(id) : nullptr;
    if (node == nullptr) {
        finish_digest(p, digest);
        return p;
    }
    p.line = line_of(node->source());
    const auto* table = node->as_table();
    if (table == nullptr) throw Fail{"build.\"" + id + "\" must be a table", p.line};
    for (const auto& [key, item, line] : in_file_order(*table)) {
        at_line(line, [&] {
            if (!kPlacementKeys.contains(key)) {
                throw Fail{"build.\"" + id + "\": unknown key '" + key + "' (a build gives rva, bytes, sha256, size, any or points_to)"};
            }
            const bool shared = key == "rva"      ? t.shared.rva.has_value()
                                : key == "bytes"  ? !t.shared.bytes.empty()
                                : key == "sha256" ? !t.digest.sha256.empty()
                                : key == "size"   ? t.digest.size.has_value()
                                : key == "any"    ? t.digest.any != nullptr
                                                  : t.shared.points_to.has_value();
            if (shared) throw Fail{"build.\"" + id + "\": " + key + " is already given for every build"};
            parse_placement_key(p, digest, key, *item);
        });
    }
    at_line(p.line, [&] { finish_digest(p, digest); });
    return p;
}

// The rules of design §5.2 that one build's placement must meet before its values are read.
// `first` is the placement of an earlier build the symbol is in, if any.
void check_placement(Symbol& s, const SymbolPlacement& p, const SymbolPlacement* first) {
    if (p.rva.has_value() == !s.export_name.empty()) throw Fail{"needs exactly one of 'rva' and 'export'"};
    const int kinds = !s.signature.empty() + p.points_to.has_value() + !s.type.empty();
    if (kinds > 1) throw Fail{"declares more than one of 'signature' (function), 'points_to' (slot) and 'type' (data)"};
    const SymbolKind kind = !s.signature.empty() ? SymbolKind::Function
                            : p.points_to        ? SymbolKind::Slot
                            : !s.type.empty()    ? SymbolKind::Data
                                                 : SymbolKind::Site;
    if (first == nullptr) {
        s.kind = kind;
    } else if (kind != s.kind) {
        throw Fail{"is a " + std::string(symbol_kind_name(kind)) + " here and a " + symbol_kind_name(s.kind) + " in " +
                   first->build + "; give points_to for every build or for none"};
    }
    if (p.rva) {
        if (!s.memory) throw Fail{"a symbol at an rva needs 'memory' (code, read or write)"};
        if (p.bytes.empty() && !p.points_to && kind != SymbolKind::Data) {
            throw Fail{"a symbol at an rva needs 'bytes', 'sha256' or 'points_to' to be verified"};
        }
    }
}

// `builds`: the builds the file declares; `per_build`: it lists them in `builds`,
// so the symbol may have build."<id>" tables.
Symbol parse_symbol(const std::string& name, const toml::node& node, const std::vector<std::string>& builds,
                    bool per_build) {
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{"must be a table"};
    Symbol s;
    s.name = name;
    SymbolTables t;
    for (const auto& [key, item, line] : in_file_order(*table)) {
        at_line(line, [&] { parse_symbol_key(s, t, key, *item, line); });
    }
    const auto absent = [&](const std::string& id) { return std::find(t.absent.begin(), t.absent.end(), id) != t.absent.end(); };
    if (!t.absent.empty()) {
        if (!per_build) throw Fail{"absent needs the list of builds at the top: builds = [...]", t.absent_line};
        for (const auto& id : t.absent) {
            if (std::find(builds.begin(), builds.end(), id) == builds.end()) {
                throw Fail{"absent: builds does not list " + id, t.absent_line};
            }
        }
        if (t.absent.size() == builds.size()) throw Fail{"absent lists every build; remove the symbol instead", t.absent_line};
    }
    if (t.builds != nullptr) {
        if (!per_build) throw Fail{"build.\"<id>\" tables need the list of builds at the top: builds = [...]", t.builds_line};
        for (const auto& [id, item, line] : in_file_order(*t.builds)) {
            if (std::find(builds.begin(), builds.end(), id) == builds.end()) {
                throw Fail{"build.\"" + id + "\": builds does not list " + id, line};
            }
            if (absent(id)) throw Fail{"build.\"" + id + "\": absent lists " + id, line};
        }
    }
    std::vector<uint32_t> value_lines;
    const SymbolPlacement* first = nullptr;  // of the first build the symbol is in
    s.placements.reserve(builds.size());
    for (const auto& id : builds) {
        if (absent(id)) {
            SymbolPlacement p;
            p.build = id;
            p.absent = true;
            s.placements.push_back(std::move(p));
            continue;
        }
        try {
            const SymbolPlacement p = placement_for(id, t);
            at_line(p.line, [&] { check_placement(s, p, first); });
            // Parsed once, after the first build's checks, as when files had one build.
            if (t.values != nullptr && first == nullptr) {
                for (const auto& [value_name, item, line] : in_file_order(*t.values)) {
                    at_line(line, [&] { s.values.push_back(parse_value(value_name, *item)); });
                    value_lines.push_back(line);
                }
            }
            for (size_t i = 0; i < s.values.size(); ++i) at_line(value_lines[i], [&] { check_value(s.values[i], p); });
            if (!s.resume.empty() && p.bytes.empty()) at_line(p.line, [] { throw Fail{"resume needs 'bytes': the block it ends"}; });
            s.placements.push_back(p);
            if (first == nullptr) first = &s.placements.back();
        } catch (Fail& f) {
            if (per_build) f.message = "in build " + id + ": " + f.message;
            throw;
        }
    }
    const SymbolPlacement& selected = s.placements.front();
    s.absent = selected.absent;
    s.rva = selected.rva;
    s.bytes = selected.bytes;
    s.mask = selected.mask;
    s.sha256 = selected.sha256;
    s.points_to = selected.points_to;
    s.placement_line = selected.line;
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

bool SymbolFile::declares(std::string_view build_id) const {
    return std::find(builds.begin(), builds.end(), build_id) != builds.end();
}

std::string SymbolFile::build_list() const {
    std::string out;
    for (const auto& b : builds) out += (out.empty() ? "" : ", ") + b;
    return out;
}

std::string SymbolFile::build_words() const { return (builds.size() > 1 ? "builds " : "build ") + build_list(); }

bool select_build(SymbolFile& file, std::string_view build_id) {
    const auto at = std::find(file.builds.begin(), file.builds.end(), build_id);
    if (at == file.builds.end()) return false;
    const size_t index = static_cast<size_t>(at - file.builds.begin());
    for (auto& s : file.symbols) {
        if (index >= s.placements.size()) continue;  // a symbol not from the parser keeps its own fields
        const SymbolPlacement& p = s.placements[index];
        s.absent = p.absent;
        s.rva = p.rva;
        s.bytes = p.bytes;
        s.mask = p.mask;
        s.sha256 = p.sha256;
        s.points_to = p.points_to;
        s.placement_line = p.line;
    }
    file.build = *at;
    return true;
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
        const auto* build = table.get("build");
        const auto* builds = table.get("builds");
        if (build != nullptr && builds != nullptr) {
            throw Fail{"give either 'build' or 'builds', not both", line_of(builds->source())};
        }
        if (build != nullptr) {
            file.build_line = line_of(build->source());
            at_line(file.build_line, [&] { file.builds.push_back(get_string(*build, "build")); });
        } else if (builds != nullptr) {
            file.builds_listed = true;
            file.build_line = line_of(builds->source());
            at_line(file.build_line, [&] {
                const auto* list = builds->as_array();
                if (list == nullptr || list->empty()) throw Fail{"builds must be a list of build ids such as [\"eu4-…\"]"};
                for (const auto& item : *list) {
                    const std::string id = get_string(item, "builds[]");
                    if (file.declares(id)) throw Fail{"builds lists " + id + " twice"};
                    file.builds.push_back(id);
                }
            });
        } else {
            throw Fail{"missing 'build' (or 'builds')"};
        }
        file.build = file.builds.front();
        if (const auto* node = table.get("symbol")) {
            const auto* symbols = node->as_table();
            if (symbols == nullptr) throw Fail{"symbol must be a table of [symbol.<name>] tables", line_of(node->source())};
            for (const auto& [name, item, line] : in_file_order(*symbols)) {
                where = "symbol " + name + ": ";
                Symbol s;
                at_line(line, [&] {
                    if (!valid_name(name)) throw Fail{"the name must match [a-z][a-z0-9_]* (max 64 characters)"};
                    s = parse_symbol(name, *item, file.builds, file.builds_listed);
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
            const Symbol* target = file.find(s.resume);
            if (target == nullptr) throw Fail{"resume names '" + s.resume + "', which is not declared", s.line};
            // The block resumes there on every build it is in.
            for (size_t i = 0; i < s.placements.size() && i < target->placements.size(); ++i) {
                if (!s.placements[i].absent && target->placements[i].absent) {
                    throw Fail{"resume names '" + s.resume + "', which is absent in build " + s.placements[i].build, s.line};
                }
            }
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
    std::error_code size_error;
    if (const std::uintmax_t size = std::filesystem::file_size(file, size_error);
        !size_error && size > kMaxPluginTomlBytes) {
        SymbolFileResult result;
        result.error = file.filename().string() + " is larger than 16 MB";
        return result;
    }
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

i18n::Message build_mismatch(const SymbolFile& file, std::string_view build_id) {
    if (file.builds.size() == 1) return i18n::Message("symbols_build", {file.builds.front(), std::string(build_id)});
    return i18n::Message("symbols_builds", {file.build_list(), std::string(build_id)});
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
        if (s.absent) {
            out.symbols.back().absent = true;
            continue;
        }
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
        std::vector<uint8_t> actual(s.bytes.size());
        if (!s.bytes.empty()) {
            if (!reader.read(rva, actual.data(), actual.size())) {
                fail(where + ": the bytes cannot be read");
                continue;
            }
            if (!s.sha256.empty()) {
                std::vector<uint8_t> masked(actual.size());
                for (size_t i = 0; i < actual.size(); ++i) masked[i] = actual[i] & s.mask[i];
                const std::string found = sha256_bytes(masked.data(), masked.size());
                if (found != s.sha256) {
                    fail(where + ": the " + std::to_string(actual.size()) + " bytes there have SHA-256 " + found + ", expected " + s.sha256);
                    continue;
                }
            }
            bool same = true;
            for (size_t i = 0; i < actual.size() && same && s.sha256.empty(); ++i) {
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
        // Values lie on fixed bytes that now match, so the bytes read are the declared ones.
        VerifiedSymbol verified;
        verified.rva = rva;
        bool values_ok = true;
        for (const auto& v : s.values) {
            uint64_t raw = 0;
            std::memcpy(&raw, actual.data() + v.at, v.rip_relative != 0 ? 4 : v.size);
            if (v.rip_relative == 0) {
                if (v.is && *v.is != raw) {
                    fail(where + ": value " + v.name + " is " + hex(raw) + ", declared " + hex(*v.is));
                    values_ok = false;
                    break;
                }
                verified.values.emplace_back(v.name, raw);
                continue;
            }
            const int64_t target = static_cast<int64_t>(rva) + v.rip_relative + static_cast<int32_t>(static_cast<uint32_t>(raw));
            if (target < 0 || target >= static_cast<int64_t>(image.size_of_image())) {
                fail(where + ": value " + v.name + " points outside the image");
                values_ok = false;
                break;
            }
            if (v.is && *v.is != static_cast<uint64_t>(target)) {
                fail(where + ": value " + v.name + " is " + hex(static_cast<uint64_t>(target)) + ", declared " + hex(*v.is));
                values_ok = false;
                break;
            }
            verified.values.emplace_back(v.name, static_cast<uint64_t>(target));
        }
        if (!s.sha256.empty()) verified.bytes = std::move(actual);
        if (values_ok) out.symbols.back() = std::move(verified);
    }
    return out;
}

void adopt_verified_bytes(SymbolFile& file, const SymbolVerification& verification) {
    for (auto& s : file.symbols) {
        if (s.sha256.empty() || s.id >= verification.symbols.size()) continue;
        if (!verification.symbols[s.id].bytes.empty()) s.bytes = verification.symbols[s.id].bytes;
    }
}

}  // namespace anvil
