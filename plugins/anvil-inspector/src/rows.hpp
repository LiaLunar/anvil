// The window's rows from a snapshot, within the limits of anvil-overlay.panel
// v1 (1024 rows, each text at most 4096 bytes): headings that fold, the items
// of a list as far as kItemsShown, and a Refresh button. No filter and no
// choice of a tag in v1 (eu4-engine-lab#65).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "snapshot.hpp"
#include "texts.hpp"

namespace anvil_inspector {

inline constexpr std::uint32_t kText = 1;       // ANVIL_PANEL_TEXT
inline constexpr std::uint32_t kButton = 2;     // ANVIL_PANEL_BUTTON
inline constexpr std::uint32_t kSeparator = 3;  // ANVIL_PANEL_SEPARATOR
inline constexpr std::size_t kMaxRows = 1024;
inline constexpr std::size_t kItemsShown = 60;  // per list; the rest is "N more"

struct Row {
    std::uint32_t kind = kText;
    std::string label;
    std::string value;
    bool has_value = false;
    std::uint64_t id = 0;
};

// A heading: a role and one of its parts, or the role itself.
enum class Part : std::uint32_t { all = 0, flags = 1, variables = 2, modifiers = 3, monarchs = 4 };

// What the player folded; everything starts open.
struct View {
    std::set<std::uint32_t> closed;  // heading keys
};

enum class Action : std::uint32_t { none = 0, toggle = 1, refresh = 2 };

std::uint64_t make_id(Action action, std::uint32_t key);
Action action_of(std::uint64_t id);
std::uint32_t heading_key(Role role, Part part);

// A click on a heading folds or opens it: true when the view changed.
bool apply_click(View& view, std::uint64_t id);

std::vector<Row> rows(const Snapshot& snapshot, const View& view, const Texts& t);

// A value x 1000 as the game's scripts write it: "2.5", "-1", "0.001".
std::string milli_text(std::int32_t value);
// A date as the game's scripts write it: "1444.11.11".
std::string date_text(const AnvilGameDate& date);

}  // namespace anvil_inspector
