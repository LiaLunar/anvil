// Unit tests for anvil.exe (design §16): the error lines of the parsers,
// `gen`, `check` with and without --exe against a synthetic PE file,
// instruction boundaries, symbols.lock, the output formats and exit codes.
// Usage: anvil_cli_tests <path of anvil.exe>
#include "cli/check.hpp"
#include "cli/gen.hpp"
#include "core/game_build.hpp"
#include "core/manifest.hpp"
#include "core/settings.hpp"
#include "core/symbols.hpp"
#include "test_image.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace anvil;
using namespace anvil::cli;
namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The test image with code for the instruction-boundary rule in .text.
TestImage boundary_image() {
    TestImage image;
    image.put(0x1060, {0xE3, 0x10, 0x48, 0x89, 0xC8, 0xC3});              // jrcxz; mov rax, rcx; ret
    image.put(0x1068, {0x48, 0x89, 0xC8, 0xE2, 0xFB, 0xC3});              // mov rax, rcx; loop; ret
    image.put(0x1070, {0x48, 0x83, 0xEC, 0x28, 0x90, 0xE2, 0xF0, 0xC3});  // sub rsp, 0x28; nop; loop at +5; ret
    image.put(0x1080, {0x06, 0x06, 0x06, 0x06, 0x06});                    // push es: invalid in 64-bit mode
    image.put(0x1090, {0x67, 0xE3, 0x10, 0x90, 0x90, 0x90});              // jecxz; nop x3
    return image;
}

const char* kGoodSymbols = R"toml(build = "test-build"

[symbol.speed_object]
rva = 0x1000
bytes = "48 8B ?? 18 04 00 00 C3"
memory = "code"
signature = "void* (void* session)"
note = "returns the */ speed object"

[symbol.root_and_date]
rva = 0x1010
bytes = "48 8B 05 F1 1F 00 00 8B 98 D0 1D 00 00"
memory = "code"
values.root        = { at = 3, rip_relative = 7, type = "Root" }
values.date_offset = { at = 9, size = 4 }

[symbol.tick_slot]
rva = 0x2000
memory = "read"
points_to = 0x1000

[symbol.exported]
export = "Exported_fn"
bytes = "40 53 48 83 EC 20"
signature = "int (int a, const char* b)"
values.frame = { at = 5, size = 1 }

[symbol.root_data]
rva = 0x3008
memory = "write"
type = "Root"

[symbol.late_loop]
rva = 0x1070
bytes = "48 83 EC 28 90 E2 F0 C3"
memory = "code"

[symbol.short_site]
rva = 0x1060
bytes = "E3 10"
memory = "code"
)toml";

struct Fixture {
    fs::path root;
    fs::path exe;
    std::string exe_sha256;

    Fixture() {
        root = fs::temp_directory_path() / ("anvil_cli_tests_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root);
        exe = root / "game" / "eu4.exe";
        const auto bytes = boundary_image().file();
        write(exe, std::string(bytes.begin(), bytes.end()));
        exe_sha256 = sha256_file(exe);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // A plugin folder with plugin.toml and, unless empty, symbols.toml.
    fs::path plugin(const std::string& name, const std::string& symbols) const {
        const fs::path dir = root / name;
        fs::remove_all(dir);
        write(dir / "plugin.toml", plugin_toml(name, symbols.empty() ? "" : "symbols = \"symbols.toml\"\n"));
        if (!symbols.empty()) write(dir / "symbols.toml", symbols);
        return dir;
    }

    CheckReport check(const fs::path& dir, bool with_exe, const fs::path& exe_override = {}) const {
        CheckOptions options;
        options.plugin_dir = dir;
        if (with_exe) options.exe = exe_override.empty() ? exe : exe_override;
        const std::string sha = exe_sha256;
        options.build_of = [sha](std::string_view actual) -> const char* { return actual == sha ? "test-build" : nullptr; };
        return check_plugin(options);
    }
};

void print_problems(const CheckReport& report) {
    if (!report.cannot_check.empty()) std::printf("  (cannot check) %s\n", report.cannot_check.c_str());
    for (const auto& p : report.problems) std::printf("  (problem) %s:%u: %s\n", p.file.c_str(), p.line, p.message.c_str());
}

bool has_problem(const CheckReport& report, uint32_t line, const std::string& part) {
    for (const auto& p : report.problems) {
        if (p.line == line && contains(p.message, part)) return true;
    }
    print_problems(report);
    return false;
}

std::string symbol(const std::string& name, const std::string& body) {
    return "build = \"test-build\"\n\n[symbol." + name + "]\n" + body;
}

struct Run {
    int exit_code = -1;
    std::string out;
};

// Runs anvil.exe with `args` through cmd.exe; captures stdout.
Run run_anvil(const std::string& anvil, const std::string& args) {
    const std::string command = "\"\"" + anvil + "\" " + args + " 2>nul\"";
    Run run;
    FILE* pipe = _popen(command.c_str(), "rb");
    if (pipe == nullptr) return run;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0;) run.out.append(buffer, n);
    run.exit_code = _pclose(pipe);
    return run;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_cli_tests <anvil.exe>\n");
        return 2;
    }
    const std::string anvil = fs::path(argv[1]).make_preferred().string();
    const Fixture fx;

    run("symbols.toml errors carry their line", [] {
        auto line_of = [](const std::string& text) { return parse_symbols_text(text).line; };
        CHECK(line_of("build = \"b\"\n\n[symbol.a]\nrva = 0x10\nmemroy = \"code\"\n") == 5);
        CHECK(line_of("build = \"b\"\n[symbol.a]\nrva = 0x10\nbytes = \"zz\"\nmemory = \"code\"\n") == 4);
        CHECK(line_of("build = \"b\"\n[symbol.a]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"
                      "values.v = { at = 0, size = 3 }\n") == 6);
        CHECK(line_of("build = \"b\"\n\n\n[symbol.Bad]\nrva = 0x10\n") == 4);
        CHECK(line_of("build = \"b\"\n[symbol.a]\nrva = 0x10\nmemory = \"code\"\n") == 2);  // needs bytes: the table
        CHECK(line_of("build = \"b\"\n[symbol.a]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\nresume = \"x\"\n") == 2);
        CHECK(line_of("build = 3\n") == 1);
        CHECK(line_of("[symbol.a]\nrva = 0x10\n") == 0);  // missing build: the whole file
        CHECK(line_of("build = \"b\"\n\nx = [\n") == 3);    // TOML syntax
        CHECK(line_of("build = \"b\"\nextra = 1\n") == 2);
        const auto file = parse_symbols_text(kGoodSymbols);
        CHECK(file.file && file.file->build_line == 1 && file.file->symbols[0].line == 3 && file.file->symbols[1].line == 10);
    });

    run("plugin.toml errors carry their line", [] {
        auto line_of = [](const std::string& text) { return parse_manifest_text(text, L"C:\\p").line; };
        CHECK(line_of(plugin_toml("a", "colour = 1\n")) == 8);
        CHECK(line_of(plugin_toml("a", "", "1.0.0", "late")) == 6);
        CHECK(line_of(plugin_toml("a", "\n[[dependencies]]\nid = \"b\"\nverison = \"1\"\n")) == 11);
        CHECK(line_of(plugin_toml("a", "\n[[conflicts]]\nid = \"a\"\n")) == 10);
        CHECK(line_of(plugin_toml("a", "symbols = \"../s.toml\"\n")) == 8);
        CHECK(line_of(plugin_toml("a", "authors = [\"x\", 2]\n")) == 8);
        CHECK(line_of("id = \"a\"\napi = 1\n") == 2);
        CHECK(line_of("id = \"a\"\nname = \"a\"\nversion = \"1.0.0\"\napi = 2\n") == 0);  // missing entry
    });

    run("gen writes ids, function types, values and typed data", [] {
        const auto file = parse_symbols_text(kGoodSymbols);
        const GeneratedHeaders h = generate_headers(*file.file, "symbols.toml", true, true);
        CHECK(h.error.empty());
        CHECK(contains(h.c, "SYM_SPEED_OBJECT = 0,"));
        CHECK(contains(h.c, "SYM_SHORT_SITE = 6,"));
        CHECK(contains(h.c, "typedef void* (*sym_speed_object_fn)(void* session);"));
        CHECK(contains(h.c, "typedef int (*sym_exported_fn)(int a, const char* b);"));
        CHECK(contains(h.c, "#define SYMVAL_ROOT_AND_DATE_ROOT UINT64_C(0x3008)"));
        CHECK(contains(h.c, "#define SYMVAL_ROOT_AND_DATE_DATE_OFFSET UINT64_C(0x1DD0)"));
        CHECK(contains(h.c, "#define SYMVAL_EXPORTED_FRAME UINT64_C(0x20)"));
        CHECK(!contains(h.c, "the */ speed"));
        CHECK(contains(h.cpp, "struct speed_object : ::anvil::Function<void*(void* session)> {\n"
                              "    static constexpr const char* symbol_name = \"speed_object\";\n};"));
        CHECK(contains(h.cpp, "inline constexpr Types::speed_object speed_object{{0}};  // returns the */ speed"));
        CHECK(contains(h.cpp, "struct root_and_date : ::anvil::Site {"));
        CHECK(contains(h.cpp, "static constexpr ::std::uint64_t date_offset = 0x1DD0;"));
        CHECK(contains(h.cpp, "inline constexpr Types::root_and_date root_and_date{{1}};"));
        CHECK(contains(h.cpp, "inline constexpr ::anvil::Data<Root> root{1, \"root\"};"));
        CHECK(contains(h.cpp, "struct tick_slot : ::anvil::Slot {"));
        CHECK(contains(h.cpp, "inline constexpr Types::tick_slot tick_slot{{2}};"));
        CHECK(contains(h.cpp, "struct root_data : ::anvil::Data<Root> {"));
        CHECK(contains(h.cpp, "inline constexpr Types::root_data root_data{{4}};"));
        const GeneratedHeaders c_only = generate_headers(*file.file, "symbols.toml", true, false);
        CHECK(!c_only.c.empty() && c_only.cpp.empty());

        // A rip_relative value of an export is resolved at load, not known to gen.
        const auto exported = parse_symbols_text(
            symbol("e", "export = \"E\"\nbytes = \"48 8B 05 F1 1F 00 00\"\nvalues.p = { at = 3, rip_relative = 7, type = \"T\" }\n"));
        const GeneratedHeaders e = generate_headers(*exported.file, "symbols.toml", true, true);
        CHECK(contains(e.c, "SYMVAL_E_P: an rva resolved when the game loads"));
        CHECK(contains(e.cpp, "// p: an rva resolved when the game loads"));
        CHECK(contains(e.cpp, "inline constexpr ::anvil::Data<T> p{0, \"p\"};"));
    });

    run("gen refuses names the headers cannot hold", [] {
        auto error = [](const std::string& text, bool c, bool cpp) {
            const auto file = parse_symbols_text(text);
            if (!file.file) return "parse: " + file.error;
            const GeneratedHeaders h = generate_headers(*file.file, "symbols.toml", c, cpp);
            return h.error + "@" + std::to_string(h.line);
        };
        CHECK(contains(error(symbol("delete", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"), true, true),
                       "'delete' is a C++ keyword; rename it@3"));
        CHECK(error(symbol("delete", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"), true, false) == "@0");  // C only
        CHECK(contains(error(symbol("s", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\nvalues.id = { at = 0, size = 1 }\n"),
                             false, true),
                       "would hide the symbol's 'id' member"));
        CHECK(contains(error(symbol("s", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\nvalues.symbol_name = { at = 0, size = 1 }\n"),
                             false, true),
                       "would hide the symbol's 'symbol_name' member"));
        CHECK(contains(error(symbol("s", "rva = 0x10\nbytes = \"48 8B 05 F1 1F 00 00\"\nmemory = \"code\"\n"
                                         "values.s = { at = 3, rip_relative = 7, type = \"T\" }\n"),
                             false, true),
                       "sym::s is already symbol s"));
        CHECK(contains(error("build = \"b\"\n[symbol.a_b]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"
                             "values.c = { at = 0, size = 1 }\n[symbol.a]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"
                             "values.b_c = { at = 0, size = 1 }\n",
                             true, false),
                       "gives SYMVAL_A_B_C, which another value already gives@7"));
    });

    run("gen rust: ids, function types, values and warnings", [] {
        const auto file = parse_symbols_text(kGoodSymbols);
        const GeneratedHeaders h = generate_headers(*file.file, "symbols.toml", false, false, true);
        CHECK(h.error.empty() && h.c.empty() && h.cpp.empty() && h.warnings.empty());
        CHECK(contains(h.rust, "pub const SPEED_OBJECT: ::anvil_sys::AnvilSymbol = 0;"));
        CHECK(contains(h.rust, "pub const SHORT_SITE: ::anvil_sys::AnvilSymbol = 6;"));
        CHECK(contains(h.rust, "/// function, rva 0x1000: returns the */ speed object\n"));
        CHECK(contains(h.rust, "pub type SpeedObjectFn = unsafe extern \"C\" fn(session: *mut c_void) -> *mut c_void;"));
        CHECK(contains(h.rust, "pub type ExportedFn = unsafe extern \"C\" fn(a: c_int, b: *const c_char) -> c_int;"));
        CHECK(contains(h.rust, "pub const ROOT_AND_DATE_ROOT: u64 = 0x3008;"));
        CHECK(contains(h.rust, "pub const ROOT_AND_DATE_DATE_OFFSET: u64 = 0x1DD0;"));
        CHECK(!contains(h.rust, "TickSlot"));  // only functions get a type

        // What gen reads: pointers (const placement), integers, floating point, bool, void, "...".
        auto fn_of = [](const std::string& signature, std::vector<GenWarning>* warnings = nullptr) {
            const auto parsed =
                parse_symbols_text("build = \"b\"\n[symbol.f]\nexport = \"F\"\nsignature = \"" + signature + "\"\n");
            if (!parsed.file) return "parse: " + parsed.error;
            const GeneratedHeaders g = generate_headers(*parsed.file, "symbols.toml", false, false, true);
            if (warnings != nullptr) *warnings = g.warnings;
            const auto at = g.rust.find("pub type FFn = ");
            if (at == std::string::npos) return "none: " + g.error;
            const auto end = g.rust.find(";\n", at);
            return g.rust.substr(at + 15, end - at - 15);
        };
        const std::string pre = "unsafe extern \"C\" fn(";
        CHECK(fn_of("void ()") == pre + ")");
        CHECK(fn_of("bool (void)") == pre + ") -> bool");
        CHECK(fn_of("unsigned long long (unsigned x, long y)") == pre + "x: c_uint, y: c_long) -> c_ulonglong");
        CHECK(fn_of("float (double d, short s, signed char c)") == pre + "d: f64, s: c_short, c: c_schar) -> f32");
        CHECK(fn_of("uint32_t (size_t n, int64_t m)") == pre + "n: usize, m: i64) -> u32");
        CHECK(fn_of("char* (const char* a, char* const b, const char* const* c, void** d)") ==
              pre + "a: *const c_char, b: *mut c_char, c: *const *const c_char, d: *mut *mut c_void) -> *mut c_char");
        CHECK(fn_of("int (int, const char*)") == pre + "arg0: c_int, arg1: *const c_char) -> c_int");
        CHECK(fn_of("int (int type, int fn)") == pre + "type_: c_int, fn_: c_int) -> c_int");
        CHECK(fn_of("int (const char* format, ...)") == pre + "format: *const c_char, ...) -> c_int");
        CHECK(fn_of("struct Root* (const Root* r)") == pre + "r: *const c_void) -> *mut c_void");

        // Anything else by value is *mut c_void and a warning, at the symbol's line; behind a pointer it is quiet.
        std::vector<GenWarning> warnings;
        CHECK(fn_of("Root (Root r, void (*cb)(int), int t[4])", &warnings) ==
              pre + "r: *mut c_void, arg1: *mut c_void, arg2: *mut c_void) -> *mut c_void");
        CHECK(warnings.size() == 4 && warnings[0].line == 2 && contains(warnings[0].message, "symbol f: parameter 1 (Root r)"));
        CHECK(contains(warnings[0].message, "does not know the type 'Root' passed by value"));
        CHECK(contains(warnings[1].message, "parameter 2") && contains(warnings[1].message, "does not translate this syntax"));
        CHECK(contains(warnings[3].message, "symbol f: result (Root)"));
        fn_of("Root* (Root* r)", &warnings);
        CHECK(warnings.empty());

        // Names the module cannot hold twice.
        const auto clash = parse_symbols_text(
            "build = \"b\"\n[symbol.a_b]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\nvalues.c = { at = 0, size = 1 }\n"
            "[symbol.a]\nrva = 0x20\nbytes = \"48\"\nmemory = \"code\"\nvalues.b_c = { at = 0, size = 1 }\n");
        const GeneratedHeaders c = generate_headers(*clash.file, "symbols.toml", false, false, true);
        CHECK(contains(c.error, "the Rust name A_B_C is already symbol a_b: value c") && c.line == 7);
        CHECK(c.rust.empty() && c.warnings.empty());
    });

    run("check without symbols, and the files it needs", [&] {
        const auto report = fx.check(fx.plugin("plain", ""), false);
        CHECK(report.exit_code == kExitOk && report.problems.empty() && report.plugin == "plain");
        CHECK(contains(format_report(report, Format::Text), "plain: no symbols.toml (runs on every game build): OK"));
        CHECK(fx.check(fx.root / "missing", false).exit_code == kExitCannotCheck);

        const fs::path bad = fx.plugin("bad", "");
        write(bad / "plugin.toml", plugin_toml("bad", "phase2 = 1\n"));
        const auto bad_report = fx.check(bad, false);
        CHECK(bad_report.exit_code == kExitProblems && has_problem(bad_report, 8, "unknown key 'phase2'"));
        CHECK(bad_report.problems.size() == 1 && bad_report.problems[0].file.ends_with("bad/plugin.toml"));

        const fs::path no_file = fx.plugin("no-file", "");
        write(no_file / "plugin.toml", plugin_toml("no-file", "symbols = \"symbols.toml\"\n"));
        CHECK(has_problem(fx.check(no_file, false), 8, "symbols file 'symbols.toml' not found"));

        const auto broken = fx.check(fx.plugin("broken", symbol("s", "rva = 0x10\nmemory = \"code\"\nbytes = \"4\"\n")), false);
        CHECK(broken.exit_code == kExitProblems && has_problem(broken, 6, "bytes must be pairs"));
        CHECK(broken.problems[0].file.ends_with("broken/symbols.toml"));
    });

    run("check settings.toml: the schema at its line, and the loader's reason", [&] {
        const fs::path dir = fx.plugin("with-settings", "");
        write(dir / "plugin.toml", plugin_toml("with-settings", "settings = \"settings.toml\"\n"));
        write(dir / "settings.toml", "[setting.enabled]\ntype = \"bool\"\ndefault = true\nlabel = \"Enabled\"\n");
        const auto ok = fx.check(dir, false);
        CHECK(ok.exit_code == kExitOk && ok.has_settings && ok.settings == 1 && ok.skip_reason.empty());
        CHECK(contains(format_report(ok, Format::Text),
                       "with-settings: no symbols.toml (runs on every game build); 1 setting(s): OK"));
        CHECK(contains(report_json(ok), "\"settings\": 1,"));
        write(dir / "settings.toml", "\xEF\xBB\xBF[setting.enabled]\ntype = \"bool\"\ndefault = 1\nlabel = \"Enabled\"\n");
        const auto bad = fx.check(dir, false);
        CHECK(bad.exit_code == kExitProblems && has_problem(bad, 3, "setting enabled: default must be true or false"));
        CHECK(bad.problems.size() == 1 && bad.problems[0].file.ends_with("with-settings/settings.toml"));
        CHECK(bad.skip_reason == "settings.toml: setting enabled: default must be true or false");
    });

    // #80: a name gen refuses passed check, and the build failed later.
    run("check reports the names gen refuses, at their line", [&] {
        const auto value = fx.check(fx.plugin("value-id", symbol("script", "rva = 0x10\nbytes = \"48 8B 05 00\"\nmemory = \"code\"\n"
                                                                            "values.id = { at = 3, size = 1 }\n")),
                                    false);
        CHECK(value.exit_code == kExitProblems &&
              has_problem(value, 3, "symbol script: value id: the name would hide the symbol's 'id' member; rename it"));
        CHECK(value.skip_reason.empty());  // the loader does not care
        const auto keyword = fx.check(fx.plugin("keyword", symbol("delete", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n")), false);
        CHECK(has_problem(keyword, 3, "symbol delete: 'delete' is a C++ keyword; rename it"));
        const auto rust = fx.check(
            fx.plugin("rust-clash", "build = \"test-build\"\n[symbol.a_b]\nrva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"
                                    "[symbol.a]\nrva = 0x20\nbytes = \"48\"\nmemory = \"code\"\n"
                                    "values.b = { at = 0, size = 1 }\n"),
            false);  // SYM_A_B and SYMVAL_A_B in C; A_B twice in Rust
        CHECK(has_problem(rust, 6, "symbol a: value b: the Rust name A_B is already symbol a_b"));
        CHECK(rust.problems[0].file.ends_with("rust-clash/symbols.toml"));

        const fs::path dir = fx.plugin("option-get", "");
        write(dir / "plugin.toml", plugin_toml("option-get", "settings = \"settings.toml\"\n"));
        write(dir / "settings.toml", "[setting.m]\ntype = \"enum\"\nvalues = [{ name = \"a\" }, { name = \"get\" }]\n"
                                     "default = \"a\"\nlabel = \"M\"\n");
        const auto option = fx.check(dir, false);
        CHECK(option.exit_code == kExitProblems && option.problems.size() == 1 &&
              has_problem(option, 1, "setting m: option get: the name would hide the setting's 'get' member"));
        CHECK(option.problems[0].file.ends_with("option-get/settings.toml") && option.skip_reason.empty());
    });

    run("gen writes settings headers and refuses names they cannot hold", [] {
        auto error = [](const std::string& text, bool c, bool cpp) {
            const auto file = parse_settings_text(text);
            if (!file.file) return "parse: " + file.error;
            const GeneratedHeaders h = generate_settings_headers(*file.file, "settings.toml", c, cpp);
            return h.error + "@" + std::to_string(h.line);
        };
        auto with_options = [](const std::string& name, const std::string& options) {
            return "[setting." + name + "]\ntype = \"enum\"\nvalues = [" + options + "]\ndefault = \"a\"\nlabel = \"X\"\n";
        };
        const std::string keyword = with_options("m", "{ name = \"a\" }, { name = \"delete\" }");
        CHECK(contains(error(keyword, false, true), "setting m: option delete: 'delete' is a C++ keyword; rename it@1"));
        CHECK(error(keyword, true, false) == "@0");  // C only
        CHECK(contains(error(with_options("m", "{ name = \"a\" }, { name = \"get\" }"), false, true),
                       "would hide the setting's 'get' member"));
        CHECK(contains(error(with_options("m", "{ name = \"a\" }") + with_options("m_value", "{ name = \"a\" }"), false, true),
                       "the options of setting m: cfg::Types::m_value is already setting m_value"));
        CHECK(contains(error(with_options("m", "{ name = \"a\" }, { name = \"b\" }") + with_options("m_b", "{ name = \"a\" }"),
                             true, false),
                       "setting m: option b gives CFG_M_B, which setting m_b already gives@1"));
        CHECK(contains(error("[setting.new]\ntype = \"bool\"\ndefault = true\nlabel = \"X\"\n", true, true),
                       "'new' is a C++ keyword"));
        const auto none = generate_settings_headers(SettingsFile{}, "settings.toml", true, true);
        CHECK(none.error.empty() && contains(none.c, "declares no settings") && contains(none.cpp, "namespace cfg {"));
    });

    run("gen rust: settings ids, enum options and key flags", [] {
        const auto file = parse_settings_text(
            "[setting.budget]\ntype = \"int\"\ndefault = 5\nmin = 1\nmax = 9\nlabel = \"Budget\"\n"
            "[setting.mode]\ntype = \"enum\"\nvalues = [{ name = \"fast\", label = \"Fast one\" }, { name = \"slow\" }]\n"
            "default = \"slow\"\nlabel = \"Mode\"\nrestart = false\n"
            "[setting.toggle]\ntype = \"key\"\ndefault = \"\"\nlabel = \"Toggle\"\n");
        CHECK(file.file.has_value());
        const GeneratedHeaders h = generate_settings_headers(*file.file, "settings.toml", false, false, true);
        CHECK(h.error.empty() && h.c.empty() && h.cpp.empty());
        CHECK(contains(h.rust, "pub const BUDGET: ::anvil_sys::AnvilSetting = 0;"));
        CHECK(contains(h.rust, "pub const MODE: ::anvil_sys::AnvilSetting = 1;"));
        CHECK(contains(h.rust, "pub const TOGGLE: ::anvil_sys::AnvilSetting = 2;"));
        CHECK(contains(h.rust, "/// Fast one\npub const MODE_FAST: i64 = 0;"));
        CHECK(contains(h.rust, "pub const MODE_SLOW: i64 = 1;"));
        CHECK(contains(h.rust, "pub const KEY_CTRL: i64 = 0x200;"));
        CHECK(!contains(generate_settings_headers(*parse_settings_text("[setting.a]\ntype = \"bool\"\ndefault = true\nlabel = \"A\"\n").file,
                                                  "settings.toml", false, false, true)
                            .rust,
                        "KEY_"));
        // Names the module cannot hold twice.
        const auto clash = parse_settings_text(
            "[setting.m]\ntype = \"enum\"\nvalues = [{ name = \"b\" }, { name = \"c\" }]\ndefault = \"b\"\nlabel = \"M\"\n"
            "[setting.m_b]\ntype = \"bool\"\ndefault = true\nlabel = \"X\"\n");
        const GeneratedHeaders c = generate_settings_headers(*clash.file, "settings.toml", false, false, true);
        CHECK(contains(c.error, "setting m: option b gives M_B, which setting m_b already gives") && c.line == 1);
        const auto none = generate_settings_headers(SettingsFile{}, "settings.toml", false, false, true);
        CHECK(none.error.empty() && contains(none.rust, "declares no settings"));
    });

    run("check --exe verifies, writes symbols.lock, and check without it reads the lock", [&] {
        const fs::path dir = fx.plugin("good", kGoodSymbols);
        const auto missing = fx.check(dir, false);
        CHECK(missing.exit_code == kExitProblems && has_problem(missing, 0, "symbols.lock is missing"));

        const auto first = fx.check(dir, true);
        print_problems(first);
        CHECK(first.exit_code == kExitOk && first.lock == "written" && first.exe_build == "test-build");
        CHECK(first.exe_sha256 == fx.exe_sha256 && first.symbols == 7 && first.build == "test-build");
        const std::string lock = read(dir / kLockFile);
        CHECK(contains(lock, "result = \"ok\"") && contains(lock, "build = \"test-build\""));
        CHECK(contains(lock, "exe_sha256 = \"" + fx.exe_sha256 + "\""));
        CHECK(contains(lock, "tool = \"anvil " ANVIL_VERSION "\""));
        CHECK(fx.check(dir, true).lock == "unchanged");

        const auto offline = fx.check(dir, false);
        CHECK(offline.exit_code == kExitOk && offline.lock == "matches");

        // CRLF and a BOM (another checkout of the same file) keep the digest.
        std::string crlf = "\xEF\xBB\xBF";
        for (char c : std::string(kGoodSymbols)) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        write(dir / "symbols.toml", crlf);
        CHECK(fx.check(dir, false).exit_code == kExitOk);

        write(dir / "symbols.toml", std::string(kGoodSymbols) + "# changed\n");
        const auto stale = fx.check(dir, false);
        CHECK(stale.exit_code == kExitProblems && has_problem(stale, 5, "stale: symbols.toml changed"));
        CHECK(stale.problems[0].file.ends_with("good/symbols.lock"));

        write(dir / kLockFile, "format = 1\nresult = \"ok\"\n");
        CHECK(has_problem(fx.check(dir, false), 0, "missing or invalid 'tool'"));
        write(dir / kLockFile, lock + "extra = 1\n");
        CHECK(has_problem(fx.check(dir, false), 9, "unknown key 'extra'"));
    });

    run("check --exe reports the build and every failing symbol at its line", [&] {
        const fs::path other = fx.plugin("other-build", "build = \"other-build\"\n" + std::string(kGoodSymbols).substr(21));
        const auto mismatch = fx.check(other, true);
        CHECK(mismatch.exit_code == kExitProblems && has_problem(mismatch, 1, "symbols.toml is for other-build, eu4.exe is test-build"));
        CHECK(mismatch.problems.size() == 1);
        CHECK(contains(read(other / kLockFile), "result = \"problems\""));
        const auto offline = fx.check(other, false);
        CHECK(has_problem(offline, 8, "the last anvil check --exe (eu4.exe test-build) found problems"));

        const fs::path unknown_exe = fx.root / "game" / "other.exe";
        TestImage image = boundary_image();
        image.put(0x10F0, {0xCC});
        const auto bytes = image.file();
        write(unknown_exe, std::string(bytes.begin(), bytes.end()));
        const auto unknown = fx.check(fx.plugin("unknown", kGoodSymbols), true, unknown_exe);
        CHECK(has_problem(unknown, 1, "is not a game build this Anvil knows"));
        CHECK(contains(read(fx.root / "unknown" / kLockFile), "build = \"unknown\""));

        const auto failing = fx.check(fx.plugin("failing", std::string(kGoodSymbols) +
                                                                "\n[symbol.wrong]\nrva = 0x1000\nbytes = \"48 8B 82\"\nmemory = \"code\"\n"
                                                                "\n[symbol.not_read]\nrva = 0x2000\nmemory = \"write\"\npoints_to = 0x1000\n"),
                                      true);
        CHECK(failing.problems.size() == 2);
        CHECK(has_problem(failing, 43, "symbol wrong (rva 0x1000): byte +0x2 (rva 0x1002) expected 0x82, found 0x81"));
        CHECK(has_problem(failing, 48, "declared write memory, section .rdata is read"));

        CHECK(fx.check(fx.plugin("no-exe", kGoodSymbols), true, fx.root / "nothing.exe").exit_code == kExitCannotCheck);
        write(fx.root / "game" / "text.exe", "MZ not a PE");
        const auto not_pe = fx.check(fx.plugin("not-pe", kGoodSymbols), true, fx.root / "game" / "text.exe");
        CHECK(not_pe.exit_code == kExitCannotCheck && !fs::exists(fx.root / "not-pe" / kLockFile));
    });

    run("check --exe with several builds keeps one check per build in symbols.lock", [&] {
        const fs::path second_exe = fx.root / "game" / "second.exe";
        TestImage image = boundary_image();
        image.put(0x1000, {0x48, 0x8B, 0x81, 0x08, 0x04, 0x00, 0x00, 0xC3});  // the field 0x10 lower
        const auto image_bytes = image.file();
        write(second_exe, std::string(image_bytes.begin(), image_bytes.end()));
        const std::string first_sha = fx.exe_sha256, second_sha = sha256_file(second_exe);
        auto check = [&](const fs::path& dir, const fs::path& exe) {
            CheckOptions options;
            options.plugin_dir = dir;
            options.exe = exe;
            options.build_of = [=](std::string_view sha) -> const char* {
                return sha == first_sha ? "test-build" : sha == second_sha ? "second-build" : nullptr;
            };
            return check_plugin(options);
        };
        const std::string text = "builds = [\"test-build\", \"second-build\"]\n"
                                 "\n"
                                 "[symbol.speed_object]\n"
                                 "rva = 0x1000\n"
                                 "memory = \"code\"\n"
                                 "signature = \"void* (void* session)\"\n"
                                 "values.speed = { at = 3, size = 4 }\n"
                                 "build.\"test-build\" = { bytes = \"48 8B 81 18 04 00 00 C3\" }\n"
                                 "build.\"second-build\" = { bytes = \"48 8B 81 08 04 00 00 C3\" }\n";
        const fs::path dir = fx.plugin("several", text);

        const auto first = check(dir, fx.exe);
        print_problems(first);
        CHECK(first.exit_code == kExitOk && first.lock == "written" && first.build == "test-build");
        CHECK(first.builds == std::vector<std::string>({"test-build", "second-build"}));
        CHECK(contains(format_report(first, Format::Text), "1 symbol(s) for test-build, second-build"));
        CHECK(contains(report_json(first), "\"builds\": [\"test-build\", \"second-build\"]"));
        std::string lock = read(dir / kLockFile);
        CHECK(contains(lock, "format = 2\n") && contains(lock, "[[check]]\nbuild = \"test-build\"\nexe_sha256 = \"" + first_sha));
        CHECK(!contains(lock, "second-build"));
        const auto incomplete = fx.check(dir, false);
        CHECK(incomplete.exit_code == kExitProblems && has_problem(incomplete, 0, "no check of build second-build"));

        const auto second = check(dir, second_exe);
        print_problems(second);
        CHECK(second.exit_code == kExitOk && second.lock == "written" && second.build == "second-build");
        lock = read(dir / kLockFile);
        CHECK(contains(lock, "build = \"test-build\"") && lock.find("test-build\"\nexe") < lock.find("second-build\"\nexe"));
        CHECK(check(dir, second_exe).lock == "unchanged" && check(dir, fx.exe).lock == "unchanged");
        const auto offline = fx.check(dir, false);
        CHECK(offline.exit_code == kExitOk && offline.lock == "matches");
        const LockResult parsed = parse_lock(lock);
        CHECK(parsed.lock && parsed.lock->format == 2 && parsed.lock->checks.size() == 2 && parsed.lock->checks[1].ok);

        // A build the file does not declare is reported and leaves the lock alone.
        const fs::path unknown_exe = fx.root / "game" / "third.exe";
        image.put(0x10F0, {0xCC});
        const auto unknown_bytes = image.file();
        write(unknown_exe, std::string(unknown_bytes.begin(), unknown_bytes.end()));
        const auto unknown = check(dir, unknown_exe);
        CHECK(has_problem(unknown, 1, "symbols.toml declares test-build, second-build") && unknown.lock.empty());
        CHECK(read(dir / kLockFile) == lock);

        // A failing symbol is reported at its build's table; a changed file drops the other build's check.
        write(dir / "symbols.toml", text.substr(0, text.rfind("08 04")) + "09 04 00 00 C3\" }\n");
        const auto failing = check(dir, second_exe);
        CHECK(has_problem(failing, 9, "in build second-build: symbol speed_object (rva 0x1000): byte +0x3"));
        lock = read(dir / kLockFile);
        CHECK(contains(lock, "result = \"problems\"") && !contains(lock, "test-build\"\nexe"));
        CHECK(has_problem(fx.check(dir, false), 10, "the last anvil check --exe (eu4.exe second-build) found problems"));

        write(dir / kLockFile, "format = 2\ntool = \"t\"\nsymbols_sha256 = \"x\"\n\n[[check]]\nbuild = \"b\"\nexe_sha256 = \"y\"\n"
                               "result = \"ok\"\nextra = 1\n");
        CHECK(has_problem(fx.check(dir, false), 9, "unknown key 'check.extra'"));
        write(dir / kLockFile, "format = 3\n");
        CHECK(has_problem(fx.check(dir, false), 1, "format 3 is not supported"));
    });

    run("check --exe: a symbol absent on the exe's build is listed, not verified", [&] {
        const fs::path second_exe = fx.root / "game" / "second-absent.exe";
        TestImage image = boundary_image();
        image.put(0x10F8, {0xCC});
        const auto image_bytes = image.file();
        write(second_exe, std::string(image_bytes.begin(), image_bytes.end()));
        const std::string first_sha = fx.exe_sha256, second_sha = sha256_file(second_exe);
        auto check = [&](const fs::path& dir, const fs::path& exe) {
            CheckOptions options;
            options.plugin_dir = dir;
            options.exe = exe;
            options.build_of = [=](std::string_view sha) -> const char* {
                return sha == first_sha ? "test-build" : sha == second_sha ? "second-build" : nullptr;
            };
            return check_plugin(options);
        };
        // lobby's bytes are in neither exe: only `absent` exempts it, and only on second-build.
        const fs::path dir = fx.plugin("absent", "builds = [\"test-build\", \"second-build\"]\n"
                                                 "[symbol.speed_object]\nrva = 0x1000\nmemory = \"code\"\n"
                                                 "bytes = \"48 8B 81 18 04 00 00 C3\"\n"
                                                 "[symbol.lobby]\nrva = 0x1000\nmemory = \"code\"\nbytes = \"FF FF FF FF FF\"\n"
                                                 "absent = [\"second-build\"]\n");
        const auto second = check(dir, second_exe);
        print_problems(second);
        CHECK(second.exit_code == kExitOk && second.absent == std::vector<std::string>{"lobby"});
        CHECK(contains(format_report(second, Format::Text), "checked against second-absent.exe (second-build); absent on second-build: lobby; symbols.lock written"));
        CHECK(contains(report_json(second), "\"absent\": [\"lobby\"]"));
        const auto first = check(dir, fx.exe);
        CHECK(first.exit_code == kExitProblems && first.absent.empty() &&
              has_problem(first, 6, "in build test-build: symbol lobby (rva 0x1000): byte +0x0"));
        CHECK(contains(report_json(first), "\"absent\": []"));
    });

    run("gen with several builds: a value is a constant only when every build has it", [] {
        const auto file = parse_symbols_text("builds = [\"a\", \"b\"]\n[symbol.s]\nmemory = \"code\"\n"
                                             "values.same = { at = 0, size = 1 }\nvalues.differs = { at = 3, size = 4 }\n"
                                             "values.target = { at = 3, rip_relative = 7 }\n"
                                             "build.\"a\" = { rva = 0x1000, bytes = \"48 8B 05 10 00 00 00\" }\n"
                                             "build.\"b\" = { rva = 0x1010, bytes = \"48 8B 05 00 00 00 00\" }\n");
        CHECK(file.file.has_value());
        const GeneratedHeaders h = generate_headers(*file.file, "symbols.toml", true, true, true);
        CHECK(h.error.empty());
        CHECK(contains(h.c, "(builds a, b). Do not edit."));
        CHECK(contains(h.c, "/* site, rva 0x1000 in a, 0x1010 in b */"));
        CHECK(contains(h.c, "#define SYMVAL_S_SAME UINT64_C(0x48)"));
        CHECK(contains(h.c, "/* SYMVAL_S_DIFFERS: differs between the game builds"));
        // 0x1000 + 7 + 0x10 and 0x1010 + 7 + 0: the same RVA in both.
        CHECK(contains(h.c, "#define SYMVAL_S_TARGET UINT64_C(0x1017)"));
        CHECK(contains(h.cpp, "static constexpr ::std::uint64_t same = 0x48;"));
        CHECK(contains(h.cpp, "// differs: differs between the game builds") && !contains(h.cpp, "differs = "));
        CHECK(contains(h.rust, "// S_DIFFERS: differs between the game builds"));
    });

    run("gen: an absent symbol says where it is missing; values come from the builds it is in", [] {
        const auto file = parse_symbols_text("builds = [\"a\", \"b\", \"c\"]\n[symbol.lobby]\nmemory = \"code\"\n"
                                             "signature = \"void (void* lobby)\"\nbytes = \"48 8B\"\n"
                                             "values.op = { at = 0, size = 1 }\nabsent = [\"b\"]\n"
                                             "build.\"a\" = { rva = 0x1000 }\nbuild.\"c\" = { rva = 0x1010 }\n");
        CHECK(file.file.has_value());
        if (!file.file) return;
        const GeneratedHeaders h = generate_headers(*file.file, "symbols.toml", true, true, true);
        CHECK(h.error.empty());
        CHECK(contains(h.c, "/* function, rva 0x1000 in a, 0x1010 in c; absent in b: check that it is present before using it */"));
        CHECK(contains(h.c, "#define SYMVAL_LOBBY_OP UINT64_C(0x48)"));
        CHECK(contains(h.cpp, "// function, rva 0x1000 in a, 0x1010 in c; absent in b"));
        CHECK(contains(h.cpp, "static constexpr ::std::uint64_t op = 0x48;"));
    });

    run("instruction boundaries: whole instructions, no JRCXZ/JECXZ/LOOP* in the stolen bytes", [&] {
        auto check_one = [&](const std::string& body) {
            return fx.check(fx.plugin("boundaries", symbol("s", body)), true);
        };
        auto problem = [&](const std::string& body, const std::string& part) {
            const auto report = check_one(body);
            return report.exit_code == kExitProblems && has_problem(report, 3, part);
        };
        CHECK(problem("rva = 0x1060\nbytes = \"E3 10 48 89 C8 C3\"\nmemory = \"code\"\n",
                      "jrcxz at +0x0 (rva 0x1060) lies in the first 5 bytes"));
        CHECK(problem("rva = 0x1068\nbytes = \"48 89 C8 E2 FB C3\"\nmemory = \"code\"\n", "loop at +0x3 (rva 0x106B)"));
        CHECK(problem("rva = 0x1090\nbytes = \"67 E3 10 90 90 90\"\nmemory = \"code\"\n", "jecxz at +0x0"));
        CHECK(problem("rva = 0x1000\nbytes = \"48 8B 81 18 04 00\"\nmemory = \"code\"\nsignature = \"void ()\"\n",
                      "the 7-byte instruction at +0x0 (rva 0x1000) ends past the declared 6 bytes; declare whole "
                      "instructions (7 bytes)"));
        CHECK(problem("rva = 0x1080\nbytes = \"06 06 06 06 06\"\nmemory = \"code\"\n",
                      "no valid x86-64 instruction at +0x0 (rva 0x1080)"));
        // Not subject to the rule: fewer than 5 bytes (byte patches), data in code.
        CHECK(check_one("rva = 0x1060\nbytes = \"E3 10 48 89\"\nmemory = \"code\"\n").exit_code == kExitOk);
        CHECK(check_one("rva = 0x1080\nbytes = \"06 06 06 06 06\"\nmemory = \"code\"\ntype = \"Table\"\n").exit_code == kExitOk);
        // A LOOP after the stolen bytes is fine; "??" bytes decode as the image's.
        CHECK(check_one("rva = 0x1070\nbytes = \"48 83 EC 28 90 E2 F0 C3\"\nmemory = \"code\"\n").exit_code == kExitOk);
        CHECK(check_one("rva = 0x1000\nbytes = \"48 8B ?? ?? ?? ?? ?? C3\"\nmemory = \"code\"\n").exit_code == kExitOk);
        // A block's resume target must be code.
        const auto resume = fx.check(fx.plugin("resume", symbol("block", "rva = 0x1000\nbytes = \"48 8B 81 18 04 00 00\"\n"
                                                                          "memory = \"code\"\nresume = \"slot\"\n"
                                                                          "[symbol.slot]\nrva = 0x2000\nmemory = \"read\"\n"
                                                                          "points_to = 0x1000\n")),
                                     true);
        CHECK(has_problem(resume, 3, "resume names slot, which is not in code (section .rdata)"));
    });

    run("output formats", [] {
        CheckReport report;
        report.exit_code = kExitProblems;
        report.plugin = "p";
        report.has_symbols = true;
        report.build = "b";
        report.symbols = 2;
        report.problems = {{"dir,1/symbols.toml", 7, "bad \"x\" 100%\nnext"}, {"dir/plugin.toml", 0, "whole file"}};
        const std::string github = format_report(report, Format::Github);
        CHECK(contains(github, "::error file=dir%2C1/symbols.toml,line=7,title=anvil check::bad \"x\" 100%25%0Anext\n"));
        CHECK(contains(github, "::error file=dir/plugin.toml,title=anvil check::whole file\n"));
        const std::string text = format_report(report, Format::Text);
        CHECK(contains(text, "dir,1/symbols.toml:7: error: bad \"x\" 100%\nnext\n"));
        CHECK(contains(text, "dir/plugin.toml: error: whole file\n"));
        CHECK(contains(text, "p: 2 symbol(s) for b: 2 problem(s)\n"));
        const std::string json = format_report(report, Format::Json);
        CHECK(contains(json, "\"result\": \"problems\""));
        CHECK(contains(json, "{\"file\": \"dir,1/symbols.toml\", \"line\": 7, \"message\": \"bad \\\"x\\\" 100%\\nnext\"}"));
        CHECK(contains(json, "\"plugin\": \"p\"") && contains(json, "\"exit_code\": 1"));

        CheckReport cannot;
        cannot.exit_code = kExitCannotCheck;
        cannot.cannot_check = "no plugin.toml in x";
        CHECK(contains(format_report(cannot, Format::Json), "\"result\": \"cannot-check\""));
        CHECK(contains(format_report(cannot, Format::Json), "\"plugin\": null"));
        CHECK(format_report(cannot, Format::Github) == "::error title=anvil check::no plugin.toml in x\n");
        CHECK(parse_format("github") == Format::Github && !parse_format("xml"));
    });

    run("anvil.exe exit codes and arguments", [&] {
        CHECK(run_anvil(anvil, "--version").exit_code == 0);
        CHECK(contains(run_anvil(anvil, "--help").out, "check <plugin-dir> [--exe <eu4.exe>] [--format text|json|github]"));
        CHECK(run_anvil(anvil, "").exit_code == 2);
        CHECK(run_anvil(anvil, "frobnicate").exit_code == 2);

        const fs::path plain = fx.plugin("exe-plain", "");
        const Run ok = run_anvil(anvil, "check " + quoted(plain) + " --format=json");
        CHECK(ok.exit_code == 0 && contains(ok.out, "\"result\": \"ok\""));
        const fs::path good = fx.plugin("exe-good", kGoodSymbols);
        const Run no_lock = run_anvil(anvil, "check " + quoted(good) + " --format github");
        CHECK(no_lock.exit_code == 1 && contains(no_lock.out, "::error file=") && contains(no_lock.out, "symbols.lock is missing"));
        // anvil.exe knows only real game builds: the synthetic exe is unknown, but every symbol verifies.
        const Run unknown = run_anvil(anvil, "check " + quoted(good) + " --exe " + quoted(fx.exe));
        CHECK(unknown.exit_code == 1 && contains(unknown.out, "is not a game build this Anvil knows"));
        CHECK(contains(unknown.out, "1 problem(s)"));
        CHECK(run_anvil(anvil, "check " + quoted(fx.root / "missing")).exit_code == 2);
        CHECK(run_anvil(anvil, "check " + quoted(good) + " --format xml").exit_code == 2);
        CHECK(run_anvil(anvil, "check " + quoted(good) + " --exe").exit_code == 2);
        CHECK(run_anvil(anvil, "check " + quoted(good) + " --game " + quoted(fx.root)).exit_code == 2);
        CHECK(run_anvil(anvil, "check").exit_code == 2);

        const fs::path out = fx.root / "gen-out";
        const Run gen = run_anvil(anvil, "gen " + quoted(good / "symbols.toml") + " " + quoted(out));
        CHECK(gen.exit_code == 0 && fs::exists(out / "symbols.gen.h") && fs::exists(out / "symbols.gen.hpp"));
        CHECK(contains(gen.out, "symbols.gen.h: written"));
        CHECK(contains(run_anvil(anvil, "gen " + quoted(good / "symbols.toml") + " " + quoted(out)).out, "symbols.gen.hpp: unchanged"));
        const fs::path c_only = fx.root / "gen-c";
        CHECK(run_anvil(anvil, "gen --lang c " + quoted(good / "symbols.toml") + " " + quoted(c_only)).exit_code == 0);
        CHECK(fs::exists(c_only / "symbols.gen.h") && !fs::exists(c_only / "symbols.gen.hpp"));
        const fs::path rust_only = fx.root / "gen-rust";
        const Run rust = run_anvil(anvil, "gen --lang rust " + quoted(good / "symbols.toml") + " " + quoted(rust_only));
        CHECK(rust.exit_code == 0 && contains(rust.out, "symbols.gen.rs: written"));
        CHECK(fs::exists(rust_only / "symbols.gen.rs") && !fs::exists(rust_only / "symbols.gen.h") &&
              !fs::exists(rust_only / "symbols.gen.hpp"));
        CHECK(!fs::exists(out / "symbols.gen.rs"));  // without --lang: C and C++ only
        CHECK(contains(run_anvil(anvil, "gen --lang rust " + quoted(good / "symbols.toml") + " " + quoted(rust_only)).out,
                       "symbols.gen.rs: unchanged"));
        CHECK(run_anvil(anvil, "gen --lang go " + quoted(good / "symbols.toml") + " " + quoted(out)).exit_code == 2);
        write(fx.root / "warn" / "symbols.toml",
              "build = \"b\"\n[symbol.f]\nexport = \"F\"\nsignature = \"Root ()\"\n");
        const Run warned =
            run_anvil(anvil, "gen --lang rust " + quoted(fx.root / "warn" / "symbols.toml") + " " + quoted(fx.root / "warn"));
        CHECK(warned.exit_code == 0 && contains(warned.out, "symbols.toml:2: warning: symbol f: result (Root)"));
        CHECK(run_anvil(anvil, "gen " + quoted(fx.root / "none.toml") + " " + quoted(out)).exit_code == 2);
        CHECK(run_anvil(anvil, "gen " + quoted(good / "symbols.toml")).exit_code == 2);
        write(fx.root / "kw" / "symbols.toml", symbol("new", "rva = 0x10\nbytes = \"48\"\nmemory = \"code\"\n"));
        const Run keyword = run_anvil(anvil, "gen " + quoted(fx.root / "kw" / "symbols.toml") + " " + quoted(fx.root / "kw"));
        CHECK(keyword.exit_code == 1 && contains(keyword.out, "symbols.toml:3: error: symbol new: 'new' is a C++ keyword"));
        write(fx.root / "bad" / "symbols.toml", "build = \"b\"\nbogus = 1\n");
        const Run bad = run_anvil(anvil, "gen " + quoted(fx.root / "bad" / "symbols.toml") + " " + quoted(fx.root / "bad"));
        CHECK(bad.exit_code == 1 && contains(bad.out, "symbols.toml:2: error: unknown key 'bogus'"));

        // settings.toml, by name or by its [setting.<name>] tables, with symbols.toml in one call.
        const std::string options = "[setting.mode]\ntype = \"enum\"\nvalues = [{ name = \"off\" }]\ndefault = \"off\"\n"
                                    "label = \"Mode\"\n";
        write(fx.root / "set" / "settings.toml", options);
        write(fx.root / "set" / "options.toml", "# options\n" + options);
        const fs::path set_out = fx.root / "gen-set";
        const Run both = run_anvil(anvil, "gen " + quoted(good / "symbols.toml") + " " + quoted(fx.root / "set" / "settings.toml") +
                                              " " + quoted(set_out));
        CHECK(both.exit_code == 0 && contains(both.out, "settings.gen.hpp: written") && fs::exists(set_out / "symbols.gen.h"));
        CHECK(contains(read(set_out / "settings.gen.h"), "CFG_MODE_OFF = 0,"));
        const fs::path rust_set = fx.root / "gen-set-rust";
        CHECK(run_anvil(anvil, "gen --lang rust " + quoted(good / "symbols.toml") + " " + quoted(fx.root / "set" / "settings.toml") +
                                   " " + quoted(rust_set))
                  .exit_code == 0);
        CHECK(contains(read(rust_set / "settings.gen.rs"), "pub const MODE_OFF: i64 = 0;") &&
              fs::exists(rust_set / "symbols.gen.rs") && !fs::exists(rust_set / "settings.gen.h"));
        const fs::path by_content = fx.root / "gen-options";
        CHECK(run_anvil(anvil, "gen --lang cpp " + quoted(fx.root / "set" / "options.toml") + " " + quoted(by_content)).exit_code == 0);
        CHECK(fs::exists(by_content / "settings.gen.hpp") && !fs::exists(by_content / "settings.gen.h"));
        CHECK(run_anvil(anvil, "gen " + quoted(fx.root / "set" / "settings.toml") + " " + quoted(fx.root / "set" / "options.toml") +
                                   " " + quoted(set_out))
                  .exit_code == 2);
        write(fx.root / "set-bad" / "settings.toml", "[setting.x]\ntype = \"int\"\ndefault = 1\nlabel = \"X\"\nmin = 2\n");
        const Run set_bad = run_anvil(anvil, "gen " + quoted(fx.root / "set-bad" / "settings.toml") + " " + quoted(fx.root / "set-bad"));
        CHECK(set_bad.exit_code == 1 && contains(set_bad.out, "settings.toml:3: error: setting x: default: 1 is below the minimum 2"));
        CHECK(!fs::exists(fx.root / "set-bad" / "settings.gen.h"));
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
