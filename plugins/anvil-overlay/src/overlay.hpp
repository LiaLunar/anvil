// anvil-overlay's state shared by the Direct3D 9 hooks and the window
// procedure (plugin.cpp) and the ImGui frame (ui.cpp).
#pragma once

#include "control_client.hpp"
#include "panels.hpp"

#include <anvil/anvil_game.h>
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace overlay {

struct Shared {
    ControlClient control;
    // The ImGui context and everything the UI keeps. The window procedure
    // takes it for input messages, the EndScene hook for a frame.
    std::recursive_mutex mutex;

    std::atomic<bool> panel_open{false};
    // An ImGui frame runs in each EndScene (the panel is open or a notice is
    // shown). While false and nothing is dirty, the hook only passes on.
    std::atomic<bool> visible{false};
    std::atomic<bool> dirty{false};  // the next EndScene re-evaluates what to show
    std::atomic<bool> start_ready{false};      // the game is ready: show the start notices
    std::atomic<bool> session_changed{false};  // rebuild the session notices
    std::atomic<bool> uses_game_plugin{false};
    // anvil-game's service when it reports the main menu (1.8.0 and later),
    // and the menu as its last event said.
    const AnvilGameService* game = nullptr;
    std::atomic<bool> main_menu{false};
    // anvil-game's service of any version, for the clicks of plugins' panels.
    const AnvilGameService* game_work = nullptr;
    // The windows of plugins (anvil-overlay.panel).
    PanelStore panels;

    // Settings (settings.toml), updated by their callbacks.
    std::atomic<int64_t> panel_key{0};
    std::atomic<bool> menu_badge{true};
    std::atomic<bool> notifications{true};
    std::atomic<int64_t> notification_seconds{30};
    std::atomic<double> scale{1.0};

    // What ImGui wanted at the end of the last frame.
    std::atomic<bool> want_mouse{false};
    std::atomic<bool> want_keyboard{false};
    std::atomic<bool> want_text{false};
    // The settings editor waits for a key: the next key press is recorded.
    std::atomic<bool> recording_key{false};
    std::atomic<int64_t> recorded_key{-1};  // -1 none, 0 cancelled, else a key code

    // The device and window the overlay draws on.
    std::atomic<IDirect3DDevice9*> device{nullptr};
    std::atomic<HWND> window{nullptr};

    void mark_dirty() { dirty.store(true, std::memory_order_release); }
};

Shared& shared();

// ui.cpp, all under Shared::mutex.
// One frame on `device` (EndScene): re-evaluates when dirty, then draws the
// notices and the panel if anything is visible.
void ui_frame(IDirect3DDevice9* device);
// Before IDirect3DDevice9::Reset: drops the device objects ImGui holds.
void ui_invalidate(IDirect3DDevice9* device);
// Feeds a window message to ImGui when a frame runs; true when ImGui wants
// the message for itself (the game should not see it).
bool ui_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
// The clipboard work the last frame asked for, done outside the mutex.
void ui_flush_clipboard();
// Read on Ctrl+V on the window's thread, outside the mutex, for ImGui's paste.
void ui_capture_clipboard();

void log_info(const char* text);
void log_warning(const char* text);

}  // namespace overlay
