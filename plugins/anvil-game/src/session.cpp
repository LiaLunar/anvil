#include "session.hpp"

#include <utility>

namespace anvil_game {

namespace {

constexpr std::int64_t kHoursToYearZero = 43'800'000;  // 5000 years * 365 days * 24 hours
constexpr std::uint32_t kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    std::int64_t q = value / divisor;
    if (value % divisor != 0 && (value < 0) != (divisor < 0)) --q;
    return q;
}

}  // namespace

AnvilGameDate decode_date(std::int32_t hours) {
    const std::int64_t days = floor_div(static_cast<std::int64_t>(hours) - kHoursToYearZero, 24);
    const std::int64_t year = floor_div(days, 365);
    auto day_of_year = static_cast<std::uint32_t>(days - year * 365);
    std::uint32_t month = 0;
    while (day_of_year >= kMonthDays[month]) day_of_year -= kMonthDays[month++];
    AnvilGameDate out{};
    out.size = sizeof(AnvilGameDate);
    out.year = static_cast<std::int32_t>(year);
    out.month = month + 1;
    out.day = day_of_year + 1;
    return out;
}

OosTracker::Change OosTracker::update(bool running, std::uint64_t session, std::uint32_t flags) {
    Change out;
    if (!running) {
        session_ = 0;
        flags_ = 0;
        return out;
    }
    if (session != session_) {
        session_ = session;
        flags_ = 0;
    }
    if (flags == flags_) return out;
    out.changed = true;
    out.event.size = sizeof(AnvilGameOosChange);
    out.event.flags = flags;
    out.event.previous = flags_;
    out.event.session = session;
    flags_ = flags;
    return out;
}

DateEvents DateTracker::update(bool running, std::uint64_t session, std::int32_t hours) {
    DateEvents out;
    if (!running) {
        known_ = false;
        return out;
    }
    const bool same = known_ && session == session_;
    const std::int64_t previous_days = floor_div(hours_, 24);
    const std::int64_t days = floor_div(hours, 24);
    const std::int32_t previous = hours_;
    known_ = true;
    session_ = session;
    hours_ = hours;
    if (!same || days <= previous_days) return out;
    out.change.size = sizeof(AnvilGameDateChange);
    out.change.days = static_cast<std::uint32_t>(days - previous_days);
    out.change.date = decode_date(hours);
    out.change.date.session = session;
    out.change.previous = decode_date(previous);
    out.change.previous.session = session;
    out.day = true;
    out.year = out.change.date.year != out.change.previous.year;
    out.month = out.year || out.change.date.month != out.change.previous.month;
    return out;
}

WarTracker::Changes WarTracker::update(std::uint64_t session, std::vector<WarSeen> seen) {
    Changes out;
    const bool same = known_ && session == session_;
    if (same) {
        const auto has = [](const std::vector<WarSeen>& list, std::uint64_t id) {
            for (const WarSeen& w : list) {
                if (w.id == id) return true;
            }
            return false;
        };
        for (const WarSeen& w : wars_) {
            if (!has(seen, w.id)) out.ended.push_back(w);
        }
        for (const WarSeen& w : seen) {
            if (!has(wars_, w.id)) out.started.push_back(w);
        }
    }
    known_ = true;
    session_ = session;
    wars_ = std::move(seen);
    return out;
}

const WarSeen* WarTracker::find(std::uint64_t session, std::uint64_t id) const {
    if (!known_ || session != session_) return nullptr;
    for (const WarSeen& w : wars_) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

SessionChanges SessionTracker::update(const SessionView& view) {
    SessionChanges out;
    if (!view.readable) return out;
    if (active_) {
        const bool same = view.running && view.session == session_ && view.installs == installs_ &&
                          (current_.ironman != 0) == view.ironman && (current_.multiplayer != 0) == view.multiplayer;
        if (same) return out;
        AnvilGameSession& ended = out.items[out.count++];
        ended = current_;
        ended.change = ANVIL_GAME_SESSION_ENDED;
        active_ = false;
    }
    if (view.running) {
        current_ = AnvilGameSession{};
        current_.size = sizeof(AnvilGameSession);
        current_.change = ANVIL_GAME_SESSION_STARTED;
        current_.ironman = view.ironman ? 1u : 0u;
        current_.multiplayer = view.multiplayer ? 1u : 0u;
        current_.number = ++started_;
        session_ = view.session;
        installs_ = view.installs;
        active_ = true;
        out.items[out.count++] = current_;
    }
    return out;
}

}  // namespace anvil_game
