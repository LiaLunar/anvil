#pragma once

#include "core/settings.hpp"
#include "core/symbols.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace anvil::cli {

// Something gen could not translate faithfully (design §16); the output is
// still written.
struct GenWarning {
    std::string message;
    uint32_t line = 0;  // the line of the .toml; 0 = the whole file
};

// The generated headers for one symbols.toml or settings.toml (design §16). A
// header is empty when it was not asked for or when `error` is set.
struct GeneratedHeaders {
    std::string c;     // symbols.gen.h / settings.gen.h
    std::string cpp;   // symbols.gen.hpp / settings.gen.hpp
    std::string rust;  // symbols.gen.rs / settings.gen.rs
    std::string error;
    uint32_t line = 0;  // the line of the .toml the error is about; 0 = the whole file
    std::vector<GenWarning> warnings;
};

// symbols.gen.h (C): SYM_<NAME> ids, sym_<name>_fn function pointer types
// and SYMVAL_<SYMBOL>_<VALUE> constants.
// symbols.gen.hpp (C++): namespace sym with an anvil::Function, Site, Slot or
// Data constant per symbol (include/anvil/anvil_symbols.hpp), values as
// static constexpr members, and an anvil::Data<T> for every typed value.
// symbols.gen.rs (Rust): the ids as `pub const <NAME>: u32` (anvil_sys::AnvilSymbol),
// `<Name>Fn` types (`unsafe extern "C" fn`) from `signature` and the values as
// `pub const <SYMBOL>_<VALUE>: u64`. Pointers, integers, floating point and
// bool translate; any other type passed by value becomes `*mut c_void` with a
// warning (a pointer to a type gen does not know is an opaque pointer, quietly).
// `source` names symbols.toml in the header comment.
GeneratedHeaders generate_headers(const SymbolFile& file, const std::string& source, bool c, bool cpp, bool rust = false);

// The headers for one settings.toml (design §16): `c` is settings.gen.h,
// `cpp` settings.gen.hpp, `line` a line of settings.toml.
// settings.gen.h (C): CFG_<NAME> ids for host->setting_*(self, id),
// CFG_<NAME>_<OPTION> indices of enum options, and ANVIL_KEY_* for key codes.
// settings.gen.hpp (C++): namespace cfg with an anvil::Setting<T> constant per
// setting (cfg::<name>.get(), include/anvil/anvil.hpp) whose type
// cfg::Types::<name> holds setting_name, restart, default_value, the limits,
// and for an enum the options of its enum class cfg::Types::<name>_value.
// settings.gen.rs (Rust): `pub const <NAME>: u32` ids (anvil_sys::AnvilSetting),
// `<NAME>_<OPTION>: i64` indices of enum options and KEY_* modifier flags.
GeneratedHeaders generate_settings_headers(const SettingsFile& file, const std::string& source, bool c, bool cpp,
                                           bool rust = false);

}  // namespace anvil::cli
