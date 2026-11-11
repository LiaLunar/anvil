// anvil-log-throttle: the standard plugin that keeps a repeating line from
// filling the game's logs (anvil#172; design §4).
//
// Every log file of the game (logs/<name>.log) is a file sink of one class.
// The plugin swaps two slots of that class's vtable:
//   - the write (slot +8): a line of a log the `logs` setting names goes
//     through the Throttle (throttle.hpp), which says whether it is written
//     or counted and which notices and reports to write first;
//   - the deleting destructor (slot 0): before the game closes a log at exit,
//     the counts not reported yet are written to it.
// Everything else the game does with a line (the router's own count, its
// echo to a debugger) is untouched: only the bytes that reach the file differ.
// The writes come from any thread of the game.
#include <anvil/anvil.hpp>
#include <anvil/anvil_log_throttle.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#include "panel.hpp"
#include "tally.hpp"
#include "throttle.hpp"

namespace {

// An MSVC std::string as eu4.exe lays it out (32 bytes): the text in place up
// to 15 characters, else a pointer to it; then the size and the capacity.
struct GameString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;

    std::string_view view() const { return {capacity > 15 ? pointer : buffer, size}; }
};
static_assert(sizeof(GameString) == 32);

// A view of `text` the game's writer can read (it only reads it). `text`
// must outlive it.
GameString game_string(const std::string& text) {
    GameString s;
    std::memset(&s, 0, sizeof(s));
    s.size = text.size();
    if (text.size() < 16) {
        std::memcpy(s.buffer, text.c_str(), text.size() + 1);
        s.capacity = 15;
    } else {
        s.pointer = text.c_str();
        s.capacity = text.size();
    }
    return s;
}

using WriteFn = void(void* sink, int channel, const GameString* file, int line, const GameString* message);
using DestroyFn = void*(void* sink, unsigned flags);

anvil::PointerSwap<WriteFn> g_write;
anvil::PointerSwap<DestroyFn> g_destroy;
// Kept for the process: the game may still write a line while the DLLs detach.
log_throttle::Throttle& g_throttle = *new log_throttle::Throttle;
// The messages with their counts, for the service and the panel; kept only
// once one of them asks (Tally::keep).
log_throttle::Tally& g_tally = *new log_throttle::Tally;
const log_throttle::LogNames* g_logs = nullptr;  // set before the swaps
std::uint32_t g_file_offset = 0;  // sink+: the file object
std::uint32_t g_path_offset = 0;  // file object+: its path

std::atomic<std::int64_t> g_repeats{10};
std::atomic<std::int64_t> g_report_seconds{60};
std::atomic<bool> g_full_log{false};

// anvil.log names the first messages that are counted, not every one.
constexpr int kNamedMessages = 20;
std::atomic<int> g_named{0};

// The path of a sink's log, as the game's router reads it.
std::string_view log_path(const void* sink) {
    const auto* bytes = static_cast<const std::uint8_t*>(sink);
    const auto* file = *reinterpret_cast<const std::uint8_t* const*>(bytes + g_file_offset);
    if (file == nullptr) return {};
    return reinterpret_cast<const GameString*>(file + g_path_offset)->view();
}

using log_throttle::file_name;
// A game message may end with its own line break; anvil.log adds one.
using log_throttle::without_line_end;

std::string shortened(std::string_view text) {
    constexpr std::size_t kMax = 300;
    return text.size() <= kMax ? std::string(text) : std::string(text.substr(0, kMax)) + "...";
}

void write_lines(const std::vector<log_throttle::Line>& lines) {
    WriteFn* const next = g_write.previous();
    const anvil::Host host;
    for (const log_throttle::Line& l : lines) {
        const GameString file = game_string(l.file);
        const GameString text = game_string(l.text);
        next(const_cast<void*>(l.sink), l.channel, &file, l.line, &text);
        if (!l.notice) continue;
        const int named = g_named.fetch_add(1, std::memory_order_relaxed);
        if (named < kNamedMessages) {
            host.info("{}: counting the repeats of [{}:{}]: {}", file_name(log_path(l.sink)), file_name(l.file), l.line,
                      shortened(without_line_end(std::string_view(l.text).substr(l.message_at))));
        } else if (named == kNamedMessages) {
            host.info("more messages are counted; anvil.log names no more of them");
        }
    }
}

void on_write(void* sink, int channel, const GameString* file, int line, const GameString* message) noexcept {
    WriteFn* const next = g_write.previous();
    const std::string_view path = log_path(sink);
    // The game's writer drops the message CALL_STACK; it is not counted either.
    if (file == nullptr || message == nullptr || message->view() == "CALL_STACK" || !g_logs->matches(path)) {
        next(sink, channel, file, line, message);
        return;
    }
    log_throttle::Config config;
    config.repeats = static_cast<std::uint64_t>(g_repeats.load(std::memory_order_relaxed));
    config.report_ms = static_cast<std::uint64_t>(g_report_seconds.load(std::memory_order_relaxed)) * 1000;
    config.full_log = g_full_log.load(std::memory_order_relaxed);
    std::vector<log_throttle::Line> out;
    log_throttle::Verdict verdict = log_throttle::Verdict::write;
    try {
        verdict = g_throttle.record(sink, channel, file->view(), line, message->view(), GetTickCount64(), config, out);
        write_lines(out);
        if (g_tally.keeping()) {
            g_tally.add(file_name(path), file->view(), line, message->view(), verdict == log_throttle::Verdict::count);
        }
    } catch (...) {
        // Only a failed allocation can throw: the game's line is written.
        verdict = log_throttle::Verdict::write;
    }
    if (verdict == log_throttle::Verdict::write) next(sink, channel, file, line, message);
}

void* on_destroy(void* sink, unsigned flags) noexcept {
    const std::string_view path = log_path(sink);
    if (g_logs->matches(path)) {
        try {
            std::vector<log_throttle::Line> out;
            const log_throttle::SinkStats stats = g_throttle.close(sink, GetTickCount64(), out);
            write_lines(out);
            if (stats.lines != 0) {
                const anvil::Host host;
                host.info("{}: {} lines, {} written, {} counted (repeating messages: {})", file_name(path), stats.lines,
                          stats.written, stats.counted, stats.messages);
            }
        } catch (...) {
            // The log closes without the last counts.
        }
    }
    return g_destroy.previous()(sink, flags);
}

// ---- the service anvil-log-throttle.messages (anvil_log_throttle.h) ----

AnvilResult svc_keep() {
    try {
        g_tally.keep(GetTickCount64());
        return ANVIL_OK;
    } catch (...) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

AnvilResult svc_totals(AnvilLogTotals* out) {
    if (out == nullptr || out->size < sizeof(AnvilLogTotals)) return ANVIL_ERROR_INVALID_ARGUMENT;
    const log_throttle::TallyTotals t = g_tally.totals();
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(AnvilLogTotals));
    out->size = size;
    out->keeping = t.keeping ? 1 : 0;
    out->version = t.version;
    out->since_ms = t.since_ms;
    out->messages = t.messages;
    out->written = t.written;
    out->counted = t.counted;
    out->untracked = t.untracked;
    return ANVIL_OK;
}

AnvilResult svc_read(std::uint64_t first, std::uint64_t changed_after, AnvilLogMessage* out, std::uint32_t capacity,
                     std::uint32_t message_size, std::uint32_t* count, std::uint64_t* next) {
    if (count == nullptr || next == nullptr || (out == nullptr && capacity != 0) || message_size < sizeof(AnvilLogMessage)) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<log_throttle::TallyRow> rows;
        rows.reserve(capacity < 4096 ? capacity : 4096);
        *next = g_tally.read(first, changed_after, capacity, rows);
        auto* bytes = reinterpret_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const log_throttle::TallyRow& r = rows[i];
            auto* m = reinterpret_cast<AnvilLogMessage*>(bytes + i * message_size);
            std::memset(m, 0, message_size);
            m->index = r.index;
            m->version = r.version;
            m->written = r.written;
            m->counted = r.counted;
            m->log = r.message->log.c_str();
            m->source = r.message->source.c_str();
            m->source_line = r.message->source_line;
            m->text = r.message->text.c_str();
        }
        *count = static_cast<std::uint32_t>(rows.size());
        return ANVIL_OK;
    } catch (...) {
        *count = 0;
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
}

const AnvilLogMessagesService kService = {sizeof(AnvilLogMessagesService), &svc_keep, &svc_totals, &svc_read};

struct LogThrottle : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_file_offset = sym::channel_log_path.file;
        g_path_offset = sym::channel_log_path.path;
        const std::string logs = cfg::logs.get();
        g_logs = new log_throttle::LogNames(log_throttle::LogNames::parse(logs));
        g_repeats.store(cfg::repeats.get());
        g_report_seconds.store(cfg::report_seconds.get());
        g_full_log.store(cfg::full_log.get());
        host.subscribe_setting(cfg::repeats, [](std::int64_t v) { g_repeats.store(v); });
        host.subscribe_setting(cfg::report_seconds, [](std::int64_t v) { g_report_seconds.store(v); });
        host.subscribe_setting(cfg::full_log, [](bool on) { g_full_log.store(on); });
        host.register_service(ANVIL_LOG_MESSAGES_SERVICE, ANVIL_LOG_MESSAGES_SERVICE_VERSION, kService);
        log_throttle::panel::load(host, g_tally, cfg::panel.get());
        host.subscribe_setting(cfg::panel, [](bool on) { log_throttle::panel::set_on(on); });
        if (!g_logs->all && g_logs->names.empty()) {
            host.info("no logs to throttle (logs = \"{}\"); nothing changed", logs);
            return;
        }
        host.swap(g_write, sym::file_sink_write_slot, &on_write);
        host.swap(g_destroy, sym::file_sink_destroy_slot, &on_destroy);
        host.info("throttling {} after {} lines per message, reports every {} s{}", g_logs->all ? "every log" : logs,
                  g_repeats.load(), g_report_seconds.load(), g_full_log.load() ? " (off: full_log)" : "");
    }
};

}  // namespace

ANVIL_PLUGIN(LogThrottle)
