// PROTOTYPE (eu4-engine-lab#11), throwaway: branch prototype/script-errors-panel,
// never merged. A panel of the game's error.log for modders, drawn by
// anvil-overlay.panel v1, in three variants the player flips with a button.
//
// It reads the log file (a thread, every 2 s, from where it stopped), since
// anvil-log-throttle has no service for its table yet: what the game wrote,
// plus the throttle's notices and reports, gives each message's count. A
// real version would take the counts from the throttle directly.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.gen.hpp"

#include "model.hpp"
#include "rows.hpp"

namespace se = script_errors;

namespace {

const AnvilPanelService* g_panels = nullptr;
AnvilPanel* g_panel = nullptr;
std::mutex g_mutex;  // the log and the view
se::Log g_log;
se::View g_view;
std::vector<se::Root> g_roots;
std::filesystem::path g_source;
bool g_open_links = true;

std::wstring wide(std::string_view s) { return se::path_from_utf8(s).native(); }

std::filesystem::path documents_eu4() {
    PWSTR docs = nullptr;
    std::filesystem::path out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        out = std::filesystem::path(docs) / L"Paradox Interactive" / L"Europa Universalis IV";
    }
    CoTaskMemFree(docs);
    return out;
}

// EU4's user folder: -userdir=<path> on the command line, else Documents.
std::filesystem::path user_dir() {
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    const std::size_t at = line.find(key);
    if (at != std::wstring::npos) {
        std::size_t from = at + key.size();
        bool quoted = from < line.size() && line[from] == L'"';
        if (quoted) ++from;
        std::size_t end = line.find(quoted ? L'"' : L' ', from);
        if (end == std::wstring::npos) end = line.size();
        return std::filesystem::path(line.substr(from, end - from));
    }
    return documents_eu4();
}

std::filesystem::path game_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// Under g_mutex.
void publish_locked() {
    if (g_panels == nullptr || g_panel == nullptr) return;
    const std::vector<se::Row> rows = se::build_rows(g_log, g_view);
    std::vector<AnvilPanelRow> out(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        out[i].kind = rows[i].kind;
        out[i].label = rows[i].kind == se::kSeparator ? nullptr : rows[i].label.c_str();
        out[i].value = rows[i].has_value ? rows[i].value.c_str() : nullptr;
        out[i].id = rows[i].id;
    }
    const AnvilResult r = g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow));
    if (r != ANVIL_OK) anvil::Host{}.warn("set_rows failed ({}) for {} rows", static_cast<int>(r), out.size());
}

void open_link(const std::string& uri) {
    const anvil::Host host;
    host.info("open: {}{}", uri, g_open_links ? "" : " (open_links = false: not opened)");
    if (!g_open_links) return;
    std::thread([w = wide(uri)] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

void copy_text(const std::string& text) {
    const std::wstring w = wide(text);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem != nullptr) {
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) GlobalFree(mem);
    }
    CloseClipboard();
}

void on_click(void*, std::uint64_t id) {
    std::lock_guard lock(g_mutex);
    if (!se::apply_view_click(g_view, g_log, id)) {
        auto& ms = g_log.messages();
        const std::size_t i = se::first_of(id);
        if (i >= ms.size()) return;
        se::Message& m = ms[i];
        switch (se::action_of(id)) {
            case se::Action::open:
                if (!m.resolved.empty()) {
                    open_link(se::vscode_uri(m.resolved, m.line));
                    g_view.note = "opened " + m.file + (m.line ? ":" + std::to_string(m.line) : "");
                }
                break;
            case se::Action::copy: {
                const std::string text = se::utf8_from_path(m.resolved) + (m.line ? ":" + std::to_string(m.line) : "");
                copy_text(text);
                g_view.note = "copied " + text;
                break;
            }
            case se::Action::open_hit: {
                const std::size_t h = se::second_of(id);
                if (h < m.hits.size()) {
                    open_link(se::vscode_uri(m.hits[h].abs, m.hits[h].line));
                    g_view.note = "opened " + m.hits[h].rel + ":" + std::to_string(m.hits[h].line);
                }
                break;
            }
            case se::Action::find:
                if (m.search == 0) {
                    m.search = 1;
                    std::thread([i, copy = m] {
                        std::vector<se::Hit> hits;
                        std::size_t total = 0, files = 0;
                        const auto start = std::chrono::steady_clock::now();
                        se::search(g_roots, copy, hits, total, files);
                        const auto ms_taken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                        anvil::Host{}.info("find '{}': {} uses in {} files, {} ms", copy.token, total, files, ms_taken);
                        std::lock_guard inner(g_mutex);
                        auto& list = g_log.messages();
                        if (i < list.size() && list[i].text == copy.text) {
                            list[i].hits = std::move(hits);
                            list[i].hit_total = total;
                            list[i].hit_files = files;
                            list[i].search = 2;
                            g_view.note = "searched '" + copy.token + "' in " + std::to_string(ms_taken) + " ms";
                        }
                        publish_locked();
                    }).detach();
                }
                break;
            default:
                break;
        }
    }
    publish_locked();
}

bool g_expand = false;

// For screenshots (setting expand): every heading of B and C open.
void expand_all_locked() {
    for (int k = 0; k < se::kKinds; ++k) g_view.open.insert(static_cast<std::uint32_t>(k));
    const int variant = g_view.variant;
    for (int v = 1; v < se::kVariants; ++v) {
        g_view.variant = v;
        for (const se::Row& r : se::build_rows(g_log, g_view)) {
            if (se::action_of(r.id) == se::Action::toggle) g_view.open.insert(se::first_of(r.id));
        }
    }
    g_view.variant = variant;
}

// The reader: new bytes of the log every 2 s; a shorter file is a new log.
void read_loop() {
    std::uint64_t offset = 0;
    std::string buffer;
    for (;;) {
        HANDLE f = CreateFileW(g_source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            const auto total = static_cast<std::uint64_t>(size.QuadPart);
            bool reset = total < offset;
            if (reset) offset = 0;
            if (total > offset) {
                LARGE_INTEGER at{};
                at.QuadPart = static_cast<LONGLONG>(offset);
                SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
                buffer.resize(1 << 20);
                std::string fresh;
                DWORD got = 0;
                while (offset < total && ReadFile(f, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got != 0) {
                    fresh.append(buffer.data(), got);
                    offset += got;
                }
                std::lock_guard lock(g_mutex);
                if (reset) g_log.reset();
                g_log.feed(fresh);
                for (se::Message& m : g_log.messages()) {
                    if (!m.resolve_tried) se::resolve(m, g_roots);
                }
                if (g_expand) expand_all_locked();
                publish_locked();
            }
            CloseHandle(f);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

struct PrototypeScriptErrors : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
        if (g_panels == nullptr) {
            host.warn("no anvil-overlay.panel: nothing to draw");
            return;
        }
        const std::filesystem::path user = user_dir();
        const std::string source = cfg::source.get();
        const bool replay = !source.empty();
        g_source = replay ? se::path_from_utf8(source) : user / L"logs" / L"error.log";
        const std::string playset = cfg::playset.get();
        const std::filesystem::path dlc = playset.empty() ? user / L"dlc_load.json" : se::path_from_utf8(playset);
        g_roots = se::playset_roots(dlc, dlc.parent_path(), game_dir());
        g_open_links = cfg::open_links.get();
        g_expand = cfg::expand.get();
        for (const se::Root& r : g_roots) host.info("layer {}: {}", r.name, se::utf8_from_path(r.path));
        {
            std::lock_guard lock(g_mutex);
            g_view.variant = static_cast<int>(cfg::variant.get());
            g_view.source = (replay ? "replay of " : "") + se::utf8_from_path(g_source.filename()) +
                            (replay ? " (" + se::utf8_from_path(g_source.parent_path().filename()) + ")" : "");
        }
        if (g_panels->create("Script errors (PROTOTYPE)", &g_panel) != ANVIL_OK) {
            host.warn("cannot create the panel");
            return;
        }
        g_panels->on_click(g_panel, &on_click, nullptr);
        g_panels->show(g_panel, 1);
        host.info("reading {} ({} layers)", se::utf8_from_path(g_source), g_roots.size());
        std::thread(read_loop).detach();
    }
};

}  // namespace

ANVIL_PLUGIN(PrototypeScriptErrors)
