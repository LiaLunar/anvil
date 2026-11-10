// anvil-overlay.input: the bindings of plugins and the keys held down, kept
// apart from the window procedure (plugin.cpp) so they can be tested without
// a window. Every member may be called from any thread.
#pragma once

#include <anvil/anvil_input.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

struct AnvilInputBinding {
    std::int64_t code = 0;
    std::uint32_t flags = 0;
    AnvilInputHandler handler = nullptr;
    void* context = nullptr;
    bool live = true;
};

namespace overlay {

// A key or button going down, as the window procedure saw it.
struct InputPress {
    unsigned vk = 0;
    bool shift = false, ctrl = false, alt = false;
    bool repeat = false;
    int x = 0, y = 0;
};

// A press that matched a binding, to deliver on the main thread.
struct InputMatch {
    AnvilInputBinding* binding = nullptr;
    bool consume = false;  // the game does not see the press
    bool deliver = true;   // the handler gets it (not a repeat it does not want)
    AnvilInputEvent event{};
};

class InputRouter {
public:
    static constexpr std::size_t kMaxBindings = 256;

    // A code of anvil_input.h, 0 included.
    static bool valid_code(std::int64_t code);
    static bool is_mouse(unsigned vk);

    // `reserved` is a code no binding may take (the panel key), 0 for none.
    AnvilResult bind(std::int64_t code, std::uint32_t flags, AnvilInputHandler handler, void* context,
                     std::int64_t reserved, AnvilInputBinding** out);
    AnvilResult set_code(AnvilInputBinding* binding, std::int64_t code, std::int64_t reserved);
    AnvilResult unbind(AnvilInputBinding* binding);

    // The binding a press matches, unless `state` (ANVIL_INPUT_* flags) holds
    // it back or there is none; a repeat the binding does not want only when
    // it consumes, so the game does not see it either.
    std::optional<InputMatch> press(const InputPress& press, std::uint32_t state, std::uint64_t frame) const;
    // The handler of a queued match while its binding lives.
    bool handler_of(const InputMatch& match, AnvilInputHandler& handler, void*& context) const;

    // Keys and buttons held down, from the window's messages.
    void key(unsigned vk, bool down);
    void release_all();
    bool down(unsigned vk) const;
    // held() of the service; the modifiers count when the window's messages
    // or the caller (GetKeyState) say they are down.
    int held(std::int64_t code, std::uint32_t state, bool shift, bool ctrl, bool alt) const;

private:
    // The live binding holding `code`, other than `self`. Under mutex_.
    const AnvilInputBinding* holder(std::int64_t code, const AnvilInputBinding* self) const;
    bool known(const AnvilInputBinding* binding) const;  // under mutex_

    mutable std::mutex mutex_;
    std::deque<AnvilInputBinding> bindings_;  // addresses stay valid until the process ends
    std::array<std::atomic<bool>, 256> down_{};
};

}  // namespace overlay
