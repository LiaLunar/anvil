#include "cli/gen.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace anvil::cli {
namespace {

// Names that cannot be C++ identifiers in namespace sym. Symbol and value
// names are lower-case, so only lower-case keywords matter.
const std::set<std::string_view> kCppKeywords = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char",
    "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
    "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires", "restrict", "return",
    "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
    "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
// Members of the anvil:: symbol types (and of every generated symbol type)
// that a value member would hide.
const std::set<std::string_view> kReservedMembers = {"id", "value", "symbol_name"};

struct Fail {
    std::string message;
    uint32_t line;
};

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

std::string upper(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// Text for a comment: on one line and, in a /* */ comment, unable to end it.
std::string comment_text(std::string_view text, bool block = true) {
    std::string out;
    for (char c : text) out.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    if (!block) return out;
    for (size_t at = out.find("*/"); at != std::string::npos; at = out.find("*/", at)) out.replace(at, 2, "* /");
    return out;
}

// "<return type> (<parameters>)", already checked by the parser.
struct Signature {
    std::string result;
    std::string parameters;  // as written; may be empty
};

Signature split_signature(std::string_view text) {
    const std::string_view s = trim(text);
    int depth = 0;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == ')') ++depth;
        if (s[i] == '(' && --depth == 0) {
            return {std::string(trim(s.substr(0, i))), std::string(trim(s.substr(i + 1, s.size() - i - 2)))};
        }
    }
    return {std::string(s), {}};
}

// A value's constant in one build: the little-endian integer at `at`, or for
// rip_relative the RVA it addresses. Unknown for a rip_relative value of an
// export, whose RVA is resolved when the game loads.
std::optional<uint64_t> constant_in(const Symbol& s, const std::optional<uint32_t>& rva, const std::vector<uint8_t>& bytes,
                                    const std::string& sha256, const SymbolValue& v) {
    if (!sha256.empty()) return v.is;  // the bytes are only in the game: `is` states what they give
    uint64_t raw = 0;
    std::memcpy(&raw, bytes.data() + v.at, v.rip_relative != 0 ? 4 : v.size);
    if (v.rip_relative == 0) return raw;
    if (!rva) return std::nullopt;
    const int64_t target = static_cast<int64_t>(*rva) + v.rip_relative + static_cast<int32_t>(static_cast<uint32_t>(raw));
    if (target < 0 || target > 0xFFFFFFFFll) {
        throw Fail{"symbol " + s.name + ": value " + v.name + " addresses rva " + std::to_string(target) +
                       ", outside any image",
                   s.line};
    }
    return static_cast<uint64_t>(target);
}

// The constant a value has in every build the file declares; unknown when
// constant_in is, or when the builds differ (unknown_why says which).
std::optional<uint64_t> constant_of(const Symbol& s, const SymbolValue& v) {
    if (s.placements.empty()) return constant_in(s, s.rva, s.bytes, s.sha256, v);
    std::optional<uint64_t> out;
    for (const auto& p : s.placements) {
        if (p.absent) continue;
        const auto c = constant_in(s, p.rva, p.bytes, p.sha256, v);
        if (!c || (out && *out != *c)) return std::nullopt;
        out = c;
    }
    return out;
}

std::string unknown_why(const Symbol& s, const SymbolValue& v) {
    if (v.rip_relative != 0 && !s.export_name.empty()) return "an rva resolved when the game loads (export)";
    if (!v.is && std::any_of(s.placements.begin(), s.placements.end(), [](const SymbolPlacement& p) { return !p.sha256.empty(); })) {
        return "the digest does not hold it: give `is`, or read it when the game loads (symbol_value)";
    }
    return "differs between the game builds: read it when the game loads (symbol_value)";
}

std::string where_of(const Symbol& s) {
    std::string out = std::string(symbol_kind_name(s.kind)) + ", ";
    std::string absent;  // the builds the symbol is not in
    for (const auto& p : s.placements) {
        if (p.absent) absent += (absent.empty() ? "" : ", ") + p.build;
    }
    if (!s.export_name.empty()) {
        out += "export " + s.export_name;
    } else if (s.placements.size() <= 1) {
        out += "rva " + hex(*s.rva);
    } else {
        out += "rva";
        bool listed = false;
        for (const auto& p : s.placements) {
            if (p.absent) continue;
            out += std::string(listed ? ", " : " ") + hex(*p.rva) + " in " + p.build;
            listed = true;
        }
    }
    if (!absent.empty()) out += "; absent in " + absent + ": check that it is present before using it";
    return out;
}

std::string header_comment(const SymbolFile& file, const std::string& source) {
    return "Generated by anvil gen " ANVIL_VERSION " from " + comment_text(source) + " (" +
           comment_text(file.build_words()) + "). Do not edit.";
}

std::string generate_c(const SymbolFile& file, const std::string& source) {
    std::string out = "/* " + header_comment(file, source) + " */\n#pragma once\n\n";
    out += "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n";
    out += "/* Symbol ids (AnvilSymbol): the declaration order in symbols.toml. */\nenum {\n";
    for (const auto& s : file.symbols) {
        out += "    SYM_" + upper(s.name) + " = " + std::to_string(s.id) + ",  /* " + where_of(s) + " */\n";
    }
    out += "};\n";

    std::string types;
    for (const auto& s : file.symbols) {
        if (s.kind != SymbolKind::Function) continue;
        const Signature sig = split_signature(s.signature);
        types += "typedef " + sig.result + " (*sym_" + s.name + "_fn)(" + (sig.parameters.empty() ? "void" : sig.parameters) +
                 ");\n";
    }
    if (!types.empty()) out += "\n/* Function pointer types (signature). */\n" + types;

    std::set<std::string> macros;
    std::string values;
    for (const auto& s : file.symbols) {
        for (const auto& v : s.values) {
            const std::string macro = "SYMVAL_" + upper(s.name) + "_" + upper(v.name);
            if (!macros.insert(macro).second) {
                throw Fail{"symbol " + s.name + ": value " + v.name + " gives " + macro + ", which another value already gives",
                           s.line};
            }
            const auto constant = constant_of(s, v);
            const std::string what = v.rip_relative != 0 ? "rva" : std::to_string(v.size) + " bytes";
            if (!constant) {
                values += "/* " + macro + ": " + unknown_why(s, v) + " */\n";
                continue;
            }
            values += "#define " + macro + " UINT64_C(" + hex(*constant) + ")  /* " + what +
                      (v.type.empty() ? "" : ", type " + comment_text(v.type)) + " */\n";
        }
    }
    if (!values.empty()) out += "\n/* Values (values.<name>) from the verified bytes; rip_relative values are RVAs. */\n" + values;
    return out;
}

std::string cpp_type(const Symbol& s) {
    switch (s.kind) {
    case SymbolKind::Function: {
        const Signature sig = split_signature(s.signature);
        return "::anvil::Function<" + sig.result + "(" + sig.parameters + ")>";
    }
    case SymbolKind::Slot: return "::anvil::Slot";
    case SymbolKind::Data: return "::anvil::Data<" + s.type + ">";
    case SymbolKind::Site: return "::anvil::Site";
    }
    return {};
}

std::string generate_cpp(const SymbolFile& file, const std::string& source) {
    // Every name in namespace sym: the symbols and the typed values.
    std::map<std::string, std::string> names;
    auto claim = [&](const std::string& name, const std::string& owner, uint32_t line) {
        if (kCppKeywords.contains(name)) throw Fail{owner + ": '" + name + "' is a C++ keyword; rename it", line};
        const auto [it, added] = names.emplace(name, owner);
        if (!added) throw Fail{owner + ": sym::" + name + " is already " + it->second, line};
    };
    for (const auto& s : file.symbols) claim(s.name, "symbol " + s.name, s.line);
    for (const auto& s : file.symbols) {
        for (const auto& v : s.values) {
            const std::string owner = "symbol " + s.name + ": value " + v.name;
            if (kCppKeywords.contains(v.name)) throw Fail{owner + ": '" + v.name + "' is a C++ keyword; rename it", s.line};
            if (kReservedMembers.contains(v.name)) {
                throw Fail{owner + ": the name would hide the symbol's '" + v.name + "' member; rename it", s.line};
            }
            if (!v.type.empty() && v.rip_relative != 0) claim(v.name, owner, s.line);
        }
    }

    std::string out = "// " + header_comment(file, source) + "\n#pragma once\n\n";
    out += "#include <anvil/anvil_symbols.hpp>\n\n#include <cstddef>\n#include <cstdint>\n\n";
    out += "// Types named in symbols.toml (signature, type) must be declared before this header.\n";
    out += "namespace sym {\n";
    for (const auto& s : file.symbols) {
        const std::string type = cpp_type(s);
        const std::string note = s.note.empty() ? "" : "  // " + comment_text(s.note, false);
        out += "\n// " + where_of(s) + "\n";
        // Each symbol has a type of its own, so that a compiler error about it
        // (the wrapper's static_asserts) names it. Values become static
        // constexpr members: sym::<symbol>.<value>.
        out += "namespace Types {\nstruct " + s.name + " : " + type + " {\n";
        out += "    static constexpr const char* symbol_name = \"" + s.name + "\";\n";
        for (const auto& v : s.values) {
            const auto constant = constant_of(s, v);
            const std::string what = v.rip_relative != 0 ? "rva" : std::to_string(v.size) + " bytes";
            if (!constant) {
                out += "    // " + v.name + ": " + unknown_why(s, v) + "\n";
                continue;
            }
            out += "    static constexpr ::std::uint64_t " + v.name + " = " + hex(*constant) + ";  // " + what +
                   (v.type.empty() ? "" : ", type " + comment_text(v.type, false)) + "\n";
        }
        out += "};\n}  // namespace Types\n";
        out += "inline constexpr Types::" + s.name + " " + s.name + "{{" + std::to_string(s.id) + "}};" + note + "\n";
        // A typed rip_relative value addresses data: sym::<value> is an anvil::Data<type>.
        for (const auto& v : s.values) {
            if (v.type.empty() || v.rip_relative == 0) continue;
            out += "inline constexpr ::anvil::Data<" + v.type + "> " + v.name + "{" + std::to_string(s.id) + ", \"" + v.name +
                   "\"};  // at rva " + s.name + "." + v.name + "\n";
        }
    }
    out += "\n}  // namespace sym\n";
    return out;
}

// ---- settings.gen.h / settings.gen.hpp ----------------------------------------------

// Members of anvil::Setting and of every generated setting type that an enum
// option (a static member of the same type) would hide.
const std::set<std::string_view> kReservedSettingMembers = {
    "id", "get", "setting_name", "restart", "default_value", "minimum", "maximum"};

// A C/C++ string literal: printable ASCII as is, everything else escaped in
// octal (which, unlike \x, never takes the next character along).
std::string c_string(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\' || c == '?') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7F) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\%03o", static_cast<unsigned>(c));
            out += escape;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string float_literal(double value) {
    char text[64];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    std::string out(text, ec == std::errc() ? end : text);
    if (out.find_first_of(".e") == std::string::npos) out += ".0";
    return out;
}

std::string int_literal(int64_t value) {
    if (value == std::numeric_limits<int64_t>::min()) return "(-9223372036854775807 - 1)";
    return std::to_string(value);
}

std::string describe(const Setting& s) {
    std::string out = std::string(setting_type_name(s.type)) + ", default " + setting_toml(s, s.default_value);
    if (s.min || s.max) {
        out += ", " + (s.min ? setting_toml(s, *s.min) : std::string()) + ".." + (s.max ? setting_toml(s, *s.max) : std::string());
    }
    out += s.restart ? ", applies at the next start" : ", applies at once";
    return out + ": " + s.label;
}

// comment_text for the player's words (labels, defaults) without a way to
// continue a line comment onto the next line: no "??" (a trigraph for a
// backslash in C) and no backslash at the end.
std::string settings_text(std::string_view text, bool block = true) {
    std::string out = comment_text(text, block);
    for (size_t at = out.find("??"); at != std::string::npos; at = out.find("??", at)) out.replace(at, 2, "? ?");
    if (!block && out.ends_with('\\')) out += '.';
    return out;
}

std::string settings_comment(const std::string& source) {
    return "Generated by anvil gen " ANVIL_VERSION " from " + settings_text(source) + ". Do not edit.";
}

std::string generate_settings_c(const SettingsFile& file, const std::string& source) {
    std::string out = "/* " + settings_comment(source) + " */\n#pragma once\n";
    if (file.settings.empty()) return out + "\n/* settings.toml declares no settings. */\n";
    // CFG_<SETTING> and CFG_<SETTING>_<OPTION> share one namespace.
    std::map<std::string, std::string> names;
    auto claim = [&](const std::string& macro, const std::string& owner, uint32_t line) {
        const auto [it, added] = names.emplace(macro, owner);
        if (!added) throw Fail{owner + " gives " + macro + ", which " + it->second + " already gives", line};
    };
    for (const auto& s : file.settings) claim("CFG_" + upper(s.name), "setting " + s.name, s.line);
    out += "\n/* Setting ids (AnvilSetting): the declaration order in settings.toml.\n"
           "   Read them with host->setting_bool/int/float/string(self, CFG_<NAME>). */\nenum {\n";
    for (const auto& s : file.settings) {
        out += "    CFG_" + upper(s.name) + " = " + std::to_string(s.id) + ",  /* " + settings_text(describe(s)) + " */\n";
    }
    out += "};\n";
    std::string options;
    bool keys = false;
    for (const auto& s : file.settings) {
        keys = keys || s.type == SettingType::Key;
        for (size_t i = 0; i < s.options.size(); ++i) {
            const std::string macro = "CFG_" + upper(s.name) + "_" + upper(s.options[i].name);
            claim(macro, "setting " + s.name + ": option " + s.options[i].name, s.line);
            options += "    " + macro + " = " + std::to_string(i) + ",  /* " + settings_text(s.options[i].label) + " */\n";
        }
    }
    if (!options.empty()) out += "\n/* Options of enum settings: the index setting_int reads. */\nenum {\n" + options + "};\n";
    if (keys) {
        out += "\n/* Key bindings: setting_int reads the virtual-key code with the modifiers above it. */\n"
               "#ifndef ANVIL_KEY_SHIFT\n#define ANVIL_KEY_SHIFT 0x100\n#define ANVIL_KEY_CTRL 0x200\n"
               "#define ANVIL_KEY_ALT 0x400\n#define ANVIL_KEY_VK(code) ((int)((code) & 0xFF))\n#endif\n";
    }
    return out;
}

std::string cpp_value_type(const Setting& s) {
    switch (s.type) {
    case SettingType::Bool: return "bool";
    case SettingType::Int: return "::std::int64_t";
    case SettingType::Float: return "double";
    case SettingType::Enum: return s.name + "_value";
    case SettingType::String: return "::std::string";
    case SettingType::Key: return "::anvil::Key";
    }
    return {};
}

std::string cpp_constant(const Setting& s, const SettingValue& v) {
    switch (s.type) {
    case SettingType::Bool: return v.integer != 0 ? "true" : "false";
    case SettingType::Int: return int_literal(v.integer);
    case SettingType::Float: return float_literal(v.real);
    case SettingType::Enum: return s.name + "_value::" + s.options[static_cast<size_t>(v.integer)].name;
    case SettingType::String: return c_string(v.text);
    case SettingType::Key: return "::anvil::Key{" + hex(static_cast<uint64_t>(v.integer)) + "}";
    }
    return {};
}

std::string generate_settings_cpp(const SettingsFile& file, const std::string& source) {
    // Names in cfg::Types: the settings and the enum classes of enum settings.
    std::map<std::string, std::string> names;
    auto claim = [&](const std::string& name, const std::string& owner, uint32_t line) {
        if (kCppKeywords.contains(name)) throw Fail{owner + ": '" + name + "' is a C++ keyword; rename it", line};
        const auto [it, added] = names.emplace(name, owner);
        if (!added) throw Fail{owner + ": cfg::Types::" + name + " is already " + it->second, line};
    };
    for (const auto& s : file.settings) claim(s.name, "setting " + s.name, s.line);
    for (const auto& s : file.settings) {
        if (s.type != SettingType::Enum) continue;
        claim(s.name + "_value", "the options of setting " + s.name, s.line);
        for (const auto& o : s.options) {
            const std::string owner = "setting " + s.name + ": option " + o.name;
            if (kCppKeywords.contains(o.name)) throw Fail{owner + ": '" + o.name + "' is a C++ keyword; rename it", s.line};
            if (kReservedSettingMembers.contains(o.name)) {
                throw Fail{owner + ": the name would hide the setting's '" + o.name + "' member; rename it", s.line};
            }
        }
    }

    std::string out = "// " + settings_comment(source) + "\n#pragma once\n\n";
    out += "#include <anvil/anvil.hpp>\n\n#include <cstdint>\n#include <string>\n\n";
    out += "// cfg::<name>.get() reads a setting's validated value; restart = false settings can be\n"
           "// subscribed to with anvil::Host::subscribe_setting.\nnamespace cfg {\n";
    for (const auto& s : file.settings) {
        const std::string value = cpp_value_type(s);
        out += "\n// " + settings_text(describe(s), false) + "\nnamespace Types {\n";
        if (s.type == SettingType::Enum) {
            out += "enum class " + s.name + "_value : ::std::int64_t {";
            for (size_t i = 0; i < s.options.size(); ++i) {
                out += std::string(i == 0 ? " " : ", ") + s.options[i].name + " = " + std::to_string(i);
            }
            out += " };\n";
        }
        out += "struct " + s.name + " : ::anvil::Setting<" + value + "> {\n";
        out += "    static constexpr const char* setting_name = \"" + s.name + "\";\n";
        out += std::string("    static constexpr bool restart = ") + (s.restart ? "true" : "false") + ";\n";
        const std::string constant_type = s.type == SettingType::String ? "const char*" : value;
        out += "    static constexpr " + constant_type + " default_value = " + cpp_constant(s, s.default_value) + ";\n";
        if (s.min) out += "    static constexpr " + value + " minimum = " + cpp_constant(s, *s.min) + ";\n";
        if (s.max) out += "    static constexpr " + value + " maximum = " + cpp_constant(s, *s.max) + ";\n";
        for (const auto& o : s.options) {
            out += "    static constexpr " + value + " " + o.name + " = " + value + "::" + o.name + ";  // " +
                   settings_text(o.label, false) + "\n";
        }
        out += "};\n}  // namespace Types\n";
        out += "inline constexpr Types::" + s.name + " " + s.name + "{{" + std::to_string(s.id) + "}};\n";
    }
    out += "\n}  // namespace cfg\n";
    return out;
}

// ---- Rust (symbols.gen.rs) --------------------------------------------------

// Words that cannot name a parameter of a Rust function type as they are.
const std::set<std::string_view> kRustKeywords = {
    "abstract", "as",     "async",   "await",  "become", "box",   "break",  "const",   "continue", "crate",
    "do",       "dyn",    "else",    "enum",   "extern", "false", "final",  "fn",      "for",      "gen",
    "if",       "impl",   "in",      "let",    "loop",   "macro", "match",  "mod",     "move",     "mut",
    "override", "priv",   "pub",     "ref",    "return", "self",  "static", "struct",  "super",    "trait",
    "true",     "try",    "type",    "typeof", "unsafe", "unsized", "use",  "virtual", "where",    "while",
    "yield"};

// The words of the C types gen translates; sizes are those of Windows x64 (long is 32 bits).
const std::set<std::string_view> kBuiltinWords = {"void",   "char",     "short", "int",  "long",  "float",
                                                  "double", "signed",   "unsigned", "bool", "_Bool"};
const std::map<std::string_view, std::string_view> kTypedefs = {
    {"int8_t", "i8"},     {"int16_t", "i16"},     {"int32_t", "i32"},    {"int64_t", "i64"},
    {"uint8_t", "u8"},    {"uint16_t", "u16"},    {"uint32_t", "u32"},   {"uint64_t", "u64"},
    {"size_t", "usize"},  {"uintptr_t", "usize"}, {"intptr_t", "isize"}, {"ptrdiff_t", "isize"},
    {"ssize_t", "isize"}, {"wchar_t", "u16"},     {"char16_t", "u16"},   {"char32_t", "u32"}};
const std::set<std::string_view> kQualifiers = {"volatile", "restrict", "__restrict"};
const std::set<std::string_view> kTags = {"struct", "union", "enum"};

bool is_word(std::string_view token) {
    return !token.empty() && (std::isalpha(static_cast<unsigned char>(token[0])) != 0 || token[0] == '_');
}

// Words, '*', "..." and any other single character, in order.
std::vector<std::string> tokenize_c(std::string_view text) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c) != 0) {
            ++i;
        } else if (std::isalpha(c) != 0 || c == '_') {
            size_t j = i;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) != 0 || text[j] == '_')) ++j;
            tokens.emplace_back(text.substr(i, j - i));
            i = j;
        } else if (text.substr(i, 3) == "...") {
            tokens.emplace_back("...");
            i += 3;
        } else {
            tokens.emplace_back(1, text[i]);
            ++i;
        }
    }
    return tokens;
}

// One C type with the name that may follow it (a parameter), as far as gen reads it.
struct CType {
    bool ok = false;        // false: syntax gen does not translate (function pointers, arrays, references, ...)
    bool variadic = false;  // "..."
    std::string base;       // Rust type of the base, without pointers; empty for void and for unknown types
    std::string unknown;    // the C name of an unknown base
    bool is_void = false;
    bool base_const = false;
    std::vector<bool> star_const;  // const after each '*'
    std::string name;
};

CType parse_c_type(std::string_view text) {
    CType t;
    const auto tokens = tokenize_c(text);
    if (tokens.size() == 1 && tokens[0] == "...") {
        t.ok = t.variadic = true;
        return t;
    }
    std::vector<std::string> words;  // before the first '*', without qualifiers
    size_t i = 0;
    for (; i < tokens.size() && tokens[i] != "*"; ++i) {
        const std::string& w = tokens[i];
        if (!is_word(w)) return t;
        if (w == "const") t.base_const = true;
        else if (!kQualifiers.contains(w) && !kTags.contains(w)) words.push_back(w);
    }
    size_t stars = 0;
    for (; i < tokens.size(); ++i) {
        const std::string& w = tokens[i];
        if (w == "*") {
            ++stars;
            t.star_const.push_back(false);
        } else if (w == "const") {
            t.star_const.back() = true;
        } else if (kQualifiers.contains(w)) {
            continue;
        } else if (is_word(w) && t.name.empty()) {
            t.name = w;
        } else {
            return t;
        }
    }

    size_t builtin = 0;
    while (builtin < words.size() && kBuiltinWords.contains(words[builtin])) ++builtin;
    if (builtin > 0) {
        if (builtin < words.size()) {  // a name after the type, which no pointer separates
            if (stars > 0 || builtin + 1 != words.size() || !t.name.empty()) return t;
            t.name = words[builtin];
        }
        int longs = 0;
        bool is_unsigned = false, is_signed = false, is_char = false, is_short = false, is_float = false,
             is_double = false, is_bool = false;
        for (size_t k = 0; k < builtin; ++k) {
            const std::string& w = words[k];
            if (w == "long") ++longs;
            else if (w == "unsigned") is_unsigned = true;
            else if (w == "signed") is_signed = true;
            else if (w == "char") is_char = true;
            else if (w == "short") is_short = true;
            else if (w == "float") is_float = true;
            else if (w == "double") is_double = true;
            else if (w == "bool" || w == "_Bool") is_bool = true;
        }
        if (words[0] == "void") t.is_void = true;
        else if (is_bool) t.base = "bool";
        else if (is_float) t.base = "f32";
        else if (is_double) t.base = "f64";  // long double is a double on Windows x64
        else if (is_char) t.base = is_unsigned ? "c_uchar" : is_signed ? "c_schar" : "c_char";
        else if (is_short) t.base = is_unsigned ? "c_ushort" : "c_short";
        else if (longs >= 2) t.base = is_unsigned ? "c_ulonglong" : "c_longlong";
        else if (longs == 1) t.base = is_unsigned ? "c_ulong" : "c_long";
        else t.base = is_unsigned ? "c_uint" : "c_int";
    } else {
        // A typedef name or a type gen does not know, then perhaps a name.
        if (words.empty() || words.size() > 2 || (words.size() == 2 && (stars > 0 || !t.name.empty()))) return t;
        if (words.size() == 2) t.name = words[1];
        const auto known = kTypedefs.find(words[0]);
        if (known != kTypedefs.end()) t.base = std::string(known->second);
        else t.unknown = words[0];
    }
    t.ok = true;
    return t;
}

// The Rust spelling of `t` as a parameter or result type, or empty for void by
// value. An unknown type by value becomes `*mut c_void` and a warning (what the
// game passes does not necessarily fit a pointer); behind a pointer it is an
// opaque pointee, quietly. `where` names the place for the warning.
std::string rust_type_of(const CType& t, const std::string& where, const std::string& written, uint32_t line,
                         std::vector<GenWarning>& warnings) {
    auto fallback = [&](const std::string& why) {
        warnings.push_back({where + " (" + comment_text(written, false) + "): " + why +
                                "; the Rust function type has *mut c_void there",
                            line});
        return std::string("*mut c_void");
    };
    if (!t.ok) return fallback("anvil gen does not translate this syntax to Rust");
    if (t.star_const.empty()) {
        if (t.is_void) return {};
        if (t.base.empty()) return fallback("anvil gen does not know the type '" + t.unknown + "' passed by value");
        return t.base;
    }
    std::string out = t.base.empty() ? "c_void" : t.base;
    for (size_t level = 0; level < t.star_const.size(); ++level) {
        const bool is_const = level == 0 ? t.base_const : t.star_const[level - 1];
        out = std::string(is_const ? "*const " : "*mut ") + out;
    }
    return out;
}

std::string camel_case(std::string_view name) {
    std::string out;
    bool up = true;
    for (char c : name) {
        if (c == '_') {
            up = true;
        } else {
            out.push_back(up && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
            up = false;
        }
    }
    return out;
}

// `unsafe extern "C" fn(a: T, ...) -> R` for a symbol's signature.
std::string rust_fn_type(const Symbol& s, std::vector<GenWarning>& warnings) {
    const Signature sig = split_signature(s.signature);
    std::vector<std::string> parameters;
    int depth = 0;
    std::string current;
    for (char c : sig.parameters) {
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            parameters.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!trim(current).empty() || !parameters.empty()) parameters.push_back(current);

    std::string list;
    bool variadic = false;
    const std::string symbol = "symbol " + s.name;
    if (!(parameters.size() == 1 && trim(parameters[0]) == "void")) {
        for (size_t i = 0; i < parameters.size(); ++i) {
            const std::string written(trim(parameters[i]));
            const CType t = parse_c_type(written);
            if (t.variadic) {
                variadic = true;
                continue;
            }
            std::string name = t.name;
            if (name.empty()) name = "arg" + std::to_string(i);
            else if (kRustKeywords.contains(name)) name += "_";
            if (!list.empty()) list += ", ";
            list += name + ": " + rust_type_of(t, symbol + ": parameter " + std::to_string(i + 1), written, s.line, warnings);
        }
    }
    if (variadic) list += list.empty() ? "..." : ", ...";
    const std::string written_result(trim(sig.result));
    const std::string result = rust_type_of(parse_c_type(written_result), symbol + ": result", written_result, s.line, warnings);
    return "unsafe extern \"C\" fn(" + list + ")" + (result.empty() ? "" : " -> " + result);
}

std::string generate_rust(const SymbolFile& file, const std::string& source, std::vector<GenWarning>& warnings) {
    // Ids, function types and values share the module's namespace.
    std::map<std::string, std::string> names;
    auto claim = [&](const std::string& name, const std::string& owner, uint32_t line) {
        const auto [it, added] = names.emplace(name, owner);
        if (!added) throw Fail{owner + ": the Rust name " + name + " is already " + it->second, line};
    };
    for (const auto& s : file.symbols) {
        claim(upper(s.name), "symbol " + s.name, s.line);
        if (s.kind == SymbolKind::Function) claim(camel_case(s.name) + "Fn", "the function type of symbol " + s.name, s.line);
    }
    for (const auto& s : file.symbols) {
        for (const auto& v : s.values) {
            claim(upper(s.name) + "_" + upper(v.name), "symbol " + s.name + ": value " + v.name, s.line);
        }
    }

    std::string out = "// " + header_comment(file, source) + "\n";
    out += "// Include it in a module: #[allow(dead_code)] mod sym { include!(\"symbols.gen.rs\"); }\n\n";
    out += "#[allow(unused_imports)]\nuse ::core::ffi::{\n    c_char, c_int, c_long, c_longlong, c_schar, c_short, c_uchar, c_uint, c_ulong, c_ulonglong,\n    c_ushort, c_void,\n};\n\n";
    // Plain u32 (the type anvil_sys::AnvilSymbol names), so the file needs no
    // crate: a plugin on anvil-sdk alone includes it as one on anvil-sys does.
    out += "// Symbol ids (u32, anvil_sys::AnvilSymbol): the declaration order in symbols.toml.\n";
    for (const auto& s : file.symbols) {
        out += "/// " + where_of(s) + (s.note.empty() ? "" : ": " + comment_text(s.note, false)) + "\n";
        out += "pub const " + upper(s.name) + ": u32 = " + std::to_string(s.id) + ";\n";
    }

    std::string types;
    for (const auto& s : file.symbols) {
        if (s.kind != SymbolKind::Function) continue;
        const size_t before = warnings.size();
        const std::string fn = rust_fn_type(s, warnings);
        types += "/// `" + comment_text(s.signature, false) + "`\n";
        for (size_t w = before; w < warnings.size(); ++w) types += "// warning: " + comment_text(warnings[w].message, false) + "\n";
        types += "pub type " + camel_case(s.name) + "Fn = " + fn + ";\n";
    }
    if (!types.empty()) out += "\n// Function types (signature).\n" + types;

    std::string values;
    for (const auto& s : file.symbols) {
        for (const auto& v : s.values) {
            const std::string name = upper(s.name) + "_" + upper(v.name);
            const auto constant = constant_of(s, v);
            if (!constant) {
                values += "// " + name + ": " + unknown_why(s, v) + "\n";
                continue;
            }
            values += "/// " + std::string(v.rip_relative != 0 ? "rva" : std::to_string(v.size) + " bytes") +
                      (v.type.empty() ? "" : ", type " + comment_text(v.type, false)) + "\n";
            values += "pub const " + name + ": u64 = " + hex(*constant) + ";\n";
        }
    }
    if (!values.empty()) out += "\n// Values (values.<name>) from the verified bytes; rip_relative values are RVAs.\n" + values;
    return out;
}

// ---- settings.gen.rs ---------------------------------------------------------------

std::string generate_settings_rust(const SettingsFile& file, const std::string& source) {
    std::string out = "// " + settings_comment(source) + "\n";
    out += "// Include it in a module: #[allow(dead_code)] mod cfg { include!(\"settings.gen.rs\"); }\n";
    if (file.settings.empty()) return out + "\n// settings.toml declares no settings.\n";
    // Ids, enum options and the key flags share the module's namespace.
    std::map<std::string, std::string> names;
    auto claim = [&](const std::string& name, const std::string& owner, uint32_t line) {
        const auto [it, added] = names.emplace(name, owner);
        if (!added) throw Fail{owner + " gives " + name + ", which " + it->second + " already gives", line};
    };
    bool keys = false;
    for (const auto& s : file.settings) {
        claim(upper(s.name), "setting " + s.name, s.line);
        keys = keys || s.type == SettingType::Key;
    }
    std::string options;
    for (const auto& s : file.settings) {
        for (size_t i = 0; i < s.options.size(); ++i) {
            const std::string name = upper(s.name) + "_" + upper(s.options[i].name);
            claim(name, "setting " + s.name + ": option " + s.options[i].name, s.line);
            options += "/// " + settings_text(s.options[i].label, false) + "\npub const " + name + ": i64 = " + std::to_string(i) + ";\n";
        }
    }
    if (keys) {
        for (const char* name : {"KEY_SHIFT", "KEY_CTRL", "KEY_ALT", "KEY_VK_MASK"}) {
            claim(name, std::string("the key flag ") + name, file.settings.front().line);
        }
    }
    out += "\n// Setting ids (u32, anvil_sys::AnvilSetting): the declaration order in settings.toml.\n"
           "// Read them with host.setting_bool/int/float/string(self, <NAME>).\n";
    for (const auto& s : file.settings) {
        out += "/// " + settings_text(describe(s), false) + "\n";
        out += "pub const " + upper(s.name) + ": u32 = " + std::to_string(s.id) + ";\n";
    }
    if (!options.empty()) out += "\n// Options of enum settings: the index setting_int reads.\n" + options;
    if (keys) {
        out += "\n// Key bindings: setting_int reads the virtual-key code with the modifiers above it.\n"
               "pub const KEY_SHIFT: i64 = 0x100;\npub const KEY_CTRL: i64 = 0x200;\npub const KEY_ALT: i64 = 0x400;\n"
               "pub const KEY_VK_MASK: i64 = 0xFF;\n";
    }
    return out;
}

}  // namespace

GeneratedHeaders generate_settings_headers(const SettingsFile& file, const std::string& source, bool c, bool cpp,
                                           bool rust) {
    GeneratedHeaders out;
    try {
        if (c) out.c = generate_settings_c(file, source);
        if (cpp) out.cpp = generate_settings_cpp(file, source);
        if (rust) out.rust = generate_settings_rust(file, source);
    } catch (const Fail& f) {
        out = {};
        out.error = "settings.toml: " + f.message;
        out.line = f.line;
    }
    return out;
}

GeneratedHeaders generate_headers(const SymbolFile& file, const std::string& source, bool c, bool cpp, bool rust) {
    GeneratedHeaders out;
    try {
        if (c) out.c = generate_c(file, source);
        if (cpp) out.cpp = generate_cpp(file, source);
        if (rust) out.rust = generate_rust(file, source, out.warnings);
    } catch (const Fail& f) {
        out = {};
        out.error = "symbols.toml: " + f.message;
        out.line = f.line;
    }
    return out;
}

}  // namespace anvil::cli
