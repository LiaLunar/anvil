// anvil-overlay end to end on a real Direct3D 9 device: the plugin loads
// through the host table with this executable as the "game" image (it imports
// Direct3DCreate9(Ex), GetAsyncKeyState and GetKeyState like eu4.exe), the
// core's anvil.control reports a skipped plugin, and the test renders frames
// on a window of its own. It checks the hooks, that a notice and the panel are
// drawn on the back buffer, the panel key through the window procedure, a
// settings change, a device reset, and the badge of the main menu against a
// stand-in of anvil-game's service. Without a Direct3D 9 device (some CI
// machines) it reports that and passes.
// Usage: anvil_overlay_d3d_tests <folder with one folder per plugin>
#include "core/changes.hpp"
#include "core/control.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/services.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_input.h>
#include <anvil/anvil_panel.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

constexpr D3DCOLOR kClear = D3DCOLOR_XRGB(0, 0, 0);
constexpr UINT kWidth = 1280, kHeight = 720;

// The stand-in of anvil-game's service: ready, no session, and the main menu
// as the test sets it (a nested frame makes main_menu() 0, as anvil-game does).
bool g_main_menu = false;
bool g_nested_frame = false;
int fake_ready() { return 1; }
uint32_t fake_main_thread() { return GetCurrentThreadId(); }
uint64_t fake_frame_number() { return 0; }
std::vector<std::pair<AnvilGameWork, void*>> g_work;
AnvilResult fake_run_on_main_thread(AnvilGameWork work, void* context) {
    g_work.emplace_back(work, context);
    return ANVIL_OK;
}
void run_work() {
    const auto work = std::move(g_work);
    g_work.clear();
    for (const auto& [run, context] : work) run(context);
}
int fake_session(AnvilGameSession*) { return 0; }
int fake_date(AnvilGameDate*) { return 0; }
int fake_main_menu() { return g_main_menu && !g_nested_frame ? 1 : 0; }
uint32_t fake_out_of_sync() { return 0; }
// The GUI's keyboard as the test sets it.
uint32_t g_game_keyboard = 0;
int fake_keyboard(uint32_t* flags) {
    if (flags != nullptr) *flags = g_game_keyboard;
    return 1;
}
const AnvilGameService kFakeGame = {sizeof(AnvilGameService), fake_ready,   fake_main_thread, fake_frame_number,
                                    fake_run_on_main_thread,  fake_session, fake_date,        fake_main_menu,
                                    fake_out_of_sync,         fake_keyboard};

// The game's own window procedure: which button and key presses reached it.
int g_game_button_downs = 0;
std::vector<WPARAM> g_game_keys;
LRESULT CALLBACK game_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_LBUTTONDOWN) ++g_game_button_downs;
    if (message == WM_KEYDOWN || message == WM_RBUTTONDOWN) g_game_keys.push_back(message == WM_KEYDOWN ? wparam : VK_RBUTTON);
    return DefWindowProcW(window, message, wparam, lparam);
}

// A stand-in of DXVK's window hook (src/d3d9/d3d9_window.cpp in DXVK 2.7.1).
// A swap chain in exclusive fullscreen hooks its window inside CreateDevice
// and takes the procedure on top as its "previous" one; when the swap chain
// goes, it puts that back only if its own procedure is still on top, and
// otherwise just forgets it. The overlay must therefore sit under it.
HWND g_dxvk_window = nullptr;
WNDPROC g_dxvk_previous = nullptr;
int g_dxvk_depth = 0;
int g_dxvk_max_depth = 0;
int g_dxvk_game_messages = 0;
constexpr UINT kDxvkProbe = WM_APP + 7;

LRESULT CALLBACK dxvk_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_dxvk_previous == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if (g_dxvk_depth >= 8) {  // a cycle of procedures; cut short instead of overflowing the stack
        g_dxvk_max_depth = 99;
        return 0;
    }
    g_dxvk_max_depth = std::max(g_dxvk_max_depth, ++g_dxvk_depth);
    const LRESULT result = CallWindowProcW(g_dxvk_previous, window, message, wparam, lparam);
    --g_dxvk_depth;
    return result;
}
void dxvk_reset_window_proc(HWND window) {
    if (g_dxvk_previous == nullptr) return;
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &dxvk_window_proc) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_dxvk_previous));
    }
    g_dxvk_previous = nullptr;
}
void dxvk_hook_window_proc(HWND window) {
    dxvk_reset_window_proc(window);
    g_dxvk_previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&dxvk_window_proc)));
}
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
CreateDeviceFn g_dxvk_next = nullptr;
HRESULT STDMETHODCALLTYPE dxvk_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                             D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** out) {
    const HRESULT hr = g_dxvk_next(d3d, adapter, type, focus, flags, parameters, out);
    if (SUCCEEDED(hr) && focus == g_dxvk_window) dxvk_hook_window_proc(focus);  // "fullscreen" on this window
    return hr;
}
LRESULT CALLBACK dxvk_game_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kDxvkProbe) ++g_dxvk_game_messages;
    return DefWindowProcW(window, message, wparam, lparam);
}

LPARAM point(int x, int y) { return MAKELPARAM(x, y); }

bool contains(const std::vector<std::string>& log, const std::string& part) {
    return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
}

// Pixels of the back buffer that are not the clear colour, inside a rectangle.
size_t drawn_pixels(IDirect3DDevice9* device, RECT area) {
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* copy = nullptr;
    size_t count = 0;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(back, copy))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(copy->LockRect(&locked, &area, D3DLOCK_READONLY))) {
            for (LONG y = 0; y < area.bottom - area.top; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch);
                for (LONG x = 0; x < area.right - area.left; ++x) count += (row[x] & 0xFFFFFF) != (kClear & 0xFFFFFF) ? 1 : 0;
            }
            copy->UnlockRect();
        }
    }
    if (copy) copy->Release();
    if (back) back->Release();
    return count;
}

// One frame, then what it drew in `area` (read before Present).
size_t frame(IDirect3DDevice9* device, RECT area) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0);
    device->BeginScene();
    device->EndScene();
    const size_t drawn = drawn_pixels(device, area);
    device->Present(nullptr, nullptr, nullptr, nullptr);
    // The real cursor is elsewhere, so Windows answers the test's mouse moves
    // with WM_MOUSELEAVE at once; dropped, as if the cursor stayed.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message != WM_MOUSELEAVE) DispatchMessageW(&msg);
    }
    return drawn;
}

// The overlay reads modifiers with GetKeyState, the key state of this thread,
// while the test sends its keys with SendMessage, which does not change it. A
// Shift, Ctrl or Alt that the machine's real keyboard (or another program's
// SendInput) holds can show up there, and a key without modifiers then does
// not match. Before a test sends keys, its thread's modifiers are set up.
bool release_modifiers() {
    BYTE state[256]{};
    if (!GetKeyboardState(state)) return false;
    for (const int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                         VK_LWIN, VK_RWIN}) {
        state[vk] = 0;
    }
    return SetKeyboardState(state) != 0;
}

// For a failed check: what the overlay would have read.
void print_modifiers() {
    std::printf("    modifiers: shift %d ctrl %d alt %d (async %d %d %d)\n", GetKeyState(VK_SHIFT) & 0x8000 ? 1 : 0,
                GetKeyState(VK_CONTROL) & 0x8000 ? 1 : 0, GetKeyState(VK_MENU) & 0x8000 ? 1 : 0,
                GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 1 : 0, GetAsyncKeyState(VK_CONTROL) & 0x8000 ? 1 : 0,
                GetAsyncKeyState(VK_MENU) & 0x8000 ? 1 : 0);
}

D3DPRESENT_PARAMETERS parameters(HWND window) {
    D3DPRESENT_PARAMETERS p{};
    p.Windowed = TRUE;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferWidth = kWidth;
    p.BackBufferHeight = kHeight;
    p.hDeviceWindow = window;
    p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_overlay_d3d_tests <plugin folder>\n");
        return 2;
    }
    const fs::path dir = fs::absolute(argv[1]) / "anvil-overlay";
    const fs::path root = fs::temp_directory_path() / "anvil-overlay-d3d";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "anvil");
    // The polling imports the overlay replaces, so this image has them.
    std::printf("keys %d %d\n", GetAsyncKeyState(VK_F24), GetKeyState(VK_F24));

    // As in the game, everything outlives the plugin.
    HMODULE self = GetModuleHandleW(nullptr);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(self) + reinterpret_cast<const IMAGE_DOS_HEADER*>(self)->e_lfanew);
    ChangeRegistry& changes = *new ChangeRegistry(reinterpret_cast<uintptr_t>(self), nt->OptionalHeader.SizeOfImage);
    ServiceBus& bus = *new ServiceBus;
    std::vector<std::string>& log = *new std::vector<std::string>;
    HostContext& context = *new HostContext;
    context.changes = &changes;
    context.bus = &bus;
    context.image = self;
    context.log = [&log](const AnvilPlugin& p, AnvilLogLevel, std::string_view message) {
        log.push_back(p.manifest.id + ": " + std::string(message));
        std::printf("    %s: %.*s\n", p.manifest.id.c_str(), static_cast<int>(message.size()), message.data());
    };
    AnvilGameInfo& info = *new AnvilGameInfo{};
    info.size = sizeof(info);
    const AnvilHost& host = *new AnvilHost(make_host(&info));

    auto& plugins = *new std::vector<std::unique_ptr<AnvilPlugin>>;
    auto& skipped = *new std::vector<Skipped>{{"tongs", "requires plugin 'anvil-game', which is not loaded", root, false}};
    {
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest.has_value());
        if (!parsed.manifest) return 1;
        auto p = std::make_unique<AnvilPlugin>();
        p->context = &context;
        p->manifest = *parsed.manifest;
        p->owner = changes.add_owner(p->manifest.id);
        p->plugin_dir = dir.wstring();
        p->data_dir = (root / "anvil" / "data" / "anvil-overlay").wstring();
        auto schema = load_settings(dir / p->manifest.settings);
        CHECK(schema.file.has_value());
        if (!schema.file) return 1;
        p->settings.init(std::move(*schema.file), fs::path(p->data_dir) / "settings.toml", p->manifest.id,
                         [](const std::string&) { return std::optional<std::string>(); });
        plugins.push_back(std::move(p));
    }
    ControlSources sources;
    sources.anvil_dir = root / "anvil";
    sources.anvil_version = "test";
    sources.plugins = &plugins;
    sources.skipped = &skipped;
    Control& control = *new Control(sources);
    bus.register_service("anvil", ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION, control.service());
    bus.register_service(ANVIL_GAME_ID, ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, &kFakeGame);

    AnvilPlugin& overlay = *plugins.front();
    run("overlay: loads early and hooks Direct3DCreate9(Ex)", [&] {
        overlay.module = LoadLibraryExW((dir / overlay.manifest.entry).c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(overlay.module != nullptr);
        if (overlay.module == nullptr) return;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(overlay.module, "anvil_plugin_load"));
        const LoadOutcome outcome = run_plugin_load(overlay, host, fn);
        CHECK(outcome.result == ANVIL_OK && outcome.staged == 4 && changes.count(overlay.owner) == 4);
        overlay.state = outcome.result == ANVIL_OK ? "loaded" : "failed";
        CHECK(contains(log, "Direct3DCreate9(Ex) hooked; panel key Ctrl+Shift+F12"));
        control.loading_done();
        bus.publish(ANVIL_GAME_EVENT_READY, nullptr);
    });
    if (overlay.state != "loaded") {
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = game_window_proc;
    wc.hInstance = self;
    wc.lpszClassName = L"anvil-overlay-test";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"anvil-overlay test", WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight,
                                  nullptr, nullptr, self, nullptr);
    IDirect3D9Ex* d3d_ex = nullptr;
    if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_ex)) && d3d_ex != nullptr) d3d_ex->Release();
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS pp = parameters(window);
    if (d3d == nullptr || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp,
                                                   &device))) {
        std::printf("skipped: no Direct3D 9 device on this machine\n%d checks, %d failed\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    const RECT top_right{static_cast<LONG>(kWidth) - 480, 0, static_cast<LONG>(kWidth), 200};
    const RECT middle{static_cast<LONG>(kWidth) / 2 - 100, static_cast<LONG>(kHeight) / 2 - 50,
                      static_cast<LONG>(kWidth) / 2 + 100, static_cast<LONG>(kHeight) / 2 + 50};

    run("overlay: the device is hooked and a notice is drawn", [&] {
        CHECK(contains(log, "Direct3D 9 device found"));
        // + CreateDevice and CreateDeviceEx (Direct3DCreate9Ex came first), EndScene, Reset.
        CHECK(changes.count(overlay.owner) == 8);
        size_t drawn = 0;
        for (int i = 0; i < 3; ++i) drawn = frame(device, top_right);
        CHECK(contains(log, "ImGui 1.93.0 started") && contains(log, "1 notice(s) at the start"));
        CHECK(drawn > 1000);
        CHECK(frame(device, middle) == 0);  // no panel yet
    });

    run("overlay: the panel key opens the panel; a setting changes it", [&] {
        // F11 alone, so the test needs no modifier keys held.
        AnvilControlResult result{};
        CHECK(control.service()->setting_set_text("anvil-overlay", 0, "F11", &result) == ANVIL_OK && result.applied == 1);
        CHECK(release_modifiers());
        SendMessageW(window, WM_KEYDOWN, VK_F11, 1);
        size_t drawn = 0;
        for (int i = 0; i < 3; ++i) drawn = frame(device, middle);
        CHECK(drawn > 1000);
        if (drawn <= 1000) {
            std::printf("    drawn %zu in the middle\n", drawn);
            print_modifiers();
        }
        SendMessageW(window, WM_KEYDOWN, VK_F11, 1);
        frame(device, middle);
        CHECK(frame(device, middle) == 0);
        CHECK(control.service()->setting_reset("anvil-overlay", 0, &result) == ANVIL_OK);
    });

    run("overlay: a device reset keeps drawing", [&] {
        D3DPRESENT_PARAMETERS again = parameters(window);
        CHECK(SUCCEEDED(device->Reset(&again)));
        size_t drawn = 0;
        for (int i = 0; i < 2; ++i) drawn = frame(device, top_right);
        CHECK(drawn > 1000);
    });

    run("overlay: a change of scale or language while something is shown keeps drawing", [&] {
        AnvilControlResult result{};
        const auto drawn_after = [&] {
            size_t drawn = 0;
            for (int i = 0; i < 4; ++i) drawn = frame(device, top_right);
            return drawn;
        };
        for (const char* scale : {"1.5", "2.5", "0.75"}) {
            CHECK(control.service()->setting_set_text("anvil-overlay", 3, scale, &result) == ANVIL_OK && result.applied == 1);
            CHECK(drawn_after() > 300);
        }
        CHECK(control.service()->setting_reset("anvil-overlay", 3, &result) == ANVIL_OK);
        CHECK(drawn_after() > 300);
        CHECK(control.service()->setting_set_text("anvil-overlay", 5, "ko", &result) == ANVIL_OK && result.applied == 1);
        CHECK(drawn_after() > 300);
        D3DPRESENT_PARAMETERS again = parameters(window);
        CHECK(SUCCEEDED(device->Reset(&again)));
        CHECK(drawn_after() > 300);
        CHECK(control.service()->setting_reset("anvil-overlay", 5, &result) == ANVIL_OK);
        CHECK(drawn_after() > 300);
    });

    run("overlay: notices close after their time; then no ImGui frame", [&] {
        AnvilControlResult result{};
        CHECK(control.service()->setting_set_text("anvil-overlay", 1, "false", &result) == ANVIL_OK);  // notifications off
        frame(device, top_right);
        CHECK(frame(device, top_right) == 0);
    });

    run("overlay: the badge in the main menu; a click on it opens the panel, one beside it reaches the game", [&] {
        const RECT badge{0, static_cast<LONG>(kHeight) / 2 - 30, 160, static_cast<LONG>(kHeight) / 2 + 30};
        CHECK(frame(device, badge) == 0);  // not in the menu yet
        g_main_menu = true;
        bus.publish(ANVIL_GAME_EVENT_MAIN_MENU, nullptr);
        size_t drawn = 0;
        for (int i = 0; i < 2; ++i) drawn = frame(device, badge);
        CHECK(drawn > 200 && frame(device, middle) == 0 && frame(device, top_right) == 0);

        // Over the badge: ImGui takes the press; the release still reaches the game.
        // ImGui lays out in the window's client area, which the back buffer is
        // stretched to.
        RECT client{};
        GetClientRect(window, &client);
        const LPARAM on_badge = point(40, client.bottom / 2);
        SendMessageW(window, WM_MOUSEMOVE, 0, on_badge);
        frame(device, badge);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, on_badge);
        frame(device, badge);
        SendMessageW(window, WM_LBUTTONUP, 0, on_badge);
        frame(device, badge);
        CHECK(g_game_button_downs == 0);
        for (int i = 0; i < 2; ++i) drawn = frame(device, middle);
        CHECK(drawn > 1000);  // the panel

        // Beside the badge and the panel: the game gets the press.
        const LPARAM beside = point(client.right - 20, client.bottom - 20);
        SendMessageW(window, WM_MOUSEMOVE, 0, beside);
        frame(device, badge);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, beside);
        frame(device, badge);
        SendMessageW(window, WM_LBUTTONUP, 0, beside);
        frame(device, badge);
        CHECK(g_game_button_downs == 1);

        // The badge again closes the panel. ImGui takes one input change per
        // frame, so each event gets its frame.
        SendMessageW(window, WM_MOUSEMOVE, 0, on_badge);
        for (int i = 0; i < 2; ++i) frame(device, badge);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, on_badge);
        frame(device, badge);
        SendMessageW(window, WM_LBUTTONUP, 0, on_badge);
        for (int i = 0; i < 2; ++i) frame(device, badge);
        CHECK(frame(device, middle) == 0);
        CHECK(g_game_button_downs == 1);
        SendMessageW(window, WM_MOUSEMOVE, 0, beside);
        frame(device, badge);

        // A frame drawn inside a save load (the loading screen): no badge.
        g_nested_frame = true;
        CHECK(frame(device, badge) == 0);
        g_nested_frame = false;
        CHECK(frame(device, badge) > 200);

        // The setting turns it off; on again; then the menu is left.
        AnvilControlResult result{};
        CHECK(control.service()->setting_set_text("anvil-overlay", 4, "false", &result) == ANVIL_OK && result.applied == 1);
        frame(device, badge);
        CHECK(frame(device, badge) == 0);
        CHECK(control.service()->setting_reset("anvil-overlay", 4, &result) == ANVIL_OK);
        frame(device, badge);
        CHECK(frame(device, badge) > 200);
        g_main_menu = false;
        bus.publish(ANVIL_GAME_EVENT_MAIN_MENU, nullptr);
        frame(device, badge);
        CHECK(frame(device, badge) == 0);
    });

    run("overlay: a plugin's panel is drawn, takes its clicks after a frame, and draws CJK text", [&] {
        const auto* panels = static_cast<const AnvilPanelService*>(bus.get_service(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION));
        CHECK(panels != nullptr && panels->size == sizeof(AnvilPanelService) && ANVIL_PANEL_HAS(panels, language));
        if (panels == nullptr) return;
        CHECK(std::string(panels->language()) == "en");
        AnvilPanel* panel = nullptr;
        CHECK(panels->create("Test panel", &panel) == ANVIL_OK);
        const RECT area{0, 100, 500, 500};
        const size_t empty = frame(device, area);
        // A panel with no rows yet still shows its title.
        CHECK(panels->show(panel, 1) == ANVIL_OK);
        size_t bare = 0;
        for (int i = 0; i < 3; ++i) bare = frame(device, area);
        CHECK(bare > 1500);
        CHECK(panels->show(panel, 0) == ANVIL_OK);
        frame(device, area);

        const AnvilPanelRow rows[] = {{ANVIL_PANEL_TEXT, 0, "Treasury", "100", 0},
                                      {ANVIL_PANEL_TEXT, 0, "Stability", "3", 0},
                                      {ANVIL_PANEL_SEPARATOR, 0, nullptr, nullptr, 0},
                                      {ANVIL_PANEL_BUTTON, 0, "Save the stats", nullptr, 42}};
        CHECK(panels->set_rows(panel, rows, 4, sizeof(AnvilPanelRow)) == ANVIL_OK);
        static uint64_t clicked = 0;
        CHECK(panels->on_click(panel, [](void*, uint64_t id) { clicked = id; }, nullptr) == ANVIL_OK);
        CHECK(panels->show(panel, 1) == ANVIL_OK && panels->shown(panel) == 1);
        size_t drawn = 0;
        for (int i = 0; i < 3; ++i) drawn = frame(device, area);
        CHECK(empty == 0 && drawn > 2000);
        CHECK(!contains(log, "system fonts merged"));

        // The button, under the window's title bar and the two rows. ImGui lays
        // out in the client area, which the back buffer is stretched to.
        RECT client{};
        GetClientRect(window, &client);
        const auto x = [&](int v) { return v * client.right / static_cast<int>(kWidth); };
        const auto y = [&](int v) { return v * client.bottom / static_cast<int>(kHeight); };
        const int presses = g_game_button_downs;
        for (int row = 290; row < 370 && clicked == 0; row += 6) {
            const LPARAM at = point(x(100), y(row));
            SendMessageW(window, WM_MOUSEMOVE, 0, at);
            for (int i = 0; i < 2; ++i) frame(device, area);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at);
            frame(device, area);
            SendMessageW(window, WM_LBUTTONUP, 0, at);
            for (int i = 0; i < 2; ++i) frame(device, area);
            CHECK(clicked == 0);  // only after an outer frame
            run_work();
        }
        CHECK(clicked == 42 && g_game_button_downs == presses);
        SendMessageW(window, WM_MOUSEMOVE, 0, point(client.right - 20, client.bottom - 20));
        frame(device, area);

        // Chinese, Japanese and Korean from the fonts of Windows: more is drawn
        // than the same rows without them, where only a placeholder would be.
        const AnvilPanelRow cjk[] = {{ANVIL_PANEL_TEXT, 0, "\xE5\x8C\x97\xE4\xBA\xAC\xE5\xB8\x82", "\xE6\x9D\xB1\xE4\xBA\xAC", 0},
                                     {ANVIL_PANEL_TEXT, 0, "\xEC\x84\x9C\xEC\x9A\xB8", "\xE3\x81\x82\xE3\x81\x84", 0}};
        CHECK(panels->set_rows(panel, cjk, 2, sizeof(AnvilPanelRow)) == ANVIL_OK);
        for (int i = 0; i < 4; ++i) drawn = frame(device, area);
        CHECK(drawn > 800);
        CHECK(contains(log, "system fonts merged"));

        // After a device reset it still draws.
        D3DPRESENT_PARAMETERS again = parameters(window);
        CHECK(SUCCEEDED(device->Reset(&again)));
        frame(device, area);
        CHECK(frame(device, area) > 800);

        CHECK(panels->show(panel, 0) == ANVIL_OK && panels->shown(panel) == 0);
        frame(device, area);
        CHECK(frame(device, area) == 0);
    });

    run("overlay: a plugin's binding gets its key after a frame, and nothing while the player types or uses the panel", [&] {
        const auto* input = static_cast<const AnvilInputService*>(bus.get_service(ANVIL_INPUT_SERVICE, ANVIL_INPUT_SERVICE_VERSION));
        CHECK(input != nullptr && input->size == sizeof(AnvilInputService) && ANVIL_INPUT_HAS(input, held));
        if (input == nullptr) return;
        CHECK(release_modifiers());
        static std::vector<AnvilInputEvent> got;
        const AnvilInputHandler handler = [](void*, const AnvilInputEvent* e) { got.push_back(*e); };
        const int64_t ctrl_k = 'K' | ANVIL_INPUT_CTRL;
        AnvilInputBinding* binding = nullptr;
        AnvilInputBinding* other = nullptr;
        CHECK(input->bind(ctrl_k, ANVIL_INPUT_CONSUME, handler, nullptr, &binding) == ANVIL_OK);
        // The overlay's panel key (Ctrl+Shift+F12) is taken.
        CHECK(input->bind(VK_F12 | ANVIL_INPUT_CTRL | ANVIL_INPUT_SHIFT, 0, handler, nullptr, &other) == ANVIL_ERROR_CONFLICT);
        CHECK((input->state() & ANVIL_INPUT_HOLD_KEYS) == 0);
        const auto press = [&](WPARAM vk) {
            g_game_keys.clear();
            SendMessageW(window, WM_KEYDOWN, VK_CONTROL, 1);
            SendMessageW(window, WM_KEYDOWN, vk, 1);
            SendMessageW(window, WM_KEYUP, vk, 0xC0000001);
            SendMessageW(window, WM_KEYUP, VK_CONTROL, 0xC0000001);
            run_work();
        };
        // Delivered on the "main thread" after a frame, not in the window
        // procedure, and the game sees Ctrl but not K.
        SendMessageW(window, WM_KEYDOWN, VK_CONTROL, 1);
        SendMessageW(window, WM_KEYDOWN, 'K', 1);
        CHECK(got.empty() && g_work.size() == 1);
        SendMessageW(window, WM_KEYUP, 'K', 0xC0000001);
        SendMessageW(window, WM_KEYUP, VK_CONTROL, 0xC0000001);
        run_work();
        CHECK(got.size() == 1 && got[0].code == ctrl_k && got[0].size == sizeof(AnvilInputEvent) && got[0].flags == 0);
        // K alone is not the binding, and reaches the game.
        g_game_keys.clear();
        SendMessageW(window, WM_KEYDOWN, 'K', 1);
        run_work();
        CHECK(got.size() == 1 && g_game_keys == std::vector<WPARAM>{'K'});
        SendMessageW(window, WM_KEYUP, 'K', 0xC0000001);
        press('K');
        CHECK(got.size() == 2 && g_game_keys == std::vector<WPARAM>{VK_CONTROL});
        // A text field of the game has focus: the key goes to the game.
        g_game_keyboard = ANVIL_GAME_KEYBOARD_TEXT;
        CHECK((input->state() & ANVIL_INPUT_HOLD_KEYS) == ANVIL_INPUT_GAME_TEXT);
        press('K');
        CHECK(got.size() == 2 && (g_game_keys == std::vector<WPARAM>{VK_CONTROL, 'K'}));
        g_game_keyboard = ANVIL_GAME_KEYBOARD_SHORTCUT;
        press('K');
        CHECK(got.size() == 2 && (input->state() & ANVIL_INPUT_HOLD_KEYS) == ANVIL_INPUT_GAME_SHORTCUT);
        g_game_keyboard = 0;
        // The panel is open (F11, so no modifier is needed for it).
        AnvilControlResult result{};
        CHECK(control.service()->setting_set_text("anvil-overlay", 0, "F11", &result) == ANVIL_OK && result.applied == 1);
        SendMessageW(window, WM_KEYDOWN, VK_F11, 1);
        for (int i = 0; i < 3; ++i) frame(device, middle);
        CHECK((input->state() & ANVIL_INPUT_OVERLAY_PANEL) != 0);
        press('K');
        CHECK(got.size() == 2);
        SendMessageW(window, WM_KEYDOWN, VK_F11, 1);
        for (int i = 0; i < 2; ++i) frame(device, middle);
        CHECK((input->state() & ANVIL_INPUT_OVERLAY_PANEL) == 0);
        CHECK(control.service()->setting_reset("anvil-overlay", 0, &result) == ANVIL_OK);
        press('K');
        CHECK(got.size() == 3);
        // Another code; held keys; a Ctrl+right click with its cursor.
        CHECK(input->set_code(binding, VK_RBUTTON | ANVIL_INPUT_CTRL) == ANVIL_OK);
        g_game_keys.clear();
        SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON | MK_CONTROL, point(120, 80));
        SendMessageW(window, WM_RBUTTONUP, MK_CONTROL, point(120, 80));
        run_work();
        CHECK(got.size() == 4 && got[3].code == (VK_RBUTTON | ANVIL_INPUT_CTRL) && got[3].x == 120 && got[3].y == 80);
        CHECK(g_game_keys.empty());
        SendMessageW(window, WM_KEYDOWN, 'W', 1);
        CHECK(input->held('W') == 1 && input->held('W' | ANVIL_INPUT_ALT) == 0);
        g_game_keyboard = ANVIL_GAME_KEYBOARD_TEXT;
        CHECK(input->held('W') == 0);
        g_game_keyboard = 0;
        SendMessageW(window, WM_KEYUP, 'W', 0xC0000001);
        CHECK(input->held('W') == 0);
        // Queued, then unbound: not delivered.
        SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON | MK_CONTROL, point(1, 1));
        CHECK(input->unbind(binding) == ANVIL_OK && input->unbind(binding) == ANVIL_ERROR_NOT_FOUND);
        run_work();
        CHECK(got.size() == 4);
        SendMessageW(window, WM_RBUTTONUP, 0, point(1, 1));
    });

    run("overlay: under DXVK's window hook, devices made and released in fullscreen keep one chain to the game", [&] {
        // EU4 on DXVK in exclusive fullscreen: a device, released, then the
        // device it keeps, each hooking the window inside CreateDevice.
        WNDCLASSEXW dc{sizeof(dc)};
        dc.lpfnWndProc = dxvk_game_window_proc;
        dc.hInstance = self;
        dc.lpszClassName = L"anvil-overlay-dxvk-test";
        RegisterClassExW(&dc);
        HWND game = CreateWindowExW(0, dc.lpszClassName, L"anvil-overlay dxvk test", WS_OVERLAPPEDWINDOW, 0, 0, kWidth,
                                    kHeight, nullptr, nullptr, self, nullptr);
        g_dxvk_window = game;
        // DXVK as the code under the overlay's CreateDevice hook (a later owner sits further in).
        const OwnerId dxvk = changes.add_owner("dxvk");
        ChangeTarget target;
        target.address = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(d3d))[16]);
        target.inside_image = false;
        CHECK(changes.hook_function(dxvk, target, reinterpret_cast<void*>(&dxvk_create_device),
                                    reinterpret_cast<void**>(&g_dxvk_next)) == ChangeStatus::Ok);
        const auto create = [&] {
            IDirect3DDevice9* made = nullptr;
            D3DPRESENT_PARAMETERS p = parameters(game);
            CHECK(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, game,
                                              D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &p, &made)));
            return made;
        };
        const auto release = [&](IDirect3DDevice9* gone) {
            dxvk_reset_window_proc(game);  // as DXVK's swap chain when it goes
            if (gone != nullptr) gone->Release();
        };
        const auto probe = [&] {
            g_dxvk_max_depth = 0;
            SendMessageW(game, kDxvkProbe, 0, 0);
        };
        release(create());
        IDirect3DDevice9* kept = create();
        probe();
        CHECK(g_dxvk_game_messages == 1);  // reached the game once, not a cycle
        CHECK(g_dxvk_max_depth == 1);
        release(kept);
        probe();
        CHECK(g_dxvk_game_messages == 2);  // after DXVK leaves, the overlay still leads to the game
        CHECK(g_dxvk_max_depth == 0);
        changes.rollback(dxvk);
        DestroyWindow(game);
    });

    device->Release();
    d3d->Release();
    DestroyWindow(window);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
