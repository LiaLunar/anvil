//! `anvil-overlay.input`: keys and mouse buttons for plugins, from the game
//! window's procedure, held back while the player types in the game or uses
//! Anvil's panel (`anvil_input.h`, anvil-overlay 1.18.0).
//!
//! Depend on the overlay in plugin.toml (`[[dependencies]] id =
//! "anvil-overlay"`, `optional = true` for a plugin that also works without
//! it) and look the service up with [`Input::get`]. A code is a [`Key`]: what
//! a setting of type `key` holds, a mouse button's virtual key with the same
//! modifiers, or `Key(0)` for none.
//!
//! The first binding of a code keeps it: a second one is
//! [`ErrorCode::Conflict`] and stays inert until it gets a free code. The
//! handler runs on the game's main thread after the next outer frame, so it
//! may read [`World`](crate::world::World). Without anvil-game,
//! [`Input::bind`] is [`ErrorCode::Unsupported`].
//!
//! [`ErrorCode::Conflict`]: crate::ErrorCode::Conflict
//! [`ErrorCode::Unsupported`]: crate::ErrorCode::Unsupported

use std::ffi::c_void;
use std::fmt;
use std::ptr::{self, NonNull};
use std::sync::atomic::AtomicBool;

use crate::error::{check, Error, ErrorCode, Result};
use crate::event::read_sized;
use crate::host::{Host, Key};
use crate::panic::run_callback;
use crate::sys::{
    AnvilInputBinding, AnvilInputEvent, AnvilInputService, ANVIL_INPUT_CONSUME,
    ANVIL_INPUT_EVENT_REPEAT, ANVIL_INPUT_GAME_SHORTCUT, ANVIL_INPUT_GAME_TEXT,
    ANVIL_INPUT_GAME_UNKNOWN, ANVIL_INPUT_OVERLAY_KEYBOARD, ANVIL_INPUT_OVERLAY_MOUSE,
    ANVIL_INPUT_OVERLAY_PANEL, ANVIL_INPUT_REPEAT, ANVIL_INPUT_SERVICE_VERSION,
};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-overlay.input";
/// The service's major version.
pub const VERSION: u32 = ANVIL_INPUT_SERVICE_VERSION;

/// What holds input back now (`ANVIL_INPUT_*` of `state()`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct State(pub u32);

impl State {
    /// Anvil's panel is open.
    pub const OVERLAY_PANEL: u32 = ANVIL_INPUT_OVERLAY_PANEL;
    /// The overlay takes the keyboard.
    pub const OVERLAY_KEYBOARD: u32 = ANVIL_INPUT_OVERLAY_KEYBOARD;
    /// The mouse is over a window of the overlay.
    pub const OVERLAY_MOUSE: u32 = ANVIL_INPUT_OVERLAY_MOUSE;
    /// A text field of the game has focus.
    pub const GAME_TEXT: u32 = ANVIL_INPUT_GAME_TEXT;
    /// The game's shortcut editor waits for a key.
    pub const GAME_SHORTCUT: u32 = ANVIL_INPUT_GAME_SHORTCUT;
    /// anvil-game cannot say whether the two above hold.
    pub const GAME_UNKNOWN: u32 = ANVIL_INPUT_GAME_UNKNOWN;

    /// Whether keys are held back.
    pub const fn holds_keys(self) -> bool {
        self.0
            & (Self::OVERLAY_PANEL | Self::OVERLAY_KEYBOARD | Self::GAME_TEXT | Self::GAME_SHORTCUT)
            != 0
    }
    /// Whether mouse buttons are held back.
    pub const fn holds_mouse(self) -> bool {
        self.0 & Self::OVERLAY_MOUSE != 0
    }
}

/// How a binding treats a press.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct BindFlags {
    /// The game does not see the press (it sees the release).
    pub consume: bool,
    /// Also the auto-repeat of a held key.
    pub repeat: bool,
}

impl BindFlags {
    fn raw(self) -> u32 {
        (if self.consume { ANVIL_INPUT_CONSUME } else { 0 })
            | (if self.repeat { ANVIL_INPUT_REPEAT } else { 0 })
    }
}

/// A press of a binding's code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Press {
    /// The binding's code at the press.
    pub code: Key,
    /// An auto-repeat.
    pub repeat: bool,
    /// The cursor at the press, in the window's client pixels.
    pub x: i32,
    /// See `x`.
    pub y: i32,
    /// anvil-game's frame number at the press.
    pub frame: u64,
}

/// The `anvil-overlay.input` service.
#[derive(Clone, Copy)]
pub struct Input {
    table: &'static AnvilInputService,
}

impl fmt::Debug for Input {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Input")
            .field("size", &self.table.size)
            .finish()
    }
}

/// A binding. It is `Copy`: the overlay keeps it until the process ends.
#[derive(Clone, Copy)]
pub struct Binding {
    table: &'static AnvilInputService,
    raw: NonNull<AnvilInputBinding>,
}

// SAFETY: every input call may come from any thread (anvil_input.h, "Threads").
unsafe impl Send for Binding {}
// SAFETY: as above.
unsafe impl Sync for Binding {}

impl fmt::Debug for Binding {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Binding").field(&self.raw).finish()
    }
}

struct PressAdapter<F> {
    handler: F,
    panicked: AtomicBool,
}

unsafe extern "C" fn press_adapter<F: Fn(Press) + Send + Sync + 'static>(
    context: *mut c_void,
    event: *const AnvilInputEvent,
) {
    // SAFETY: the adapter bind registered, never freed.
    let adapter = unsafe { &*context.cast::<PressAdapter<F>>() };
    // SAFETY: the overlay's event, valid for the call, starting with its size.
    let Some(raw) = (unsafe { read_sized::<AnvilInputEvent>(event.cast()) }) else {
        return;
    };
    let press = Press {
        code: Key(raw.code),
        repeat: raw.flags & ANVIL_INPUT_EVENT_REPEAT != 0,
        x: raw.x,
        y: raw.y,
        frame: raw.frame,
    };
    run_callback("input handler", &adapter.panicked, (), || {
        (adapter.handler)(press)
    });
}

impl Input {
    /// The service; `None` without the overlay or with one older than 1.18.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-overlay.input version 1.
        let table = unsafe { host.service::<AnvilInputService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilInputService {
        self.table
    }

    /// What holds input back now; `None` when the service lacks the call.
    pub fn state(&self) -> Option<State> {
        // SAFETY: the service's function.
        function!(self.table, AnvilInputService, state)
            .ok()
            .map(|f| State(unsafe { f() }))
    }

    /// Binds `code` (`Key(0)` for none yet). A code another binding holds is
    /// [`ErrorCode::Conflict`], and the overlay's inert binding is dropped:
    /// use [`Input::bind_or_inert`] to keep it and give it a free code later.
    /// 256 bindings a process, a dropped one included.
    pub fn bind<F>(&self, code: Key, flags: BindFlags, handler: F) -> Result<Binding>
    where
        F: Fn(Press) + Send + Sync + 'static,
    {
        match self.bind_or_inert(code, flags, handler)? {
            (binding, None) => Ok(binding),
            (_, Some(conflict)) => Err(conflict),
        }
    }

    /// Binds `code` as [`Input::bind`] does, but keeps the binding when
    /// another one holds the code, as the C API does: it is inert (code 0)
    /// until [`Binding::set_code`] gives it a free one, such as from the key
    /// setting's callback once the player picks another key. The error is
    /// `Some` with [`ErrorCode::Conflict`] for such a binding, `None` when the
    /// binding holds `code`. Other errors make no binding.
    pub fn bind_or_inert<F>(
        &self,
        code: Key,
        flags: BindFlags,
        handler: F,
    ) -> Result<(Binding, Option<Error>)>
    where
        F: Fn(Press) + Send + Sync + 'static,
    {
        let f = function!(self.table, AnvilInputService, bind)?;
        // Never freed: a queued press may still run the handler after unbind.
        let adapter: &'static PressAdapter<F> = Box::leak(Box::new(PressAdapter {
            handler,
            panicked: AtomicBool::new(false),
        }));
        let mut raw: *mut AnvilInputBinding = ptr::null_mut();
        // SAFETY: an adapter that lives for the process and never unwinds, and an out pointer.
        let result = unsafe {
            f(
                code.0,
                flags.raw(),
                Some(press_adapter::<F>),
                ptr::from_ref(adapter).cast_mut().cast(),
                &mut raw,
            )
        };
        let conflict = match check(result, || format!("input bind {:#x}", code.0)) {
            Ok(()) => None,
            Err(error) if error.code() == ErrorCode::Conflict => Some(error),
            Err(error) => return Err(error),
        };
        let Some(raw) = NonNull::new(raw) else {
            // A conflict without the binding the header promises: nothing to keep.
            return Err(conflict
                .unwrap_or_else(|| Error::new(ErrorCode::Internal, "input bind: no binding")));
        };
        Ok((
            Binding {
                table: self.table,
                raw,
            },
            conflict,
        ))
    }

    /// Whether the code's key is down with at least its modifiers, and not
    /// held back: for what is polled every frame.
    pub fn held(&self, code: Key) -> bool {
        // SAFETY: the service's function.
        function!(self.table, AnvilInputService, held)
            .ok()
            .is_some_and(|f| unsafe { f(code.0) } != 0)
    }
}

impl Binding {
    /// The raw binding, for the C table.
    pub fn as_raw(&self) -> *mut AnvilInputBinding {
        self.raw.as_ptr()
    }

    /// Another code, such as after its setting changed; `Key(0)` suspends it.
    pub fn set_code(&self, code: Key) -> Result<()> {
        let f = function!(self.table, AnvilInputService, set_code)?;
        // SAFETY: the service's function with its binding.
        check(unsafe { f(self.raw.as_ptr(), code.0) }, || {
            format!("input set_code {:#x}", code.0)
        })
    }

    /// Ends the binding: its queued presses are not delivered.
    pub fn unbind(&self) -> Result<()> {
        let f = function!(self.table, AnvilInputService, unbind)?;
        // SAFETY: the service's function with its binding.
        check(unsafe { f(self.raw.as_ptr()) }, || "input unbind")
    }
}
