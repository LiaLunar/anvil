// AnvilManager.exe (design §15, issue #59): the player's installer and
// management tool. One exe with the Anvil package of its own version in its
// resources; Dear ImGui on Direct3D 11.
//
//   AnvilManager.exe [--game <EU4 folder>]
//   AnvilManager.exe --check-package          exit code 0 when the package and the catalog it carries are sound
//   AnvilManager.exe --remove-after-exit <exe> <pid>   (internal: the uninstall's last step)
#include "core/log.hpp"
#include "core/playset.hpp"
#include "i18n/cjk_font.hpp"
#include "i18n/translate.hpp"
#include "manager_app.hpp"
#include "platform.hpp"
#include "ui/imgui_colors.hpp"

#include <windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <memory>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

using namespace anvil::manager;
namespace fs = std::filesystem;

struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    bool occluded = false;
    UINT resize_width = 0;
    UINT resize_height = 0;

    bool create(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                       D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        if (result == DXGI_ERROR_UNSUPPORTED) {  // no GPU driver: the software rasterizer
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        }
        if (FAILED(result)) return false;
        create_target();
        return true;
    }
    void create_target() {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            device->CreateRenderTargetView(back, nullptr, &target);
            back->Release();
        }
    }
    void release_target() {
        if (target != nullptr) target->Release();
        target = nullptr;
    }
    ~Graphics() {
        release_target();
        if (swap_chain != nullptr) swap_chain->Release();
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
    }
};

Graphics* g_graphics = nullptr;
ManagerApp* g_app = nullptr;
float g_scale = 1.0f;
bool g_fonts_dirty = true;
anvil::i18n::Language g_fonts_language = anvil::i18n::Language::English;  // the atlas's

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path font = fs::path(windows) / "Fonts" / "segoeui.ttf";
    ImFontConfig config;
    config.OversampleH = 2;
    std::error_code ec;
    const auto font_name = font.u8string();  // ImGui opens it as UTF-8
    if (!fs::is_regular_file(font, ec) ||
        io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(font_name.c_str()), 17.0f * scale, &config) == nullptr) {
        ImFontConfig fallback;
        fallback.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fallback);
    }
    // Chinese, Japanese and Korean: the language's font, merged into the Latin
    // one (i18n/cjk_font.hpp). ImGui rasterizes a glyph when a text first needs it.
    g_fonts_language = anvil::i18n::language();
    const anvil::i18n::FontData cjk = anvil::i18n::cjk_font(g_fonts_language);
    if (cjk.size > 0) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;  // the exe's resource
        merge.OversampleH = 1;
        merge.PixelSnapH = true;
        // Source Han Sans fills more of its height with its ascent and
        // descent than Segoe UI, so the same size draws smaller ideographs.
        merge.GlyphOffset.y = 1.0f * scale;
        io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(cjk.data), static_cast<int>(cjk.size), 17.0f * 1.12f * scale,
                                       &merge);
    }
    ImGui::GetStyle() = ImGuiStyle();
    anvil::ui::use_dark_colors();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 7);
    style.FrameRounding = 3;
    style.TabRounding = 3;
    style.ScaleAllSizes(scale);
}

bool is_button_message(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

// Posted mouse input (issue #106): automated tests post WM_MOUSEMOVE and the
// button messages to the window without moving the cursor. The Win32 backend
// would lose such a position at once: its TrackMouseEvent sends WM_MOUSELEAVE
// as soon as the real cursor is outside the window, and without tracking it
// reads GetCursorPos every frame while the window is in front. A posted
// message shows itself by a cursor position (GetMessagePos) that is not the
// one in lParam; while the last mouse message was posted, the leave is kept
// from ImGui, so the position stays until the real mouse comes back.
bool g_posted_pointer = false;

bool is_posted(HWND window, LPARAM lparam) {
    POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ClientToScreen(window, &at);
    const DWORD cursor = GetMessagePos();
    return at.x != GET_X_LPARAM(cursor) || at.y != GET_Y_LPARAM(cursor);
}

// Returns true when the message is not for ImGui.
bool filter_mouse(HWND window, UINT message, LPARAM lparam) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (message == WM_MOUSELEAVE) return g_posted_pointer;
    if (message != WM_MOUSEMOVE && !is_button_message(message)) return false;
    const bool posted = is_posted(window, lparam);
    if (g_posted_pointer && !posted) {
        // The real mouse is back. The posted move's tracking ended in the leave
        // ImGui did not get, so its leaving is tracked again here.
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
    }
    g_posted_pointer = posted;
    // A button message carries the position of the click; ImGui gets it before
    // the button, so a click lands there even without a move before it. A real
    // click has the position of its last move anyway.
    if (is_button_message(message)) {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
    }
    return false;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (filter_mouse(window, message, lparam)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_SIZE:
        if (g_graphics != nullptr && wparam != SIZE_MINIMIZED) {
            g_graphics->resize_width = LOWORD(lparam);
            g_graphics->resize_height = HIWORD(lparam);
        }
        return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wparam) / 96.0f;
        g_fonts_dirty = true;
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {static_cast<LONG>(720 * g_scale), static_cast<LONG>(480 * g_scale)};
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        wchar_t path[MAX_PATH * 4] = {};
        if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0 && DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0 &&
            g_app != nullptr) {
            g_app->dropped(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no menu on Alt
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// --check-package: the package is in the resources, reads back and carries
// this Manager's version.dll, and the plugin catalog validates (the CI and
// release check).
int check_package() {
    std::string error;
    if (!load_embedded_catalog(error)) return 1;
    const auto package = load_embedded_package(error);
    if (!package) return 1;
    const ZipEntry* dll = package->zip.find("version.dll");
    if (dll == nullptr) return 1;
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const fs::path copy = fs::path(temp) / (L"anvil-manager-check-" + std::to_wstring(GetCurrentProcessId()) + L".dll");
    const auto bytes = package->zip.read(*dll, error);
    bool ok = bytes.has_value();
    if (ok) {
        HANDLE file = CreateFileW(copy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &written, nullptr) &&
             written == bytes->size();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    const auto product = ok ? read_dll_product(copy) : std::nullopt;
    DeleteFileW(copy.c_str());
    return product && product->name == "Anvil" && product->version == ANVIL_VERSION ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Never load DLLs from the game folder (it holds Anvil's own version.dll) or the current one.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    fs::path game;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--check-package") return check_package();
        if (args[i] == L"--remove-after-exit" && i + 2 < args.size()) {
            return run_remove_after_exit(args[i + 1], static_cast<DWORD>(std::wcstoul(args[i + 2].c_str(), nullptr, 10)));
        }
        if (args[i] == L"--game" && i + 1 < args.size()) game = args[++i];
    }
    remove_stale_copies();
    // The language of EU4's settings, then Steam's or Windows' CJK (#158, lab#93);
    // ManagerApp follows the chosen game folder later.
    anvil::i18n::set_language(anvil::i18n::choose_manager_language(anvil::eu4_user_dir(game)).language);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string package_error;
    std::optional<Package> package = load_embedded_package(package_error);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"AnvilManager";
    RegisterClassExW(&window_class);

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (const auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            get_dpi(monitor, 0, &dpi_x, &dpi_y);
        }
    }
    g_scale = dpi_x / 96.0f;
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Anvil Manager", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                        CW_USEDEFAULT, static_cast<int>(1040 * g_scale), static_cast<int>(720 * g_scale),
                                        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    g_scale = GetDpiForWindow(window) / 96.0f;

    Graphics graphics;
    if (!graphics.create(window)) {
        MessageBoxW(window, anvil::widen(anvil::i18n::tr("Direct3D 11 is not available on this computer.")).c_str(),
                    L"Anvil Manager", MB_ICONERROR);
        return 1;
    }
    g_graphics = &graphics;
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, show);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(graphics.device, graphics.context);

    auto app = std::make_unique<ManagerApp>(window, std::move(package), package_error, game);
    g_app = app.get();

    bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (graphics.occluded && graphics.swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        graphics.occluded = false;
        if (graphics.resize_width != 0 && graphics.resize_height != 0) {
            graphics.release_target();
            graphics.swap_chain->ResizeBuffers(0, graphics.resize_width, graphics.resize_height, DXGI_FORMAT_UNKNOWN, 0);
            graphics.resize_width = graphics.resize_height = 0;
            graphics.create_target();
        }
        if (g_fonts_dirty || g_fonts_language != anvil::i18n::language()) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            load_fonts(g_scale);
            g_fonts_dirty = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app->draw();
        ImGui::Render();
        const float clear[4] = {0.08f, 0.08f, 0.09f, 1.0f};
        graphics.context->OMSetRenderTargets(1, &graphics.target, nullptr);
        graphics.context->ClearRenderTargetView(graphics.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        graphics.occluded = graphics.swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
        if (app->should_close()) DestroyWindow(window);
    }

    g_app = nullptr;
    app.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    CoUninitialize();
    return 0;
}
