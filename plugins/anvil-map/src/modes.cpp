#include "modes.hpp"

#include "text.hpp"

#include <cstddef>
#include <cstring>

namespace anvil_map {

namespace {

bool plugin_id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }
bool name_char(char c) { return plugin_id_char(c) || c == '_'; }
bool sprite_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// A C string of at most `max` bytes (and one more to see that it is longer).
bool bounded(const char* text, std::size_t max, std::string_view& out) {
    const void* end = std::memchr(text, 0, max + 1);
    if (end == nullptr) return false;
    out = std::string_view(text, static_cast<std::size_t>(static_cast<const char*>(end) - text));
    return true;
}

}  // namespace

bool valid_id(std::string_view id) {
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot > 64) return false;
    const std::string_view plugin = id.substr(0, dot), name = id.substr(dot + 1);
    if (name.empty() || name.size() > kNameMax) return false;
    for (const char c : plugin) {
        if (!plugin_id_char(c)) return false;
    }
    for (const char c : name) {
        if (!name_char(c)) return false;
    }
    return true;
}

AnvilResult Registry::add(const AnvilMapModeInfo* info, bool utf8_game, const AnvilMapMode** out, std::string& why) {
    if (info == nullptr || out == nullptr) {
        why = "info or out is NULL";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (info->size < offsetof(AnvilMapModeInfo, context) + sizeof(info->context)) {
        why = "AnvilMapModeInfo.size is too small";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::string_view id, label, description, sprite;
    if (info->id == nullptr || !bounded(info->id, 64 + 1 + kNameMax, id) || !valid_id(id)) {
        why = "the id is not <plugin-id>.<name> with a name of [a-z0-9_-]{1,32}";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (info->label == nullptr || !bounded(info->label, kLabelMax, label) || label.empty() || !valid_utf8(label)) {
        why = "the label is not UTF-8 of 1 to 64 bytes";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (info->description != nullptr &&
        (!bounded(info->description, kDescriptionMax, description) || !valid_utf8(description))) {
        why = "the description is not UTF-8 of at most 512 bytes";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (info->sprite != nullptr) {
        if (!bounded(info->sprite, kSpriteMax, sprite) || sprite.empty()) {
            why = "the sprite name is empty or longer than 128 bytes";
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
        for (const char c : sprite) {
            if (!sprite_char(c)) {
                why = "the sprite name has a character other than [A-Za-z0-9_]";
                return ANVIL_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (info->tab > kTabs) {
        why = "the tab is not 0 to 4";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }

    const std::lock_guard lock(mutex_);
    for (const AnvilMapMode* m : modes_) {
        if (m->id == id) {
            why = "the id is already registered";
            return ANVIL_ERROR_ALREADY_EXISTS;
        }
    }
    if (modes_.size() >= kPool.size()) {
        why = "all 13 numbers of the pool are lent";
        return ANVIL_ERROR_UNSUPPORTED;
    }
    auto* mode = new AnvilMapMode;
    mode->id = std::string(id);
    mode->label = game_text(label, utf8_game);
    mode->description = game_text(description, utf8_game);
    mode->sprite = std::string(sprite);
    mode->tab = info->tab == ANVIL_MAP_TAB_DEFAULT ? kTabs : info->tab;
    mode->number = kPool[modes_.size()];
    mode->describe = info->describe;
    mode->context = info->context;
    modes_.push_back(mode);
    numbers_[static_cast<std::size_t>(mode->number)].store(mode, std::memory_order_release);
    *out = mode;
    return ANVIL_OK;
}

std::size_t Registry::tab_numbers(std::uint32_t tab, std::int32_t* out, std::size_t capacity) const {
    const std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const AnvilMapMode* m : modes_) {
        if (m->tab == tab && n < capacity) out[n++] = m->number;
    }
    return n;
}

std::size_t Registry::size() const {
    const std::lock_guard lock(mutex_);
    return modes_.size();
}

std::vector<AnvilMapMode*> Registry::all() const {
    const std::lock_guard lock(mutex_);
    return modes_;
}

void Registry::clear_session() {
    for (AnvilMapMode* m : all()) {
        set_colors(*m, nullptr);
        keep_text(*m, 0, {});
    }
}

AnvilResult make_colors(const std::uint32_t* colors, std::uint32_t count, std::int32_t limit, Colors& out) {
    if (colors == nullptr && count != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (limit > 0 && count > static_cast<std::uint32_t>(limit)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (limit <= 0 && count > (1u << 20)) return ANVIL_ERROR_INVALID_ARGUMENT;
    out.values.assign(colors, colors + count);
    bool complete = limit > 0 && count == static_cast<std::uint32_t>(limit);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t top = out.values[i] >> 24;
        if (top == 0xFF) {
            out.values[i] = ANVIL_MAP_NO_COLOR;
            // Province 0 is the game's empty entry: it counts for nothing.
            if (i != 0) complete = false;
        } else if (top != 0) {
            return ANVIL_ERROR_INVALID_ARGUMENT;
        }
    }
    if (count > 0) out.values[0] = ANVIL_MAP_NO_COLOR;
    out.complete = complete;
    return ANVIL_OK;
}

void set_colors(AnvilMapMode& mode, std::shared_ptr<const Colors> colors) {
    {
        const std::lock_guard lock(mode.colors_mutex);
        mode.colors = std::move(colors);
    }
    mode.generation.fetch_add(1, std::memory_order_acq_rel);
}

std::string kept_text(AnvilMapMode& mode, std::int32_t province) {
    const std::lock_guard lock(mode.text_mutex);
    return mode.text_province == province ? mode.text : std::string();
}

void keep_text(AnvilMapMode& mode, std::int32_t province, std::string text) {
    const std::lock_guard lock(mode.text_mutex);
    mode.text_province = province;
    mode.text = std::move(text);
}

}  // namespace anvil_map
