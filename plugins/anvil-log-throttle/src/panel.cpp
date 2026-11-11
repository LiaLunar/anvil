// The panel's thread, its clicks and what they do outside the panel (open a
// link, copy, search). The rows come from errors_model.cpp.
//
// One thread of the plugin, started when the setting is first on, does all
// the panel's work: it creates and shows the panel (a setting's callback runs
// inside the overlay's own drawing, where it must not call the overlay back),
// and once a second, while the panel is shown, it reads what changed in the
// table and hands over new rows. A hidden panel costs a wake-up a second.
// Clicks come on the game's main thread (anvil-game's run_on_main_thread) and
// only change the view and wake the thread; a search and the opening of a
// link run on threads of their own.
#include "panel.hpp"

#include <anvil/anvil_panel.h>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "errors_model.hpp"
#include "playset.hpp"
#include "texts.hpp"

namespace log_throttle::panel {

namespace se = script_errors;

namespace {

const AnvilPanelService* g_panels = nullptr;
Tally* g_tally = nullptr;
AnvilPanel* g_panel = nullptr;  // the thread's

std::atomic<bool> g_on{false};
std::atomic<bool> g_show{false};  // show it at the next wake-up
std::atomic<bool> g_started{false};
bool g_from_start = false;  // set before the thread starts

std::mutex g_wake_mutex;
std::condition_variable g_wake_cv;
bool g_wake = false;

// The model and the view, for the thread, the clicks and the searches.
std::mutex g_mutex;
se::Model& g_model = *new se::Model;  // kept for the process, like every pointer of a panel
se::View g_view;
bool g_dirty = true;
std::uint64_t g_version = 0;  // of the table, as far as the model has read it
std::vector<se::Layer> g_layers;
bool g_layers_read = false;

se::Texts texts() { return se::Texts{se::lang_of(g_panels->language != nullptr ? g_panels->language() : "en")}; }

void wake() {
    {
        std::lock_guard lock(g_wake_mutex);
        g_wake = true;
    }
    g_wake_cv.notify_one();
}

// Under g_mutex.
const std::vector<se::Layer>& layers_locked() {
    if (!g_layers_read) {
        g_layers_read = true;
        const std::filesystem::path user = se::user_dir();
        g_layers = se::playset_layers(user / L"dlc_load.json", user, se::game_dir());
    }
    return g_layers;
}

void resolve_locked(se::Message& m) {
    if (m.resolve_tried) return;
    m.resolve_tried = true;
    if (!m.file.empty() && m.kind != se::Kind::files) se::resolve(layers_locked(), m.file, m.resolved, m.layer);
}

// What changed in the table since the last read, into the model.
bool read_table() {
    const TallyTotals totals = g_tally->totals();
    if (totals.version == g_version) return false;
    std::vector<TallyRow> rows;
    std::uint64_t first = 0;
    std::lock_guard lock(g_mutex);
    for (;;) {
        rows.clear();
        first = g_tally->read(first, g_version, 4096, rows);
        for (const TallyRow& r : rows) {
            g_model.update(r.index, r.message->source, r.message->source_line, r.message->text, r.written, r.counted);
        }
        if (rows.size() < 4096) break;
    }
    g_version = totals.version;
    g_dirty = true;
    return true;
}

void publish() {
    const se::Texts t = texts();
    std::vector<se::Row> rows;
    {
        std::lock_guard lock(g_mutex);
        if (!g_dirty) return;
        g_dirty = false;
        // The files the game names: looked up once, for the messages the panel can show with them.
        for (std::uint64_t i = 0; i < g_model.size(); ++i) {
            se::Message* m = g_model.message(i);
            if (m != nullptr && m->kind == se::Kind::located) resolve_locked(*m);
        }
        for (se::Message* m : g_model.item(g_view.selected)) resolve_locked(*m);
        const TallyTotals totals = g_tally->totals();
        rows = g_model.rows(g_view, se::Totals{totals.untracked, g_from_start}, t);
    }
    std::vector<AnvilPanelRow> out(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        out[i].kind = rows[i].kind;
        out[i].label = rows[i].kind == se::kSeparator ? nullptr : rows[i].label.c_str();
        out[i].value = rows[i].has_value ? rows[i].value.c_str() : nullptr;
        out[i].id = rows[i].id;
    }
    const AnvilResult r =
        g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow));
    if (r != ANVIL_OK) anvil::Host{}.warn("the script errors panel: set_rows failed ({}) for {} rows", static_cast<int>(r), out.size());
}

void open_link(const std::string& uri, const std::string& what) {
    anvil::Host{}.info("the script errors panel opens {}", uri);
    std::thread([uri, what] {
        const std::wstring w = se::path_of(uri).native();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        CoUninitialize();
        {
            std::lock_guard lock(g_mutex);
            g_view.note = r > 32 ? texts().f("Opened {0}", {what}) : texts().f("No program opened {0}.", {what});
            g_dirty = true;
        }
        wake();
    }).detach();
}

void copy_text(const std::string& text) {
    const std::wstring w = se::path_of(text).native();
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)); mem != nullptr) {
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) GlobalFree(mem);
    }
    CloseClipboard();
}

std::string file_line(const std::string& path, int line) { return line > 0 ? path + ":" + std::to_string(line) : path; }

void start_search(std::uint64_t key, std::string token) {
    std::thread([key, token = std::move(token)] {
        std::vector<se::Layer> layers;
        {
            std::lock_guard lock(g_mutex);
            layers = layers_locked();
        }
        const auto start = std::chrono::steady_clock::now();
        se::Search found = se::search(layers, token);
        const auto ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        anvil::Host{}.info("the script errors panel: '{}' has {} uses in {} files ({} ms)", token, found.total, found.files, ms);
        {
            std::lock_guard lock(g_mutex);
            const std::vector<se::Message*> item = g_model.item(key);
            if (!item.empty()) {
                se::Message& m = *item.front();
                m.hits = std::move(found.hits);
                m.hit_total = found.total;
                m.hit_files = found.files;
                m.search_ms = ms;
                m.search = 2;
            }
            g_dirty = true;
        }
        wake();
    }).detach();
}

void on_click(void*, std::uint64_t id) {
    {
        std::lock_guard lock(g_mutex);
        g_dirty = true;
        if (!g_model.apply_click(g_view, id)) {
            const std::vector<se::Message*> item = g_model.item(g_view.selected);
            if (item.empty()) return;
            se::Message& m = *item.front();
            switch (se::action_of(id)) {
            case se::Action::open:
                if (!m.resolved.empty()) open_link(se::vscode_uri(m.resolved, m.line), file_line(m.file, m.line));
                break;
            case se::Action::copy:
                if (!m.resolved.empty()) {
                    const std::string text = file_line(m.resolved, m.line);
                    copy_text(text);
                    g_view.note = texts().f("Copied {0}", {text});
                }
                break;
            case se::Action::open_hit:
                if (const std::uint64_t h = se::arg_of(id); h < m.hits.size()) {
                    open_link(se::vscode_uri(m.hits[h].abs, m.hits[h].line), file_line(m.hits[h].rel, m.hits[h].line));
                }
                break;
            case se::Action::find:
                if (m.search == 0 && m.searchable) {
                    m.search = 1;
                    start_search(g_view.selected, m.token);
                }
                break;
            default:
                break;
            }
        }
    }
    wake();
}

void run() {
    for (;;) {
        {
            std::unique_lock lock(g_wake_mutex);
            g_wake_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_wake; });
            g_wake = false;
        }
        try {
            if (!g_on.load(std::memory_order_acquire)) {
                if (g_panel != nullptr && g_panels->shown(g_panel)) g_panels->show(g_panel, 0);
                continue;
            }
            if (g_panel == nullptr) {
                if (g_panels->create(texts()("Script errors"), &g_panel) != ANVIL_OK) {
                    anvil::Host{}.warn("the script errors panel: the overlay has no room for another panel");
                    g_on.store(false);
                    continue;
                }
                g_panels->on_click(g_panel, &on_click, nullptr);
            }
            if (g_show.exchange(false)) g_panels->show(g_panel, 1);
            if (!g_panels->shown(g_panel)) continue;
            read_table();
            publish();
        } catch (...) {
            // A failed allocation: the next wake-up tries again.
        }
    }
}

void start_thread() {
    if (g_started.exchange(true)) return;
    std::thread(run).detach();
}

}  // namespace

void load(const anvil::Host& host, Tally& tally, bool on) {
    g_tally = &tally;
    g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
    if (g_panels == nullptr || g_panels->size < sizeof(AnvilPanelService)) {
        g_panels = nullptr;
        host.info("no anvil-overlay.panel: the script errors panel is not available");
        return;
    }
    if (!on) return;
    tally.keep(GetTickCount64());
    g_from_start = true;
    g_on.store(true);
    g_show.store(true);
    start_thread();
    host.info("the script errors panel is on: it counts every message of the throttled logs from now on");
}

void set_on(bool on) {
    if (g_panels == nullptr) return;
    if (on) g_tally->keep(GetTickCount64());
    g_on.store(on, std::memory_order_release);
    if (on) g_show.store(true);
    start_thread();
    wake();
}

}  // namespace log_throttle::panel
