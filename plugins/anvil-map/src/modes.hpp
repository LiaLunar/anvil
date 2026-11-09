// anvil-map: the registered plugin modes, their borrowed numbers, colours and
// tooltip texts, without the game (docs/spec/map-modes-v1.md).
#pragma once

#include <anvil/anvil_map.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace anvil_map {

// The numbers of the game's map mode enum a plugin mode may borrow, in the
// order they are lent (ADR 0007).
inline constexpr std::array<std::int32_t, 13> kPool = {80, 81, 82, 83, 50, 57, 58, 59, 60, 66, 68, 71, 84};
// The game's enum has 85 values (its switches compare with 0x54).
inline constexpr std::int32_t kModeLimit = 85;
inline constexpr std::uint32_t kTabs = 4;
inline constexpr std::size_t kLabelMax = 64;
inline constexpr std::size_t kDescriptionMax = 512;
inline constexpr std::size_t kNameMax = 32;
inline constexpr std::size_t kSpriteMax = 128;

// "<plugin-id>.<name>": a plugin id ([a-z0-9-]+), '.', a name of
// [a-z0-9_-]{1,32}.
bool valid_id(std::string_view id);

// A colour buffer as the paint copies it.
struct Colors {
    std::vector<std::uint32_t> values;  // values[i] for province ID i
    bool complete = false;              // no entry is ANVIL_MAP_NO_COLOR and it covers the province limit
};

}  // namespace anvil_map

// The handle a plugin holds (opaque in anvil_map.h). Never freed.
struct AnvilMapMode {
    std::string id;
    std::string label;        // as the game gets it (text.hpp)
    std::string description;  // as the game gets it
    std::string sprite;       // empty: the generic one
    std::uint32_t tab = 0;    // 1..4
    std::int32_t number = 0;  // the borrowed number
    AnvilMapDescribe describe = nullptr;
    void* context = nullptr;

    // Colours: any thread hands over, the paint takes the last full copy.
    std::mutex colors_mutex;
    std::shared_ptr<const anvil_map::Colors> colors;
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t dirtied = 0;  // main thread: the generation last marked dirty

    // The tooltip text kept for one province (main thread, but the tooltip
    // may be built from a frame nested in a step's handler).
    std::mutex text_mutex;
    std::int32_t text_province = 0;
    std::string text;

    std::shared_ptr<const anvil_map::Colors> snapshot() {
        const std::lock_guard lock(colors_mutex);
        return colors;
    }
};

namespace anvil_map {

class Registry {
public:
    // Registers a mode (docs: register_mode). `utf8_game` says how its texts
    // are written. On ANVIL_OK *out is the new mode; `why` says what was
    // wrong otherwise.
    AnvilResult add(const AnvilMapModeInfo* info, bool utf8_game, const AnvilMapMode** out, std::string& why);

    // The mode that borrowed `number`, or null; lock-free, for the hooks.
    AnvilMapMode* by_number(std::int32_t number) const noexcept {
        if (number < 0 || number >= kModeLimit) return nullptr;
        return numbers_[static_cast<std::size_t>(number)].load(std::memory_order_acquire);
    }

    // The numbers of the modes of tab 1..4 in the order they registered.
    // Writes at most `capacity`; returns how many it wrote.
    std::size_t tab_numbers(std::uint32_t tab, std::int32_t* out, std::size_t capacity) const;

    std::size_t size() const;

    // Every mode, in the order of registration.
    std::vector<AnvilMapMode*> all() const;

    // Drops every colour buffer and kept text (the end of a session).
    void clear_session();

private:
    mutable std::mutex mutex_;
    std::vector<AnvilMapMode*> modes_;
    std::array<std::atomic<AnvilMapMode*>, kModeLimit> numbers_{};
};

// Checks a buffer for set_colors and copies it. `limit` is the session's
// province limit (0 when it is not known). ANVIL_OK fills `out`.
AnvilResult make_colors(const std::uint32_t* colors, std::uint32_t count, std::int32_t limit, Colors& out);

// Hands `colors` to `mode` and counts a new generation.
void set_colors(AnvilMapMode& mode, std::shared_ptr<const Colors> colors);

// The kept tooltip text of `mode` for `province`, empty when it has none.
std::string kept_text(AnvilMapMode& mode, std::int32_t province);
void keep_text(AnvilMapMode& mode, std::int32_t province, std::string text);

}  // namespace anvil_map
