// The windows of plugins (anvil-overlay.panel, include/anvil/anvil_panel.h): what
// plugins hand over, kept for the frame that draws it. No ImGui here.
#pragma once

#include <anvil/anvil_panel.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// What a plugin holds is this, by another name: the header's type stays opaque.
struct AnvilPanel {
    struct Row {
        std::uint32_t kind = 0;
        std::string label;
        std::string value;
        bool has_value = false;
        std::uint64_t id = 0;
    };
    std::uint32_t index = 0;
    std::string title;
    std::vector<Row> rows;
    bool shown = false;
    AnvilPanelClick click = nullptr;
    void* context = nullptr;
};

namespace overlay {

inline constexpr std::uint32_t kMaxPanels = 64;
inline constexpr std::uint32_t kMaxRows = 1024;
inline constexpr std::size_t kMaxText = 4096;

// Whether a text holds a character of Chinese, Japanese or Korean, which the
// overlay's Latin font does not have.
bool has_cjk(std::string_view utf8);

class PanelStore {
public:
    AnvilResult create(const char* title, AnvilPanel** out);
    AnvilResult set_rows(AnvilPanel* panel, const AnvilPanelRow* rows, std::uint32_t count, std::uint32_t row_size);
    AnvilResult on_click(AnvilPanel* panel, AnvilPanelClick click, void* context);
    AnvilResult show(AnvilPanel* panel, bool shown);
    bool shown(const AnvilPanel* panel) const;

    // Any panel is shown: a frame is needed.
    bool any_shown() const { return shown_count_.load(std::memory_order_acquire) != 0; }
    // Some text so far had a character the system fonts must draw.
    bool wants_cjk() const { return wants_cjk_.load(std::memory_order_acquire); }

    // The frame's way in: `visit` gets every panel, under the store's lock.
    template <class Visit>
    void each(Visit&& visit) {
        std::lock_guard lock(mutex_);
        for (AnvilPanel& p : panels_) visit(p);
    }
    // A panel the frame closed or showed, under `each`.
    void set_shown_locked(AnvilPanel& panel, bool shown);

    // A click's handler, read at the moment it runs.
    void deliver(const AnvilPanel* panel, std::uint64_t id) const;

private:
    bool known(const AnvilPanel* panel) const;

    mutable std::mutex mutex_;
    std::deque<AnvilPanel> panels_;  // a deque: nothing moves, and a panel lives for the process
    std::atomic<std::uint32_t> shown_count_{0};
    std::atomic<bool> wants_cjk_{false};
};

}  // namespace overlay
