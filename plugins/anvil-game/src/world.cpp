#include "world.hpp"

#include "session.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace anvil_game {

namespace {

constexpr std::size_t kMaxCores = 1024;        // a longer core vector is taken as garbage
constexpr std::int32_t kMaxBucketTags = 4096;  // likewise a bucket
constexpr std::size_t kMaxText = 65536;        // likewise a string
constexpr std::int32_t kMaxWars = 4096;        // likewise the running wars
constexpr std::size_t kMaxWarSide = 1024;      // likewise one side of a war
constexpr std::int64_t kMaxModifiers = 65536;  // likewise the definitions

bool read_raw(std::uintptr_t address, void* out, std::size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

template <typename T>
bool read(std::uintptr_t address, T& out) {
    return read_raw(address, &out, sizeof(T));
}

// MSVC std::string: a 16-byte buffer or a pointer, its size at +0x10, its capacity at +0x18.
bool read_string(std::uintptr_t address, std::string& out) {
    unsigned char raw[0x20];
    if (!read_raw(address, raw, sizeof raw)) return false;
    std::uint64_t size = 0, capacity = 0;
    std::memcpy(&size, raw + 0x10, 8);
    std::memcpy(&capacity, raw + 0x18, 8);
    if (size > capacity || size > kMaxText) return false;
    if (capacity < 16) {
        out.assign(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(size));
        return true;
    }
    std::uintptr_t data = 0;
    std::memcpy(&data, raw, 8);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || read_raw(data, out.data(), out.size());
}

std::uint8_t byte_of(std::uint64_t tag, std::uint32_t index) { return static_cast<std::uint8_t>(tag >> (8 * index)); }

// The tag's characters, or "" when its valid byte says no country.
AnvilGameTag tag_text(std::uint64_t tag, const WorldLayout& l) {
    AnvilGameTag out{};
    if (byte_of(tag, l.tag_valid) == 0) return out;
    for (std::uint32_t i = 0; i < 3 && byte_of(tag, i) != 0; ++i) out.text[i] = static_cast<char>(byte_of(tag, i));
    return out;
}

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

// The first `count` characters of the stored tag against `text`, ignoring case, as _strnicmp.
bool same_tag(std::uint64_t tag, std::string_view text, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const char stored = static_cast<char>(byte_of(tag, i));
        const char wanted = i < text.size() ? text[i] : '\0';
        if (upper(stored) != upper(wanted)) return false;
        if (stored == '\0') return true;
    }
    return true;
}

bool read_root(const WorldLayout& l, std::uintptr_t& root) { return read(l.root, root) && root != 0; }

// Entries in the root's province vector, the dummy entry 0 included.
bool province_count(std::uintptr_t root, const WorldLayout& l, std::int64_t& count) {
    std::uintptr_t begin = 0, end = 0;
    if (!read(root + l.provinces, begin) || !read(root + l.provinces_end, end) || end < begin) return false;
    count = static_cast<std::int64_t>((end - begin) >> l.pointer_shift);
    return true;
}

// The entry of province `id`, checked by the ID it holds itself.
AnvilResult province_at(const WorldLayout& l, std::int32_t id, std::uintptr_t& province) {
    std::uintptr_t root = 0, array = 0;
    std::int64_t count = 0;
    if (!read_root(l, root) || !province_count(root, l, count)) return ANVIL_ERROR_INTERNAL;
    if (id < 1 || id >= count) return ANVIL_ERROR_NOT_FOUND;
    if (!read(root + l.province_array, array) || array == 0) return ANVIL_ERROR_INTERNAL;
    province = array + static_cast<std::uintptr_t>(id) * l.province_size;
    std::int32_t own_id = 0;
    return read(province + l.province_id, own_id) && own_id == id ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

// The game's own lookup, walked here instead of called; the country is checked
// by the tag it holds itself.
AnvilResult country_at(const WorldLayout& l, std::string_view text, std::uintptr_t& country, std::uint64_t& own) {
    if (text.size() != 3) return ANVIL_ERROR_INVALID_ARGUMENT;
    for (const char c : text) {
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alnum) return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::uintptr_t db = 0;
    if (!read(l.countries_db, db) || db == 0) return ANVIL_ERROR_INTERNAL;
    const std::uintptr_t bucket = db + static_cast<std::uintptr_t>(tag_bucket(text, l.bucket_mask)) * l.bucket_stride;
    std::uintptr_t tags = 0;
    std::int32_t n = 0;
    if (!read(bucket + l.buckets, tags) || !read(bucket + l.bucket_count, n) || n < 0 || n > kMaxBucketTags) {
        return ANVIL_ERROR_INTERNAL;
    }
    std::uint64_t tag = 0;
    bool found = false;
    for (std::int32_t i = 0; i < n && !found; ++i) {
        if (!read(tags + static_cast<std::uintptr_t>(i) * 8, tag)) return ANVIL_ERROR_INTERNAL;
        found = same_tag(tag, text, l.compared);
    }
    if (!found) return ANVIL_ERROR_NOT_FOUND;
    const auto index = static_cast<std::int16_t>(tag >> l.country_index_shift);
    std::uintptr_t countries = 0;
    if (index < 0 || !read(db + l.countries, countries) || countries == 0 ||
        !read(countries + static_cast<std::uintptr_t>(index) * 8, country) || country == 0) {
        return ANVIL_ERROR_INTERNAL;
    }
    if (!read(country + l.country_tag, own) || !same_tag(own, text, 3)) return ANVIL_ERROR_INTERNAL;
    return ANVIL_OK;
}

// `id` among the running wars: an id from a plugin is never read before this.
AnvilResult war_at(const WorldLayout& l, std::uint64_t id) {
    std::vector<std::uint64_t> ids;
    const AnvilResult r = read_war_ids(l, ids);
    if (r != ANVIL_OK) return r;
    for (const std::uint64_t each : ids) {
        if (each == id) return ANVIL_OK;
    }
    return ANVIL_ERROR_NOT_FOUND;
}

// A UTF-8 sequence's length at `at`, 0 when none starts there.
std::size_t utf8_length(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) return 1;
    std::size_t length = 0;
    std::uint32_t code = 0, least = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2, code = lead & 0x1F, least = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3, code = lead & 0x0F, least = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4, code = lead & 0x07, least = 0x10000;
    } else {
        return 0;
    }
    if (at + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80) return 0;
        code = (code << 6) | (next & 0x3F);
    }
    if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
    return length;
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = utf8_length(text, i);
        if (length == 0) return false;
        i += length;
    }
    return true;
}

}  // namespace

namespace {

// How many definitions there are: the types a modifier can be asked for.
bool definition_range(const WorldLayout& l, std::uintptr_t& begin, std::int64_t& count) {
    std::uintptr_t end = 0;
    if (!read(l.definitions, begin) || !read(l.definitions_end, end) || begin == 0 || end < begin ||
        l.definition_size == 0 || (end - begin) % l.definition_size != 0) {
        return false;
    }
    count = static_cast<std::int64_t>((end - begin) / l.definition_size);
    return count <= kMaxModifiers;
}

using ModifierValueFn = std::int32_t* (*)(const void* modifier, std::int32_t* out, std::int32_t type);

bool call_modifier_value(std::uintptr_t function, std::uintptr_t modifier, std::int32_t id, std::int32_t& value) {
    __try {
        reinterpret_cast<ModifierValueFn>(function)(reinterpret_cast<const void*>(modifier), &value, id);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

AnvilResult modifier_value(const WorldLayout& l, std::uintptr_t modifier, std::int32_t id, std::int32_t& value) {
    std::uintptr_t begin = 0;
    std::int64_t count = 0;
    if (!definition_range(l, begin, count)) return ANVIL_ERROR_INTERNAL;
    if (id < 0 || id >= count) return ANVIL_ERROR_NOT_FOUND;  // the game's function does not check
    return call_modifier_value(l.modifier_value, modifier, id, value) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

}  // namespace

AnvilResult read_modifier_id(const WorldLayout& l, std::string_view key, std::int32_t& id) {
    if (l.modifier_value == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t begin = 0;
    std::int64_t count = 0;
    if (!definition_range(l, begin, count)) return ANVIL_ERROR_INTERNAL;
    std::string text;
    for (std::int64_t i = 0; i < count; ++i) {
        if (!read_string(begin + static_cast<std::uintptr_t>(i) * l.definition_size, text)) return ANVIL_ERROR_INTERNAL;
        if (text == key) {
            id = static_cast<std::int32_t>(i);
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

AnvilResult read_country_modifier(const WorldLayout& l, std::string_view tag, std::int32_t id, std::int32_t& value) {
    if (l.modifier_value == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    const std::uintptr_t modifiers = country + l.country_modifiers;
    std::int32_t state = 0;
    if (!read(modifiers, state) || state == static_cast<std::int32_t>(l.calculating)) return ANVIL_ERROR_INTERNAL;
    return modifier_value(l, modifiers + l.country_total, id, value);
}

AnvilResult read_province_modifier(const WorldLayout& l, std::int32_t province, std::int32_t id, std::int32_t& value) {
    if (l.modifier_value == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t p = 0;
    if (const AnvilResult r = province_at(l, province, p); r != ANVIL_OK) return r;
    return modifier_value(l, p + l.province_modifier, id, value);
}

std::uint32_t tag_bucket(std::string_view tag, std::uint32_t mask) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 3 && i < tag.size(); ++i) sum += static_cast<unsigned char>(upper(tag[i]));
    return sum & mask;
}

std::string game_text_to_utf8(std::string_view text) {
    if (valid_utf8(text)) return std::string(text);
    // 0x80-0x9F of Windows-1252; its five unassigned bytes stay C1 controls.
    static constexpr char16_t kHigh[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                           0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out;
    out.reserve(text.size() * 2);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xA7) {  // a colour code: § and the character after it are not displayed
            ++i;
            continue;
        }
        const char32_t code = c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80] : c;
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

std::size_t fit_utf8(std::string_view text, std::size_t capacity) {
    if (capacity == 0) return 0;
    std::size_t fits = 0;
    while (fits < text.size()) {
        const std::size_t length = utf8_length(text, fits);
        if (length == 0 || fits + length > capacity - 1) break;
        fits += length;
    }
    return fits;
}

std::int32_t read_province_limit(const WorldLayout& l) {
    std::uintptr_t root = 0;
    std::int64_t count = 0;
    if (!read_root(l, root) || !province_count(root, l, count) || count > INT32_MAX) return 0;
    return static_cast<std::int32_t>(count);
}

AnvilResult read_province(const WorldLayout& l, std::int32_t id, AnvilGameProvince& out) {
    std::uintptr_t p = 0;
    if (const AnvilResult r = province_at(l, id, p); r != ANVIL_OK) return r;
    std::uint64_t owner = 0, controller = 0;
    std::uintptr_t map_part = 0;
    if (!read(p + l.owner, owner) || !read(p + l.controller, controller) || !read(p + l.base_tax, out.base_tax) ||
        !read(p + l.base_production, out.base_production) || !read(p + l.base_manpower, out.base_manpower) ||
        !read(p + l.static_part, map_part) || map_part == 0) {
        return ANVIL_ERROR_INTERNAL;
    }
    std::uint8_t land = 0, wasteland = 0;
    if (!read(map_part + l.land_flags, land) || !read(map_part + l.wasteland_flags, wasteland)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.id = id;
    out.owner = tag_text(owner, l);
    out.controller = tag_text(controller, l);
    out.flags = (((land >> l.land_bit) & 1) != 0 ? ANVIL_GAME_PROVINCE_LAND : 0u) |
                (((wasteland >> l.wasteland_bit) & 1) != 0 ? ANVIL_GAME_PROVINCE_WASTELAND : 0u);
    return ANVIL_OK;
}

AnvilResult read_province_area(const WorldLayout& l, std::int32_t id, std::uint32_t map_part, std::uint32_t area,
                               std::uintptr_t& out) {
    std::uintptr_t p = 0;
    if (const AnvilResult r = province_at(l, id, p); r != ANVIL_OK) return r;
    std::uintptr_t part = 0;
    if (!read(p + map_part, part) || part == 0 || !read(part + area, out)) return ANVIL_ERROR_INTERNAL;
    return out != 0 ? ANVIL_OK : ANVIL_ERROR_NOT_FOUND;
}

AnvilResult read_province_name(const WorldLayout& l, std::int32_t id, std::string& utf8) {
    std::uintptr_t p = 0, map_part = 0;
    if (const AnvilResult r = province_at(l, id, p); r != ANVIL_OK) return r;
    std::string name;
    if (!read(p + l.static_part, map_part) || map_part == 0 || !read_string(map_part + l.name, name)) {
        return ANVIL_ERROR_INTERNAL;
    }
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_province_cores(const WorldLayout& l, std::int32_t id, std::vector<AnvilGameTag>& out) {
    std::uintptr_t p = 0, cores = 0, cores_end = 0;
    if (const AnvilResult r = province_at(l, id, p); r != ANVIL_OK) return r;
    if (!read(p + l.cores, cores) || !read(p + l.cores + l.cores_end, cores_end)) return ANVIL_ERROR_INTERNAL;
    if (cores_end < cores || (cores_end - cores) % 8 != 0 || (cores_end - cores) / 8 > kMaxCores) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.clear();
    for (std::uintptr_t at = cores; at < cores_end; at += 8) {
        std::uint64_t core = 0;
        if (!read(at, core)) return ANVIL_ERROR_INTERNAL;
        out.push_back(tag_text(core, l));
    }
    return ANVIL_OK;
}

AnvilResult read_country(const WorldLayout& l, std::string_view tag, AnvilGameCountry& out) {
    std::uintptr_t root = 0, country = 0;
    std::uint64_t own = 0;
    std::int64_t count = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    if (!read_root(l, root) || !province_count(root, l, count)) return ANVIL_ERROR_INTERNAL;
    std::int32_t capital = 0;
    if (!read(country + l.owned_count, out.province_count) || !read(country + l.capital, capital) ||
        out.province_count < 0 || out.province_count > count) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.tag = tag_text(own, l);
    out.capital = capital >= 1 && capital < count ? capital : 0;
    out.flags = byte_of(own, l.tag_kind) == l.observer_kind ? ANVIL_GAME_COUNTRY_OBSERVER : 0u;
    out.values = 0;
    const auto value = [&](std::uint32_t offset, std::uint32_t bit, std::int32_t& field) {
        field = 0;
        if (offset == 0) return true;
        out.values |= bit;
        return read(country + offset, field);
    };
    if (!value(l.treasury, ANVIL_GAME_VALUE_TREASURY, out.treasury) ||
        !value(l.stability, ANVIL_GAME_VALUE_STABILITY, out.stability) ||
        !value(l.prestige, ANVIL_GAME_VALUE_PRESTIGE, out.prestige) ||
        !value(l.manpower, ANVIL_GAME_VALUE_MANPOWER, out.manpower) ||
        !value(l.monthly_income, ANVIL_GAME_VALUE_MONTHLY_INCOME, out.monthly_income)) {
        return ANVIL_ERROR_INTERNAL;
    }
    return ANVIL_OK;
}

AnvilResult read_country_provinces(const WorldLayout& l, std::string_view tag, std::vector<std::int32_t>& out) {
    std::uintptr_t root = 0, country = 0, owned = 0;
    std::uint64_t own = 0;
    std::int64_t count = 0;
    std::int32_t n = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    if (!read_root(l, root) || !province_count(root, l, count) || !read(country + l.owned_count, n) ||
        !read(country + l.owned, owned) || n < 0 || n > count || (n > 0 && owned == 0)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.resize(static_cast<std::size_t>(n));
    if (n > 0 && !read_raw(owned, out.data(), out.size() * sizeof(std::int32_t))) return ANVIL_ERROR_INTERNAL;
    return ANVIL_OK;
}

AnvilResult read_war_ids(const WorldLayout& l, std::vector<std::uint64_t>& out) {
    if (l.wars == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t root = 0, wars = 0;
    std::int32_t count = 0;
    if (!read_root(l, root) || !read(root + l.wars, wars) || !read(root + l.war_count, count) || count < 0 ||
        count > kMaxWars || (count > 0 && wars == 0)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.resize(static_cast<std::size_t>(count));
    if (count > 0 && !read_raw(wars, out.data(), out.size() * sizeof(std::uint64_t))) return ANVIL_ERROR_INTERNAL;
    return ANVIL_OK;
}

AnvilResult read_war(const WorldLayout& l, std::uint64_t id, AnvilGameWar& out) {
    if (const AnvilResult r = war_at(l, id); r != ANVIL_OK) return r;
    std::uint64_t attacker = 0, defender = 0;
    if (!read(id + l.war_attacker, attacker) || !read(id + l.war_defender, defender)) return ANVIL_ERROR_INTERNAL;
    out.id = id;
    out.attacker = tag_text(attacker, l);
    out.defender = tag_text(defender, l);
    return ANVIL_OK;
}

AnvilResult read_war_name(const WorldLayout& l, std::uint64_t id, std::string& utf8) {
    if (const AnvilResult r = war_at(l, id); r != ANVIL_OK) return r;
    std::string name;
    if (!read_string(id + l.war_name, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_war_side(const WorldLayout& l, std::uint64_t id, std::uint32_t side, std::vector<AnvilGameTag>& out) {
    if (side != ANVIL_GAME_WAR_ATTACKERS && side != ANVIL_GAME_WAR_DEFENDERS) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (const AnvilResult r = war_at(l, id); r != ANVIL_OK) return r;
    const bool attackers = side == ANVIL_GAME_WAR_ATTACKERS;
    std::uintptr_t begin = 0, end = 0;
    if (!read(id + (attackers ? l.war_attackers : l.war_defenders), begin) ||
        !read(id + (attackers ? l.war_attackers_end : l.war_defenders_end), end)) {
        return ANVIL_ERROR_INTERNAL;
    }
    if (end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxWarSide) return ANVIL_ERROR_INTERNAL;
    out.clear();
    for (std::uintptr_t at = begin; at < end; at += 8) {
        std::uint64_t tag = 0;
        if (!read(at, tag)) return ANVIL_ERROR_INTERNAL;
        out.push_back(tag_text(tag, l));
    }
    return ANVIL_OK;
}

AnvilResult read_player(const WorldLayout& l, AnvilGameTag& out) {
    std::uintptr_t root = 0;
    std::uint64_t player = 0, observed = 0;
    if (!read_root(l, root) || !read(root + l.player, player) || !read(root + l.observed, observed)) {
        return ANVIL_ERROR_INTERNAL;
    }
    // The rule of every reader in the game (player_country).
    const bool observer = byte_of(player, l.tag_kind) == l.observer_kind;
    out = tag_text(observer && byte_of(observed, l.tag_valid) != 0 ? observed : player, l);
    return ANVIL_OK;
}

AnvilResult read_own_country(const WorldLayout& l, AnvilGameTag& out) {
    std::uintptr_t root = 0;
    std::uint64_t player = 0;
    if (!read_root(l, root) || !read(root + l.player, player)) return ANVIL_ERROR_INTERNAL;
    if (byte_of(player, l.tag_kind) == l.observer_kind || byte_of(player, l.tag_valid) == 0) return ANVIL_ERROR_NOT_FOUND;
    out = tag_text(player, l);
    return ANVIL_OK;
}

// ---- units --------------------------------------------------------------------

bool decode_constant_getter(const std::uint8_t* code, std::size_t size, std::int64_t& value) {
    if (size >= 3 && (code[0] == 0x33 || code[0] == 0x31) && code[1] == 0xC0 && code[2] == 0xC3) {
        value = 0;  // xor eax,eax; ret
        return true;
    }
    if (size >= 6 && code[0] == 0xB8 && code[5] == 0xC3) {  // mov eax,imm32; ret
        std::int32_t imm = 0;
        std::memcpy(&imm, code + 1, sizeof imm);
        value = imm;
        return true;
    }
    return false;
}

namespace {

constexpr std::int32_t kMaxListNodes = 100000;  // a longer unit list or selection is taken as garbage
constexpr std::size_t kMaxSubunits = 4096;

// The constant that the getter in `slot` of `object`'s vtable returns, read from
// its code: the getter is not called. False when it is not such a getter.
bool constant_of(const WorldLayout& l, std::uintptr_t object, std::uint32_t slot, std::int64_t& value) {
    std::uintptr_t vtable = 0, getter = 0;
    if (!read(object, vtable) || vtable == 0 || !read(vtable + slot, getter)) return false;
    if (getter < l.image_begin || getter + 6 > l.image_end) return false;
    std::uint8_t code[6] = {};
    return read_raw(getter, code, sizeof code) && decode_constant_getter(code, sizeof code, value);
}

// The objects of one of the game's linked lists, in order.
bool read_list(const WorldLayout& l, std::uintptr_t list, std::vector<std::uintptr_t>& out) {
    std::uintptr_t node = 0;
    std::int32_t count = 0;
    if (!read(list + l.list_head, node) || !read(list + l.list_count, count) || count < 0 || count > kMaxListNodes) {
        return false;
    }
    out.clear();
    while (node != 0) {
        if (static_cast<std::int32_t>(out.size()) >= kMaxListNodes) return false;  // a loop
        std::uintptr_t object = 0, next = 0;
        if (!read(node + l.node_object, object) || !read(node + l.node_next, next)) return false;
        if (object != 0) out.push_back(object);
        node = next;
    }
    return true;
}

AnvilResult country_unit_list(const WorldLayout& l, std::string_view tag, std::vector<std::uintptr_t>& units) {
    if (l.units == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    return read_list(l, country + l.units, units) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

// The unit of `tag` with `id`, found in that country's list.
AnvilResult unit_at(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::uintptr_t& unit) {
    std::vector<std::uintptr_t> units;
    if (const AnvilResult r = country_unit_list(l, tag, units); r != ANVIL_OK) return r;
    for (const std::uintptr_t each : units) {
        std::uint64_t own = 0;
        if (!read(each + l.unit_id, own)) return ANVIL_ERROR_INTERNAL;
        if (own == id) {
            unit = each;
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

// A province pointer as its ID, checked against the province array; 0 for none.
bool province_id_of(const WorldLayout& l, std::uintptr_t province, std::int32_t& id) {
    id = 0;
    if (province == 0) return true;
    std::int32_t own = 0;
    std::uintptr_t at = 0;
    if (!read(province + l.province_id, own)) return false;
    if (province_at(l, own, at) != ANVIL_OK || at != province) return false;
    id = own;
    return true;
}

bool unit_kind(const WorldLayout& l, std::uintptr_t unit, std::uint32_t& kind) {
    std::int64_t value = 0;
    if (!constant_of(l, unit, l.kind_slot, value) ||
        (value != ANVIL_GAME_UNIT_ARMY && value != ANVIL_GAME_UNIT_NAVY)) {
        return false;
    }
    kind = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

AnvilResult read_country_units(const WorldLayout& l, std::string_view tag, std::vector<std::uint64_t>& out) {
    std::vector<std::uintptr_t> units;
    if (const AnvilResult r = country_unit_list(l, tag, units); r != ANVIL_OK) return r;
    out.clear();
    for (const std::uintptr_t unit : units) {
        std::uint64_t id = 0;
        if (!read(unit + l.unit_id, id)) return ANVIL_ERROR_INTERNAL;
        out.push_back(id);
    }
    return ANVIL_OK;
}

AnvilResult read_unit(const WorldLayout& l, std::string_view tag, std::uint64_t id, AnvilGameUnit& out) {
    std::uintptr_t unit = 0;
    return read_unit_object(l, tag, id, unit, out);
}

AnvilResult read_unit_object(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::uintptr_t& unit,
                             AnvilGameUnit& out) {
    if (const AnvilResult r = unit_at(l, tag, id, unit); r != ANVIL_OK) return r;
    std::uint64_t country = 0;
    std::uintptr_t leader = 0, location = 0, mission = 0, begin = 0, end = 0;
    if (!read(unit + l.unit_country, country) || !read(unit + l.unit_leader, leader) ||
        !read(unit + l.unit_location, location) || !read(unit + l.unit_mission, mission) ||
        !read(unit + l.subunits, begin) || !read(unit + l.subunits_end, end) || end < begin || (end - begin) % 8 != 0 ||
        (end - begin) / 8 > kMaxSubunits) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.id = id;
    out.country = tag_text(country, l);
    if (!unit_kind(l, unit, out.kind) || !province_id_of(l, location, out.location)) return ANVIL_ERROR_INTERNAL;
    std::int64_t type = 0;
    out.mission = mission != 0 && constant_of(l, mission, l.mission_slot, type) && type >= 0 && type < l.mission_limit
                      ? static_cast<std::int32_t>(type)
                      : ANVIL_GAME_MISSION_UNKNOWN;
    out.subunits = static_cast<std::uint32_t>((end - begin) / 8);
    std::int64_t strength = 0;
    for (std::uintptr_t at = begin; at < end; at += 8) {
        std::uintptr_t subunit = 0;
        std::int32_t each = 0;
        if (!read(at, subunit) || subunit == 0 || !read(subunit + l.subunit_strength, each)) return ANVIL_ERROR_INTERNAL;
        strength += each;
    }
    out.strength = static_cast<std::int32_t>(std::clamp<std::int64_t>(strength, INT32_MIN, INT32_MAX));
    out.reserved = 0;
    out.leader = 0;
    if (leader != 0 && !read(leader + l.leader_id, out.leader)) return ANVIL_ERROR_INTERNAL;
    if ((out.leader >> 32) == 0) out.leader = 0;  // the game's placeholder for "no leader" has id 0
    return ANVIL_OK;
}

AnvilResult read_unit_name(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::string& utf8) {
    std::uintptr_t unit = 0;
    if (const AnvilResult r = unit_at(l, tag, id, unit); r != ANVIL_OK) return r;
    std::string name;
    if (!read_string(unit + l.unit_name, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_unit_leader_name(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::string& utf8) {
    std::uintptr_t unit = 0, leader = 0;
    if (const AnvilResult r = unit_at(l, tag, id, unit); r != ANVIL_OK) return r;
    if (!read(unit + l.unit_leader, leader)) return ANVIL_ERROR_INTERNAL;
    if (leader == 0) return ANVIL_ERROR_NOT_FOUND;
    // A unit without a leader points at the game's placeholder leader, whose id is 0.
    std::uint64_t leader_id = 0;
    if (!read(leader + l.leader_id, leader_id)) return ANVIL_ERROR_INTERNAL;
    if ((leader_id >> 32) == 0) return ANVIL_ERROR_NOT_FOUND;
    std::string name;
    if (!read_string(leader + l.leader_name, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_selected_units(const WorldLayout& l, std::vector<AnvilGameUnitRef>& out) {
    if (l.units == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t root = 0, session = 0, input = 0;
    if (!read_root(l, root) || !read(root + l.session, session) || session == 0 || !read(session + l.map_input, input)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.clear();
    if (input == 0) return ANVIL_OK;  // no map input yet: nothing selected
    std::vector<std::uintptr_t> objects;
    if (!read_list(l, input + l.selection, objects)) return ANVIL_ERROR_INTERNAL;
    for (const std::uintptr_t object : objects) {
        std::int64_t kind = 0;
        if (!constant_of(l, object, l.kind_slot, kind)) continue;  // not a constant getter: not a unit
        if (kind != ANVIL_GAME_UNIT_ARMY && kind != ANVIL_GAME_UNIT_NAVY) continue;
        AnvilGameUnitRef ref{};
        std::uint64_t country = 0;
        if (!read(object + l.unit_id, ref.id) || !read(object + l.unit_country, country)) return ANVIL_ERROR_INTERNAL;
        ref.country = tag_text(country, l);
        ref.kind = static_cast<std::uint32_t>(kind);
        out.push_back(ref);
    }
    return ANVIL_OK;
}

// ---- rulers, heirs and consorts -------------------------------------------------

bool decode_bool_getter(const std::uint8_t* code, std::size_t size, bool& value) {
    if (size < 3 || code[2] != 0xC3) return false;
    if (code[0] == 0x32 && code[1] == 0xC0) {  // xor al,al; ret
        value = false;
        return true;
    }
    if (code[0] == 0xB0 && code[1] == 0x01) {  // mov al,1; ret
        value = true;
        return true;
    }
    return false;
}

namespace {

constexpr std::size_t kMaxFlags = 32768;       // a flag index is an int16
constexpr std::uint64_t kMaxVariables = 65536;  // a longer variable list is taken as garbage

// The country's monarch of `kind`, or NOT_FOUND for none: a null pointer or one
// of the game's placeholders, which are never read.
AnvilResult monarch_of(const WorldLayout& l, std::uintptr_t country, std::uint32_t kind, std::uintptr_t& monarch) {
    const std::uint32_t slot = kind == ANVIL_GAME_MONARCH_RULER ? l.ruler
                               : kind == ANVIL_GAME_MONARCH_HEIR ? l.heir
                                                                  : l.consort;
    if (!read(country + slot, monarch)) return ANVIL_ERROR_INTERNAL;
    if (monarch == 0) return ANVIL_ERROR_NOT_FOUND;
    for (const std::uintptr_t global : {l.null_monarch, l.null_heir, l.null_consort}) {
        std::uintptr_t placeholder = 0;
        if (!read(global, placeholder)) return ANVIL_ERROR_INTERNAL;
        if (placeholder != 0 && placeholder == monarch) return ANVIL_ERROR_NOT_FOUND;
    }
    return ANVIL_OK;
}

AnvilResult monarch_at(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::uintptr_t& country,
                       std::uintptr_t& monarch) {
    if (l.ruler == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    return monarch_of(l, country, kind, monarch);
}

// The dynasty the game shows (CMonarch::GetDynasty): unless the monarch is a
// queen regent, one with a regency whose country has an heir shows the heir's.
// IsQueenRegent is decoded from its getter, never called; a getter that is not
// a constant is a queen's, so the monarch's own dynasty then stands.
bool shown_dynasty(const WorldLayout& l, std::uintptr_t country, std::uintptr_t monarch, std::uintptr_t& dynasty) {
    for (int step = 0; step < 4; ++step) {
        std::uintptr_t regency = 0, vtable = 0, getter = 0;
        if (!read(monarch + l.regency, regency)) return false;
        if (regency == 0) break;
        if (!read(monarch, vtable) || vtable == 0 || !read(vtable + l.queen_regent_slot, getter)) return false;
        std::uint8_t code[3] = {};
        bool queen_regent = true;
        if (getter >= l.image_begin && getter + sizeof code <= l.image_end && read_raw(getter, code, sizeof code)) {
            if (!decode_bool_getter(code, sizeof code, queen_regent)) queen_regent = true;
        }
        if (queen_regent) break;
        std::uintptr_t heir = 0;
        if (monarch_of(l, country, ANVIL_GAME_MONARCH_HEIR, heir) != ANVIL_OK || heir == monarch) break;
        monarch = heir;
    }
    return read(monarch + l.dynasty, dynasty);
}

// The flags of one CIntegerFlags, appended to `out`.
bool append_flags(const WorldLayout& l, std::uintptr_t flags, std::vector<FlagEntry>& out) {
    std::uintptr_t begin = 0, end = 0, dates = 0, dates_end = 0;
    if (!read(flags + l.flags_begin, begin) || !read(flags + l.flags_end, end) || !read(flags + l.flag_dates, dates) ||
        !read(flags + l.flag_dates + 8, dates_end) || end < begin || (end - begin) % 2 != 0 ||
        (end - begin) / 2 > kMaxFlags || dates_end < dates || (dates_end - dates) / 4 < (end - begin) / 2) {
        return false;
    }
    const std::size_t n = (end - begin) / 2;
    std::vector<std::int16_t> ids(n);
    std::vector<std::int32_t> hours(n);
    if (n > 0 && (!read_raw(begin, ids.data(), n * 2) || !read_raw(dates, hours.data(), n * 4))) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const bool seen = std::any_of(out.begin(), out.end(), [&](const FlagEntry& e) { return e.id == ids[i]; });
        if (!seen && ids[i] >= 0) out.push_back(FlagEntry{ids[i], hours[i]});
    }
    return true;
}

// The names' array: begin and how many.
bool flag_name_range(const WorldLayout& l, std::uintptr_t& begin, std::size_t& count) {
    std::uintptr_t end = 0;
    if (!read(l.flag_names, begin) || !read(l.flag_names + 8, end) || end < begin || (end - begin) % 0x20 != 0 ||
        (end - begin) / 0x20 > kMaxFlags) {
        return false;
    }
    count = (end - begin) / 0x20;
    return true;
}

// The nodes of an MSVC unordered_map, walked as its list: `list` holds the
// sentinel node, the count follows it, and a node's next is its first field.
bool read_map_nodes(std::uintptr_t list, std::uint64_t max, std::vector<std::uintptr_t>& out) {
    std::uintptr_t head = 0, node = 0;
    std::uint64_t count = 0;
    if (!read(list, head) || head == 0 || !read(list + 8, count) || count > max || !read(head, node)) return false;
    out.clear();
    while (node != head) {
        if (out.size() >= count || node == 0) return false;  // a loop, or a list cut short
        out.push_back(node);
        if (!read(node, node)) return false;
    }
    return out.size() == count;
}

// The variables of one CVariables: the map's list, from its sentinel node.
bool read_variables(const WorldLayout& l, std::uintptr_t variables, std::vector<VariableEntry>& out) {
    std::vector<std::uintptr_t> nodes;
    if (!read_map_nodes(variables + l.variable_head, kMaxVariables, nodes)) return false;
    out.clear();
    for (const std::uintptr_t node : nodes) {
        VariableEntry e;
        if (!read_string(node + l.variable_value - 0x20, e.name) || !read(node + l.variable_value, e.value)) {
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

}  // namespace

AnvilResult read_monarch(const WorldLayout& l, std::string_view tag, std::uint32_t kind, AnvilGameMonarch& out) {
    std::uintptr_t country = 0, monarch = 0;
    if (const AnvilResult r = monarch_at(l, tag, kind, country, monarch); r != ANVIL_OK) return r;
    std::uint64_t own = 0, leader = 0;
    std::int32_t skills[3] = {}, birth = 0;
    std::uint8_t female = 0;
    std::uintptr_t regency = 0;
    if (!read(country + l.country_tag, own) || !read(monarch + l.object_id, out.id) ||
        !read_raw(monarch + l.skills, skills, sizeof skills) || !read(monarch + l.birth, birth) ||
        !read(monarch + l.female, female) || !read(monarch + l.regency, regency) ||
        !read(monarch + l.monarch_leader, leader)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.kind = kind;
    out.country = tag_text(own, l);
    out.flags = (female != 0 ? ANVIL_GAME_MONARCH_FEMALE : 0u) | (regency != 0 ? ANVIL_GAME_MONARCH_REGENCY : 0u);
    out.adm = skills[0];
    out.dip = skills[1];
    out.mil = skills[2];
    out.claim = 0;
    if (kind == ANVIL_GAME_MONARCH_HEIR && !read(monarch + l.claim, out.claim)) return ANVIL_ERROR_INTERNAL;
    out.birth = decode_date(birth);
    const bool is_leader = static_cast<std::uint32_t>(leader) == l.leader_type && (leader >> 32) != 0;
    out.leader = is_leader ? leader : 0;
    return ANVIL_OK;
}

AnvilResult read_monarch_name(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::string& utf8) {
    std::uintptr_t country = 0, monarch = 0;
    if (const AnvilResult r = monarch_at(l, tag, kind, country, monarch); r != ANVIL_OK) return r;
    std::string name;
    if (!read_string(monarch + l.monarch_name, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_monarch_dynasty(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::string& utf8) {
    std::uintptr_t country = 0, monarch = 0, dynasty = 0;
    if (const AnvilResult r = monarch_at(l, tag, kind, country, monarch); r != ANVIL_OK) return r;
    std::string name;
    if (!shown_dynasty(l, country, monarch, dynasty)) return ANVIL_ERROR_INTERNAL;
    if (dynasty == 0) return ANVIL_ERROR_NOT_FOUND;
    if (!read_string(dynasty + l.dynasty_name, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_flag_id(const WorldLayout& l, std::string_view name, std::int32_t& id) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t begin = 0;
    std::size_t count = 0;
    if (!flag_name_range(l, begin, count)) return ANVIL_ERROR_INTERNAL;
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        if (!read_string(begin + i * 0x20, text)) return ANVIL_ERROR_INTERNAL;
        if (text == name) {
            id = static_cast<std::int32_t>(i);
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

AnvilResult read_flag_name(const WorldLayout& l, std::int32_t id, std::string& utf8) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t begin = 0;
    std::size_t count = 0;
    if (!flag_name_range(l, begin, count)) return ANVIL_ERROR_INTERNAL;
    if (id < 0 || static_cast<std::size_t>(id) >= count) return ANVIL_ERROR_NOT_FOUND;
    std::string name;
    if (!read_string(begin + static_cast<std::uintptr_t>(id) * 0x20, name)) return ANVIL_ERROR_INTERNAL;
    utf8 = game_text_to_utf8(name);
    return ANVIL_OK;
}

AnvilResult read_country_flags(const WorldLayout& l, std::string_view tag, std::vector<FlagEntry>& out) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    out.clear();
    return append_flags(l, country + l.country_flags, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

AnvilResult read_province_flags(const WorldLayout& l, std::int32_t province, std::vector<FlagEntry>& out) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t p = 0;
    if (const AnvilResult r = province_at(l, province, p); r != ANVIL_OK) return r;
    out.clear();
    return append_flags(l, p + l.province_flags, out) && append_flags(l, p + l.permanent_flags, out)
               ? ANVIL_OK
               : ANVIL_ERROR_INTERNAL;
}

AnvilResult read_monarch_flags(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::vector<FlagEntry>& out) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0, monarch = 0;
    if (const AnvilResult r = monarch_at(l, tag, kind, country, monarch); r != ANVIL_OK) return r;
    out.clear();
    return append_flags(l, monarch + l.monarch_flags, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

AnvilResult read_global_flags(const WorldLayout& l, std::vector<FlagEntry>& out) {
    if (l.flag_names == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t root = 0;
    if (!read_root(l, root)) return ANVIL_ERROR_INTERNAL;
    out.clear();
    return append_flags(l, root + l.global_flags, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

AnvilResult read_country_variables(const WorldLayout& l, std::string_view tag, std::vector<VariableEntry>& out) {
    if (l.variable_head == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    return read_variables(l, country + l.country_variables, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

AnvilResult read_province_variables(const WorldLayout& l, std::int32_t province, std::vector<VariableEntry>& out) {
    if (l.variable_head == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t p = 0;
    if (const AnvilResult r = province_at(l, province, p); r != ANVIL_OK) return r;
    return read_variables(l, p + l.province_variables, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

// ---- pending events -------------------------------------------------------------

namespace {

constexpr std::int32_t kMaxPending = 4096;   // a longer pending list is taken as garbage
constexpr std::size_t kMaxOptions = 1024;    // likewise an event's options
constexpr std::uint64_t kMaxSeries = 65536;  // likewise the countries of a series
constexpr std::size_t kMaxYears = 100000;    // likewise the years of one

// The pending list's entries and how many.
bool pending_range(const WorldLayout& l, std::uintptr_t& data, std::int32_t& count) {
    std::uintptr_t root = 0;
    return read_root(l, root) && read(root + l.pending_list, data) && read(root + l.pending_count, count) &&
           count >= 0 && count <= kMaxPending && (count == 0 || data != 0);
}

// The entry of `key` in the pending list: NOT_FOUND when it is gone.
AnvilResult pending_at(const WorldLayout& l, const PendingKey& key, std::uintptr_t& entry) {
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    if (!pending_range(l, data, count)) return ANVIL_ERROR_INTERNAL;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t at = data + static_cast<std::uintptr_t>(i) * l.pending_size;
        PendingKey seen;
        if (!read(at + l.pending_id, seen.id) || !read(at + l.pending_event, seen.event) ||
            !read(at + l.pending_scope, seen.scope)) {
            return ANVIL_ERROR_INTERNAL;
        }
        if (seen == key) {
            entry = at;
            return ANVIL_OK;
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

bool tag_text_valid(std::string_view text) {
    if (text.size() != 3) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

}  // namespace

AnvilResult read_pending_keys(const WorldLayout& l, std::vector<PendingKey>& out) {
    if (l.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    if (!pending_range(l, data, count)) return ANVIL_ERROR_INTERNAL;
    out.clear();
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t at = data + static_cast<std::uintptr_t>(i) * l.pending_size;
        PendingKey key;
        if (!read(at + l.pending_id, key.id) || !read(at + l.pending_event, key.event) ||
            !read(at + l.pending_scope, key.scope)) {
            return ANVIL_ERROR_INTERNAL;
        }
        out.push_back(key);
    }
    return ANVIL_OK;
}

AnvilResult read_pending(const WorldLayout& l, const PendingKey& key, AnvilGamePendingEvent& out) {
    if (l.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t entry = 0;
    if (const AnvilResult r = pending_at(l, key, entry); r != ANVIL_OK) return r;
    std::int32_t auto_select = 0, province = 0;
    std::uint64_t country = 0, override_tag = 0;
    std::uint8_t override_valid = 0, hidden = 0;
    std::uintptr_t options = 0, options_end = 0;
    if (key.event == 0 || key.scope == 0 || !read(entry + l.auto_select, auto_select) ||
        !read(key.scope + l.scope_province, province) || !read(key.scope + l.scope_country, country) ||
        !read(key.scope + l.scope_override, override_tag) ||
        !read(key.scope + l.scope_override_valid, override_valid) || !read(key.event + l.event_options, options) ||
        !read(key.event + l.event_options_end, options_end) || !read(key.event + l.event_hidden, hidden) ||
        options_end < options || (options_end - options) % 8 != 0 || (options_end - options) / 8 > kMaxOptions) {
        return ANVIL_ERROR_INTERNAL;
    }
    // The game's rule: the override when it is set, else the scope's country,
    // and for a tag that is not valid the scope province's owner.
    std::uint64_t tag = override_valid != 0 ? override_tag : country;
    std::uintptr_t p = 0;
    const bool has_province = province_at(l, province, p) == ANVIL_OK;
    if (byte_of(tag, l.tag_valid) == 0) {
        tag = 0;
        if (has_province && !read(p + l.owner, tag)) return ANVIL_ERROR_INTERNAL;
    }
    out.pending_id = key.id;
    out.country = tag_text(tag, l);
    out.province = has_province ? province : 0;
    out.options = static_cast<std::uint32_t>((options_end - options) / 8);
    out.flags = hidden != 0 ? ANVIL_GAME_PENDING_HIDDEN : 0u;
    out.auto_select = decode_date(auto_select);
    return ANVIL_OK;
}

AnvilResult read_pending_text(const WorldLayout& l, const PendingKey& key, bool title, std::string& utf8) {
    if (l.pending_list == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t entry = 0;
    if (const AnvilResult r = pending_at(l, key, entry); r != ANVIL_OK) return r;
    std::string text;
    if (key.event == 0 || !read_string(key.event + (title ? l.event_title : l.event_key), text)) {
        return ANVIL_ERROR_INTERNAL;
    }
    utf8 = game_text_to_utf8(text);
    return ANVIL_OK;
}

// ---- statistics -------------------------------------------------------------------

AnvilResult read_statistics(const WorldLayout& l, std::string_view tag, std::uint32_t kind,
                            std::vector<AnvilGameStatisticPoint>& out) {
    if (l.statistics_head == 0) return ANVIL_ERROR_NOT_DECLARED;
    if (!tag_text_valid(tag)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::uintptr_t root = 0;
    std::vector<std::uintptr_t> nodes;
    if (!read_root(l, root) || !read_map_nodes(root + l.statistics[kind] + l.statistics_head, kMaxSeries, nodes)) {
        return ANVIL_ERROR_INTERNAL;
    }
    std::string key;
    for (const std::uintptr_t node : nodes) {
        if (!read_string(node + l.statistics_key, key)) return ANVIL_ERROR_INTERNAL;
        if (key.size() != 3 || upper(key[0]) != upper(tag[0]) || upper(key[1]) != upper(tag[1]) ||
            upper(key[2]) != upper(tag[2])) {
            continue;
        }
        std::uintptr_t values = 0, values_end = 0, years = 0, years_end = 0;
        if (!read(node + l.statistics_values, values) || !read(node + l.statistics_values + 8, values_end) ||
            !read(node + l.statistics_years, years) || !read(node + l.statistics_years + 8, years_end) ||
            values_end < values || years_end < years || (values_end - values) % 4 != 0 ||
            values_end - values != years_end - years || (values_end - values) / 4 > kMaxYears) {
            return ANVIL_ERROR_INTERNAL;
        }
        const std::size_t n = (values_end - values) / 4;
        std::vector<std::int32_t> value(n), year(n);
        if (n > 0 && (!read_raw(values, value.data(), n * 4) || !read_raw(years, year.data(), n * 4))) {
            return ANVIL_ERROR_INTERNAL;
        }
        out.clear();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(AnvilGameStatisticPoint{year[i], value[i]});
        return ANVIL_OK;
    }
    return ANVIL_ERROR_NOT_FOUND;
}

// ---- timed modifiers and the province view (1.21.0) -----------------------------

namespace {

constexpr std::size_t kMaxTimed = 65536;  // a longer list is taken as garbage

bool read_timed(const WorldLayout& l, std::int32_t never, std::uintptr_t timed, TimedEntry& out) {
    std::uintptr_t modifier = 0;
    std::uint8_t ruler = 0, hidden = 0;
    if (timed == 0 || !read(timed + l.timed_modifier, modifier) || modifier == 0 ||
        !read_string(modifier + l.timed_key, out.key) || !read(timed + l.timed_start, out.start) ||
        !read(timed + l.timed_end, out.end) || !read(timed + l.timed_ruler, ruler) ||
        !read(timed + l.timed_hidden, hidden)) {
        return false;
    }
    out.flags = (hidden != 0 ? ANVIL_GAME_TIMED_HIDDEN : 0u) | (ruler != 0 ? ANVIL_GAME_TIMED_RULER : 0u);
    // Given for good (a negative duration): the game's date for never; never set: the unset date.
    if (out.end == l.unset_hours || out.end == never) out.flags |= ANVIL_GAME_TIMED_NO_END;
    if (out.start == l.unset_hours) out.flags |= ANVIL_GAME_TIMED_NO_START;
    return true;
}

}  // namespace

AnvilResult read_country_timed(const WorldLayout& l, std::string_view tag, std::vector<TimedEntry>& out) {
    if (l.timed_key == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0, node = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    std::int32_t never = 0;
    if (!read(country + l.country_timed, node) || !read(l.timed_never, never)) return ANVIL_ERROR_INTERNAL;
    out.clear();
    while (node != 0) {
        if (out.size() >= kMaxTimed) return ANVIL_ERROR_INTERNAL;  // a loop
        std::uintptr_t timed = 0;
        TimedEntry e;
        if (!read(node, timed) || !read_timed(l, never, timed, e) || !read(node + l.timed_node_next, node)) {
            return ANVIL_ERROR_INTERNAL;
        }
        out.push_back(std::move(e));
    }
    return ANVIL_OK;
}

AnvilResult read_province_timed(const WorldLayout& l, std::int32_t province, std::vector<TimedEntry>& out) {
    if (l.timed_key == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t p = 0, begin = 0, end = 0;
    std::int32_t never = 0;
    if (const AnvilResult r = province_at(l, province, p); r != ANVIL_OK) return r;
    if (!read(l.timed_never, never) || !read(p + l.province_timed, begin) || !read(p + l.province_timed_end, end) || end < begin ||
        (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxTimed) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.clear();
    for (std::uintptr_t at = begin; at < end; at += 8) {
        std::uintptr_t timed = 0;
        TimedEntry e;
        if (!read(at, timed) || !read_timed(l, never, timed, e)) return ANVIL_ERROR_INTERNAL;
        out.push_back(std::move(e));
    }
    return ANVIL_OK;
}

AnvilResult read_province_view(const WorldLayout& l, std::int32_t& province) {
    if (l.view_holder == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t root = 0, session = 0, holder = 0, view = 0, shown = 0, window = 0;
    if (!read_root(l, root) || !read(root + l.session, session) || session == 0 || !read(session + l.view_holder, holder)) {
        return ANVIL_ERROR_INTERNAL;
    }
    if (holder == 0) return ANVIL_ERROR_NOT_FOUND;  // no selection object yet
    if (!read(holder + l.view, view)) return ANVIL_ERROR_INTERNAL;
    if (view == 0) return ANVIL_ERROR_NOT_FOUND;
    if (!read(view + l.view_province, shown) || !read(view + l.view_window, window)) return ANVIL_ERROR_INTERNAL;
    if (shown == 0 || window == 0) return ANVIL_ERROR_NOT_FOUND;
    std::uint8_t visible = 0;
    if (!read(window + l.window_part + l.widget_shown, visible)) return ANVIL_ERROR_INTERNAL;
    if (visible == 0) return ANVIL_ERROR_NOT_FOUND;
    std::int32_t id = 0;
    if (!province_id_of(l, shown, id) || id == 0) return ANVIL_ERROR_INTERNAL;
    province = id;
    return ANVIL_OK;
}

// ---- diplomacy (1.22.0) -------------------------------------------------------------

namespace {

constexpr std::int32_t kMaxRelations = 4096;  // a longer list of one kind is taken as garbage
constexpr std::size_t kMaxRivals = 64;        // likewise the rivals (the game allows three)
constexpr std::size_t kMaxSubjects = 4096;    // likewise the subjects
constexpr std::size_t kMaxCountries = 65536;  // likewise a vector by country index

// The country index a tag carries, which the game's diplomacy keys on.
std::int16_t index_of(const WorldLayout& l, std::uint64_t tag) { return static_cast<std::int16_t>(tag >> l.country_index_shift); }

// A country that owns a province now; NOT_FOUND for one without, whose
// diplomacy the game no longer keeps up.
AnvilResult living_country(const WorldLayout& l, std::string_view tag, std::uintptr_t& country, std::uint64_t& own) {
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    std::int32_t owned = 0;
    if (!read(country + l.owned_count, owned) || owned < 0) return ANVIL_ERROR_INTERNAL;
    return owned > 0 ? ANVIL_OK : ANVIL_ERROR_NOT_FOUND;
}

// The relation objects of one kind, each checked to name the country.
AnvilResult relation_objects(const WorldLayout& l, std::uintptr_t country, std::uint64_t own, std::uint32_t kind,
                             std::vector<std::uintptr_t>& out) {
    const std::uintptr_t diplomacy = country + l.diplomacy;
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    if (!read(diplomacy + l.relations[kind], data) || !read(diplomacy + l.relation_counts[kind], count) || count < 0 ||
        count > kMaxRelations || (count > 0 && data == 0)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.clear();
    out.reserve(static_cast<std::size_t>(count));
    const std::int16_t self = index_of(l, own);
    for (std::int32_t i = 0; i < count; ++i) {
        std::uintptr_t relation = 0;
        std::uint64_t first = 0, second = 0;
        if (!read(data + static_cast<std::uintptr_t>(i) * 16 + 8, relation) || relation == 0 ||
            !read(relation + l.relation_first, first) || !read(relation + l.relation_second, second) ||
            (index_of(l, first) != self && index_of(l, second) != self)) {
            return ANVIL_ERROR_INTERNAL;
        }
        out.push_back(relation);
    }
    return ANVIL_OK;
}

bool read_tag_vector(const WorldLayout& l, std::uintptr_t at, std::vector<AnvilGameTag>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!read(at, begin) || !read(at + 8, end) || end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxSubjects) {
        return false;
    }
    out.clear();
    for (std::uintptr_t p = begin; p < end; p += 8) {
        std::uint64_t tag = 0;
        if (!read(p, tag)) return false;
        out.push_back(tag_text(tag, l));
    }
    return true;
}

}  // namespace

AnvilResult read_relations(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::vector<RelationEntry>& out) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    std::vector<std::uintptr_t> objects;
    if (const AnvilResult r = relation_objects(l, country, own, kind, objects); r != ANVIL_OK) return r;
    out.clear();
    out.reserve(objects.size());
    for (const std::uintptr_t relation : objects) {
        std::uint64_t first = 0, second = 0;
        RelationEntry e;
        if (!read(relation + l.relation_first, first) || !read(relation + l.relation_second, second) ||
            !read(relation + l.relation_start, e.start) || !read(relation + l.relation_end, e.end)) {
            return ANVIL_ERROR_INTERNAL;
        }
        e.first = tag_text(first, l);
        e.second = tag_text(second, l);
        e.has_start = e.start != l.relation_unset;
        e.has_end = e.end != l.relation_unset;
        out.push_back(e);
    }
    return ANVIL_OK;
}

AnvilResult read_subject_type(const WorldLayout& l, std::string_view overlord, std::string_view subject, std::string& utf8) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0, other = 0;
    std::uint64_t own = 0, wanted = 0;
    if (const AnvilResult r = country_at(l, overlord, country, own); r != ANVIL_OK) return r;
    if (const AnvilResult r = country_at(l, subject, other, wanted); r != ANVIL_OK) return r;
    std::vector<std::uintptr_t> objects;
    if (const AnvilResult r = relation_objects(l, country, own, ANVIL_GAME_RELATION_DEPENDENCY, objects); r != ANVIL_OK) {
        return r;
    }
    for (const std::uintptr_t relation : objects) {
        std::uint64_t first = 0, second = 0;
        if (!read(relation + l.relation_first, first) || !read(relation + l.relation_second, second)) {
            return ANVIL_ERROR_INTERNAL;
        }
        if (index_of(l, first) != index_of(l, own) || index_of(l, second) != index_of(l, wanted)) continue;
        std::uintptr_t type = 0;
        std::string key;
        if (!read(relation + l.subject_type, type) || type == 0 || !read_string(type + l.subject_type_key, key)) {
            return ANVIL_ERROR_INTERNAL;
        }
        utf8 = game_text_to_utf8(key);
        return ANVIL_OK;
    }
    return ANVIL_ERROR_NOT_FOUND;
}

AnvilResult read_diplomacy_status(const WorldLayout& l, std::string_view from, std::string_view to,
                                  AnvilGameDiplomacyStatus& out) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0, other = 0;
    std::uint64_t own = 0, target = 0;
    if (const AnvilResult r = living_country(l, from, country, own); r != ANVIL_OK) return r;
    if (const AnvilResult r = living_country(l, to, other, target); r != ANVIL_OK) return r;
    const std::int16_t index = index_of(l, target);
    if (index == index_of(l, own)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (index < 0) return ANVIL_ERROR_INTERNAL;
    const auto at = static_cast<std::uintptr_t>(index);

    // The opinion: GetOpinionValue reads the cache below its size and works the
    // value out above it. The service never does the latter.
    std::uintptr_t cache = 0, cache_end = 0;
    if (!read(country + l.opinion_cache, cache) || !read(country + l.opinion_cache_end, cache_end) || cache_end < cache ||
        (cache_end - cache) % 2 != 0 || (cache_end - cache) / 2 > kMaxCountries) {
        return ANVIL_ERROR_INTERNAL;
    }
    if (at >= (cache_end - cache) / 2) return ANVIL_ERROR_NOT_FOUND;
    std::int16_t opinion = 0;
    if (!read(cache + at * 2, opinion)) return ANVIL_ERROR_INTERNAL;

    std::uintptr_t statuses = 0, statuses_end = 0;
    if (!read(country + l.statuses, statuses) || !read(country + l.statuses + 8, statuses_end) || statuses_end < statuses ||
        (statuses_end - statuses) % l.status_size != 0 || (statuses_end - statuses) / l.status_size > kMaxCountries) {
        return ANVIL_ERROR_INTERNAL;
    }
    if (at >= (statuses_end - statuses) / l.status_size) return ANVIL_ERROR_NOT_FOUND;
    const std::uintptr_t status = statuses + at * l.status_size;
    std::uintptr_t parent = 0;
    std::uint64_t whom = 0;
    std::int16_t cached_sum = 0;
    std::uint8_t trust = 0, truce = 0;
    std::int32_t favors = 0;
    if (!read(status, parent) || parent != country + l.diplomacy || !read(status + l.status_target, whom) ||
        index_of(l, whom) != index || !read(status + l.status_cached_sum, cached_sum) ||
        !read(status + l.status_trust, trust) || !read(status + l.status_favors, favors) ||
        !read(status + l.status_truce, truce)) {
        return ANVIL_ERROR_INTERNAL;
    }
    out.opinion = opinion;
    out.timed_sum = cached_sum;
    out.trust = trust;
    out.favors = favors;
    out.flags = (truce & l.truce_bit) != 0 ? ANVIL_GAME_DIPLOMACY_TRUCE : 0u;
    return ANVIL_OK;
}

AnvilResult read_rivals(const WorldLayout& l, std::string_view tag, std::vector<AnvilGameTag>& out) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0, node = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    if (!read(country + l.rivals, node)) return ANVIL_ERROR_INTERNAL;
    out.clear();
    // Like IsRival, every node counts, whatever its deleted flag says.
    while (node != 0) {
        std::uint64_t rival = 0;
        if (out.size() >= kMaxRivals || !read(node + l.rival_tag, rival) || !read(node + l.rival_next, node)) {
            return ANVIL_ERROR_INTERNAL;
        }
        out.push_back(tag_text(rival, l));
    }
    return ANVIL_OK;
}

AnvilResult read_overlord(const WorldLayout& l, std::string_view tag, AnvilGameTag& out) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0, overlord = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    if (!read(country + l.overlord, overlord)) return ANVIL_ERROR_INTERNAL;
    out = tag_text(overlord, l);
    return out.text[0] != '\0' ? ANVIL_OK : ANVIL_ERROR_NOT_FOUND;
}

AnvilResult read_subjects(const WorldLayout& l, std::string_view tag, std::vector<AnvilGameTag>& out) {
    if (l.diplomacy == 0) return ANVIL_ERROR_NOT_DECLARED;
    std::uintptr_t country = 0;
    std::uint64_t own = 0;
    if (const AnvilResult r = country_at(l, tag, country, own); r != ANVIL_OK) return r;
    return read_tag_vector(l, country + l.subjects, out) ? ANVIL_OK : ANVIL_ERROR_INTERNAL;
}

}  // namespace anvil_game
