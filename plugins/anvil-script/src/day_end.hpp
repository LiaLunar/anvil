// anvil-script: the subscribers of the Day end and the date it carries
// (docs/adr/0009-day-end.md, docs/spec/day-end-v1.md sections 2-4). No game
// knowledge: the plugin asks the core for the caller's manifest and passes the
// answers in, and gives the date as the game keeps it (int32 hours).
#pragma once

#include <anvil/anvil_script.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace anvil_script {

constexpr std::size_t kMaxDayEnd = 64;

struct DayEndEntry {
    AnvilScriptDayEnd callback = nullptr;
    void* context = nullptr;
    const AnvilPlugin* owner = nullptr;
};

// The game's date, hours since 1 January -5000 in 365-day years (anvil-game's
// decode), with the flags of the day it starts.
AnvilScriptDay decode_day(std::int32_t hours);

class DayEndList {
public:
    // The checks of subscribe in the spec's order (section 3); `why` names
    // the reason of a refusal for the log. `known` and `affects_gameplay`
    // come from anvil.control.
    AnvilResult add(bool known, bool affects_gameplay, const AnvilPlugin* self, AnvilScriptDayEnd callback,
                    void* context, std::string& why);

    // The first Day end: no subscription after it. Cheap after the first call.
    void close() noexcept {
        if (!closed_.load(std::memory_order_relaxed)) {
            std::lock_guard lock(mutex_);
            closed_.store(true, std::memory_order_release);
        }
    }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // Read only after close(): the list no longer changes then.
    std::size_t size() const noexcept { return count_.load(std::memory_order_acquire); }
    const DayEndEntry& entry(std::size_t index) const noexcept { return entries_[index]; }

private:
    std::mutex mutex_;
    std::array<DayEndEntry, kMaxDayEnd> entries_{};
    std::atomic<std::size_t> count_{0};
    std::atomic<bool> closed_{false};
};

}  // namespace anvil_script
