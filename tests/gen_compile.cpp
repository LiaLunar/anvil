// Compiles the C++ headers `anvil gen` writes for tests/fixtures/gen/symbols.toml
// (the design §5.2 example) and settings.toml (every type) and checks what
// they declare at compile time. gen_compile.c does the same for the C headers.
struct Root;  // types named in symbols.toml are declared before the header

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#include <anvil/anvil_benchmark.hpp>

#include <cstdio>
#include <string_view>
#include <type_traits>

static_assert(sym::speed_object.id == 0 && sym::physfs_get_write_dir.id == 4);
static_assert(std::is_same_v<decltype(sym::speed_object)::Signature, void*(void*)>);
static_assert(std::is_same_v<decltype(sym::physfs_get_write_dir)::Pointer, const char* (*)()>);
static_assert(std::is_base_of_v<anvil::Site, sym::Types::person_name_block>);
static_assert(std::is_base_of_v<anvil::Slot, sym::Types::frontend_tick_slot>);
static_assert(std::string_view(sym::frontend_tick_slot.symbol_name) == "frontend_tick_slot");
static_assert(std::is_base_of_v<anvil::Site, sym::Types::root_and_date> && sym::root_and_date.id == 3);
static_assert(sym::root_and_date.root == 0x233FE78 && sym::root_and_date.date_offset == 0x1DD0);
static_assert(sym::digest_site.offset == 0x418);  // a digest gives no bytes: the constant comes from `is`
static_assert(std::is_same_v<decltype(sym::root)::Type, Root> && sym::root.id == 3);
static_assert(std::string_view(sym::root.value) == "root");

// The anvil.benchmark wrapper: fields at compile time, set() by value type.
constexpr const char* kBenchModes[] = {"off", "on"};
constexpr AnvilBenchmarkField kBenchMode = anvil::Benchmark::enum_field("warmup.mode", kBenchModes, "Mode");
static_assert(kBenchMode.kind == ANVIL_BENCHMARK_ENUM && kBenchMode.option_count == 2);
static_assert(anvil::Benchmark::int_field("warmup.opened", ANVIL_BENCHMARK_UNIT_COUNT).unit == ANVIL_BENCHMARK_UNIT_COUNT);
static_assert(anvil::Benchmark::flag_field("saved").kind == ANVIL_BENCHMARK_FLAG);
static_assert(requires(const anvil::Benchmark b) {
    { b.set("a", 1) } -> std::same_as<AnvilResult>;
    { b.set("a", 1.5) } -> std::same_as<AnvilResult>;
    { b.set("a", true) } -> std::same_as<AnvilResult>;
    { b.set_enum("a", "on") } -> std::same_as<AnvilResult>;
});

static_assert(cfg::enabled.id == 0 && cfg::panel_key.id == 5);
static_assert(std::is_same_v<decltype(cfg::enabled)::Value, bool> && cfg::enabled.default_value && !cfg::enabled.restart);
static_assert(std::is_same_v<decltype(cfg::budget_mb)::Value, std::int64_t> && cfg::budget_mb.restart);
static_assert(cfg::budget_mb.default_value == 1024 && cfg::budget_mb.minimum == 64 && cfg::budget_mb.maximum == 8192);
static_assert(std::is_same_v<decltype(cfg::scale)::Value, double>);
static_assert(cfg::scale.default_value == 1.0 && cfg::scale.minimum == 0.5 && cfg::scale.maximum == 2.5);
static_assert(std::is_same_v<decltype(cfg::mode)::Value, cfg::Types::mode_value>);
static_assert(cfg::mode.default_value == cfg::mode.verify && static_cast<int>(cfg::mode.on) == 2);
static_assert(std::is_same_v<decltype(cfg::greeting)::Value, std::string>);
static_assert(std::string_view(cfg::greeting.default_value) == "Hello, \"world\"?? \\ é */");
static_assert(cfg::panel_key.default_value == anvil::Key{0x37B} && cfg::panel_key.default_value.ctrl() &&
              cfg::panel_key.default_value.shift() && !cfg::panel_key.default_value.alt() &&
              cfg::panel_key.default_value.vk() == 0x7B);
static_assert(std::string_view(cfg::Types::panel_key::setting_name) == "panel_key");

extern "C" int c_last_symbol();   // gen_compile.c
extern "C" int c_last_setting();  // gen_compile.c

int main() {
    if (c_last_symbol() != 4 || c_last_setting() != static_cast<int>(cfg::panel_key.id)) {
        std::printf("the C header disagrees with the C++ header\n");
        return 1;
    }
    std::printf("generated headers compile as C and C++\n");
    return 0;
}
