//! Panics never cross the FFI: every adapter runs the plugin's code under
//! `catch_unwind`. The panic hook keeps the message (with its place) for the
//! adapter that catches it, and logs a panic no adapter catches.

use std::any::Any;
use std::cell::{Cell, RefCell};
use std::panic::{self as std_panic, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Once;

use crate::host::{Host, LogLevel};

thread_local! {
    static CATCHING: Cell<u32> = const { Cell::new(0) };
    static MESSAGE: RefCell<Option<String>> = const { RefCell::new(None) };
}

static HOOK: Once = Once::new();

/// Installs the panic hook once (at load). Each plugin DLL has its own copy of
/// the standard library, so the hook is this plugin's only.
pub(crate) fn install_hook() {
    HOOK.call_once(|| {
        // A panic no adapter catches also goes to the hook before (stderr).
        let previous = std_panic::take_hook();
        std_panic::set_hook(Box::new(move |info| {
            let text = match info.location() {
                Some(at) => format!(
                    "{} at {}:{}",
                    payload_text(info.payload()),
                    at.file(),
                    at.line()
                ),
                None => payload_text(info.payload()),
            };
            // try_with: a thread that is ending has no thread-locals left.
            if CATCHING.try_with(Cell::get).unwrap_or(0) > 0 {
                let _ = MESSAGE.try_with(|message| *message.borrow_mut() = Some(text));
            } else {
                if let Some(host) = Host::current() {
                    host.log(LogLevel::Error, format_args!("panic: {text}"));
                }
                previous(info);
            }
        }));
    });
}

fn payload_text(payload: &(dyn Any + Send)) -> String {
    if let Some(text) = payload.downcast_ref::<&str>() {
        (*text).to_owned()
    } else if let Some(text) = payload.downcast_ref::<String>() {
        text.clone()
    } else {
        "a panic without a message".to_owned()
    }
}

/// Runs `body`; a panic comes back as its message (and place).
pub(crate) fn catch<R>(body: impl FnOnce() -> R) -> Result<R, String> {
    catch_raw(body).map_err(|payload| {
        let text = MESSAGE
            .with(|message| message.borrow_mut().take())
            .unwrap_or_else(|| payload_text(&*payload));
        // Dropping a payload may panic in turn; never let that out.
        let _ = std_panic::catch_unwind(AssertUnwindSafe(move || drop(payload)));
        text
    })
}

/// Runs `body` with the hook keeping the message; the payload comes back.
pub(crate) fn catch_raw<R>(body: impl FnOnce() -> R) -> std::thread::Result<R> {
    CATCHING.with(|n| n.set(n.get() + 1));
    let result = std_panic::catch_unwind(AssertUnwindSafe(body));
    CATCHING.with(|n| n.set(n.get() - 1));
    result
}

/// The adapters' rule: a panic is logged once per callback (`logged` is the
/// callback's flag), and `fallback` is answered instead of unwinding.
pub(crate) fn run_callback<R>(
    what: &str,
    logged: &AtomicBool,
    fallback: R,
    body: impl FnOnce() -> R,
) -> R {
    match catch(body) {
        Ok(value) => value,
        Err(text) => {
            if !logged.swap(true, Ordering::AcqRel) {
                let _ = catch(|| {
                    if let Some(host) = Host::current() {
                        host.log(
                            LogLevel::Error,
                            format_args!(
                                "{what}: the callback panicked ({text}); the game carries on, and later panics of this callback are not logged"
                            ),
                        );
                    }
                });
            }
            fallback
        }
    }
}

/// Runs `body`; a panic is logged as "`what` panicked (…)" and `fallback` is
/// returned instead of unwinding. For function-hook detours and other plain
/// functions the game calls directly, where no adapter of this crate stands
/// between: a panic out of an `extern "C"` function aborts the game.
pub fn catch_panic<R>(what: &str, fallback: R, body: impl FnOnce() -> R) -> R {
    match catch(body) {
        Ok(value) => value,
        Err(text) => {
            let _ = catch(|| {
                if let Some(host) = Host::current() {
                    host.log(
                        LogLevel::Error,
                        format_args!("{what} panicked ({text}); carrying on"),
                    );
                }
            });
            fallback
        }
    }
}
