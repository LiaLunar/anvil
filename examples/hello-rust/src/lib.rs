//! Hello in Rust: a function hook and a mid hook on declared symbols and an
//! event subscription, with the safe `anvil` crate (`anvil-sdk`) and the
//! symbols that `anvil gen --lang rust` writes from `symbols.toml`.
//!
//! Host calls are safe. What stays `unsafe` is what reaches the game's code:
//! the detour is a plain function the game calls, and calling the rest of
//! its chain (`next`) is an `unsafe` call of the game's function.
#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::c_void;
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};

use anvil::{Change, FunctionHook, Host, Plugin};

/// Symbol ids and function types from `symbols.toml` (`anvil gen --lang rust`, see build.rs).
#[allow(dead_code)]
mod sym {
    include!(concat!(env!("OUT_DIR"), "/symbols.gen.rs"));
}

/// The hook of speed_object: its type is the generated one, so a detour of
/// another shape does not compile.
static SPEED_OBJECT: FunctionHook<sym::SpeedObjectFn> = FunctionHook::new();
static FIRST_SPEED_OBJECT: AtomicBool = AtomicBool::new(true);

/// The game's speed_object: pass the call on and log the first result. The
/// game calls it directly, so the logging runs under `catch_panic`.
unsafe extern "C" fn on_speed_object(session: *mut c_void) -> *mut c_void {
    let Some(next) = SPEED_OBJECT.next() else {
        return ptr::null_mut();
    };
    // SAFETY: the rest of the chain, a function with the declared signature,
    // called with the game's own argument.
    let speed = unsafe { next(session) };
    if FIRST_SPEED_OBJECT.swap(false, Ordering::Relaxed) {
        anvil::catch_panic("speed_object hook", (), || {
            if let Some(host) = Host::current() {
                host.info(format_args!("first game-speed object: {speed:p}"));
            }
        });
    }
    speed
}

struct Hello {
    // The mid hook lives as long as the plugin, which Anvil keeps.
    _idle_exit: Change,
}

impl Plugin for Hello {
    fn load(host: &mut Host) -> anvil::Result<Self> {
        host.info(format_args!(
            "hello from Rust on Anvil {}, game build {}",
            host.anvil_version(),
            host.game().build_id.unwrap_or("unknown")
        ));
        // Staged now, enabled with the rest when load returns Ok; an error
        // returned here rolls back everything staged so far.
        host.hook(&SPEED_OBJECT, sym::SPEED_OBJECT, on_speed_object)?;
        let log = *host;
        let idle_exit = host.mid_hook(sym::IDLE_EXIT, move |cpu| {
            log.info(format_args!("returning to the menu (rcx={:#x})", cpu.rcx));
        })?;
        host.subscribe(anvil::PLUGINS_LOADED, move |_| {
            log.info("every plugin is loaded")
        })?;
        Ok(Hello {
            _idle_exit: idle_exit,
        })
    }
}

anvil::plugin!(Hello);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_host_is_a_result_not_an_unwind() {
        // SAFETY: a null host is refused before anything is read.
        let result = unsafe { anvil_plugin_load(ptr::null(), ptr::null_mut()) };
        assert_eq!(result, anvil::sys::ANVIL_ERROR_INVALID_ARGUMENT);
    }

    #[test]
    fn symbol_ids_follow_symbols_toml() {
        assert_eq!(sym::SPEED_OBJECT, 0);
        assert_eq!(sym::IDLE_EXIT, 1);
    }
}
