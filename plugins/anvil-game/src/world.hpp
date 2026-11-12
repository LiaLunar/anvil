// The world of a running session, read straight from game memory for the
// service anvil-game.world (docs/adr/0004-plugins-without-addresses.md). The
// walk is Tongs' (its docs/design.md, "World reads"), checked live there. It
// keeps nothing between calls, so a load cannot leave a stale pointer behind.
// Every read is guarded on its own, so memory that is not what it should be
// costs one answer. It calls no game code but one function: a modifier's value
// goes through a DLC transformer per definition, and that function only reads.
#pragma once

#include <anvil/anvil_game.h>

#include "session.hpp"

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

    // Rulers, heirs and consorts: `ruler` 0 when absent on this build.
    std::uint32_t ruler = 0;                // country: CMonarch*, never null in a running game
    std::uint32_t heir = 0;                 // country: CHeir*
    std::uint32_t consort = 0;              // country: CQueen*
    std::uintptr_t null_monarch = 0;        // the addresses of the globals of the game's placeholders
    std::uintptr_t null_heir = 0;
    std::uintptr_t null_consort = 0;
    std::uint32_t object_id = 0;            // any game object: its id, 8 bytes
    std::uint32_t monarch_name = 0;         // monarch: std::string
    std::uint32_t skills = 0;               // monarch: three int32
    std::uint32_t birth = 0;                // monarch: int32, hours
    std::uint32_t female = 0;               // monarch: byte
    std::uint32_t monarch_leader = 0;       // monarch: 8 bytes, the type in the low half
    std::uint32_t leader_type = 0;          // the type of a leader's id
    std::uint32_t regency = 0;              // monarch: pointer, null without a regency
    std::uint32_t queen_regent_slot = 0;    // monarch's vtable: IsQueenRegent
    std::uint32_t dynasty = 0;              // monarch: CDynasty*
    std::uint32_t dynasty_name = 0;         // dynasty: std::string
    std::uint32_t claim = 0;                // heir: fixed point

    // Flags: `flag_names` 0 when absent on this build. Each CIntegerFlags holds
    // a vector of int16 indexes and a vector of int32 dates at the same positions.
    std::uintptr_t flag_names = 0;          // the address of the names' array: begin, end; std::string each
    std::uint32_t flags_begin = 0;          // flags: the indexes' vector
    std::uint32_t flags_end = 0;
    std::uint32_t flag_dates = 0;           // flags: the dates' vector
    std::uint32_t country_flags = 0;        // country: CIntegerFlags
    std::uint32_t province_flags = 0;       // province: CIntegerFlags
    std::uint32_t permanent_flags = 0;      // province: CIntegerFlags
    std::uint32_t global_flags = 0;         // root: CIntegerFlags
    std::uint32_t monarch_flags = 0;        // monarch: CIntegerFlags

    // Variables: `variable_head` 0 when absent on this build.
    std::uint32_t country_variables = 0;    // country: CVariables
    std::uint32_t province_variables = 0;   // province: CVariables
    std::uint32_t variable_head = 0;        // variables: the list's sentinel node, the count after it
    std::uint32_t variable_value = 0;       // node: int32 fixed point, its name the std::string before it

    // Pending events: `pending_list` 0 when absent on this build. The list is a
    // CPdxArray<CPendingEvent> in the root: data, then the count.
    std::uint32_t pending_list = 0;         // root: the entries
    std::uint32_t pending_count = 0;        // root: int32
    std::uint32_t pending_size = 0;         // an entry's size
    std::uint32_t pending_id = 0;           // entry: int32
    std::uint32_t pending_event = 0;        // entry: CEvent*
    std::uint32_t pending_scope = 0;        // entry: CEventScope*, owned
    std::uint32_t auto_select = 0;          // entry: int32 date, hours
    std::uint32_t scope_province = 0;       // scope: int32 province ID
    std::uint32_t scope_country = 0;        // scope: tag
    std::uint32_t scope_override = 0;       // scope: tag, used when its valid byte is set
    std::uint32_t scope_override_valid = 0; // scope: byte
    std::uint32_t event_key = 0;            // event: std::string
    std::uint32_t event_title = 0;          // event: std::string
    std::uint32_t event_options = 0;        // event: vector of pointers, its begin
    std::uint32_t event_options_end = 0;
    std::uint32_t event_hidden = 0;         // event: byte

    // Statistics: `statistics_head` 0 when absent on this build. Each series is
    // a CGraphStatistics in the root: an unordered_map from the tag's text, read
    // as its list from the sentinel node, as the variables are.
    std::uint32_t statistics[4] = {};       // root: by ANVIL_GAME_STATISTIC_*
    std::uint32_t statistics_head = 0;      // series: the list's sentinel node, the count after it
    std::uint32_t statistics_key = 0;       // node: std::string
    std::uint32_t statistics_values = 0;    // node: vector of int32, its begin; end after it
    std::uint32_t statistics_years = 0;     // node: vector of int32, its begin; end after it

    // Timed modifiers: `timed_key` 0 when absent on this build.
    std::uint32_t country_timed = 0;        // country: the list's head node, null when empty
    std::uint32_t timed_node_next = 0;      // node: the next node (the object is at +0)
    std::uint32_t province_timed = 0;       // province: std::vector of CTimedModifier*, its begin
    std::uint32_t province_timed_end = 0;
    std::uint32_t timed_modifier = 0;       // timed modifier: its CStaticModifier*
    std::uint32_t timed_key = 0;            // static modifier: its key, a std::string
    std::uint32_t timed_start = 0;          // timed modifier: int32, hours
    std::uint32_t timed_end = 0;
    std::uint32_t timed_ruler = 0;          // timed modifier: bool
    std::uint32_t timed_hidden = 0;
    std::int32_t unset_hours = 0;           // the date the game writes for none
    std::uintptr_t timed_never = 0;         // the address of the global date of a modifier given for good

    // The province view: `view_holder` 0 when absent on this build.
    std::uint32_t view_holder = 0;          // session: the selection object
    std::uint32_t view = 0;                 // selection object: the province view
    std::uint32_t view_province = 0;        // view: the CProvince* it shows
    std::uint32_t view_window = 0;          // view: its window's full object
    std::uint32_t window_part = 0;          // full window object: its widget
    std::uint32_t widget_shown = 0;         // widget: byte, nonzero while it is shown

    // Diplomacy: `diplomacy` 0 when absent on this build. Each kind of relation
    // is a CPdxArray in the country's CCountryDiplomacy of 16-byte entries: the
    // other country's index (int32), then the CRelation*.
    std::uint32_t diplomacy = 0;            // country: its CCountryDiplomacy, embedded
    std::uint32_t relations[8] = {};        // diplomacy: by ANVIL_GAME_RELATION_*, the entries
    std::uint32_t relation_counts[8] = {};  // diplomacy: int32
    std::uint32_t relation_first = 0;       // relation: tag
    std::uint32_t relation_second = 0;      // relation: tag
    std::uint32_t relation_start = 0;       // relation: int32, hours
    std::uint32_t relation_end = 0;
    std::int32_t relation_unset = 0;        // the date the game writes for none
    std::uint32_t subject_type = 0;         // dependency: CSubjectType*
    std::uint32_t subject_type_key = 0;     // subject type: its key, a std::string
    std::uint32_t statuses = 0;             // country: std::vector of CDiplomacyStatus by the other's index; end after it
    std::uint32_t status_size = 0;
    std::uint32_t status_target = 0;        // status: tag (its parent, the CCountryDiplomacy, at +0)
    std::uint32_t status_cached_sum = 0;    // status: int16
    std::uint32_t status_trust = 0;         // status: byte
    std::uint32_t status_favors = 0;        // status: int32, x 1000
    std::uint32_t status_truce = 0;         // status: byte, with the truce bit
    std::uint32_t truce_bit = 0;
    std::uint32_t opinion_cache = 0;        // country: std::vector of int16 by the other's index; end after it
    std::uint32_t opinion_cache_end = 0;
    std::uint32_t rivals = 0;               // country: the first node of its list, null when empty
    std::uint32_t rival_tag = 0;            // node: tag
    std::uint32_t rival_next = 0;           // node: the next node
    std::uint32_t overlord = 0;             // country: tag
    std::uint32_t subjects = 0;             // country: std::vector of tags, its begin; end after it
};

// A relation, its dates still the game's hours.
struct RelationEntry {
    AnvilGameTag first{};
    AnvilGameTag second{};
    std::int32_t start = 0;
    std::int32_t end = 0;
    bool has_start = false;
    bool has_end = false;
};

// A timed modifier, its dates still the game's hours.
struct TimedEntry {
    std::string key;  // as the game stores it
    std::uint32_t flags = 0;  // ANVIL_GAME_TIMED_*: NO_END and NO_START say the dates are not ones
    std::int32_t start = 0;
    std::int32_t end = 0;
};


// A flag that is set, its date still the game's hours.
struct FlagEntry {
    std::int32_t id = 0;
    std::int32_t hours = 0;
};

struct VariableEntry {
    std::string name;  // as the game stores it
    std::int32_t value = 0;
};

// Main thread only, after an outer frame, in a session: the service checks.
std::int32_t read_province_limit(const WorldLayout& l);
AnvilResult read_province(const WorldLayout& l, std::int32_t id, AnvilGameProvince& out);
// The province's CMapArea (its map part at `map_part`, the area at `area` in it):
// ANVIL_ERROR_NOT_FOUND for a province without one.
AnvilResult read_province_area(const WorldLayout& l, std::int32_t id, std::uint32_t map_part, std::uint32_t area,
                               std::uintptr_t& out);
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

// `kind` is ANVIL_GAME_MONARCH_*; the caller checks it. out.birth.session is 0.
AnvilResult read_monarch(const WorldLayout& l, std::string_view tag, std::uint32_t kind, AnvilGameMonarch& out);
AnvilResult read_monarch_name(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::string& utf8);
AnvilResult read_monarch_dynasty(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::string& utf8);

AnvilResult read_flag_id(const WorldLayout& l, std::string_view name, std::int32_t& id);
AnvilResult read_flag_name(const WorldLayout& l, std::int32_t id, std::string& utf8);
AnvilResult read_country_flags(const WorldLayout& l, std::string_view tag, std::vector<FlagEntry>& out);
AnvilResult read_province_flags(const WorldLayout& l, std::int32_t province, std::vector<FlagEntry>& out);
AnvilResult read_monarch_flags(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::vector<FlagEntry>& out);
AnvilResult read_global_flags(const WorldLayout& l, std::vector<FlagEntry>& out);

AnvilResult read_country_variables(const WorldLayout& l, std::string_view tag, std::vector<VariableEntry>& out);
AnvilResult read_province_variables(const WorldLayout& l, std::int32_t province, std::vector<VariableEntry>& out);

// The pending list now, in the game's order.
AnvilResult read_pending_keys(const WorldLayout& l, std::vector<PendingKey>& out);
// An entry of the list by its key, as AnvilGamePendingEvent: NOT_FOUND when it
// is no longer there. The country follows the game's rule (CEventManager::
// TriggerEvent): the override tag when it is set, else the scope's country, and
// for a tag that is not valid the owner of the scope's province.
// out.auto_select.session is 0.
AnvilResult read_pending(const WorldLayout& l, const PendingKey& key, AnvilGamePendingEvent& out);
// Its event's key (`title` false) or its title's localisation key.
AnvilResult read_pending_text(const WorldLayout& l, const PendingKey& key, bool title, std::string& utf8);

// `kind` is ANVIL_GAME_STATISTIC_*; the caller checks it.
AnvilResult read_statistics(const WorldLayout& l, std::string_view tag, std::uint32_t kind,
                            std::vector<AnvilGameStatisticPoint>& out);

AnvilResult read_country_timed(const WorldLayout& l, std::string_view tag, std::vector<TimedEntry>& out);
AnvilResult read_province_timed(const WorldLayout& l, std::int32_t province, std::vector<TimedEntry>& out);
// NOT_FOUND while no province view is shown.
AnvilResult read_province_view(const WorldLayout& l, std::int32_t& province);

// `kind` is ANVIL_GAME_RELATION_*; the caller checks it. Each relation must
// name the country as `first` or `second`.
AnvilResult read_relations(const WorldLayout& l, std::string_view tag, std::uint32_t kind, std::vector<RelationEntry>& out);
AnvilResult read_subject_type(const WorldLayout& l, std::string_view overlord, std::string_view subject, std::string& utf8);
// out.size is left as it is. NOT_FOUND for a country without provinces and
// for an index the opinion cache does not reach: never the game's fallback.
AnvilResult read_diplomacy_status(const WorldLayout& l, std::string_view from, std::string_view to,
                                  AnvilGameDiplomacyStatus& out);
AnvilResult read_rivals(const WorldLayout& l, std::string_view tag, std::vector<AnvilGameTag>& out);
AnvilResult read_overlord(const WorldLayout& l, std::string_view tag, AnvilGameTag& out);
AnvilResult read_subjects(const WorldLayout& l, std::string_view tag, std::vector<AnvilGameTag>& out);

// A getter of 3 bytes that returns a constant bool: xor al,al; ret or mov al,1; ret.
bool decode_bool_getter(const std::uint8_t* code, std::size_t size, bool& value);

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
