#include "cli/port.hpp"

#include "cli/check.hpp"
#include "cli/port_match.hpp"
#include "core/game_build.hpp"
#include "core/instructions.hpp"
#include "core/pe_image.hpp"
#include "core/symbols.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace anvil::cli {
namespace {

using Keys = std::vector<std::pair<std::string, std::string>>;

std::string hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// Whether byte `offset` of the symbol belongs to one of its `values`: the
// plugin reads those at load, so they may differ between builds.
bool in_a_value(const Symbol& s, uint32_t offset) {
    return std::any_of(s.values.begin(), s.values.end(), [&](const SymbolValue& v) {
        const uint32_t width = v.rip_relative != 0 ? 4 : v.size;
        return offset >= v.at && offset < v.at + width;
    });
}

std::string bytes_text(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& mask) {
    std::string out = "\"";
    char pair[4];
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out += ' ';
        std::snprintf(pair, sizeof(pair), "%02X", bytes[i]);
        out += mask[i] != 0 ? pair : "??";
    }
    return out + "\"";
}

// The keys that state a placement's bytes, in the form the file uses for them.
Keys code_keys(const SymbolPlacement& p) {
    if (p.sha256.empty()) return {{"bytes", bytes_text(p.bytes, p.mask)}};
    Keys keys = {{"sha256", "\"" + p.sha256 + "\""}, {"size", std::to_string(p.mask.size())}};
    std::string any;
    for (size_t i = 0; i < p.mask.size(); ++i) {
        if (p.mask[i] == 0) any += (any.empty() ? "" : ", ") + std::to_string(i);
    }
    if (!any.empty()) keys.emplace_back("any", "[" + any + "]");
    return keys;
}

// A value in bytes at `rva`, as verification computes it.
uint64_t value_at(const SymbolValue& v, uint32_t rva, const std::vector<uint8_t>& bytes) {
    uint64_t raw = 0;
    std::memcpy(&raw, bytes.data() + v.at, v.rip_relative != 0 ? 4 : v.size);
    if (v.rip_relative == 0) return raw;
    return static_cast<uint64_t>(static_cast<int64_t>(rva) + v.rip_relative + static_cast<int32_t>(static_cast<uint32_t>(raw)));
}

// Line edits of symbols.toml: everything it does not touch stays as it was,
// comments and layout included. Lines are 1-based, as toml++ counts them.
class TextEdit {
public:
    explicit TextEdit(const std::string& text) {
        eol_ = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        size_t at = 0;
        while (at <= text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines_.push_back(std::move(line));
            at = end + 1;
        }
        after_.resize(lines_.size() + 1);
        dropped_.resize(lines_.size() + 1);
    }

    std::string& line(uint32_t number) { return lines_[number - 1]; }
    void insert_after(uint32_t number, const std::string& text) { after_[number].push_back(text); }
    void drop(uint32_t number) { dropped_[number] = true; }

    // The last line of the table whose header is at `header`, before the next header.
    uint32_t end_of_table(uint32_t header) const {
        uint32_t last = header;
        for (uint32_t number = header + 1; number <= lines_.size(); ++number) {
            const std::string& text = lines_[number - 1];
            const size_t start = text.find_first_not_of(" \t");
            if (start == std::string::npos || text[start] == '#') continue;
            if (text[start] == '[') break;
            last = number;
        }
        return last;
    }

    std::string text() const {
        std::vector<std::string> out;
        for (uint32_t number = 1; number <= lines_.size(); ++number) {
            if (!dropped_[number]) out.push_back(lines_[number - 1]);
            out.insert(out.end(), after_[number].begin(), after_[number].end());
        }
        std::string joined;
        for (size_t i = 0; i < out.size(); ++i) joined += (i == 0 ? "" : eol_) + out[i];
        return joined;
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::vector<std::string>> after_;
    std::vector<bool> dropped_;
    std::string eol_;
};

// Where a line's TOML ends, before its comment and trailing spaces; 0 when it has none.
size_t code_end(const std::string& line) {
    const std::string code = line.substr(0, line.find('#'));
    const size_t last = code.find_last_not_of(" \t");
    return last == std::string::npos ? 0 : last + 1;
}

std::string inline_table(const Keys& keys) {
    std::string out = "{ ";
    for (size_t i = 0; i < keys.size(); ++i) out += (i == 0 ? "" : ", ") + keys[i].first + " = " + keys[i].second;
    return out + " }";
}

// A key of the symbol's own table that goes away: its comment stays on its line.
void drop_key(TextEdit& edit, const toml::node& node) {
    const auto& source = node.source();
    for (auto number = static_cast<uint32_t>(source.begin.line); number < source.end.line; ++number) edit.drop(number);
    const auto last = static_cast<uint32_t>(source.end.line);
    std::string& text = edit.line(last);
    const size_t comment = text.find('#', source.end.column - 1);
    if (comment == std::string::npos) {
        edit.drop(last);
    } else {
        text.erase(0, comment);
    }
}

// A value's `is` that goes away: from an inline table, or as a line of its own.
void drop_value_is(TextEdit& edit, const toml::node& is) {
    const auto& source = is.source();
    std::string& text = edit.line(static_cast<uint32_t>(source.begin.line));
    const size_t value_end = source.end.column - 1;
    const size_t key = text.rfind("is", text.rfind('=', source.begin.column - 1));
    const size_t before = key == 0 ? std::string::npos : text.find_last_not_of(" \t", key - 1);
    if (before != std::string::npos && text[before] == ',') {
        text.erase(before, value_end - before);
    } else if (before != std::string::npos && text[before] == '{') {
        const size_t next = text.find_first_not_of(", \t", value_end);
        text.erase(key, (next == std::string::npos ? value_end : next) - key);
    } else {
        drop_key(edit, is);
    }
}

// Adds `keys` to the build's table of the symbol, or writes that table.
void add_to_build(TextEdit& edit, const toml::table& symbol, const std::string& name, const std::string& build, const Keys& keys) {
    const toml::table* builds = symbol["build"].as_table();
    bool headers = false;
    uint32_t last_inline = 0, last_header = 0;
    if (builds != nullptr) {
        for (const auto& [id, node] : *builds) {
            const toml::table* table = node.as_table();
            if (table == nullptr) continue;
            if (table->is_inline()) {
                last_inline = std::max(last_inline, static_cast<uint32_t>(table->source().end.line));
            } else {
                headers = true;
                last_header = std::max(last_header, edit.end_of_table(static_cast<uint32_t>(table->source().begin.line)));
            }
            if (id.str() != build) continue;
            if (table->is_inline()) {
                std::string& text = edit.line(static_cast<uint32_t>(table->source().end.line));
                size_t close = text.rfind('}', table->source().end.column - 1);
                const size_t before = text.find_last_not_of(' ', close - 1);
                std::string added;
                for (const auto& [key, value] : keys) added += ", " + key + " = " + value;
                text.replace(before + 1, close - before - 1, added + " ");
            } else {
                for (const auto& [key, value] : keys) edit.insert_after(static_cast<uint32_t>(table->source().begin.line), key + " = " + value);
            }
            return;
        }
    }
    if (headers) {
        edit.insert_after(last_header, "");
        edit.insert_after(last_header, "[symbol." + name + ".build.\"" + build + "\"]");
        for (const auto& [key, value] : keys) edit.insert_after(last_header, key + " = " + value);
        return;
    }
    const uint32_t after = last_inline != 0 ? last_inline : edit.end_of_table(static_cast<uint32_t>(symbol.source().begin.line));
    edit.insert_after(after, "build.\"" + build + "\" = " + inline_table(keys));
}

// symbols.toml with the new build added: `placed` holds where each ported
// symbol is, `manual` gets a comment at its table, `moved` names the values that
// lose their `is`. nullopt with `error` when the text does not parse as it did.
std::optional<std::string> with_new_build(const std::string& text, const SymbolFile& file, const std::string& build,
                                          const std::map<std::string, SymbolPlacement>& placed,
                                          const std::vector<ManualSymbol>& manual,
                                          const std::map<std::string, std::vector<std::string>>& moved, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        error = std::string(e.description());
        return std::nullopt;
    }
    TextEdit edit(text);
    if (const auto* builds = root.get("builds")) {
        const auto last = static_cast<uint32_t>(builds->source().end.line);
        std::string& line = edit.line(last);
        const size_t close = line.rfind(']', builds->source().end.column - 1);
        const size_t before = close == 0 ? std::string::npos : line.find_last_not_of(" \t", close - 1);
        const std::string id = "\"" + build + "\"";
        if (before != std::string::npos) {
            line.insert(before + 1, line[before] == ',' ? " " + id : ", " + id);
        } else {
            // The bracket on its own line: the new id gets one too, after the last id.
            uint32_t above = last - 1;
            while (above > builds->source().begin.line && code_end(edit.line(above)) == 0) --above;
            std::string& previous = edit.line(above);
            const size_t end = code_end(previous);
            if (previous[end - 1] != ',') previous.insert(end, ",");
            edit.insert_after(above, previous.substr(0, previous.find_first_not_of(" \t")) + id + ",");
        }
    } else if (const auto* single = root.get("build")) {
        std::string& line = edit.line(static_cast<uint32_t>(single->source().begin.line));
        const size_t value_end = single->source().end.column - 1;
        line = "builds = [" + line.substr(single->source().begin.column - 1, value_end - (single->source().begin.column - 1)) +
               ", \"" + build + "\"]" + line.substr(value_end);
    }
    const toml::table* symbols = root["symbol"].as_table();
    for (const Symbol& s : file.symbols) {
        const toml::table* table = symbols != nullptr ? (*symbols)[s.name].as_table() : nullptr;
        if (table == nullptr) continue;
        const auto found = placed.find(s.name);
        if (found == placed.end()) {
            const auto m = std::find_if(manual.begin(), manual.end(), [&](const ManualSymbol& e) { return e.name == s.name; });
            if (m == manual.end()) continue;  // an export: nothing depends on the build
            std::string note = "# anvil port: not placed in " + build + ": " + m->reason;
            if (m->candidate) note += " (look at " + hex(*m->candidate) + ")";
            edit.insert_after(static_cast<uint32_t>(table->source().begin.line), note);
            // A shared `rva` would claim the old place for the new build: without one the
            // file does not parse, so nothing runs before a person has placed the symbol.
            if (const auto* rva = table->get("rva")) {
                drop_key(edit, *rva);
                for (const SymbolPlacement& p : s.placements) {
                    if (!p.absent && p.rva) add_to_build(edit, *table, s.name, p.build, {{"rva", hex(*p.rva)}});
                }
            }
            continue;
        }
        const SymbolPlacement& now = found->second;
        Keys fresh;
        std::map<std::string, Keys> others;
        // One group of keys: per build as it is, or shared and still true, or shared
        // and no longer true, when every build gets its own.
        const auto group = [&](std::initializer_list<const char*> names, const Keys& keys, bool same, const auto& old_keys) {
            if (keys.empty()) return;
            const bool shared = std::any_of(names.begin(), names.end(), [&](const char* key) { return table->contains(key); });
            if (shared && same) return;
            fresh.insert(fresh.end(), keys.begin(), keys.end());
            if (!shared) return;
            for (const char* key : names) {
                if (const auto* node = table->get(key)) drop_key(edit, *node);
            }
            for (const SymbolPlacement& p : s.placements) {
                if (p.absent) continue;
                const Keys moved = old_keys(p);
                auto& keys_of_build = others[p.build];
                keys_of_build.insert(keys_of_build.end(), moved.begin(), moved.end());
            }
        };
        const auto value_key = [](const char* key, std::optional<uint32_t> value) {
            return value ? Keys{{key, hex(*value)}} : Keys{};
        };
        const auto shared_u32 = [&](const char* key) { return table->get_as<int64_t>(key) != nullptr ? std::optional<uint32_t>(static_cast<uint32_t>(table->get_as<int64_t>(key)->get())) : std::nullopt; };
        group({"rva"}, value_key("rva", now.rva), shared_u32("rva") == now.rva, [&](const SymbolPlacement& p) { return value_key("rva", p.rva); });
        group({"points_to"}, value_key("points_to", now.points_to), shared_u32("points_to") == now.points_to,
              [&](const SymbolPlacement& p) { return value_key("points_to", p.points_to); });
        const bool has_code = !now.bytes.empty();
        bool same_code = false;
        if (has_code && s.bytes.size() == now.bytes.size()) {
            same_code = true;
            for (size_t i = 0; i < now.bytes.size(); ++i) same_code = same_code && (now.bytes[i] & now.mask[i]) == (s.bytes[i] & now.mask[i]);
        }
        group({"bytes", "sha256", "size", "any"}, has_code ? code_keys(now) : Keys{}, same_code, [](const SymbolPlacement& p) { return code_keys(p); });
        for (const auto& [id, keys] : others) {
            if (!keys.empty()) add_to_build(edit, *table, s.name, id, keys);
        }
        if (!fresh.empty()) add_to_build(edit, *table, s.name, build, fresh);
        if (const auto values = moved.find(s.name); values != moved.end()) {
            // Right to left, as one line may hold several.
            std::vector<const toml::node*> keys;
            for (const std::string& value : values->second) keys.push_back((*table)["values"][value]["is"].node());
            std::sort(keys.begin(), keys.end(), [](const toml::node* a, const toml::node* b) { return a->source().begin.column > b->source().begin.column; });
            for (const toml::node* key : keys) drop_value_is(edit, *key);
        }
    }
    return edit.text();
}

}  // namespace

namespace {

bool version_digits(std::string_view text) {
    if (text.empty() || text.front() == '.' || text.back() == '.') return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return c == '.' || (c >= '0' && c <= '9'); });
}

// eu4-<version>-<store>-win64
bool valid_build_id(std::string_view id) {
    constexpr std::string_view prefix = "eu4-", suffix = "-win64";
    if (!id.starts_with(prefix) || !id.ends_with(suffix) || id.size() <= prefix.size() + suffix.size()) return false;
    const std::string_view middle = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    const size_t dash = middle.find('-');
    if (dash == std::string_view::npos) return false;
    const std::string_view store = middle.substr(dash + 1);
    return version_digits(middle.substr(0, dash)) && !store.empty() &&
           std::all_of(store.begin(), store.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

}  // namespace

std::string port_build_problem(const PortOptions& options) {
    if (options.unknown_build) {
        if (valid_build_id(options.build)) return {};
        return "--build must have the form eu4-<version>-<store>-win64, as eu4-1.37.5.2-gog-win64 has";
    }
    const std::string sha256 = sha256_file(options.to);
    if (sha256.empty()) return {};  // port_symbols says why it cannot read the file
    const KnownBuild* known = find_known_build_by_sha256(sha256);
    if (known == nullptr) {
        return "--to is not a build this Anvil knows, so a plugin ported to it cannot load: add its row to the table of "
               "builds and use that anvil.exe, or pass --unknown-build to port it before that";
    }
    if (options.build == known->id) return {};
    return "--to is " + std::string(known->id) + ", not " + options.build;
}

PortReport port_symbols(const PortOptions& options) {
    PortReport report;
    report.build = options.build;
    auto cannot = [&](const std::string& why) {
        report.exit_code = kExitCannotCheck;
        report.cannot_port = why;
        return report;
    };
    std::ifstream in(options.symbols, std::ios::binary);
    if (!in) return cannot("cannot read " + shown(options.symbols));
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    const SymbolFileResult parsed = parse_symbols_text(text);
    if (!parsed.file) return cannot(parsed.error);
    SymbolFile file = *parsed.file;
    if (file.declares(options.build)) return cannot("symbols.toml already declares " + options.build);

    std::string error;
    const auto old_reader = FileImageReader::load(options.from, error);
    if (!old_reader) return cannot("--from: " + error);
    const auto old_pe = PeImage::parse(*old_reader, error);
    if (!old_pe) return cannot("--from: " + error);
    const auto new_reader = FileImageReader::load(options.to, error);
    if (!new_reader) return cannot("--to: " + error);
    const auto new_pe = PeImage::parse(*new_reader, error);
    if (!new_pe) return cannot("--to: " + error);

    report.from_build = options.from_build;
    if (report.from_build.empty()) {
        const KnownBuild* known = find_known_build_by_sha256(sha256_file(options.from));
        if (known == nullptr) return cannot("--from is not a build this Anvil knows: name it with --from-build");
        report.from_build = known->id;
    }
    if (!select_build(file, report.from_build)) {
        return cannot("symbols.toml does not declare " + report.from_build + "; it declares " + file.build_list());
    }

    // What the file says must hold on the old build, or there is nothing to carry over.
    const SymbolVerification old_check = verify_symbols(file, *old_pe);
    std::vector<std::string> old_problem(file.symbols.size());
    for (size_t i = 0; i < old_check.problems.size(); ++i) old_problem[old_check.failed[i]] = old_check.problems[i];
    adopt_verified_bytes(file, old_check);

    const auto from = BuildImage::load(options.from, error);
    if (!from) return cannot("--from: " + error);
    const auto to = BuildImage::load(options.to, error);
    if (!to) return cannot("--to: " + error);
    const BuildMatcher matcher(*from, *to);
    report.functions = matcher.functions();
    report.functions_paired = matcher.functions_paired();
    report.functions_changed = matcher.functions_changed();
    report.symbols = file.symbols.size();

    // Where each symbol is, and the bytes it has there.
    std::map<std::string, SymbolPlacement> placed;
    const auto place = [&](const Symbol& s, uint32_t rva, std::optional<uint32_t> points_to) {
        SymbolPlacement p;
        p.build = options.build;
        p.rva = rva;
        p.points_to = points_to;
        p.mask = s.mask;
        p.bytes.resize(s.bytes.size());
        if (!p.bytes.empty() && !new_reader->read(rva, p.bytes.data(), p.bytes.size())) p.bytes.assign(s.bytes.size(), 0);
        if (!s.sha256.empty()) {
            std::vector<uint8_t> masked(p.bytes.size());
            for (size_t i = 0; i < masked.size(); ++i) masked[i] = p.bytes[i] & p.mask[i];
            p.sha256 = sha256_bytes(masked.data(), masked.size());
        }
        bool changed = false;
        for (size_t i = 0; i < p.bytes.size(); ++i) changed = changed || (p.bytes[i] & p.mask[i]) != (s.bytes[i] & p.mask[i]);
        placed[s.name] = std::move(p);
        return changed;
    };

    for (const Symbol& s : file.symbols) {
        ManualSymbol manual{s.name, s.line, s.rva, {}, {}};
        if (s.absent) {
            manual.reason = "absent on " + report.from_build + ": nothing to carry over";
            report.manual.push_back(std::move(manual));
            continue;
        }
        if (!old_problem[s.id].empty()) {
            manual.reason = "does not verify on " + report.from_build + ": " + old_problem[s.id];
            report.manual.push_back(std::move(manual));
            continue;
        }
        if (!s.rva) {  // an export: found by name wherever it is
            report.ported.push_back({s.name, s.line, std::nullopt, std::nullopt, std::nullopt, "export", false, {}});
            continue;
        }
        const uint32_t size = static_cast<uint32_t>(s.bytes.size());
        PortMatch match;
        if (s.points_to) match = matcher.slot(*s.rva, *s.points_to);
        else if (from->is_code(*s.rva)) match = matcher.code(*s.rva, std::max(size, 1u));
        else match = matcher.data(*s.rva, size);
        if (!match.rva) {
            manual.reason = match.reason;
            manual.candidate = match.candidate;
            report.manual.push_back(std::move(manual));
            continue;
        }
        // Operands no value declares are the code's meaning, which a person
        // decides; the entry of a paired function is still the same function.
        std::string differ;
        if (!s.bytes.empty() && !s.points_to && from->is_code(*s.rva)) {
            for (const uint32_t offset : matcher.operands_differ(*s.rva, *match.rva, size)) {
                if (s.mask[offset] == 0 || in_a_value(s, offset)) continue;
                differ = "an operand differs at +" + hex(offset);
                break;
            }
        }
        const bool whole_function = s.kind == SymbolKind::Function && matcher.same_shape_at(*s.rva, *match.rva);
        if (!differ.empty() && !(match.paired && whole_function)) {
            manual.reason = "found, but " + differ + " and no value declares it";
            manual.candidate = match.rva;
            report.manual.push_back(std::move(manual));
            continue;
        }
        std::optional<uint32_t> points_to;
        if (s.points_to) points_to = to->pointer_at(*match.rva);
        PortedSymbol ported{s.name, s.line, s.rva, match.rva, points_to, match.method, false, differ};
        ported.bytes_changed = place(s, *match.rva, points_to);
        report.ported.push_back(std::move(ported));
    }

    // The function a ported slot holds is known even when the function alone proves nothing.
    std::vector<PortedSymbol> held;
    std::erase_if(report.manual, [&](const ManualSymbol& m) {
        const Symbol* s = file.find(m.name);
        if (s == nullptr || !s->rva || s->absent || !from->is_code(*s->rva)) return false;
        for (const PortedSymbol& ported : report.ported) {
            const Symbol* slot = file.find(ported.name);
            if (slot == nullptr || slot->points_to != s->rva || !ported.points_to) continue;
            if (!matcher.same_shape_at(*s->rva, *ported.points_to)) continue;
            std::string review;
            for (const uint32_t offset : matcher.operands_differ(*s->rva, *ported.points_to, static_cast<uint32_t>(s->bytes.size()))) {
                if (s->mask[offset] != 0 && !in_a_value(*s, offset)) review = "an operand differs at +" + hex(offset);
            }
            PortedSymbol found{s->name, s->line, s->rva, ported.points_to, std::nullopt, "held by " + slot->name, false, review};
            found.bytes_changed = place(*s, *ported.points_to, std::nullopt);
            held.push_back(std::move(found));
            return true;
        }
        return false;
    });
    report.ported.insert(report.ported.end(), held.begin(), held.end());

    // The checks of anvil check --exe on the new build, whatever Anvil knows about it yet.
    SymbolFile target = file;
    // A digest cannot show a value, so the converter writes each one as `is`, also a
    // field offset or an address, which a new build changes. Bytes declare the same
    // values without promising them, so the two forms port alike: the value is no
    // longer constant, and `anvil gen` reads it when the game loads.
    std::map<std::string, std::vector<std::string>> moved;
    for (Symbol& s : target.symbols) {
        const auto found = placed.find(s.name);
        if (found != placed.end()) {
            s.rva = found->second.rva;
            s.points_to = found->second.points_to;
            s.bytes = found->second.bytes;
            s.sha256 = found->second.sha256;
            if (s.sha256.empty()) continue;
            for (SymbolValue& v : s.values) {
                if (!v.is || *v.is == value_at(v, *s.rva, s.bytes)) continue;
                v.is.reset();
                moved[s.name].push_back(v.name);
            }
        } else if (s.export_name.empty() || s.absent || !old_problem[s.id].empty()) {
            s.absent = true;
        }
    }
    const SymbolVerification new_check = verify_symbols(target, *new_pe);
    std::map<std::string, std::string> failed;
    for (size_t i = 0; i < new_check.problems.size(); ++i) failed[target.symbols[new_check.failed[i]].name] = new_check.problems[i];
    for (const Symbol& s : target.symbols) {
        if (s.absent || !s.rva || failed.contains(s.name)) continue;
        const Section* section = new_pe->section_of(*s.rva, 1);
        if (section == nullptr || !has_instruction_boundaries(s, section->kind)) continue;
        const std::string problem = check_instruction_boundaries(s, *s.rva, *new_reader);
        if (!problem.empty()) failed[s.name] = problem;
    }
    std::erase_if(report.ported, [&](const PortedSymbol& p) {
        const auto found = failed.find(p.name);
        if (found == failed.end()) return false;
        report.manual.push_back({p.name, p.line, p.old_rva, "does not verify on the new build: " + found->second, p.rva});
        placed.erase(p.name);
        moved.erase(p.name);
        return true;
    });
    for (PortedSymbol& p : report.ported) {
        if (const auto values = moved.find(p.name); values != moved.end()) p.values_changed = values->second;
    }
    const auto by_line = [](const auto& a, const auto& b) { return a.line < b.line; };
    std::stable_sort(report.ported.begin(), report.ported.end(), by_line);
    std::stable_sort(report.manual.begin(), report.manual.end(), by_line);
    report.exit_code = report.manual.empty() ? kExitOk : kExitProblems;

    if (options.out.empty()) return report;
    const auto written = with_new_build(text, file, options.build, placed, report.manual, moved, error);
    if (!written) return cannot("cannot rewrite symbols.toml: " + error);
    // Symbols left by hand fail the schema until they are placed, never the TOML.
    try {
        (void)toml::parse(*written);
    } catch (const toml::parse_error& e) {
        return cannot("the rewritten symbols.toml is not TOML: " + std::string(e.description()));
    }
    if (report.manual.empty()) {
        // A file with symbols left to place cannot parse yet; a complete one must read
        // back and verify on both builds, or the rewrite went wrong.
        const SymbolFileResult back = parse_symbols_text(*written);
        if (!back.file) return cannot("the rewritten symbols.toml does not parse: " + back.error);
        for (const auto& [build, pe] : {std::pair{report.from_build, &*old_pe}, std::pair{options.build, &*new_pe}}) {
            SymbolFile check = *back.file;
            if (!select_build(check, build)) return cannot("the rewritten symbols.toml does not declare " + build);
            const SymbolVerification verified = verify_symbols(check, *pe);
            if (!verified.ok()) return cannot("the rewritten symbols.toml fails on " + build + ": " + verified.problems[0]);
        }
    }
    std::ofstream out(options.out, std::ios::binary | std::ios::trunc);
    out << *written;
    out.close();
    if (!out) return cannot("cannot write " + shown(options.out));
    report.written = true;
    return report;
}

std::string format_port_report(const PortReport& report, const std::filesystem::path& symbols, Format format) {
    const auto optional_hex = [](const std::optional<uint32_t>& value) { return value ? json_string(hex(*value)) : std::string("null"); };
    if (format == Format::Json) {
        std::string out = "{\n  \"tool\": " + json_string(tool_name()) + ",\n";
        const char* result = report.exit_code == kExitCannotCheck ? "cannot-port" : report.manual.empty() ? "ok" : "manual";
        out += "  \"result\": \"" + std::string(result) + "\",\n  \"exit_code\": " + std::to_string(report.exit_code) + ",\n";
        if (!report.cannot_port.empty()) out += "  \"cannot_port\": " + json_string(report.cannot_port) + ",\n";
        out += "  \"from_build\": " + json_string(report.from_build) + ",\n  \"build\": " + json_string(report.build) + ",\n";
        out += "  \"functions\": " + std::to_string(report.functions) + ",\n";
        out += "  \"functions_paired\": " + std::to_string(report.functions_paired) + ",\n";
        out += "  \"functions_changed\": " + std::to_string(report.functions_changed) + ",\n";
        // name -> address, the map a generated symbols.toml can be fed from (Mei's tools/gen-symbols.py).
        out += "  \"symbols\": {";
        bool first = true;
        for (const auto& p : report.ported) {
            if (!p.rva) continue;
            out += std::string(first ? "\n" : ",\n") + "    " + json_string(p.name) + ": " + json_string(hex(*p.rva));
            first = false;
        }
        out += first ? "},\n" : "\n  },\n";
        out += "  \"ported\": [";
        first = true;
        for (const auto& p : report.ported) {
            out += std::string(first ? "\n" : ",\n") + "    {\"name\": " + json_string(p.name) + ", \"line\": " + std::to_string(p.line) +
                   ", \"old_rva\": " + optional_hex(p.old_rva) + ", \"rva\": " + optional_hex(p.rva) + ", \"points_to\": " + optional_hex(p.points_to) +
                   ", \"method\": " + json_string(p.method) + ", \"bytes_changed\": " + (p.bytes_changed ? "true" : "false") +
                   ", \"review\": " + (p.review.empty() ? "null" : json_string(p.review));
            if (!p.values_changed.empty()) {
                out += ", \"values_changed\": [";
                for (size_t i = 0; i < p.values_changed.size(); ++i) out += (i == 0 ? "" : ", ") + json_string(p.values_changed[i]);
                out += "]";
            }
            out += "}";
            first = false;
        }
        out += first ? "],\n" : "\n  ],\n";
        out += "  \"manual\": [";
        first = true;
        for (const auto& m : report.manual) {
            out += std::string(first ? "\n" : ",\n") + "    {\"name\": " + json_string(m.name) + ", \"line\": " + std::to_string(m.line) +
                   ", \"old_rva\": " + optional_hex(m.old_rva) + ", \"reason\": " + json_string(m.reason) +
                   ", \"candidate\": " + optional_hex(m.candidate) + "}";
            first = false;
        }
        out += first ? "]\n}\n" : "\n  ]\n}\n";
        return out;
    }

    if (!report.cannot_port.empty()) {
        return format == Format::Github ? "::error title=anvil port::" + github_escape(report.cannot_port, false) + "\n"
                                        : "error: cannot port: " + report.cannot_port + "\n";
    }
    std::vector<Problem> to_do, to_read;
    const std::string file = shown(symbols);
    for (const auto& m : report.manual) {
        to_do.push_back({file, m.line, m.name + ": " + m.reason + (m.candidate ? " (look at " + hex(*m.candidate) + ")" : "")});
    }
    size_t changed = 0, values = 0;
    for (const auto& p : report.ported) {
        if (p.bytes_changed) ++changed;
        values += p.values_changed.size();
        if (!p.review.empty()) to_read.push_back({file, p.line, p.name + ": placed at " + hex(p.rva.value_or(0)) + ", but " + p.review});
    }
    std::string out = problem_lines(to_do, format, "error") + problem_lines(to_read, format, "warning");
    out += report.from_build + " -> " + report.build + ": " + std::to_string(report.ported.size()) + " of " + std::to_string(report.symbols) +
           " symbol(s) placed, " + std::to_string(changed) + " with other bytes; " + std::to_string(report.manual.size()) + " left by hand";
    out += report.written ? "; symbols.toml written\n" : "; nothing written\n";
    if (values != 0) {
        out += std::to_string(values) + " value(s) differ in the new build and lose their `is`: anvil gen reads them when the game loads, "
                                        "so code that uses one as a constant stops compiling.\n";
    }
    if (report.written && report.manual.empty()) {
        out += report.build_known ? "Next: anvil check <plugin-dir> --exe <the new eu4.exe>.\n"
                                  : "Next: anvil check <plugin-dir> --exe <the new eu4.exe>, once this Anvil knows the build.\n";
    }
    if (report.written && !report.manual.empty()) out += "The file does not parse until each symbol marked 'anvil port:' is placed by hand or declared absent.\n";
    return out;
}

}  // namespace anvil::cli
