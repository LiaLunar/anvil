#include "recorder.hpp"

#include <windows.h>

#include <mutex>
#include <new>
#include <unordered_map>

namespace script_profiler {

namespace {

std::mutex g_threads_mutex;
Thread* g_threads[kMaxThreads] = {};
std::atomic<std::size_t> g_count{0};
thread_local Thread* t_thread = nullptr;
thread_local bool t_failed = false;

}  // namespace

void ThreadTable::clear() noexcept {
    const std::uint32_t n = used_.load(std::memory_order_acquire);
    for (std::uint32_t k = 0; k < n; ++k) {
        Slot& s = slots_[order_[k]];
        s.key.store(0, std::memory_order_relaxed);
        s.calls.store(0, std::memory_order_relaxed);
        s.ticks.store(0, std::memory_order_relaxed);
        s.max.store(0, std::memory_order_relaxed);
        s.caller.store(0, std::memory_order_relaxed);
        s.effect.store(0, std::memory_order_relaxed);
    }
    used_.store(0, std::memory_order_release);
    nested_triggers_.store(0, std::memory_order_relaxed);
    nested_effects_.store(0, std::memory_order_relaxed);
    dropped_calls_.store(0, std::memory_order_relaxed);
    dropped_ticks_.store(0, std::memory_order_relaxed);
}

Thread* this_thread() noexcept {
    if (t_thread != nullptr) return t_thread;
    if (t_failed) return nullptr;
    std::lock_guard lock(g_threads_mutex);
    const std::size_t n = g_count.load(std::memory_order_relaxed);
    // Zeroed pages, committed by Windows as the table fills.
    void* memory = n < kMaxThreads ? VirtualAlloc(nullptr, sizeof(Thread), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
                                   : nullptr;
    if (memory == nullptr) {
        t_failed = true;
        return nullptr;
    }
    auto* t = new (memory) Thread;
    t->thread_id = GetCurrentThreadId();
    g_threads[n] = t;
    g_count.store(n + 1, std::memory_order_release);
    t_thread = t;
    return t;
}

std::size_t thread_count() noexcept { return g_count.load(std::memory_order_acquire); }

Thread* thread_at(std::size_t i) noexcept { return i < thread_count() ? g_threads[i] : nullptr; }

void clear_all() noexcept {
    const std::size_t n = thread_count();
    for (std::size_t i = 0; i < n; ++i) g_threads[i]->table.clear();
}

Snapshot collect(std::uint32_t main_thread_id) {
    Snapshot out;
    std::unordered_map<const void*, std::size_t> index;
    const std::size_t n = thread_count();
    for (std::size_t i = 0; i < n; ++i) {
        const Thread& t = *g_threads[i];
        const ThreadTable& table = t.table;
        const std::uint32_t used = table.used();
        const bool main = t.thread_id == main_thread_id;
        if (used != 0) ++out.threads;
        for (std::uint32_t k = 0; k < used; ++k) {
            const Slot& s = table.nth(k);
            const auto* block = reinterpret_cast<const void*>(s.key.load(std::memory_order_acquire));
            if (block == nullptr) continue;
            const std::uint64_t ticks = s.ticks.load(std::memory_order_relaxed);
            auto [it, fresh] = index.try_emplace(block, out.roots.size());
            if (fresh) {
                RootStat r;
                r.block = block;
                r.effect = s.effect.load(std::memory_order_relaxed) != 0;
                r.caller = s.caller.load(std::memory_order_relaxed);
                out.roots.push_back(r);
            }
            RootStat& r = out.roots[it->second];
            r.calls += s.calls.load(std::memory_order_relaxed);
            r.ticks += ticks;
            const std::uint64_t max = s.max.load(std::memory_order_relaxed);
            if (max > r.max) r.max = max;
            if (main) r.main_ticks += ticks;
        }
        out.nested_triggers += table.nested_triggers();
        out.nested_effects += table.nested_effects();
        out.dropped_calls += table.dropped_calls();
        out.dropped_ticks += table.dropped_ticks();
    }
    return out;
}

}  // namespace script_profiler
