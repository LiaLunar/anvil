# Anvil SDK

Everything a plugin repository needs to build an [Anvil](https://github.com/happy-dwarf-team/anvil)
plugin for Europa Universalis IV 1.37.5 (plugin API 2), for Windows x64 with
MSVC (Visual Studio 2022) and CMake 3.25 or newer.

| Path | What |
| --- | --- |
| `include/anvil/anvil_plugin.h` | the plugin API (plain C11) |
| `include/anvil/anvil.hpp` | the C++20 wrapper over it |
| `include/anvil/anvil_symbols.hpp` | the types `symbols.gen.hpp` is made of |
| `include/anvil/anvil_game.h` | the service and events of the standard plugin `anvil-game` (C11) |
| `include/anvil/anvil_benchmark.h` | `anvil.benchmark` v1, the core's service for plugin fields in the start-up benchmark (C11, Anvil 1.4 and later) |
| `include/anvil/anvil_benchmark.hpp` | the C++20 wrapper over it |
| `bin/anvil.exe` | `anvil gen`, `anvil check` and `anvil port` |
| `cmake/AnvilPluginConfig.cmake` | the `AnvilPlugin` CMake package |
| `third-party/` | licenses of the code compiled into `anvil.exe` |

## Use

In the plugin repository's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_plugin LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(anvil_sdk
    URL https://github.com/happy-dwarf-team/anvil/releases/download/v<version>/anvil-sdk-windows-x64.zip
    URL_HASH SHA256=<hash from SHA256SUMS.txt of the release>)
FetchContent_MakeAvailable(anvil_sdk)

anvil_add_plugin(my_plugin SOURCES src/plugin.cpp)
```

With an unpacked archive, `find_package(AnvilPlugin REQUIRED)` does the same:
configure with `-DAnvilPlugin_DIR=<sdk>/cmake` (or put the folder in
`CMAKE_PREFIX_PATH`).

`anvil_add_plugin(<target> SOURCES <files>...)` reads `plugin.toml` next to the
`CMakeLists.txt` (`id`, `entry`, `symbols`, `settings`, `api`), then:

- runs `anvil gen` on `symbols.toml` at build time (`symbols.gen.h`, `symbols.gen.hpp`)
  and on `settings.toml` (`settings.gen.h`, `settings.gen.hpp`: `CFG_<NAME>` in C,
  `cfg::<name>.get()` in C++),
- builds `<entry>.dll` with the static CRT, C++20 (C11 for C sources) and `/W4`,
- lays out `<build>/plugin/<id>/` with `plugin.toml`, `symbols.toml`, `settings.toml`
  (when the manifest names one) and the DLL, the same for every configuration.

It also adds:

- `<target>-check`: `anvil check <plugin folder>`. Without `EU4_EXE` it checks the
  schemas and that `symbols.lock` is current (what CI can do without the game). With
  `-DEU4_EXE=<path to eu4.exe>` it verifies the symbols against that game and
  rewrites `symbols.lock`; commit the lock.
- `<target>-deploy`, when `EU4_DIR` is set: copies the plugin folder to
  `<EU4_DIR>/anvil/plugins/<id>`.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEU4_EXE="C:/…/eu4.exe" -DEU4_DIR="C:/…/Europa Universalis IV"
cmake --build build --config Release
cmake --build build --config Release --target my_plugin-check
cmake --build build --config Release --target my_plugin-deploy
```

CI: build, then `anvil check <plugin folder> --format github` (from
`bin/anvil.exe`) prints annotations on the repository's files.

## Rust

A Rust plugin is a `cdylib` that depends on the
[`anvil-sys`](https://crates.io/crates/anvil-sys) crate (the raw API, the same version
as the release) and exports `anvil_plugin_load`. `bin/anvil.exe gen --lang rust
symbols.toml <out-dir>` writes `symbols.gen.rs`: symbol ids, a function type per
`signature` and the values. `examples/hello-rust` in the repository (and in the
release package, under `anvil/docs/examples`) shows it with a `build.rs`; link the C
runtime statically (`-C target-feature=+crt-static`) and catch panics at every
function Anvil or the game calls. See
[docs/design.md §9](../docs/design.md#9-plugin-api-2).

Release assets come with `SHA256SUMS.txt` and a build attestation
(`gh attestation verify anvil-sdk-windows-x64.zip -R happy-dwarf-team/anvil`).
The archive is tied to the Anvil release it comes from; a plugin whose
`plugin.toml` says another `api` is refused at configure time.
