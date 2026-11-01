#include "session.hpp"

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
