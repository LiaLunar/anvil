#include "day_end.hpp"

namespace anvil_script {
namespace {

constexpr std::int64_t kHoursToYearZero = 43'800'000;  // 5000 years * 365 days * 24 hours
constexpr std::uint32_t kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    std::int64_t q = value / divisor;
    if (value % divisor != 0 && (value < 0) != (divisor < 0)) --q;
    return q;
}

}  // namespace

AnvilScriptDay decode_day(std::int32_t hours) {
    const std::int64_t days = floor_div(static_cast<std::int64_t>(hours) - kHoursToYearZero, 24);
    const std::int64_t year = floor_div(days, 365);
    auto day_of_year = static_cast<std::uint32_t>(days - year * 365);
    std::uint32_t month = 0;
    while (day_of_year >= kMonthDays[month]) day_of_year -= kMonthDays[month++];
    AnvilScriptDay out{};
    out.size = sizeof(AnvilScriptDay);
    out.year = static_cast<std::int32_t>(year);
    out.month = static_cast<std::int32_t>(month + 1);
    out.day = static_cast<std::int32_t>(day_of_year + 1);
    if (out.day == 1) out.flags |= ANVIL_SCRIPT_DAY_NEW_MONTH;
    if (out.day == 1 && out.month == 1) out.flags |= ANVIL_SCRIPT_DAY_NEW_YEAR;
    return out;
}

AnvilResult DayEndList::add(bool known, bool affects_gameplay, const AnvilPlugin* self, AnvilScriptDayEnd callback,
                            void* context, std::string& why) {
    if (self == nullptr || callback == nullptr || !known) {
        why = "a NULL argument or an unknown caller";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (!affects_gameplay) {
        why = "the plugin has affects_gameplay = false; only gameplay plugins run at the day end (anvil-game.day is "
              "for the others)";
        return ANVIL_ERROR_WRONG_KIND;
    }
    std::lock_guard lock(mutex_);
    if (closed()) {
        why = "the game has already ended a day; subscribe in the plugin's load";
        return ANVIL_ERROR_WRONG_PHASE;
    }
    const std::size_t n = count_.load(std::memory_order_relaxed);
    if (n == kMaxDayEnd) {
        why = "64 subscriptions are there already";
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
    entries_[n] = DayEndEntry{callback, context, self};
    count_.store(n + 1, std::memory_order_release);
    return ANVIL_OK;
}

}  // namespace anvil_script
