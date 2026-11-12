#include "snapshot.hpp"

#include <cstring>
#include <map>
#include <utility>

namespace anvil_inspector {

namespace {

constexpr std::uint32_t kMaxItems = 100000;  // more is taken as a broken answer

#define HAS(world, field) ((world) != nullptr && ANVIL_GAME_WORLD_HAS(world, field) && (world)->field != nullptr)

// A text from a call that fills a buffer and writes the whole size: asked with
// capacity 0 first, then once more if it grew in between.
template <class Call>
AnvilResult read_text(Call call, std::string& out) {
    std::uint32_t length = 0;
    AnvilResult r = call(nullptr, 0, &length);
    for (int attempt = 0; r == ANVIL_OK && attempt < 3; ++attempt) {
        std::string buffer(static_cast<std::size_t>(length) + 1, '\0');
        std::uint32_t whole = 0;
        r = call(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &whole);
        if (r != ANVIL_OK) break;
        if (whole <= length) {
            buffer.resize(whole);
            out = std::move(buffer);
            return ANVIL_OK;
        }
        length = whole;
    }
    return r == ANVIL_OK ? ANVIL_ERROR_INTERNAL : r;
}

// A list from a call that fills an array and writes the whole count.
template <class T, class Call>
AnvilResult read_list(Call call, std::vector<T>& out) {
    std::uint32_t count = 0;
    AnvilResult r = call(nullptr, 0, &count);
    for (int attempt = 0; r == ANVIL_OK && attempt < 3; ++attempt) {
        if (count > kMaxItems) return ANVIL_ERROR_INTERNAL;
        out.assign(count, T{});
        std::uint32_t whole = 0;
        r = call(out.data(), count, &whole);
        if (r != ANVIL_OK) break;
        if (whole <= count) {
            out.resize(whole);
            return ANVIL_OK;
        }
        count = whole;
    }
    return r == ANVIL_OK ? ANVIL_ERROR_INTERNAL : r;
}

std::vector<std::string> split(const std::string& names) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < names.size()) {
        const std::size_t end = names.find('\0', at);
        if (end == std::string::npos) break;
        out.push_back(names.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

// Flag names by id, asked once per read.
class FlagNames {
public:
    explicit FlagNames(const AnvilGameWorldService* world) : world_(world) {}
    std::string operator()(std::int32_t id) {
        if (const auto it = names_.find(id); it != names_.end()) return it->second;
        std::string name;
        if (!HAS(world_, flag_name) ||
            read_text([&](char* b, std::uint32_t c, std::uint32_t* l) { return world_->flag_name(id, b, c, l); }, name) !=
                ANVIL_OK) {
            name = "#" + std::to_string(id);
        }
        return names_.emplace(id, std::move(name)).first->second;
    }

private:
    const AnvilGameWorldService* world_;
    std::map<std::int32_t, std::string> names_;
};

template <class Call>
List<Flag> read_flags(bool present, Call call, FlagNames& names) {
    List<Flag> out;
    if (!present) return out;
    std::vector<AnvilGameFlag> raw;
    out.result = read_list<AnvilGameFlag>(call, raw);
    if (out.result != ANVIL_OK) return out;
    for (const AnvilGameFlag& f : raw) out.items.push_back(Flag{names(f.id), f.date});
    return out;
}

template <class Names, class Value>
List<Variable> read_variables(bool present, Names names_call, Value value_call) {
    List<Variable> out;
    if (!present) return out;
    std::string names;
    out.result = read_text(names_call, names);
    if (out.result != ANVIL_OK) return out;
    for (std::string& name : split(names)) {
        Variable v;
        v.name = std::move(name);
        const AnvilResult r = value_call(v.name.c_str(), &v.value);
        if (r == ANVIL_ERROR_NOT_FOUND) continue;  // removed in between: cannot happen on the main thread
        if (r != ANVIL_OK) {
            out.result = r;
            return out;
        }
        out.items.push_back(std::move(v));
    }
    return out;
}

template <class ListCall, class KeysCall>
List<Timed> read_timed(bool present, ListCall list_call, KeysCall keys_call) {
    List<Timed> out;
    if (!present) return out;
    std::vector<AnvilGameTimedModifier> raw;
    std::string keys;
    out.result = read_list<AnvilGameTimedModifier>(list_call, raw);
    if (out.result == ANVIL_OK) out.result = read_text(keys_call, keys);
    if (out.result != ANVIL_OK) return out;
    const std::vector<std::string> names = split(keys);
    if (names.size() != raw.size()) {
        out.result = ANVIL_ERROR_INTERNAL;
        return out;
    }
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out.items.push_back(Timed{names[i], raw[i].flags, raw[i].start, raw[i].end});
    }
    return out;
}

Scope read_province(const AnvilGameWorldService* world, std::int32_t id, FlagNames& names) {
    Scope s;
    s.role = Role::province;
    s.province = id;
    s.result = HAS(world, province_name)
                   ? read_text([&](char* b, std::uint32_t c, std::uint32_t* l) { return world->province_name(id, b, c, l); },
                               s.name)
                   : ANVIL_ERROR_UNSUPPORTED;
    if (s.result != ANVIL_OK) return s;
    s.flags = read_flags(
        HAS(world, province_flags),
        [&](AnvilGameFlag* o, std::uint32_t c, std::uint32_t* n) { return world->province_flags(id, o, c, n); }, names);
    s.variables = read_variables(
        HAS(world, province_variable_names) && HAS(world, province_variable),
        [&](char* b, std::uint32_t c, std::uint32_t* l) { return world->province_variable_names(id, b, c, l); },
        [&](const char* name, std::int32_t* v) { return world->province_variable(id, name, v); });
    s.modifiers = read_timed(
        HAS(world, province_timed_modifiers) && HAS(world, province_timed_modifier_keys),
        [&](AnvilGameTimedModifier* o, std::uint32_t c, std::uint32_t* n) {
            return world->province_timed_modifiers(id, o, c, n);
        },
        [&](char* b, std::uint32_t c, std::uint32_t* l) { return world->province_timed_modifier_keys(id, b, c, l); });
    return s;
}

Monarch read_monarch(const AnvilGameWorldService* world, const char* tag, std::uint32_t kind, FlagNames& names) {
    Monarch m;
    if (!HAS(world, monarch)) return m;
    AnvilGameMonarch raw{};
    raw.size = sizeof(raw);
    m.result = world->monarch(tag, kind, &raw);
    if (m.result != ANVIL_OK) return m;
    m.adm = raw.adm;
    m.dip = raw.dip;
    m.mil = raw.mil;
    if (HAS(world, monarch_name)) {
        read_text([&](char* b, std::uint32_t c, std::uint32_t* l) { return world->monarch_name(tag, kind, b, c, l); },
                  m.name);
    }
    if (HAS(world, monarch_dynasty)) {
        read_text([&](char* b, std::uint32_t c, std::uint32_t* l) { return world->monarch_dynasty(tag, kind, b, c, l); },
                  m.dynasty);
    }
    m.flags = read_flags(
        HAS(world, monarch_flags),
        [&](AnvilGameFlag* o, std::uint32_t c, std::uint32_t* n) { return world->monarch_flags(tag, kind, o, c, n); },
        names);
    return m;
}

Scope read_country(const AnvilGameWorldService* world, Role role, const std::string& tag_text, FlagNames& names) {
    Scope s;
    s.role = role;
    s.tag = tag_text;
    const char* tag = s.tag.c_str();
    AnvilGameCountry country{};
    country.size = sizeof(country);
    s.result = world->country(tag, &country);
    if (s.result != ANVIL_OK) return s;
    s.flags = read_flags(
        HAS(world, country_flags),
        [&](AnvilGameFlag* o, std::uint32_t c, std::uint32_t* n) { return world->country_flags(tag, o, c, n); }, names);
    s.variables = read_variables(
        HAS(world, country_variable_names) && HAS(world, country_variable),
        [&](char* b, std::uint32_t c, std::uint32_t* l) { return world->country_variable_names(tag, b, c, l); },
        [&](const char* name, std::int32_t* v) { return world->country_variable(tag, name, v); });
    s.modifiers = read_timed(
        HAS(world, country_timed_modifiers) && HAS(world, country_timed_modifier_keys),
        [&](AnvilGameTimedModifier* o, std::uint32_t c, std::uint32_t* n) {
            return world->country_timed_modifiers(tag, o, c, n);
        },
        [&](char* b, std::uint32_t c, std::uint32_t* l) { return world->country_timed_modifier_keys(tag, b, c, l); });
    for (std::uint32_t kind = 0; kind < 3; ++kind) s.monarchs[kind] = read_monarch(world, tag, kind, names);
    return s;
}

std::string player_tag(const AnvilGameWorldService* world) {
    AnvilGameTag tag{};
    if (world == nullptr || world->player == nullptr || world->player(&tag) != ANVIL_OK) return {};
    return std::string(tag.text, strnlen(tag.text, sizeof tag.text));
}

AnvilResult view_of(const AnvilGameWorldService* world, std::int32_t& province) {
    province = 0;
    if (!HAS(world, province_view)) return ANVIL_ERROR_UNSUPPORTED;
    return world->province_view(&province);
}

bool date_of(const AnvilGameService& game, AnvilGameDate& date) {
    date = AnvilGameDate{};
    date.size = sizeof(date);
    return ANVIL_GAME_HAS(&game, date) && game.date != nullptr && game.date(&date) != 0;
}

}  // namespace

Key key_of(const AnvilGameService& game, const AnvilGameWorldService* world) {
    Key k;
    AnvilGameDate date{};
    if (!date_of(game, date)) return k;
    k.session = date.session;
    k.year = date.year;
    k.month = date.month;
    k.day = date.day;
    if (view_of(world, k.province) != ANVIL_OK) k.province = 0;
    k.player = player_tag(world);
    return k;
}

Snapshot read_snapshot(const AnvilGameService& game, const AnvilGameWorldService* world) {
    Snapshot s;
    if (!date_of(game, s.date)) return s;
    s.session = true;
    if (world == nullptr) return s;
    FlagNames names(world);
    std::int32_t province = 0;
    s.view = view_of(world, province);
    std::string owner;
    if (s.view == ANVIL_OK) {
        s.scopes.push_back(read_province(world, province, names));
        AnvilGameProvince p{};
        p.size = sizeof(p);
        if (world->province(province, &p) == ANVIL_OK) owner.assign(p.owner.text, strnlen(p.owner.text, sizeof p.owner.text));
    }
    const std::string player = player_tag(world);
    if (!owner.empty()) {
        s.scopes.push_back(read_country(world, owner == player ? Role::owner_and_player : Role::owner, owner, names));
    }
    if (!player.empty() && player != owner) s.scopes.push_back(read_country(world, Role::player, player, names));
    return s;
}

}  // namespace anvil_inspector
