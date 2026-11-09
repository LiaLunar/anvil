// PROTOTYPE (eu4-engine-lab#11), throwaway. The panel's rows for each of the
// three variants, as anvil-overlay.panel v1 draws them: text (label, value),
// button, separator. No Anvil here, so the dump tool prints the same rows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "model.hpp"

namespace script_errors {

enum RowKind : std::uint32_t { kText = 1, kButton = 2, kSeparator = 3 };

struct Row {
    std::uint32_t kind = kText;
    std::string label;
    std::string value;
    bool has_value = false;
    std::uint64_t id = 0;
};

enum class Action : std::uint8_t { none, next, mark, select, toggle, open, copy, find, open_hit, new_only };

inline std::uint64_t make_id(Action a, std::uint32_t first, std::uint32_t second = 0) {
    return (std::uint64_t(a) << 56) | (std::uint64_t(second & 0xFFFFFF) << 32) | first;
}
inline Action action_of(std::uint64_t id) { return static_cast<Action>(id >> 56); }
inline std::uint32_t first_of(std::uint64_t id) { return static_cast<std::uint32_t>(id); }
inline std::uint32_t second_of(std::uint64_t id) { return static_cast<std::uint32_t>((id >> 32) & 0xFFFFFF); }

inline constexpr int kVariants = 3;
const char* variant_name(int variant);

struct View {
    int variant = 0;
    std::size_t selected = static_cast<std::size_t>(-1);
    std::set<std::uint32_t> open;  // expanded groups
    bool new_only = false;
    std::string source;  // what the panel reads
    std::string note;    // the last action's outcome
};

// Applies a click that only changes the view; false for one with an effect
// outside it (open, copy, find), which the caller does.
bool apply_view_click(View& view, Log& log, std::uint64_t id);

std::vector<Row> build_rows(const Log& log, const View& view);

}  // namespace script_errors
