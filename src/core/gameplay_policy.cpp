#include "gameplay_policy.hpp"

#include "suspension.hpp"

#include <anvil/anvil_game.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace anvil {
namespace {

bool listed(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Empty when the plugin may stay active in this one mode.
std::string mode_reason(const Manifest& m, Allow allow, const std::vector<std::string>& opt_ins, const char* key,
                        const char* mode) {
    if (allow != Allow::OptIn) {
        return std::string("affects gameplay and its plugin.toml does not offer ") + key + " = \"opt-in\" (" + mode + ")";
    }
    if (!listed(opt_ins, m.id)) {
        return std::string("affects gameplay and is not allowed in ") + mode + " (add it to " + key + " in anvil.toml)";
    }
    return {};
}

std::string mode_text(SessionMode mode) {
    if (mode.ironman && mode.multiplayer) return "ironman, multiplayer";
    if (mode.ironman) return "ironman";
    if (mode.multiplayer) return "multiplayer";
    return "neither ironman nor multiplayer";
}

}  // namespace

std::string suspension_reason(const Manifest& manifest, SessionMode mode, const GameplayOptIns& opt_ins) {
    if (!manifest.affects_gameplay) return {};
    if (mode.ironman) {
        if (auto r = mode_reason(manifest, manifest.allow_ironman, opt_ins.ironman, "allow_ironman", "ironman");
            !r.empty()) {
            return r;
        }
    }
    if (mode.multiplayer) {
        return mode_reason(manifest, manifest.allow_multiplayer, opt_ins.multiplayer, "allow_multiplayer",
                           "multiplayer");
    }
    return {};
}

std::vector<std::string> opt_in_problems(const Manifest& manifest, const GameplayOptIns& opt_ins) {
    std::vector<std::string> out;
    const auto check = [&](Allow allow, const std::vector<std::string>& ids, const char* key, const char* mode) {
        if (!listed(ids, manifest.id)) return;
        if (!manifest.affects_gameplay) {
            out.push_back(std::string(key) + " in anvil.toml lists it, but it does not affect gameplay: it is never "
                                              "suspended, so the entry does nothing");
        } else if (allow != Allow::OptIn) {
            out.push_back(std::string(key) + " in anvil.toml lists it, but its plugin.toml does not offer " + key +
                          " = \"opt-in\": it stays suspended in " + mode);
        }
    };
    check(manifest.allow_ironman, opt_ins.ironman, "allow_ironman", "ironman");
    check(manifest.allow_multiplayer, opt_ins.multiplayer, "allow_multiplayer", "multiplayer");
    return out;
}

GameplayPolicy::GameplayPolicy(ChangeRegistry& changes, ServiceBus& bus, GameplayOptIns opt_ins, LogSink log)
    : changes_(changes), bus_(bus), opt_ins_(std::move(opt_ins)), log_(std::move(log)) {}

void GameplayPolicy::add(const Manifest& manifest, OwnerId owner) {
    for (const auto& problem : opt_in_problems(manifest, opt_ins_)) log_(ANVIL_LOG_WARNING, manifest.id, problem);
    std::lock_guard lock(mutex_);
    Entry& entry = entries_.emplace_back(Entry{manifest, owner, false, false});
    if (!session_) return;
    if (std::string reason = suspension_reason(manifest, *session_, opt_ins_); !reason.empty()) {
        apply(entry, reason);
    }
}

void GameplayPolicy::forget(OwnerId owner) {
    std::lock_guard lock(mutex_);
    for (Entry& e : entries_) {
        if (e.owner == owner) e.forgotten = true;
    }
}

void GameplayPolicy::apply(Entry& e, const std::string& reason) {
    const ChangeStatus status = suspend_plugin(changes_, bus_, e.owner, e.manifest.id);
    e.suspended = true;
    const size_t out = changes_.suspended_count(e.owner);
    log_(ANVIL_LOG_INFO, e.manifest.id,
         "suspended: " + reason + "; " + std::to_string(out) + " change(s) out of the game");
    if (status != ChangeStatus::Ok) {
        log_(ANVIL_LOG_WARNING, e.manifest.id, "suspend: some of its changes could not be taken out (see above)");
    }
}

void GameplayPolicy::release(Entry& e, const char* why) {
    const ChangeStatus status = resume_plugin(changes_, bus_, e.owner, e.manifest.id);
    e.suspended = false;
    log_(ANVIL_LOG_INFO, e.manifest.id,
         std::string("resumed: ") + why + "; " + std::to_string(changes_.count(e.owner)) + " change(s) back");
    if (status != ChangeStatus::Ok) {
        log_(ANVIL_LOG_WARNING, e.manifest.id, "resume: some of its changes could not be put back (see above)");
    }
}

void GameplayPolicy::session_started(SessionMode mode) {
    std::lock_guard lock(mutex_);
    session_ = mode;
    size_t suspended = 0, allowed = 0;
    for (Entry& e : entries_) {
        if (e.forgotten || !e.manifest.affects_gameplay) continue;
        std::string reason = suspension_reason(e.manifest, mode, opt_ins_);
        if (reason.empty()) {
            if (mode.ironman || mode.multiplayer) {
                ++allowed;
                log_(ANVIL_LOG_INFO, e.manifest.id, "stays active: the player allows it in " + mode_text(mode));
            }
            if (e.suspended) release(e, "the new session allows it");
            continue;
        }
        ++suspended;
        if (!e.suspended) apply(e, reason);
    }
    log_(ANVIL_LOG_INFO, "anvil",
         "game session started (" + mode_text(mode) + "): " + std::to_string(suspended) +
             " gameplay plugin(s) suspended, " + std::to_string(allowed) + " allowed by the player");
}

void GameplayPolicy::session_ended() {
    std::lock_guard lock(mutex_);
    if (!session_) return;
    session_.reset();
    size_t resumed = 0;
    for (Entry& e : entries_) {
        if (!e.suspended) continue;
        release(e, "the session ended");
        ++resumed;
    }
    log_(ANVIL_LOG_INFO, "anvil", "game session ended: " + std::to_string(resumed) + " plugin(s) resumed");
}

void GameplayPolicy::on_session_event(const void* payload) {
    const auto* s = static_cast<const AnvilGameSession*>(payload);
    if (s == nullptr || s->size < offsetof(AnvilGameSession, multiplayer) + sizeof(s->multiplayer)) {
        log_(ANVIL_LOG_WARNING, "anvil", std::string(ANVIL_GAME_EVENT_SESSION) + " without a valid payload: ignored");
        return;
    }
    if (s->change == ANVIL_GAME_SESSION_STARTED) {
        session_started({s->ironman != 0, s->multiplayer != 0});
    } else if (s->change == ANVIL_GAME_SESSION_ENDED) {
        session_ended();
    } else {
        log_(ANVIL_LOG_WARNING, "anvil",
             std::string(ANVIL_GAME_EVENT_SESSION) + " with unknown change " + std::to_string(s->change) + ": ignored");
    }
}

void GameplayPolicy::subscribe() {
    bus_.subscribe(
        "anvil", ANVIL_GAME_EVENT_SESSION,
        [](const char*, const void* payload, void* context) {
            static_cast<GameplayPolicy*>(context)->on_session_event(payload);
        },
        this);
}

std::optional<SessionMode> GameplayPolicy::session() const {
    std::lock_guard lock(mutex_);
    return session_;
}

bool GameplayPolicy::suspended(OwnerId owner) const {
    std::lock_guard lock(mutex_);
    for (const Entry& e : entries_) {
        if (e.owner == owner) return e.suspended;
    }
    return false;
}

}  // namespace anvil
