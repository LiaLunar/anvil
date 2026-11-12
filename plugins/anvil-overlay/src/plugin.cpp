// anvil-overlay: the standard plugin of design §15. Dear ImGui on the game's
// Direct3D 9 device, for notifications, the badge in the main menu and the
// panel.
//
// Getting the device (no symbols, so it works on every build):
//   1. at the entry point it hooks Direct3DCreate9 and Direct3DCreate9Ex in
//      d3d9.dll with hook_function_at, before the game creates its renderer;
//   2. on the object they return it hooks CreateDevice (and CreateDeviceEx)
//      in d3d9.dll with hook_function_at; there it subclasses the game window
//      for input before the device is made;
//   3. on the device it hooks EndScene, Reset (and ResetEx), chained with any
//      other plugin's Direct3D 9 hooks.
// On the OpenGL backend none of this is called and the overlay stays off.
//
// With no notice shown, the panel closed and the badge not shown (it is only
// in the main menu), the EndScene hook only passes on.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_input.h>

#include "i18n/cjk_font.hpp"
#include "input.hpp"
#include "notices.hpp"
#include "overlay.hpp"
#include "settings.gen.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace overlay {

Shared& shared() {
    static Shared s;
    return s;
}

void log_info(const char* text) { anvil::Host{}.info("{}", text); }
void log_warning(const char* text) { anvil::Host{}.warn("{}", text); }

}  // namespace overlay

namespace {

using overlay::shared;

// Vtable slots (d3d9.h declaration order).
constexpr size_t kCreateDevice = 16;    // IDirect3D9
constexpr size_t kCreateDeviceEx = 20;  // IDirect3D9Ex
constexpr size_t kReset = 16;           // IDirect3DDevice9
constexpr size_t kEndScene = 42;
constexpr size_t kResetEx = 132;  // IDirect3DDevice9Ex

using Create9Fn = IDirect3D9* WINAPI(UINT);
using Create9ExFn = HRESULT WINAPI(UINT, IDirect3D9Ex**);
using CreateDeviceFn = HRESULT STDMETHODCALLTYPE(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                  IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT STDMETHODCALLTYPE(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using EndSceneFn = HRESULT STDMETHODCALLTYPE(IDirect3DDevice9*);
using ResetFn = HRESULT STDMETHODCALLTYPE(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetExFn = HRESULT STDMETHODCALLTYPE(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using KeyStateFn = SHORT WINAPI(int);

// A function hook placed once, at the first object's method; another object
// of the same class shares it. An object whose method lies elsewhere (another
// class) is not followed.
template <class Sig> struct MethodHook {
    anvil::FunctionHook<Sig> hook;
    void* at = nullptr;
};

anvil::FunctionHook<Create9Fn> g_create9;
anvil::FunctionHook<Create9ExFn> g_create9ex;
MethodHook<CreateDeviceFn> g_create_device;
MethodHook<CreateDeviceExFn> g_create_device_ex;
MethodHook<EndSceneFn> g_end_scene;
MethodHook<ResetFn> g_reset;
MethodHook<ResetExFn> g_reset_ex;
anvil::FunctionHook<KeyStateFn> g_async_key_state;
anvil::FunctionHook<KeyStateFn> g_key_state;

std::mutex g_hook_mutex;  // the hooks placed at run time
std::atomic<bool> g_device_seen{false};

void* slot(void* object, size_t index) { return (*static_cast<void***>(object))[index]; }

template <class Sig, class D> bool hook_at(MethodHook<Sig>& method, void* address, D detour, const char* what) {
    if (method.hook.active()) {
        if (method.at == address) return true;
        anvil::Host{}.warn("{} of another class at {} (hooked: {}); not followed", what, address, method.at);
        return false;
    }
    try {
        anvil::Host{}.hook_at(method.hook, address, detour);
        method.at = address;
        return true;
    } catch (const std::exception& e) {
        anvil::Host{}.warn("cannot hook {} at {}: {}", what, address, e.what());
        return false;
    }
}

// ---- input --------------------------------------------------------------------

struct Subclassed {
    HWND window;
    WNDPROC original;
};
Subclassed g_windows[4];
std::atomic<int> g_window_count{0};

WNDPROC original_of(HWND window) {
    const int count = g_window_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (g_windows[i].window == window) return g_windows[i].original;
    }
    return nullptr;
}

bool held(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

bool is_mouse_message(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool is_button_up(UINT m) {
    return m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP || m == WM_XBUTTONUP;
}
bool is_key_message(UINT m) {
    return m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR ||
           m == WM_SYSCHAR || m == WM_DEADCHAR || m == WM_UNICHAR;
}

// ---- anvil-overlay.input -------------------------------------------------------

overlay::InputRouter g_input;
std::mutex g_input_mutex;
std::vector<overlay::InputMatch> g_input_queue;  // presses waiting for the main thread

bool modifier_held(int vk) { return g_input.down(static_cast<unsigned>(vk)) || held(vk); }

std::uint32_t input_state() {
    const overlay::Shared& s = shared();
    std::uint32_t flags = 0;
    if (s.panel_open.load(std::memory_order_acquire)) flags |= ANVIL_INPUT_OVERLAY_PANEL;
    if (s.visible.load(std::memory_order_acquire)) {
        if (s.want_keyboard.load(std::memory_order_acquire) || s.want_text.load(std::memory_order_acquire)) {
            flags |= ANVIL_INPUT_OVERLAY_KEYBOARD;
        }
        if (s.want_mouse.load(std::memory_order_acquire)) flags |= ANVIL_INPUT_OVERLAY_MOUSE;
    }
    const AnvilGameService* game = s.game_work;
    std::uint32_t keyboard = 0;
    if (game == nullptr || !ANVIL_GAME_HAS(game, keyboard) || game->keyboard(&keyboard) == 0) {
        flags |= ANVIL_INPUT_GAME_UNKNOWN;
    } else {
        if ((keyboard & ANVIL_GAME_KEYBOARD_TEXT) != 0) flags |= ANVIL_INPUT_GAME_TEXT;
        if ((keyboard & ANVIL_GAME_KEYBOARD_SHORTCUT) != 0) flags |= ANVIL_INPUT_GAME_SHORTCUT;
    }
    return flags;
}

// On the main thread after a frame: the handlers of the queued presses.
void deliver_input(void*) {
    std::vector<overlay::InputMatch> queue;
    {
        std::lock_guard lock(g_input_mutex);
        queue.swap(g_input_queue);
    }
    for (const auto& match : queue) {
        AnvilInputHandler handler = nullptr;
        void* context = nullptr;
        if (g_input.handler_of(match, handler, context)) handler(context, &match.event);
    }
}

void queue_input(const overlay::InputMatch& match) {
    const AnvilGameService* game = shared().game_work;
    if (game == nullptr) return;
    try {
        std::lock_guard lock(g_input_mutex);
        const bool first = g_input_queue.empty();
        if (g_input_queue.size() >= 256) return;  // the main thread has not run for a while
        g_input_queue.push_back(match);
        if (first && game->run_on_main_thread(deliver_input, nullptr) != ANVIL_OK) g_input_queue.clear();
    } catch (const std::bad_alloc&) {
    }
}

unsigned button_of(UINT m, WPARAM wparam) {
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    default: return 0;
    }
}
bool is_button_down(UINT m) {
    return m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDOWN || m == WM_RBUTTONDBLCLK ||
           m == WM_MBUTTONDOWN || m == WM_MBUTTONDBLCLK || m == WM_XBUTTONDOWN || m == WM_XBUTTONDBLCLK;
}

// The keys and buttons held down, from every message the window gets.
void track_input(UINT message, WPARAM wparam) {
    switch (message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: g_input.key(static_cast<unsigned>(wparam), true); break;
    case WM_KEYUP: case WM_SYSKEYUP: g_input.key(static_cast<unsigned>(wparam), false); break;
    case WM_KILLFOCUS: g_input.release_all(); break;
    case WM_ACTIVATEAPP:
        if (wparam == FALSE) g_input.release_all();
        break;
    default:
        if (const unsigned button = button_of(message, wparam)) g_input.key(button, is_button_down(message));
        break;
    }
}

// A key or button press against the plugins' bindings: true when a binding
// consumes it, so the game does not see it.
bool route_input(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    overlay::InputPress press;
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        press.vk = static_cast<unsigned>(wparam);
        press.repeat = (lparam & (1 << 30)) != 0;
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
            press.x = cursor.x;
            press.y = cursor.y;
        }
        press.shift = modifier_held(VK_SHIFT);
        press.ctrl = modifier_held(VK_CONTROL);
    } else if (is_button_down(message)) {
        press.vk = button_of(message, wparam);
        press.x = GET_X_LPARAM(lparam);
        press.y = GET_Y_LPARAM(lparam);
        press.shift = (wparam & MK_SHIFT) != 0 || g_input.down(VK_SHIFT);
        press.ctrl = (wparam & MK_CONTROL) != 0 || g_input.down(VK_CONTROL);
    } else {
        return false;
    }
    press.alt = modifier_held(VK_MENU);
    const AnvilGameService* game = shared().game_work;
    const auto match = g_input.press(press, input_state(), game != nullptr ? game->frame_number() : 0);
    if (!match) return false;
    if (match->deliver) queue_input(*match);
    return match->consume;
}

std::uint32_t input_service_state() { return input_state(); }
AnvilResult input_bind(std::int64_t code, std::uint32_t flags, AnvilInputHandler handler, void* context,
                       AnvilInputBinding** out) {
    if (shared().game_work == nullptr) return ANVIL_ERROR_UNSUPPORTED;
    try {
        return g_input.bind(code, flags, handler, context, shared().panel_key.load(), out);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}
AnvilResult input_set_code(AnvilInputBinding* binding, std::int64_t code) {
    return g_input.set_code(binding, code, shared().panel_key.load());
}
AnvilResult input_unbind(AnvilInputBinding* binding) { return g_input.unbind(binding); }
int input_held(std::int64_t code) {
    return g_input.held(code, input_state(), held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
}

constexpr AnvilInputService kInputService = {sizeof(AnvilInputService), input_service_state, input_bind,
                                             input_set_code,            input_unbind,        input_held};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC original = original_of(window);
    overlay::Shared& s = shared();
    track_input(message, wparam);
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (key_down) {
        const auto vk = static_cast<unsigned>(wparam);
        const bool repeat = (lparam & (1 << 30)) != 0;
        if (s.recording_key.load(std::memory_order_acquire)) {
            // The settings editor records the next key; Escape cancels.
            const int64_t code = vk == VK_ESCAPE ? 0 : overlay::key_code(vk, held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
            if (vk == VK_ESCAPE || code != 0) {
                s.recorded_key.store(code, std::memory_order_release);
                s.recording_key.store(false, std::memory_order_release);
                s.mark_dirty();
            }
            return 0;
        }
        if (!repeat &&
            overlay::key_matches(s.panel_key.load(std::memory_order_relaxed), vk, held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU))) {
            const bool open = !s.panel_open.load(std::memory_order_acquire);
            s.panel_open.store(open, std::memory_order_release);
            if (open) s.visible.store(true, std::memory_order_release);
            s.mark_dirty();
            return 0;
        }
        if (vk == VK_ESCAPE && s.panel_open.load(std::memory_order_acquire) && !s.want_text.load()) {
            s.panel_open.store(false, std::memory_order_release);
            s.mark_dirty();
            return 0;
        }
        if (vk == 'V' && held(VK_CONTROL) && s.want_text.load()) overlay::ui_capture_clipboard();
    }
    if (route_input(window, message, wparam, lparam)) return 0;
    if (s.visible.load(std::memory_order_acquire)) {
        if (is_mouse_message(message) || is_key_message(message) || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == WM_INPUTLANGCHANGE) {
            std::lock_guard lock(s.mutex);
            overlay::ui_message(window, message, wparam, lparam);
        }
        const bool mouse = s.want_mouse.load(std::memory_order_acquire);
        const bool keyboard = s.want_keyboard.load(std::memory_order_acquire);
        // A button or key release always reaches the game, so it never sees
        // one held down.
        if (mouse && is_mouse_message(message) && !is_button_up(message)) return 0;
        if (keyboard && is_key_message(message) && message != WM_KEYUP && message != WM_SYSKEYUP) return 0;
        if (message == WM_INPUT && (mouse || keyboard)) {
            RAWINPUTHEADER header{};
            UINT size = sizeof(header);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &header, &size, sizeof(header)) ==
                    sizeof(header) &&
                ((header.dwType == RIM_TYPEMOUSE && mouse) || (header.dwType == RIM_TYPEKEYBOARD && keyboard))) {
                return DefWindowProcW(window, message, wparam, lparam);  // frees the input
            }
        }
    }
    return original != nullptr ? CallWindowProcW(original, window, message, wparam, lparam)
                               : DefWindowProcW(window, message, wparam, lparam);
}

void subclass(HWND window) {
    if (window == nullptr || original_of(window) != nullptr) return;
    const int count = g_window_count.load(std::memory_order_acquire);
    if (count >= static_cast<int>(std::size(g_windows))) return;
    const auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
    if (previous == nullptr) {
        anvil::Host{}.warn("cannot subclass the game window (error {}); the panel key does not work", GetLastError());
        return;
    }
    g_windows[count] = {window, previous};
    g_window_count.store(count + 1, std::memory_order_release);
}

// While the panel takes the keyboard or the mouse, the game's own polling of
// keys (eu4.exe imports GetAsyncKeyState and GetKeyState) reads them as up.
bool blocked_key(int vk) {
    const overlay::Shared& s = shared();
    const bool mouse_button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
                              vk == VK_XBUTTON2;
    return mouse_button ? s.want_mouse.load(std::memory_order_relaxed) : s.want_keyboard.load(std::memory_order_relaxed);
}
SHORT WINAPI on_async_key_state(int vk) noexcept { return blocked_key(vk) ? 0 : g_async_key_state.next(vk); }
SHORT WINAPI on_key_state(int vk) noexcept { return blocked_key(vk) ? 0 : g_key_state.next(vk); }

// ---- the device -----------------------------------------------------------------

HRESULT STDMETHODCALLTYPE on_end_scene(IDirect3DDevice9* device) noexcept {
    overlay::Shared& s = shared();
    if ((s.visible.load(std::memory_order_acquire) || s.dirty.load(std::memory_order_acquire)) &&
        device == s.device.load(std::memory_order_acquire)) {
        try {
            {
                std::lock_guard lock(s.mutex);
                overlay::ui_frame(device);
            }
            overlay::ui_flush_clipboard();
        } catch (...) {
            // A failed allocation; the next frame tries again.
        }
    }
    return g_end_scene.hook.next(device);
}

void before_reset(IDirect3DDevice9* device) {
    overlay::Shared& s = shared();
    std::lock_guard lock(s.mutex);
    overlay::ui_invalidate(device);
}

HRESULT STDMETHODCALLTYPE on_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) noexcept {
    before_reset(device);
    return g_reset.hook.next(device, parameters);
}

HRESULT STDMETHODCALLTYPE on_reset_ex(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters,
                                      D3DDISPLAYMODEEX* mode) noexcept {
    before_reset(device);
    return g_reset_ex.hook.next(device, parameters, mode);
}

HWND device_window(HWND focus, const D3DPRESENT_PARAMETERS* parameters) {
    return parameters != nullptr && parameters->hDeviceWindow != nullptr ? parameters->hDeviceWindow : focus;
}

// The window is subclassed before the device is made, so the overlay sits
// under what the device itself puts on the window. DXVK in exclusive
// fullscreen hooks the window inside CreateDevice, keeps the procedure it
// found as its previous one, and when its swap chain goes puts that back only
// if its own procedure is still on top (otherwise it forgets it). Subclassed
// after it, the overlay would keep DXVK's procedure while DXVK's next hook
// keeps the overlay's: a cycle of procedures, and a stack overflow at the
// first message.
void subclass_before_device(HWND focus, const D3DPRESENT_PARAMETERS* parameters) {
    std::lock_guard lock(g_hook_mutex);
    subclass(device_window(focus, parameters));
}

void on_device(IDirect3DDevice9* device, HWND focus, const D3DPRESENT_PARAMETERS* parameters, bool ex) {
    HWND window = device_window(focus, parameters);
    std::lock_guard lock(g_hook_mutex);
    bool ok = hook_at(g_end_scene, slot(device, kEndScene), on_end_scene, "IDirect3DDevice9::EndScene");
    ok = hook_at(g_reset, slot(device, kReset), on_reset, "IDirect3DDevice9::Reset") && ok;
    if (ex) ok = hook_at(g_reset_ex, slot(device, kResetEx), on_reset_ex, "IDirect3DDevice9Ex::ResetEx") && ok;
    if (!ok) return;
    subclass(window);
    overlay::Shared& s = shared();
    s.window.store(window, std::memory_order_release);
    s.device.store(device, std::memory_order_release);
    s.mark_dirty();
    if (!g_device_seen.exchange(true)) {
        anvil::Host{}.info("Direct3D 9{} device found (window {}); the panel opens with the panel key",
                           ex ? "Ex" : "", static_cast<void*>(window));
    }
}

HRESULT STDMETHODCALLTYPE on_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** out) noexcept {
    try {
        subclass_before_device(focus, parameters);
    } catch (...) {
    }
    const HRESULT hr = g_create_device.hook.next(d3d, adapter, type, focus, flags, parameters, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        try {
            on_device(*out, focus, parameters, false);
        } catch (...) {
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE on_create_device_ex(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                              DWORD flags, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* mode,
                                              IDirect3DDevice9Ex** out) noexcept {
    try {
        subclass_before_device(focus, parameters);
    } catch (...) {
    }
    const HRESULT hr = g_create_device_ex.hook.next(d3d, adapter, type, focus, flags, parameters, mode, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        try {
            on_device(*out, focus, parameters, true);
        } catch (...) {
        }
    }
    return hr;
}

void wrap(IDirect3D9* d3d, bool ex) {
    if (d3d == nullptr) return;
    std::lock_guard lock(g_hook_mutex);
    hook_at(g_create_device, slot(d3d, kCreateDevice), on_create_device, "IDirect3D9::CreateDevice");
    if (ex) {
        hook_at(g_create_device_ex, slot(d3d, kCreateDeviceEx), on_create_device_ex, "IDirect3D9Ex::CreateDeviceEx");
    }
}

IDirect3D9* WINAPI on_create9(UINT version) noexcept {
    IDirect3D9* d3d = g_create9.next(version);
    try {
        wrap(d3d, false);
    } catch (...) {
    }
    return d3d;
}

HRESULT WINAPI on_create9ex(UINT version, IDirect3D9Ex** out) noexcept {
    const HRESULT hr = g_create9ex.next(version, out);
    if (SUCCEEDED(hr) && out != nullptr) {
        try {
            wrap(*out, true);
        } catch (...) {
        }
    }
    return hr;
}

// ---- events -----------------------------------------------------------------------

void on_game_ready() {
    overlay::Shared& s = shared();
    s.start_ready.store(true, std::memory_order_release);
    s.session_changed.store(true, std::memory_order_release);
    s.mark_dirty();
    if (!g_device_seen.load()) {
        anvil::Host{}.info("off: the game is ready without a Direct3D 9 device (the OpenGL renderer?)");
    }
}

// ---- anvil-overlay.panel ----------------------------------------------------

AnvilResult panel_create(const char* title, AnvilPanel** out) {
    try {
        return shared().panels.create(title, out);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}
AnvilResult panel_set_rows(AnvilPanel* panel, const AnvilPanelRow* rows, std::uint32_t count, std::uint32_t row_size) {
    try {
        return shared().panels.set_rows(panel, rows, count, row_size);
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}
AnvilResult panel_on_click(AnvilPanel* panel, AnvilPanelClick click, void* context) {
    return shared().panels.on_click(panel, click, context);
}
AnvilResult panel_show(AnvilPanel* panel, int shown) {
    const AnvilResult r = shared().panels.show(panel, shown != 0);
    shared().mark_dirty();
    return r;
}
int panel_shown(const AnvilPanel* panel) { return shared().panels.shown(panel) ? 1 : 0; }
const char* panel_language() { return anvil::i18n::language_code(anvil::i18n::language()); }

constexpr AnvilPanelService kPanelService = {sizeof(AnvilPanelService), panel_create, panel_set_rows, panel_on_click,
                                             panel_show,                panel_shown,  panel_language};

// The player's language: ANVIL_LANGUAGE, else the `language` setting, else
// the core's choice (EU4's settings.txt, or with Mei Steam's or Windows').
void apply_language() {
    overlay::Shared& s = shared();
    anvil::i18n::Language language = s.control.language();
    if (!anvil::i18n::forced_language()) {
        const auto option = static_cast<size_t>(cfg::language.get());
        if (option < std::size(anvil::i18n::kLanguageSettingOptions)) {
            if (const auto chosen = anvil::i18n::language_of_setting(anvil::i18n::kLanguageSettingOptions[option])) {
                language = *chosen;
            }
        }
    }
    anvil::i18n::set_language(language);
    s.mark_dirty();
}

struct AnvilOverlay : anvil::Plugin {
    void load(anvil::Host& host) override {
        overlay::Shared& s = shared();
        const auto* control = host.service<AnvilControlService>(ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION);
        if (control == nullptr || control->size < sizeof(AnvilControlService)) {
            throw anvil::Error(ANVIL_ERROR_UNSUPPORTED,
                               "needs the core service " ANVIL_CONTROL_SERVICE " v1, which this Anvil does not offer");
        }
        s.control = overlay::ControlClient(control);
        apply_language();
        s.panel_key.store(cfg::panel_key.get().code);
        s.menu_badge.store(cfg::menu_badge.get());
        s.notifications.store(cfg::notifications.get());
        s.notification_seconds.store(cfg::notification_seconds.get());
        s.scale.store(cfg::scale.get());
        s.data_dir = host.data_dir();
        s.release_check.store(static_cast<int64_t>(cfg::release_check.get()));
        s.release_check_setting = cfg::release_check.id;
        host.subscribe_setting(cfg::release_check, [](cfg::Types::release_check_value value) {
            shared().release_check.store(static_cast<int64_t>(value));
            shared().release_consent_changed.store(true, std::memory_order_release);
            shared().mark_dirty();
        });
        host.subscribe_setting(cfg::panel_key, [](anvil::Key key) { shared().panel_key.store(key.code); });
        host.subscribe_setting(cfg::menu_badge, [](bool on) {
            shared().menu_badge.store(on);
            shared().mark_dirty();
        });
        host.subscribe_setting(cfg::notifications, [](bool on) {
            shared().notifications.store(on);
            shared().mark_dirty();
        });
        host.subscribe_setting(cfg::notification_seconds, [](std::int64_t seconds) {
            shared().notification_seconds.store(seconds);
            shared().mark_dirty();
        });
        host.subscribe_setting(cfg::language, [](cfg::Types::language_value) { apply_language(); });
        host.subscribe_setting(cfg::scale, [](double scale) {
            shared().scale.store(scale);
            shared().mark_dirty();
        });

        // eu4.exe imports both from d3d9.dll, which is therefore loaded, but its
        // renderer also looks them up with GetProcAddress (the 1.37.5 lab run
        // never called the imports), so the exports themselves are hooked.
        if (HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll")) {
            host.hook_at(g_create9, reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")), on_create9);
            host.hook_at(g_create9ex, reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")), on_create9ex);
        } else {
            host.info("d3d9.dll is not loaded: no Direct3D 9, the overlay stays off");
        }
        host.replace_import(g_async_key_state, "USER32.dll", "GetAsyncKeyState", on_async_key_state);
        host.replace_import(g_key_state, "USER32.dll", "GetKeyState", on_key_state);

        // Readiness, sessions and the main menu from anvil-game; without it the
        // notices start at the first frame, and there are no session notices
        // and no badge in the menu.
        const auto* game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
        s.uses_game_plugin.store(game != nullptr);
        s.game_work = game;
        host.register_service(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION, kPanelService);
        host.register_service(ANVIL_INPUT_SERVICE, ANVIL_INPUT_SERVICE_VERSION, kInputService);
        if (game != nullptr) {
            host.subscribe(ANVIL_GAME_EVENT_READY, [] { on_game_ready(); });
            host.subscribe(ANVIL_GAME_EVENT_SESSION, [] {
                shared().session_changed.store(true, std::memory_order_release);
                shared().mark_dirty();
            });
            if (ANVIL_GAME_HAS(game, main_menu)) {
                s.game = game;
                host.subscribe(ANVIL_GAME_EVENT_MAIN_MENU, [] {
                    shared().main_menu.store(shared().game->main_menu() != 0, std::memory_order_release);
                    shared().mark_dirty();
                });
            } else {
                host.info("this anvil-game does not report the main menu: no badge there");
            }
        } else {
            host.info("anvil-game is not loaded: notices start with the first frame, without session notices");
            s.start_ready.store(true);
        }
        const char* key = host.c_api().setting_string(host.self(), cfg::panel_key.id);
        host.info("Direct3DCreate9(Ex) hooked; panel key {}", key != nullptr && *key != '\0' ? key : "(none)");
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilOverlay)
