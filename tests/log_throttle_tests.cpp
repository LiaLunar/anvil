// anvil-log-throttle (plugins/anvil-log-throttle): the throttling on its own,
// and the plugin end to end. The DLL loads through the host table against a
// synthetic image with the file sink class of eu4.exe 1.37.5 (its vtable and
// the declared bytes of symbols.toml at their RVAs); lines written through
// that vtable reach a stand-in of the game's writer as the plugin decides.
// Usage: anvil_log_throttle_tests <folder with one folder per plugin>
#include "core/changes.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/settings.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-log-throttle/src/throttle.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace lt = log_throttle;
namespace fs = std::filesystem;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// ---- the throttle on its own ----------------------------------------------------

lt::Verdict record(lt::Throttle& t, const void* sink, std::string_view text, std::uint64_t now,
                   std::vector<lt::Line>& out, const lt::Config& config = {}, int line = 7256, int channel = 0x10003) {
    return t.record(sink, channel, "C:\\src\\triggerimplementation.cpp", line, text, now, config, out);
}

void throttle_tests() {
    const int a = 0, b = 0;  // two sinks
    run("log throttle: the first lines are written, then a notice, then counts", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 3;
        std::vector<lt::Line> out;
        for (int i = 0; i < 3; ++i) CHECK(record(t, &a, "Unknown culture group x", 100, out, c) == lt::Verdict::write);
        CHECK(out.empty());
        CHECK(record(t, &a, "Unknown culture group x", 100, out, c) == lt::Verdict::count);
        CHECK(out.size() == 1 && out[0].notice && out[0].sink == &a && out[0].channel == 0x10003 &&
              out[0].line == 7256 && out[0].file == "C:\\src\\triggerimplementation.cpp");
        CHECK(!out.empty() && out[0].text ==
                                  "[anvil-log-throttle] logged 3 times; further repeats are counted and reported "
                                  "every 60 s: Unknown culture group x");
        CHECK(!out.empty() && out[0].text.substr(out[0].message_at) == "Unknown culture group x");
        out.clear();
        for (int i = 0; i < 5; ++i) CHECK(record(t, &a, "Unknown culture group x", 200, out, c) == lt::Verdict::count);
        CHECK(out.empty());
        const lt::SinkStats s = t.stats(&a);
        CHECK(s.lines == 9 && s.written == 3 && s.counted == 6 && s.messages == 1);
    });

    run("log throttle: a message is the sink, channel, source line and text", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        std::vector<lt::Line> out;
        CHECK(record(t, &a, "m", 0, out, c) == lt::Verdict::write);
        CHECK(record(t, &b, "m", 0, out, c) == lt::Verdict::write);
        CHECK(record(t, &a, "m", 0, out, c, 1) == lt::Verdict::write);
        CHECK(record(t, &a, "m", 0, out, c, 7256, 0x301) == lt::Verdict::write);
        CHECK(record(t, &a, "n", 0, out, c) == lt::Verdict::write);
        CHECK(out.empty() && t.tracked() == 5);
        CHECK(record(t, &a, "m", 0, out, c) == lt::Verdict::count && out.size() == 1);
    });

    run("log throttle: a count is reported once it is due, at the next line of any sink", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        c.report_ms = 10'000;
        std::vector<lt::Line> out;
        record(t, &a, "spam", 1'000, out, c);
        record(t, &a, "spam", 1'000, out, c);  // the notice
        for (int i = 0; i < 4; ++i) record(t, &a, "spam", 2'000 + i, out, c);
        out.clear();
        // Not due yet.
        CHECK(record(t, &b, "other 1", 10'999, out, c) == lt::Verdict::write && out.empty());
        // Due, but the due counts are looked for at most once a second.
        CHECK(record(t, &b, "other 2", 11'500, out, c) == lt::Verdict::write && out.empty());
        // Reported before the line of the other sink.
        CHECK(record(t, &b, "other 3", 12'000, out, c) == lt::Verdict::write);
        CHECK(out.size() == 1 && !out[0].notice && out[0].sink == &a &&
              out[0].text == "[anvil-log-throttle] repeated 5 more times in the last 11 s: spam" &&
              out[0].text.substr(out[0].message_at) == "spam");
        out.clear();
        // The next report counts from the last one.
        record(t, &a, "spam", 15'000, out, c);
        CHECK(out.empty());
        record(t, &b, "other 4", 21'999, out, c);
        CHECK(out.empty());
        record(t, &b, "other 5", 23'000, out, c);
        CHECK(out.size() == 1 && out[0].text == "[anvil-log-throttle] repeated 1 more times in the last 11 s: spam");
        out.clear();
        // Nothing new: no report.
        record(t, &b, "other 6", 40'000, out, c);
        CHECK(out.empty());
    });

    run("log throttle: a closing sink reports its counts and is forgotten", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 2;
        std::vector<lt::Line> out;
        for (int i = 0; i < 10; ++i) record(t, &a, "spam", 1'000, out, c);
        for (int i = 0; i < 4; ++i) record(t, &b, "spam", 1'000, out, c);
        out.clear();
        const lt::SinkStats s = t.close(&a, 1'400, out);
        CHECK(s.lines == 10 && s.written == 2 && s.counted == 8 && s.messages == 1);
        CHECK(out.size() == 1 && out[0].sink == &a &&
              out[0].text == "[anvil-log-throttle] repeated 8 more times in the last 1 s: spam");
        CHECK(t.tracked() == 1 && t.stats(&a).lines == 0);
        // The address may come back as another sink: it starts from 0.
        out.clear();
        CHECK(record(t, &a, "spam", 2'000, out, c) == lt::Verdict::write && out.empty());
        // A sink without counts reports nothing.
        out.clear();
        lt::Throttle quiet;
        record(quiet, &a, "once", 0, out, c);
        CHECK(quiet.close(&a, 10, out).lines == 1 && out.empty());
    });

    run("log throttle: full_log writes every line; earlier counts are still reported", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        c.report_ms = 1'000;
        std::vector<lt::Line> out;
        record(t, &a, "spam", 0, out, c);
        record(t, &a, "spam", 0, out, c);
        record(t, &a, "spam", 0, out, c);
        out.clear();
        c.full_log = true;
        CHECK(record(t, &a, "spam", 500, out, c) == lt::Verdict::write && out.empty());
        CHECK(record(t, &a, "spam", 1'500, out, c) == lt::Verdict::write);
        CHECK(out.size() == 1 && out[0].text == "[anvil-log-throttle] repeated 2 more times in the last 2 s: spam");
        CHECK(t.stats(&a).written == 3);
    });

    run("log throttle: a full table forgets what is not counted, else writes untracked", [&] {
        lt::Throttle t(2);
        lt::Config c;
        c.repeats = 1;
        std::vector<lt::Line> out;
        record(t, &a, "one", 0, out, c);
        record(t, &a, "two", 0, out, c);
        CHECK(t.tracked() == 2);
        // Full: "one" and "two" are forgotten and "three" is tracked.
        record(t, &a, "three", 0, out, c);
        CHECK(t.tracked() == 1);
        CHECK(record(t, &a, "three", 0, out, c) == lt::Verdict::count && out.size() == 1);
        record(t, &a, "four", 0, out, c);
        CHECK(record(t, &a, "four", 0, out, c) == lt::Verdict::count && t.tracked() == 2);
        // Every tracked message is counted: a new one is written, every time.
        out.clear();
        for (int i = 0; i < 5; ++i) CHECK(record(t, &a, "five", 0, out, c) == lt::Verdict::write);
        CHECK(out.empty() && t.tracked() == 2);
    });

    run("log throttle: the logs setting", [] {
        const lt::LogNames d = lt::LogNames::parse("error.log");
        CHECK(!d.all && d.names == std::vector<std::string>{"error.log"});
        CHECK(d.matches("logs/error.log") && d.matches("C:\\Users\\x\\logs\\ERROR.LOG") && d.matches("error.log"));
        CHECK(!d.matches("logs/error_old.log") && !d.matches("logs/setup_error.log") && !d.matches(""));
        const lt::LogNames two = lt::LogNames::parse(" Error.log ,game.log,, ");
        CHECK(two.names == (std::vector<std::string>{"error.log", "game.log"}) && two.matches("logs/game.log"));
        const lt::LogNames all = lt::LogNames::parse("error.log, *");
        CHECK(all.all && all.matches("logs/anything.log"));
        const lt::LogNames none = lt::LogNames::parse(" , ");
        CHECK(!none.all && none.names.empty() && !none.matches("logs/error.log"));
    });
}

// ---- the plugin end to end ------------------------------------------------------

// An MSVC std::string as eu4.exe lays it out.
struct GameString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;

    std::string text() const { return std::string(capacity > 15 ? pointer : buffer, size); }
};
static_assert(sizeof(GameString) == 32);

struct OwnedString {
    std::string text;
    GameString game{};
    explicit OwnedString(std::string t) : text(std::move(t)) {
        game.size = text.size();
        if (text.size() < 16) {
            std::memcpy(game.buffer, text.c_str(), text.size() + 1);
            game.capacity = 15;
        } else {
            game.pointer = text.c_str();
            game.capacity = text.size();
        }
    }
};

// A file sink as the game lays it out: the vtable, its file object at +0x20,
// whose path is at +0x08.
struct FakeFile {
    void* handle = nullptr;
    GameString path{};
};
struct FakeSink {
    const void* vtable = nullptr;
    std::uint8_t rest[0x18] = {};
    FakeFile* file = nullptr;
    std::uint8_t more[0x10] = {};
};
static_assert(offsetof(FakeSink, file) == 0x20 && offsetof(FakeFile, path) == 0x08);

using WriteFn = void(void* sink, int channel, const GameString* file, int line, const GameString* message);
using DestroyFn = void*(void* sink, unsigned flags);

// What the stand-ins of the game's writer and destructor got.
struct Written {
    const void* sink;
    int channel;
    std::string file;
    int line;
    std::string text;
};
std::vector<Written> g_written;
std::vector<const void*> g_destroyed;

void fake_write(void* sink, int channel, const GameString* file, int line, const GameString* message) {
    g_written.push_back({sink, channel, file->text(), line, message->text()});
}
void* fake_destroy(void* sink, unsigned) {
    g_destroyed.push_back(sink);
    return sink;
}

// eu4.exe's size and sections, reserved; pages are committed only where the
// symbols of anvil-log-throttle lie:
//   .text  0x1562000, 0x1563000  the writer and destructor (jumps to the stand-ins), the router's path read
//   .rdata 0x1D94000             the file sink's vtable 0x1D94BA8
struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        commit(0x1562000, PAGE_EXECUTE_READWRITE);
        commit(0x1563000, PAGE_EXECUTE_READWRITE);
        commit(0x1D94000, PAGE_READWRITE);
        write_headers();
        jump(0x1562B90, reinterpret_cast<uintptr_t>(&fake_destroy));
        jump(0x1562BE0, reinterpret_cast<uintptr_t>(&fake_write));
        put(0x15635B9, {0x48, 0x8B, 0x57, 0x20, 0x48, 0x83, 0xC2, 0x08, 0x48, 0x83, 0x7A, 0x18, 0x10, 0x72, 0x03, 0x48,
                        0x8B, 0x12});
        put_pointer(0x1D94BA8, address(0x1562B90));
        put_pointer(0x1D94BB0, address(0x1562BE0));
        DWORD old = 0;
        VirtualProtect(base + 0x1D94000, 0x1000, PAGE_READONLY, &old);
    }
    ~FakeEu4() { VirtualFree(base, 0, MEM_RELEASE); }

    void commit(uint32_t page, DWORD protect) { VirtualAlloc(base + page, 0x1000, MEM_COMMIT, protect); }
    uintptr_t address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
    const void* vtable() const { return base + 0x1D94BA8; }
    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(base + rva, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
    }
    void put_pointer(uint32_t rva, uintptr_t value) { std::memcpy(base + rva, &value, sizeof(value)); }
    // mov rax, target; jmp rax
    void jump(uint32_t rva, uintptr_t target) {
        put(rva, {0x48, 0xB8});
        put_pointer(rva + 2, target);
        put(rva + 10, {0xFF, 0xE0});
    }

    void write_headers() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(base, &dos, sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 3;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = kSize;
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::memcpy(base + 0x80, &nt, sizeof(nt));
        struct Layout {
            const char* name;
            uint32_t rva, size, flags;
        };
        const Layout sections[] = {
            {".text", 0x1000, 0x1B64D78, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            {".rdata", 0x1B66000, 0x44999A, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
            {".data", 0x1FB0000, 0x4B4FB4, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
        };
        uint32_t at = 0x80 + sizeof(IMAGE_NT_HEADERS64);
        for (const auto& l : sections) {
            IMAGE_SECTION_HEADER s{};
            std::memcpy(s.Name, l.name, std::strlen(l.name));
            s.VirtualAddress = l.rva;
            s.Misc.VirtualSize = l.size;
            s.Characteristics = l.flags;
            std::memcpy(base + at, &s, sizeof(s));
            at += sizeof(s);
        }
    }
};

// A sink of the fake game, with the path the game gives it.
struct Sink {
    FakeFile file;
    FakeSink sink;
    OwnedString path;
    Sink(const FakeEu4& image, std::string p) : path(std::move(p)) {
        file.path = path.game;
        sink.vtable = image.vtable();
        sink.file = &file;
    }
};

// A line as the game writes it: through the sink's vtable.
void write(const Sink& s, const std::string& text, int line = 7256, int channel = 0x10003) {
    const OwnedString file("C:\\mnt\\gsg\\eu4\\eu4\\eu4\\source\\triggerimplementation.cpp");
    const OwnedString message(text);
    auto* const* slots = static_cast<void* const*>(s.sink.vtable);
    reinterpret_cast<WriteFn*>(slots[1])(const_cast<FakeSink*>(&s.sink), channel, &file.game, line, &message.game);
}
void destroy(const Sink& s) {
    auto* const* slots = static_cast<void* const*>(s.sink.vtable);
    reinterpret_cast<DestroyFn*>(slots[0])(const_cast<FakeSink*>(&s.sink), 1);
}

std::vector<std::string> texts() {
    std::vector<std::string> out;
    for (const Written& w : g_written) out.push_back(w.text);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_log_throttle_tests <plugins folder>\n");
        return 2;
    }
    const fs::path plugins_dir = fs::absolute(argv[1]);
    throttle_tests();

    // As in the game, the image and the host outlive the plugin.
    FakeEu4& image = *new FakeEu4;
    ChangeRegistry& changes = *new ChangeRegistry(image.address(0), FakeEu4::kSize);
    ServiceBus& bus = *new ServiceBus;
    std::vector<std::string>& log = *new std::vector<std::string>;
    HostContext& context = *new HostContext;
    context.changes = &changes;
    context.bus = &bus;
    context.image = GetModuleHandleW(nullptr);
    context.log = [&log](const AnvilPlugin& p, AnvilLogLevel, std::string_view message) {
        log.push_back(p.manifest.id + ": " + std::string(message));
    };
    AnvilGameInfo& info = *new AnvilGameInfo{};
    info.size = sizeof(info);
    info.build_id = kBuild;
    info.image_base = image.address(0);
    info.image_size = FakeEu4::kSize;
    const AnvilHost& host = *new AnvilHost(make_host(&info));
    const fs::path data = fs::temp_directory_path() / ("anvil-log-throttle-tests-" + std::to_string(GetCurrentProcessId()));
    AnvilPlugin* plugin = nullptr;

    run("log throttle plugin: symbols verify and it swaps the file sink's write and destructor", [&] {
        const auto dir = plugins_dir / "anvil-log-throttle";
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest && parsed.manifest->phase == Phase::Early && !parsed.manifest->affects_gameplay);
        if (!parsed.manifest) return;
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes,
                                                        image.address(0), &verified, &declared);
        if (!reason.empty()) std::printf("  %s\n", reason.c_str());
        CHECK(reason.empty() && verified.symbols.size() == 3);
        if (!reason.empty()) return;
        auto* p = new AnvilPlugin;
        p->context = &context;
        p->manifest = *parsed.manifest;
        p->symbols = PluginSymbols(declared, verified, image.address(0));
        p->owner = changes.add_owner(p->manifest.id);
        p->plugin_dir = dir.wstring();
        p->data_dir = data.wstring();
        auto schema = load_settings(dir / p->manifest.settings);
        CHECK(schema.file.has_value());
        if (!schema.file) return;
        // Three lines per message, as an environment override.
        p->settings.init(std::move(*schema.file), data / "settings.toml", p->manifest.id,
                         [](const std::string& name) -> std::optional<std::string> {
                             if (name == "ANVIL_ANVIL_LOG_THROTTLE_REPEATS") return "3";
                             return std::nullopt;
                         });
        p->module = LoadLibraryExW((dir / p->manifest.entry).c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(p->module != nullptr);
        if (p->module == nullptr) return;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(p->module, "anvil_plugin_load"));
        const LoadOutcome outcome = run_plugin_load(*p, host, fn);
        CHECK(outcome.result == ANVIL_OK && outcome.staged == 2 && changes.count(p->owner) == 2);
        auto* const* slots = static_cast<void* const*>(image.vtable());
        CHECK(reinterpret_cast<uintptr_t>(slots[0]) != image.address(0x1562B90) &&
              reinterpret_cast<uintptr_t>(slots[1]) != image.address(0x1562BE0));
        CHECK(contains(log, "anvil-log-throttle: throttling error.log after 3 lines per message, reports every 60 s"));
        plugin = p;
    });
    if (plugin == nullptr) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    Sink& game_log = *new Sink(image, "logs/game.log");
    Sink& error_log = *new Sink(image, "C:/Users/player/Documents/Paradox Interactive/Europa Universalis IV/logs/error.log");
    const std::string spam = "Script error! Script Object Token: \"x\", Error: Unknown culture group y. Root Country: RUS";

    run("log throttle plugin: other logs are written as they come", [&] {
        g_written.clear();
        for (int i = 0; i < 20; ++i) write(game_log, "Monthly tick");
        CHECK(g_written.size() == 20 && g_written[0].sink == &game_log.sink && g_written[0].text == "Monthly tick" &&
              g_written[0].line == 7256 && g_written[0].channel == 0x10003 &&
              g_written[0].file == "C:\\mnt\\gsg\\eu4\\eu4\\eu4\\source\\triggerimplementation.cpp");
    });

    run("log throttle plugin: error.log keeps three lines of a message, then a notice, then counts", [&] {
        g_written.clear();
        for (int i = 0; i < 10; ++i) {
            write(error_log, spam);
            if (i == 5) write(error_log, "short", 12);
        }
        const std::string notice =
            "[anvil-log-throttle] logged 3 times; further repeats are counted and reported every 60 s: " + spam;
        CHECK((texts() == std::vector<std::string>{spam, spam, spam, notice, "short"}));
        CHECK(g_written.size() == 5 && g_written[3].sink == &error_log.sink && g_written[3].line == 7256 &&
              g_written[3].channel == 0x10003 && g_written[4].line == 12);
        CHECK(contains(log, "anvil-log-throttle: error.log: counting the repeats of [triggerimplementation.cpp:7256]: " +
                                spam));
        // CALL_STACK, which the game's writer drops, passes untouched however often it comes.
        g_written.clear();
        for (int i = 0; i < 5; ++i) write(error_log, "CALL_STACK");
        CHECK((texts() == std::vector<std::string>(5, "CALL_STACK")));
    });

    run("log throttle plugin: anvil.log names a message without the line break it ends with", [&] {
        Sink& localization = *new Sink(image, "logs/error.log");
        const std::string message =
            "Error in localization value! Key: \"MODIFIER_ALLOW_CLIENT_STATES\", Error: \"Couldn't find Latin1 character\"";
        g_written.clear();
        for (int i = 0; i < 4; ++i) write(localization, message + "\r\n", 1063);
        // The game's lines and the notice keep it: only the bytes of anvil.log change.
        CHECK(g_written.size() == 4 && g_written[3].text.ends_with(message + "\r\n"));
        CHECK(contains(log, "anvil-log-throttle: error.log: counting the repeats of [triggerimplementation.cpp:1063]: " +
                                message));
        // Closed here, so that its count is not reported in a later test.
        destroy(localization);
    });

    run("log throttle plugin: full_log writes every line", [&] {
        g_written.clear();
        CHECK(plugin->settings.set_text(0, "true").applied);
        for (int i = 0; i < 4; ++i) write(error_log, spam);
        CHECK(g_written.size() == 4);
        CHECK(plugin->settings.set_text(0, "false").applied);
        g_written.clear();
        write(error_log, spam);
        CHECK(g_written.empty());
    });

    run("log throttle plugin: the closing log gets the counts, then the game's destructor runs", [&] {
        g_written.clear();
        g_destroyed.clear();
        destroy(error_log);
        CHECK(g_written.size() == 1 && g_written[0].sink == &error_log.sink && g_written[0].line == 7256 &&
              g_written[0].text.starts_with("[anvil-log-throttle] repeated 8 more times in the last ") &&
              g_written[0].text.ends_with(" s: " + spam));
        CHECK(g_destroyed == std::vector<const void*>{&error_log.sink});
        CHECK(contains(log, "anvil-log-throttle: error.log: 16 lines, 8 written, 8 counted (repeating messages: 1)"));
        // Another log closes without a word.
        g_written.clear();
        destroy(game_log);
        CHECK(g_written.empty() && g_destroyed.size() == 2);
    });

    run("log throttle plugin: a count is reported when it is due", [&] {
        CHECK(plugin->settings.set_text(2, "1").applied);
        Sink& fresh = *new Sink(image, "logs/error.log");
        g_written.clear();
        for (int i = 0; i < 6; ++i) write(fresh, spam);
        CHECK(g_written.size() == 4);
        Sleep(1100);
        write(fresh, "after a second");
        CHECK(g_written.size() == 6 && g_written[4].text.starts_with("[anvil-log-throttle] repeated 3 more times in the last ") &&
              g_written[5].text == "after a second");
    });

    std::error_code ec;
    fs::remove_all(data, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
