# anvil-sdk

Safe Rust API for plugins of [Anvil](https://github.com/happy-dwarf-team/anvil),
the native plugin loader for Europa Universalis IV 1.37.5. The package is
`anvil-sdk`; the library is `anvil`, so code says `anvil::…`. It sits on the raw
bindings of [`anvil-sys`](https://crates.io/crates/anvil-sys) and depends on
nothing else.

```toml
[lib]
crate-type = ["cdylib"]

[dependencies]
anvil-sdk = "0.1"
```

```rust
use anvil::{Host, Plugin};
use anvil::game::{self, SessionChange};
use anvil::world::World;

struct Hello;

impl Plugin for Hello {
    fn load(host: &mut Host) -> anvil::Result<Self> {
        let world = World::get(host);
        let log = *host;
        host.subscribe(game::SESSION, move |session| {
            if session.change != SessionChange::Started {
                return;
            }
            // anvil-game events run on the game's main thread: the world may be read.
            if let Some(tag) = world.and_then(|w| w.player().ok().flatten()) {
                log.info(format_args!("session {}: playing {tag}", session.number));
            }
        })?;
        Ok(Hello)
    }
}

anvil::plugin!(Hello);
```

What it gives over the C ABI:

- `anvil::plugin!(Type)` exports `anvil_plugin_load`. `Plugin::load` returns
  the plugin or an `anvil::Error`; an error or a panic becomes the result Anvil
  rolls the plugin back on.
- Every callback runs behind an adapter with `catch_unwind`: a panic in a mid
  hook, block replacement, event, setting or click handler, or in work for the
  main thread, is logged once per callback and never crosses the FFI.
- `Change` removes its change on `Drop`; `keep()` keeps it for the process.
  `FunctionHook<sym::NameFn>` lives in a `static`, where Anvil writes the rest
  of the chain.
- Typed symbols and settings from `anvil gen --lang rust` (`symbols.gen.rs`,
  `settings.gen.rs`, written by a build script).
- Typed wrappers for the services Anvil ships: `anvil::game`
  (`anvil-game.game` and its events), `anvil::world` (`anvil-game.world`),
  `anvil::commands` (`anvil-game.commands`),
  `anvil::panel` (`anvil-overlay.panel`), `anvil::benchmark`
  (`anvil.benchmark`) and `anvil::map` (`anvil-map.modes` and its events). A
  function a running service is too old to have answers
  `ErrorCode::Unsupported`.

Host and service calls are safe. What reaches the game's memory by address, or
puts raw bytes and pointers into it, stays an `unsafe fn` with its contract:
calling a hook's `next` (the generated function types are `unsafe extern "C"
fn`), `Host::function`, `Host::data`, writing the mid-hook context
(`Cpu::regs_mut`), byte patches, pointer swaps, call redirects, import
replacements and changes at raw addresses.

Examples in the Anvil repository: `examples/hello-rust` (a function hook, a
mid hook and an event on declared symbols) and `examples/panel-rust` (the
player's country from the world service in an overlay panel, with no game
addresses).

Versions: an own `0.x` line. Anvil's source compatibility (ADR 0001) covers
the crate from its 1.0, which comes after the first real Rust plugin. A new
Anvil service, or a function appended to one, gets its wrapper here in the
same Anvil pull request as its C header.

License: MIT.
