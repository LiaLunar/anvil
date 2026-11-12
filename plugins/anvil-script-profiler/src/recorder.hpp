// The recording side of anvil-script-profiler (docs/script-profiler.md): one
// table per game thread, written only by that thread, with no lock and no
// sampling. A row is a root block: the outermost trigger or effect block
// running on its thread, keyed by its address. Inner blocks are only counted.
//
// A table never moves or shrinks once made, and is kept for the process: the
// panel's thread may read it while the game writes it, and a game thread still
// inside a removed hook may still write a row. Values are relaxed atomics, so
// a read is never torn; a read while the game writes sees slightly old counts.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace script_profiler {

// What a root block is: a trigger (AND) block, a block of effects, or a call of a scripted trigger
// (every call, also inside other blocks: its time is in the trigger list too).
enum class Kind : std::uint32_t { trigger = 0, effect = 1, scripted = 2 };

struct Slot {
    std::atomic<std::uintptr_t> key{0};  // the block's address; 0: free
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> ticks{0};  // rdtsc ticks inside the block, summed
    std::atomic<std::uint64_t> max{0};    // the longest single run, in ticks
    std::atomic<std::uint32_t> caller{0};  // RVA of the first call's return address in eu4.exe
    std::atomic<std::uint32_t> kind{0};    // Kind
};

class ThreadTable {
public:
    static constexpr std::uint32_t kBits = 16;
    static constexpr std::uint32_t kSlots = 1u << kBits;
    // At most this many blocks per thread: later new blocks are only summed
    // into the dropped counts.
    static constexpr std::uint32_t kLimit = kSlots / 4 * 3;

    // The owner thread only.
    void record(const void* block, Kind kind, std::uint64_t ticks, std::uint32_t caller) noexcept {
        const auto key = reinterpret_cast<std::uintptr_t>(block);
        std::uint32_t i = slot_of(key);
        for (;;) {
            Slot& s = slots_[i];
            const std::uintptr_t at = s.key.load(std::memory_order_relaxed);
            if (at == key) {
                add(s.calls, 1);
                add(s.ticks, ticks);
                if (ticks > s.max.load(std::memory_order_relaxed)) s.max.store(ticks, std::memory_order_relaxed);
                return;
            }
            if (at == 0) {
                const std::uint32_t n = used_.load(std::memory_order_relaxed);
                if (n >= kLimit) {
                    add(dropped_calls_, 1);
                    add(dropped_ticks_, ticks);
                    return;
                }
                s.calls.store(1, std::memory_order_relaxed);
                s.ticks.store(ticks, std::memory_order_relaxed);
                s.max.store(ticks, std::memory_order_relaxed);
                s.caller.store(caller, std::memory_order_relaxed);
                s.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
                s.key.store(key, std::memory_order_release);
                order_[n] = i;
                used_.store(n + 1, std::memory_order_release);
                return;
            }
            i = (i + 1) & (kSlots - 1);
        }
    }
    void nested(Kind kind) noexcept {
        add(kind == Kind::effect ? nested_effects_ : kind == Kind::scripted ? nested_scripted_ : nested_triggers_, 1);
    }

    // Any thread.
    std::uint32_t used() const noexcept { return used_.load(std::memory_order_acquire); }
    // The n-th block recorded (n < used()).
    const Slot& nth(std::uint32_t n) const noexcept { return slots_[order_[n]]; }
    std::uint64_t nested_triggers() const noexcept { return nested_triggers_.load(std::memory_order_relaxed); }
    std::uint64_t nested_effects() const noexcept { return nested_effects_.load(std::memory_order_relaxed); }
    std::uint64_t nested_scripted() const noexcept { return nested_scripted_.load(std::memory_order_relaxed); }
    std::uint64_t dropped_calls() const noexcept { return dropped_calls_.load(std::memory_order_relaxed); }
    std::uint64_t dropped_ticks() const noexcept { return dropped_ticks_.load(std::memory_order_relaxed); }

    // Only while no hook is installed and no game thread is inside one.
    void clear() noexcept;

private:
    static std::uint32_t slot_of(std::uintptr_t key) noexcept {
        return static_cast<std::uint32_t>(((key >> 3) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    }
    static void add(std::atomic<std::uint64_t>& v, std::uint64_t n) noexcept {
        v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }

    Slot slots_[kSlots];
    std::uint32_t order_[kLimit] = {};  // slot index of the n-th block recorded
    std::atomic<std::uint32_t> used_{0};
    std::atomic<std::uint64_t> nested_triggers_{0};
    std::atomic<std::uint64_t> nested_effects_{0};
    std::atomic<std::uint64_t> nested_scripted_{0};
    std::atomic<std::uint64_t> dropped_calls_{0};
    std::atomic<std::uint64_t> dropped_ticks_{0};
};

// A game thread's state: its table and how deep it is in trigger and effect
// blocks. The depths belong to the thread alone.
struct Thread {
    ThreadTable table;
    std::uint32_t thread_id = 0;
    int trigger_depth = 0;
    int effect_depth = 0;
    int scripted_depth = 0;
};

// The calling thread's state, made on its first block; nullptr when it cannot
// be made (no memory, or more than kMaxThreads threads), and the thread's
// blocks then go unrecorded.
inline constexpr std::size_t kMaxThreads = 256;
Thread* this_thread() noexcept;

// Every thread's state made so far, for reading (any thread).
std::size_t thread_count() noexcept;
Thread* thread_at(std::size_t i) noexcept;

// Clears every table: only while no hook is installed.
void clear_all() noexcept;

// One root block summed over every thread.
struct RootStat {
    const void* block = nullptr;
    Kind kind = Kind::trigger;
    std::uint64_t calls = 0;
    std::uint64_t ticks = 0;
    std::uint64_t max = 0;
    std::uint64_t main_ticks = 0;  // the part on the game's main thread
    std::uint32_t caller = 0;      // RVA, from the first thread that recorded it
};

struct Snapshot {
    std::vector<RootStat> roots;  // in no order
    std::uint64_t nested_triggers = 0;
    std::uint64_t nested_effects = 0;
    std::uint64_t nested_scripted = 0;
    std::uint64_t dropped_calls = 0;
    std::uint64_t dropped_ticks = 0;
    std::uint32_t threads = 0;  // threads that recorded a block
};

// Sums the tables; `main_thread_id` names the game's main thread.
Snapshot collect(std::uint32_t main_thread_id);

}  // namespace script_profiler
