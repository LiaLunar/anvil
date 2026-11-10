#include "input.hpp"

namespace overlay {

namespace {

constexpr std::int64_t kModifiers = ANVIL_INPUT_SHIFT | ANVIL_INPUT_CTRL | ANVIL_INPUT_ALT;
constexpr std::uint32_t kBindFlags = ANVIL_INPUT_CONSUME | ANVIL_INPUT_REPEAT;

bool is_modifier_key(unsigned vk) {
    switch (vk) {
    case 0x10: case 0x11: case 0x12:             // VK_SHIFT, VK_CONTROL, VK_MENU
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:  // left and right Shift, Ctrl
    case 0xA4: case 0xA5:                        // left and right Alt
    case 0x5B: case 0x5C:                        // the Windows keys
        return true;
    default:
        return false;
    }
}

std::int64_t code_of(const InputPress& p) {
    return static_cast<std::int64_t>(p.vk) | (p.shift ? ANVIL_INPUT_SHIFT : 0) | (p.ctrl ? ANVIL_INPUT_CTRL : 0) |
           (p.alt ? ANVIL_INPUT_ALT : 0);
}

}  // namespace

bool InputRouter::valid_code(std::int64_t code) {
    if (code == 0) return true;
    if (code < 0 || (code & ~(kModifiers | 0xFF)) != 0) return false;
    const auto vk = static_cast<unsigned>(code & 0xFF);
    return vk != 0 && !is_modifier_key(vk);
}

bool InputRouter::is_mouse(unsigned vk) { return vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06; }

const AnvilInputBinding* InputRouter::holder(std::int64_t code, const AnvilInputBinding* self) const {
    if (code == 0) return nullptr;
    for (const auto& b : bindings_) {
        if (&b != self && b.live && b.code == code) return &b;
    }
    return nullptr;
}

bool InputRouter::known(const AnvilInputBinding* binding) const {
    for (const auto& b : bindings_) {
        if (&b == binding) return true;
    }
    return false;
}

AnvilResult InputRouter::bind(std::int64_t code, std::uint32_t flags, AnvilInputHandler handler, void* context,
                              std::int64_t reserved, AnvilInputBinding** out) {
    if (out == nullptr || handler == nullptr || (flags & ~kBindFlags) != 0 || !valid_code(code)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    if (bindings_.size() >= kMaxBindings) return ANVIL_ERROR_OUT_OF_MEMORY;
    const bool taken = code != 0 && (code == reserved || holder(code, nullptr) != nullptr);
    AnvilInputBinding& b = bindings_.emplace_back();
    b.code = taken ? 0 : code;
    b.flags = flags;
    b.handler = handler;
    b.context = context;
    *out = &b;
    return taken ? ANVIL_ERROR_CONFLICT : ANVIL_OK;
}

AnvilResult InputRouter::set_code(AnvilInputBinding* binding, std::int64_t code, std::int64_t reserved) {
    if (binding == nullptr || !valid_code(code)) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (!known(binding)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!binding->live) return ANVIL_ERROR_NOT_FOUND;
    if (code != 0 && (code == reserved || holder(code, binding) != nullptr)) {
        binding->code = 0;
        return ANVIL_ERROR_CONFLICT;
    }
    binding->code = code;
    return ANVIL_OK;
}

AnvilResult InputRouter::unbind(AnvilInputBinding* binding) {
    if (binding == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (!known(binding)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!binding->live) return ANVIL_ERROR_NOT_FOUND;
    binding->live = false;
    binding->code = 0;
    return ANVIL_OK;
}

std::optional<InputMatch> InputRouter::press(const InputPress& p, std::uint32_t state, std::uint64_t frame) const {
    if (p.vk == 0 || p.vk > 0xFF || is_modifier_key(p.vk)) return std::nullopt;
    if ((state & (is_mouse(p.vk) ? ANVIL_INPUT_HOLD_MOUSE : ANVIL_INPUT_HOLD_KEYS)) != 0) return std::nullopt;
    const std::int64_t code = code_of(p);
    std::lock_guard lock(mutex_);
    const AnvilInputBinding* b = holder(code, nullptr);
    if (b == nullptr) return std::nullopt;
    InputMatch m;
    m.binding = const_cast<AnvilInputBinding*>(b);
    m.consume = (b->flags & ANVIL_INPUT_CONSUME) != 0;
    // A repeat the binding does not want is still hidden from the game when
    // the press was.
    m.deliver = !p.repeat || (b->flags & ANVIL_INPUT_REPEAT) != 0;
    if (!m.deliver && !m.consume) return std::nullopt;
    m.event.size = sizeof(AnvilInputEvent);
    m.event.flags = p.repeat ? ANVIL_INPUT_EVENT_REPEAT : 0;
    m.event.code = code;
    m.event.x = p.x;
    m.event.y = p.y;
    m.event.frame = frame;
    return m;
}

bool InputRouter::handler_of(const InputMatch& match, AnvilInputHandler& handler, void*& context) const {
    std::lock_guard lock(mutex_);
    if (match.binding == nullptr || !match.binding->live) return false;
    handler = match.binding->handler;
    context = match.binding->context;
    return true;
}

void InputRouter::key(unsigned vk, bool down) {
    if (vk < down_.size()) down_[vk].store(down, std::memory_order_relaxed);
}

void InputRouter::release_all() {
    for (auto& d : down_) d.store(false, std::memory_order_relaxed);
}

bool InputRouter::down(unsigned vk) const { return vk < down_.size() && down_[vk].load(std::memory_order_relaxed); }

int InputRouter::held(std::int64_t code, std::uint32_t state, bool shift, bool ctrl, bool alt) const {
    if (code == 0 || !valid_code(code)) return 0;
    const auto vk = static_cast<unsigned>(code & 0xFF);
    if ((state & (is_mouse(vk) ? ANVIL_INPUT_HOLD_MOUSE : ANVIL_INPUT_HOLD_KEYS)) != 0 || !down(vk)) return 0;
    if ((code & ANVIL_INPUT_SHIFT) != 0 && !(shift || down(0x10))) return 0;
    if ((code & ANVIL_INPUT_CTRL) != 0 && !(ctrl || down(0x11))) return 0;
    if ((code & ANVIL_INPUT_ALT) != 0 && !(alt || down(0x12))) return 0;
    return 1;
}

}  // namespace overlay
