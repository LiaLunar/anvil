#include "commands.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

namespace anvil_game {
namespace {

// The caller's struct takes only what its `size` covers, and keeps that size.
void give(AnvilGameCommandResult* out, const AnvilGameCommandResult& value) {
    if (out == nullptr) return;
    AnvilGameCommandResult copy = value;
    copy.size = out->size;
    std::memcpy(out, &copy, std::min<std::size_t>(out->size, sizeof copy));
}

bool same_ref(const AnvilGameUnitRef& a, const AnvilGameUnitRef& b) {
    return a.id == b.id && a.kind == b.kind && std::memcmp(a.country.text, b.country.text, 3) == 0;
}

// What a caller built against version 1 sets, and what EVENT_OPTION reads.
constexpr std::uint32_t kVersion1Size = offsetof(AnvilGameCommand, pending_event_id);
constexpr std::uint32_t kEventOptionSize = offsetof(AnvilGameCommand, option_index) + sizeof(std::int32_t);
constexpr std::uint32_t kSpeedSize = offsetof(AnvilGameCommand, speed) + sizeof(std::int32_t);

// The checks that need no unit: the arguments, the caller, the type, the moment.
AnvilResult check_call(CommandBackend& b, const AnvilPlugin* self, const AnvilGameCommand* c,
                       const AnvilGameCommandResult* out) {
    if (c == nullptr || c->size < kVersion1Size) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (out != nullptr && out->size < sizeof(out->size)) return ANVIL_ERROR_INVALID_ARGUMENT;
    bool affects = false, suspended = false;
    if (!b.caller(self, affects, suspended) || !affects) return ANVIL_ERROR_UNSUPPORTED;
    if (suspended) return ANVIL_ERROR_SUSPENDED;
    switch (c->type) {
    case ANVIL_GAME_COMMAND_DRILL:
        if ((c->flags & ~ANVIL_GAME_COMMAND_START) != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
        break;
    case ANVIL_GAME_COMMAND_MERGE:
    case ANVIL_GAME_COMMAND_SPLIT:
        if (c->flags != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
        break;
    case ANVIL_GAME_COMMAND_EVENT_OPTION:
        if (c->flags != 0 || c->size < kEventOptionSize || c->count != 0 || c->units != nullptr) {
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        break;
    case ANVIL_GAME_COMMAND_SPEED:
        if (c->flags != 0 || c->size < kSpeedSize || c->count != 0 || c->units != nullptr || c->speed < 1 ||
            c->speed > 5) {
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        break;
    default: return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!b.declared(c->type)) return ANVIL_ERROR_NOT_DECLARED;
    if (!b.open() || b.hot_join()) return ANVIL_ERROR_WRONG_PHASE;
    if (c->type == ANVIL_GAME_COMMAND_EVENT_OPTION || c->type == ANVIL_GAME_COMMAND_SPEED) return ANVIL_OK;
    if (c->count == 0 || c->units == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (c->count > ANVIL_GAME_COMMAND_MAX_UNITS) return ANVIL_ERROR_LIMIT;
    return ANVIL_OK;
}

// The units, each the player's own, of the kind the ref says and the type takes.
AnvilResult find_units(CommandBackend& b, const AnvilGameCommand& c, std::vector<CommandUnit>& units) {
    for (std::uint32_t i = 0; i < c.count; ++i) {
        for (std::uint32_t j = 0; j < i; ++j) {
            if (same_ref(c.units[i], c.units[j])) return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        CommandUnit unit;
        if (const AnvilResult r = b.find(c.units[i], unit); r != ANVIL_OK) return r;
        if (unit.kind != c.units[i].kind) return ANVIL_ERROR_INVALID_ARGUMENT;
        if (c.type == ANVIL_GAME_COMMAND_DRILL && unit.kind != ANVIL_GAME_UNIT_ARMY) return ANVIL_ERROR_INVALID_ARGUMENT;
        units.push_back(unit);
    }
    if (c.type == ANVIL_GAME_COMMAND_SPLIT && (units.size() != 1 || units[0].subunits < 2)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (c.type == ANVIL_GAME_COMMAND_MERGE) {
        if (units.size() < 2 || units[0].location == 0) return ANVIL_ERROR_INVALID_ARGUMENT;
        for (const CommandUnit& u : units) {
            if (u.kind != units[0].kind || u.location != units[0].location) return ANVIL_ERROR_INVALID_ARGUMENT;
        }
    }
    return ANVIL_OK;
}

}  // namespace

AnvilResult CommandPoster::choose(CommandBackend& b, const AnvilGameCommand& c, AnvilGameCommandResult* out) {
    AnvilGameCommandResult result{};
    result.size = sizeof(result);
    if (c.option_index < 0) return ANVIL_ERROR_INVALID_ARGUMENT;
    EventWindow window;
    if (const AnvilResult r = b.find_event_window(c.pending_event_id, c.option_index, window); r != ANVIL_OK) return r;
    if (!window.keep_alive) {
        // Chosen in this frame already: the window goes at the game's next update.
        result.skipped = 1;
        give(out, result);
        return ANVIL_OK;
    }
    if (!window.has_button) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!window.enabled) return ANVIL_ERROR_WRONG_PHASE;
    if (!b.post_allowed()) {
        // The game would close the window and drop the command: leave both alone.
        result.refused = 1;
        give(out, result);
        return ANVIL_OK;
    }
    const std::uint64_t frame = b.frame();
    if (frame != frame_) {
        frame_ = frame;
        used_ = 0;
    }
    if (used_ + 1 > ANVIL_GAME_COMMAND_MAX_PER_FRAME) return ANVIL_ERROR_LIMIT;
    ++used_;
    b.choose_option(window, c.option_index);
    result.posted = 1;
    give(out, result);
    return ANVIL_OK;
}

AnvilResult CommandPoster::set_speed(CommandBackend& b, const AnvilGameCommand& c, AnvilGameCommandResult* out) {
    AnvilGameCommandResult result{};
    result.size = sizeof(result);
    std::int32_t speed = 0;
    bool buttons_act = false;
    if (!b.game_speed(speed, buttons_act) || speed < 1 || speed > 5) return ANVIL_ERROR_INTERNAL;
    if (speed == c.speed) {
        result.skipped = 1;
        give(out, result);
        return ANVIL_OK;
    }
    if (!buttons_act) {
        // The game's buttons would do nothing: neither does the service.
        result.refused = 1;
        give(out, result);
        return ANVIL_OK;
    }
    const bool up = c.speed > speed;
    const auto steps = static_cast<std::uint32_t>(up ? c.speed - speed : speed - c.speed);
    const std::uint64_t frame = b.frame();
    if (frame != frame_) {
        frame_ = frame;
        used_ = 0;
    }
    if (used_ + steps > ANVIL_GAME_COMMAND_MAX_PER_FRAME) return ANVIL_ERROR_LIMIT;
    bool memory = true;
    for (std::uint32_t i = 0; i < steps; ++i) {
        void* made = b.make_speed_step(up);
        if (made == nullptr) {
            memory = false;
            break;
        }
        ++used_;
        if (b.submit(made)) {
            ++result.posted;
        } else {
            b.destroy(made);
            ++result.refused;
        }
    }
    give(out, result);
    return memory ? ANVIL_OK : ANVIL_ERROR_OUT_OF_MEMORY;
}

std::int32_t split_count(std::uint32_t subunits) { return static_cast<std::int32_t>((subunits + 1) / 2); }

AnvilResult CommandPoster::post(CommandBackend& b, const AnvilPlugin* self, const AnvilGameCommand* command,
                                AnvilGameCommandResult* out) {
    if (const AnvilResult r = check_call(b, self, command, out); r != ANVIL_OK) return r;
    const AnvilGameCommand& c = *command;
    if (c.type == ANVIL_GAME_COMMAND_EVENT_OPTION) return choose(b, c, out);
    if (c.type == ANVIL_GAME_COMMAND_SPEED) return set_speed(b, c, out);
    std::vector<CommandUnit> units;
    std::vector<const CommandUnit*> drills;
    try {
        units.reserve(c.count);
        if (const AnvilResult r = find_units(b, c, units); r != ANVIL_OK) return r;
        if (c.type == ANVIL_GAME_COMMAND_DRILL) {
            const bool start = (c.flags & ANVIL_GAME_COMMAND_START) != 0;
            for (const CommandUnit& u : units) {
                if (u.drilling != start) drills.push_back(&u);
            }
        }
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }

    AnvilGameCommandResult result{};
    result.size = sizeof(result);
    const std::uint32_t planned = c.type == ANVIL_GAME_COMMAND_DRILL ? static_cast<std::uint32_t>(drills.size()) : 1;
    result.skipped = c.type == ANVIL_GAME_COMMAND_DRILL ? c.count - planned : 0;

    const std::uint64_t frame = b.frame();
    if (frame != frame_) {
        frame_ = frame;
        used_ = 0;
    }
    if (used_ + planned > ANVIL_GAME_COMMAND_MAX_PER_FRAME) return ANVIL_ERROR_LIMIT;

    // Every command the session refuses is freed here: the game's post
    // function leaves it to the caller, and the buttons leak it.
    const auto send = [&](void* made) {
        if (made == nullptr) return false;
        ++used_;
        if (b.submit(made)) {
            ++result.posted;
        } else {
            b.destroy(made);
            ++result.refused;
        }
        return true;
    };
    bool memory = true;
    switch (c.type) {
    case ANVIL_GAME_COMMAND_DRILL:
        for (const CommandUnit* u : drills) {
            if (!(memory = send(b.make_drill(*u)))) break;
        }
        break;
    case ANVIL_GAME_COMMAND_SPLIT: memory = send(b.make_split(units[0], split_count(units[0].subunits))); break;
    case ANVIL_GAME_COMMAND_MERGE:
        try {
            memory = send(b.make_merge(units));
        } catch (const std::bad_alloc&) {
            memory = false;
        }
        break;
    }
    give(out, result);
    return memory ? ANVIL_OK : ANVIL_ERROR_OUT_OF_MEMORY;
}

}  // namespace anvil_game
