// The world of a running session, read straight from game memory for the
// service anvil-game.world (docs/adr/0004-plugins-without-addresses.md). The
// walk is Tongs' (its docs/design.md, "World reads"), checked live there. It
// keeps nothing between calls, so a load cannot leave a stale pointer behind.
// Every read is guarded on its own, so memory that is not what it should be
// costs one answer. It calls no game code but one function: a modifier's value
// goes through a DLC transformer per definition, and that function only reads.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace anvil_game {

// Where the game keeps the world: addresses of its two globals, then offsets
// from the object named in each comment. A country value of 0 is absent on
// this build.
struct WorldLayout {
    std::uintptr_t root = 0;          // the address of the game root's global
    std::uintptr_t countries_db = 0;  // the address of the country database's global

    std::uint32_t provinces = 0;       // root: vector of Province*, entry 0 included
    std::uint32_t provinces_end = 0;
    std::uint32_t pointer_shift = 0;   // its 8-byte entries
    std::uint32_t province_array = 0;  // root: the array, indexed by ID
    std::uint32_t province_size = 0;
    std::uint32_t province_id = 0;     // province: int32
    std::uint32_t owner = 0;           // province: tag
    std::uint32_t controller = 0;      // province: tag
    std::uint32_t cores = 0;           // province: std::vector of tags, its begin
    std::uint32_t cores_end = 0;       // from the vector
    std::uint32_t base_tax = 0;        // province: int32, x 1000
    std::uint32_t base_production = 0;
    std::uint32_t base_manpower = 0;
    std::uint32_t static_part = 0;     // province: pointer to the map part
    std::uint32_t name = 0;            // map part: std::string
    std::uint32_t land_flags = 0;      // map part: byte
    std::uint32_t land_bit = 0;
    std::uint32_t wasteland_flags = 0;
    std::uint32_t wasteland_bit = 0;

    // Tags: three characters, the country index (int16) above the shift, the
    // kind byte and the valid byte.
    std::uint32_t country_index_shift = 0;
    std::uint32_t tag_kind = 0;
    std::uint32_t tag_valid = 0;
    std::uint32_t observer_kind = 0;

    std::uint32_t countries = 0;     // db: Country*[], by country index
    std::uint32_t buckets = 0;       // db + n * stride: the tags of bucket n
    std::uint32_t bucket_count = 0;  // db + n * stride: int32
    std::uint32_t bucket_mask = 0;
    std::uint32_t bucket_stride = 0;
    std::uint32_t compared = 0;      // characters the game compares
    std::uint32_t country_tag = 0;   // country: its own tag
    std::uint32_t capital = 0;       // country: int32 province ID
    std::uint32_t owned = 0;         // country: int32*
    std::uint32_t owned_count = 0;   // country: int32

    std::uint32_t player = 0;    // root: tag
    std::uint32_t observed = 0;  // root: tag

    std::uint32_t treasury = 0;  // country: fixed point, x 1000
    std::uint32_t stability = 0;
    std::uint32_t prestige = 0;
    std::uint32_t manpower = 0;
    std::uint32_t monthly_income = 0;

    // Wars: `wars` 0 when absent on this build.
    std::uint32_t wars = 0;           // root: CWar*[]
    std::uint32_t war_count = 0;      // root: int32
    std::uint32_t war_name = 0;       // war: std::string
    std::uint32_t war_attacker = 0;   // war: tag
    std::uint32_t war_defender = 0;   // war: tag
    std::uint32_t war_attackers = 0;  // war: std::vector of tags, its begin
    std::uint32_t war_attackers_end = 0;
    std::uint32_t war_defenders = 0;
    std::uint32_t war_defenders_end = 0;

    // Modifiers: `modifier_value` 0 when absent on this build.
    std::uintptr_t modifier_value = 0;    // the address of the game's value function
    std::uintptr_t definitions = 0;       // the address of the definitions' begin
    std::uintptr_t definitions_end = 0;   // and of their end
    std::uint32_t definition_size = 0;    // a definition: its key, a std::string, comes first
    std::uint32_t country_modifiers = 0;  // country: the state, an int32, of its modifiers
    std::uint32_t calculating = 0;        // the state while they are worked out
    std::uint32_t country_total = 0;      // from that state: the sum, a modifier
    std::uint32_t province_modifier = 0;  // province: the sum of its own, a modifier
    // Units: `units` 0 when absent on this build. A list is the game's linked
    // list: head, count, and nodes with the object and the next node.
    std::uintptr_t image_begin = 0;  // eu4.exe in memory: a getter decoded from a vtable must be in it
    std::uintptr_t image_end = 0;
    std::uint32_t units = 0;            // country: the list of its units
    std::uint32_t list_head = 0;        // list: first node
    std::uint32_t list_count = 0;       // list: int32
    std::uint32_t node_object = 0;      // node: the object
    std::uint32_t node_next = 0;        // node: the next node, null at the end
    std::uint32_t unit_id = 0;          // unit: 8 bytes
    std::uint32_t unit_country = 0;     // unit: tag
    std::uint32_t unit_leader = 0;      // unit: pointer, may be null
    std::uint32_t unit_location = 0;    // unit: province pointer, may be null
    std::uint32_t unit_mission = 0;     // unit: mission object pointer
    std::uint32_t unit_name = 0;        // unit: std::string
    std::uint32_t subunits = 0;         // unit: std::vector of pointers, its begin
    std::uint32_t subunits_end = 0;
    std::uint32_t subunit_strength = 0; // regiment or ship: int32, x 1000
    std::uint32_t kind_slot = 0;        // unit's vtable: the getter of its kind
    std::uint32_t mission_slot = 0;     // mission's vtable: the getter of its type
    std::uint32_t mission_limit = 0;    // mission types are below this
    std::uint32_t leader_id = 0;        // leader: 8 bytes
    std::uint32_t leader_name = 0;      // leader: std::string
    std::uint32_t session = 0;          // root: the session
    std::uint32_t map_input = 0;        // session: pointer to its map input
    std::uint32_t selection = 0;        // map input: the selection list
};

// Main thread only, after an outer frame, in a session: the service checks.
std::int32_t read_province_limit(const WorldLayout& l);
AnvilResult read_province(const WorldLayout& l, std::int32_t id, AnvilGameProvince& out);
AnvilResult read_province_name(const WorldLayout& l, std::int32_t id, std::string& utf8);
AnvilResult read_province_cores(const WorldLayout& l, std::int32_t id, std::vector<AnvilGameTag>& out);
AnvilResult read_country(const WorldLayout& l, std::string_view tag, AnvilGameCountry& out);
AnvilResult read_country_provinces(const WorldLayout& l, std::string_view tag, std::vector<std::int32_t>& out);
AnvilResult read_player(const WorldLayout& l, AnvilGameTag& out);

AnvilResult read_war_ids(const WorldLayout& l, std::vector<std::uint64_t>& out);
AnvilResult read_war(const WorldLayout& l, std::uint64_t id, AnvilGameWar& out);
AnvilResult read_war_name(const WorldLayout& l, std::uint64_t id, std::string& utf8);
AnvilResult read_war_side(const WorldLayout& l, std::uint64_t id, std::uint32_t side, std::vector<AnvilGameTag>& out);

AnvilResult read_modifier_id(const WorldLayout& l, std::string_view key, std::int32_t& id);
AnvilResult read_country_modifier(const WorldLayout& l, std::string_view tag, std::int32_t id, std::int32_t& value);
AnvilResult read_province_modifier(const WorldLayout& l, std::int32_t province, std::int32_t id, std::int32_t& value);
AnvilResult read_country_units(const WorldLayout& l, std::string_view tag, std::vector<std::uint64_t>& out);
AnvilResult read_unit(const WorldLayout& l, std::string_view tag, std::uint64_t id, AnvilGameUnit& out);
AnvilResult read_unit_name(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::string& utf8);
AnvilResult read_unit_leader_name(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::string& utf8);
AnvilResult read_selected_units(const WorldLayout& l, std::vector<AnvilGameUnitRef>& out);

// For anvil-game.commands: the country the player commands, the root's player
// and never the observed one (ANVIL_ERROR_NOT_FOUND in observe mode), and a
// unit's object with what read_unit answers for it.
AnvilResult read_own_country(const WorldLayout& l, AnvilGameTag& out);
AnvilResult read_unit_object(const WorldLayout& l, std::string_view tag, std::uint64_t id, std::uintptr_t& unit,
                             AnvilGameUnit& out);

// The constant a getter of 3 or 6 bytes returns: xor eax,eax; ret (0) or
// mov eax,imm32; ret. False for any other code.
bool decode_constant_getter(const std::uint8_t* code, std::size_t size, std::int64_t& value);

// The bucket the game files a tag in: its three characters, upper-cased, summed, & mask.
std::uint32_t tag_bucket(std::string_view tag, std::uint32_t mask);

// The game's text as UTF-8. Valid UTF-8 stays, as Mei keeps it; anything else
// is Windows-1252, without its colour codes (§ and the character after it).
std::string game_text_to_utf8(std::string_view text);

// The longest start of `text` that fits `capacity - 1` bytes without cutting a
// character: what a caller's buffer gets, with its NUL.
std::size_t fit_utf8(std::string_view text, std::size_t capacity);

}  // namespace anvil_game
