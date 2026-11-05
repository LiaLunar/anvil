#include "panels.hpp"

#include <cstring>

namespace overlay {

namespace {

// The first scripts past Latin, Greek and Cyrillic that Segoe UI has: Hangul
// Jamo, then the CJK blocks up to the end of Hangul, the compatibility forms,
// the fullwidth forms, and the supplementary ideographs.
bool cjk_code_point(std::uint32_t c) {
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

// ImGui ends a label at "##" and reads the rest as an id.
std::string shown_label(std::string text) {
    for (std::size_t at = text.find("##"); at != std::string::npos; at = text.find("##", at + 2)) text.insert(at + 1, " ");
    return text;
}

}  // namespace

bool has_cjk(std::string_view utf8) {
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t length = 1;
        std::uint32_t code = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2, code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, code = lead & 0x07;
        }
        if (i + length > utf8.size()) return false;
        for (std::size_t k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        if (cjk_code_point(code)) return true;
        i += length;
    }
    return false;
}

bool PanelStore::known(const AnvilPanel* panel) const {
    for (const AnvilPanel& p : panels_) {
        if (&p == panel) return true;
    }
    return false;
}

AnvilResult PanelStore::create(const char* title, AnvilPanel** out) {
    if (title == nullptr || *title == '\0' || out == nullptr || std::strlen(title) > kMaxText) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (panels_.size() >= kMaxPanels) return ANVIL_ERROR_OUT_OF_MEMORY;
    AnvilPanel& panel = panels_.emplace_back();
    panel.index = static_cast<std::uint32_t>(panels_.size() - 1);
    panel.title = shown_label(title);
    if (has_cjk(panel.title)) wants_cjk_.store(true, std::memory_order_release);
    *out = &panel;
    return ANVIL_OK;
}

AnvilResult PanelStore::set_rows(AnvilPanel* panel, const AnvilPanelRow* rows, std::uint32_t count, std::uint32_t row_size) {
    if (panel == nullptr || (count != 0 && rows == nullptr) || count > kMaxRows ||
        row_size < offsetof(AnvilPanelRow, id) + sizeof(std::uint64_t)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::vector<AnvilPanel::Row> copy;
    copy.reserve(count);
    bool cjk = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* row = reinterpret_cast<const AnvilPanelRow*>(reinterpret_cast<const char*>(rows) + i * std::size_t{row_size});
        if (row->kind < ANVIL_PANEL_TEXT || row->kind > ANVIL_PANEL_SEPARATOR) return ANVIL_ERROR_INVALID_ARGUMENT;
        if (row->kind != ANVIL_PANEL_SEPARATOR && row->label == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
        AnvilPanel::Row& to = copy.emplace_back();
        to.kind = row->kind;
        to.id = row->id;
        if (row->kind == ANVIL_PANEL_SEPARATOR) continue;
        const std::size_t label = strnlen_s(row->label, kMaxText + 1);
        const std::size_t value = row->value != nullptr ? strnlen_s(row->value, kMaxText + 1) : 0;
        if (label > kMaxText || value > kMaxText) return ANVIL_ERROR_INVALID_ARGUMENT;
        to.label.assign(row->label, label);
        if (row->kind == ANVIL_PANEL_BUTTON) to.label = shown_label(std::move(to.label));
        if (row->value != nullptr) {
            to.value.assign(row->value, value);
            to.has_value = true;
        }
        cjk = cjk || has_cjk(to.label) || has_cjk(to.value);
    }
    std::lock_guard lock(mutex_);
    if (!known(panel)) return ANVIL_ERROR_INVALID_ARGUMENT;
    panel->rows = std::move(copy);
    if (cjk) wants_cjk_.store(true, std::memory_order_release);
    return ANVIL_OK;
}

AnvilResult PanelStore::on_click(AnvilPanel* panel, AnvilPanelClick click, void* context) {
    std::lock_guard lock(mutex_);
    if (!known(panel)) return ANVIL_ERROR_INVALID_ARGUMENT;
    panel->click = click;
    panel->context = context;
    return ANVIL_OK;
}

void PanelStore::set_shown_locked(AnvilPanel& panel, bool shown) {
    if (panel.shown == shown) return;
    panel.shown = shown;
    if (shown) {
        shown_count_.fetch_add(1, std::memory_order_acq_rel);
    } else {
        shown_count_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

AnvilResult PanelStore::show(AnvilPanel* panel, bool shown) {
    std::lock_guard lock(mutex_);
    if (!known(panel)) return ANVIL_ERROR_INVALID_ARGUMENT;
    set_shown_locked(*panel, shown);
    return ANVIL_OK;
}

bool PanelStore::shown(const AnvilPanel* panel) const {
    std::lock_guard lock(mutex_);
    return known(panel) && panel->shown;
}

void PanelStore::deliver(const AnvilPanel* panel, std::uint64_t id) const {
    AnvilPanelClick click = nullptr;
    void* context = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!known(panel)) return;
        click = panel->click;
        context = panel->context;
    }
    if (click != nullptr) click(context, id);
}

}  // namespace overlay
