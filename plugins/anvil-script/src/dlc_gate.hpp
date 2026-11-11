// has_dlc for plugin names (anvil#316): `has_dlc = "anvil:<plugin id>"` in a
// mod's script is true while that plugin is loaded and not suspended in a
// single-player session, and false everywhere else: in multiplayer for every
// plugin, outside a session, for a plugin that is not there and, without
// Anvil, because the game knows no DLC of that name. docs/spec/has-dlc-v1.md.
//
// The table is filled while the game parses (any thread, under a lock) and
// read while it evaluates triggers, also inside its ParallelFor passes: an
// answer is one relaxed atomic load, fixed when a session starts or ends.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace anvil_script {

inline constexpr std::string_view kDlcPrefix = "anvil:";
inline constexpr std::size_t kMaxGates = 256;
inline constexpr std::size_t kMaxGateId = 64;

// The plugin id of a has_dlc name, or "" when the name is not Anvil's: it must
// be the prefix and an id as plugin.toml writes one (lowercase letters,
// digits, '-' and '_', at most 64).
std::string_view gate_id(std::string_view name);

class GateTable {
public:
    // The index of `id`, added on first sight, with its answer as of the last
    // update; -1 when the table is full. Any thread.
    int intern(std::string_view id);
    // Lock-free; false for an index the table does not have.
    bool answer(int index) const;
    // A session started or ended: `allowed` is false in multiplayer and
    // without a session; `active` are the ids of the plugins loaded and not
    // suspended now. Main thread, while no parse runs.
    void update(bool allowed, std::vector<std::string> active);

    std::size_t size() const { return count_.load(std::memory_order_acquire); }
    std::string id(int index) const;
    std::uint64_t parsed(int index) const;

private:
    bool answer_for(const std::string& id) const;  // under the lock

    mutable std::mutex lock_;
    std::array<std::string, kMaxGates> ids_{};
    std::atomic<std::uint32_t> count_{0};
    std::array<std::atomic<std::uint8_t>, kMaxGates> answers_{};
    std::array<std::atomic<std::uint64_t>, kMaxGates> parsed_{};
    bool allowed_ = false;
    std::vector<std::string> active_;
};

}  // namespace anvil_script
