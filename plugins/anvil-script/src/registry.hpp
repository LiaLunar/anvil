// anvil-script: the table of registered effects (docs/spec/script-effects-v1.md
// section 3). No game knowledge: the plugin asks the core for the caller's
// manifest and passes the answers in.
#pragma once

#include <anvil/anvil_script.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace anvil_script {

constexpr std::size_t kMaxEffects = 256;
constexpr std::size_t kMaxKey = 63;

struct Entry {
    char key[kMaxKey + 1] = {};
    std::uint32_t length = 0;
    AnvilScriptExecute execute = nullptr;
    AnvilScriptDescribe describe = nullptr;
    void* context = nullptr;
    const AnvilPlugin* owner = nullptr;
};

// What the core says about the caller.
struct Caller {
    bool known = false;
    std::string id;
    bool affects_gameplay = false;
    bool early = false;
};

// "<owner>_<name>": the owner's id with '-' as '_', then '_', then at least
// one character; [a-z0-9_] and at most 63 bytes in all.
bool valid_key(std::string_view owner_id, std::string_view key);

class Registry {
public:
    // The checks of register_effect in the spec's order; `why` names the
    // reason of a refusal for the log.
    AnvilResult add(const Caller& caller, const AnvilPlugin* self, const AnvilScriptEffect* effect, std::string& why);

    // The first parse: no registration after it. Cheap after the first call.
    void close() noexcept {
        if (!closed_.load(std::memory_order_relaxed)) closed_.store(true, std::memory_order_release);
    }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // The entry for a key as a script file has it (any case); -1 when none.
    // Lock-free: entries are complete before count_ grows.
    int find(const char* key) const noexcept;
    const Entry& entry(int index) const noexcept { return entries_[static_cast<std::size_t>(index)]; }
    std::size_t size() const noexcept { return count_.load(std::memory_order_acquire); }

private:
    std::mutex mutex_;
    std::array<Entry, kMaxEffects> entries_{};
    std::atomic<std::size_t> count_{0};
    // Bit c: some key starts with the lower-case byte c. Most of the game's
    // keys leave at this test.
    std::array<std::atomic<std::uint64_t>, 4> first_{};
    std::atomic<bool> closed_{false};
};

}  // namespace anvil_script
