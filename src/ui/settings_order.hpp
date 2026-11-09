// The order in which the overlay and the Manager show a plugin's settings
// (design §11): the settings without a group first, then each group once, in
// the order of its first setting in the file; within a group, file order. A
// plugin whose groups are interleaved in settings.toml still gets one header
// per group, and a setting's id stays its position in the file.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace anvil::ui {

// `groups[i]` is the group of the setting at position i ("" for none). The
// positions, in the order to show them.
inline std::vector<std::size_t> settings_display_order(const std::vector<std::string_view>& groups) {
    std::vector<std::size_t> order;
    order.reserve(groups.size());
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].empty()) order.push_back(i);
    }
    for (std::size_t first = 0; first < groups.size(); ++first) {
        const std::string_view group = groups[first];
        if (group.empty()) continue;
        bool seen = false;
        for (std::size_t j = 0; j < first && !seen; ++j) seen = groups[j] == group;
        if (seen) continue;
        for (std::size_t i = first; i < groups.size(); ++i) {
            if (groups[i] == group) order.push_back(i);
        }
    }
    return order;
}

}  // namespace anvil::ui
