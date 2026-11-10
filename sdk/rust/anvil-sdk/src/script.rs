//! The services of the standard plugin anvil-script (`anvil_script.h`).
//!
//! **`anvil-script.effects`** ([`Effects`], ADR 0008): effects of the
//! plugin's own in EU4's script language. A mod writes `chisel_set_terrain =
//! mountain` in an event option, a decision or a scripted effect; the game
//! parses it as one of its own effects and, when it runs it, calls the
//! plugin's handler with the value and the scope. Only an early plugin
//! (`phase = "early"`) with `affects_gameplay = true` registers, and only in
//! its [`Plugin::load`](crate::Plugin), before the game parses its first
//! effect. Keys are `<owner>_<name>`: the plugin's id with `-` as `_`, lower
//! case. The handlers run where the game runs its own effects and tooltips
//! (the main thread, inside a command or the day), where
//! [`World`](crate::world::World) answers `WrongPhase`. In multiplayer every
//! client runs its own copy of the effect, so `execute` must be
//! deterministic. While Anvil suspends the plugin, its effects do nothing.
//!
//! **`anvil-script.day_end`** ([`DayEnd`], ADR 0009): a callback at the Day
//! end, once a day after the game has finished the day (with its month and
//! year), on the main thread, when no worker thread of the day runs, before
//! the game compares checksums. Only a plugin with `affects_gameplay = true`
//! subscribes, and only before the first Day end of the process: in its
//! `Plugin::load`. The handler runs inside the game's day command, where
//! `World` answers `WrongPhase` too, must be deterministic for multiplayer,
//! and is not called while Anvil suspends the plugin.
//!
//! Depend on anvil-script in plugin.toml (`[[dependencies]] id =
//! "anvil-script"`, `version = "^1.16"` for the effects, `"^1.18"` for the Day
//! end) and look the services up with [`Effects::get`] and [`DayEnd::get`].
//!
//! ```no_run
//! use anvil::script::{Call, Effects};
//! # fn load(host: &anvil::Host) -> anvil::Result<()> {
//! let effects = Effects::get(host).expect("anvil-script.effects");
//! effects.register_with_tooltip(
//!     host,
//!     "hello_script_log",
//!     |call: &Call<'_>| {
//!         if let Some(host) = anvil::Host::current() {
//!             host.log(anvil::LogLevel::Info, format_args!("{} in {:?}", call.value, call.province));
//!         }
//!     },
//!     |call: &Call<'_>| Some(format!("Writes \"{}\" to anvil.log", call.value)),
//! )?;
//! # Ok(())
//! # }
//! ```

use std::borrow::Cow;
use std::ffi::{c_char, c_void, CStr, CString};
use std::fmt;
use std::ptr;
use std::sync::atomic::AtomicBool;

use crate::error::{check, Error, ErrorCode, Result};
use crate::host::Host;
use crate::panic::run_callback;
use crate::sys::{
    AnvilGameTag, AnvilScriptCall, AnvilScriptDay, AnvilScriptDayEndService, AnvilScriptEffect,
    AnvilScriptEffectsService, ANVIL_SCRIPT_DAY_END_SERVICE_VERSION, ANVIL_SCRIPT_DAY_NEW_MONTH,
    ANVIL_SCRIPT_DAY_NEW_YEAR, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION,
};
use crate::Tag;

/// The Day end service's name.
pub const SERVICE: &CStr = c"anvil-script.day_end";
/// The Day end service's major version.
pub const VERSION: u32 = ANVIL_SCRIPT_DAY_END_SERVICE_VERSION;

/// The effects service's name.
pub const EFFECTS_SERVICE: &CStr = c"anvil-script.effects";
/// The effects service's major version.
pub const EFFECTS_VERSION: u32 = ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION;

// ---- the effects --------------------------------------------------------------

/// One execution or description of one effect: valid only during the call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Call<'a> {
    /// The registered key, e.g. `chisel_set_terrain`.
    pub key: &'a str,
    /// The right-hand side as the file has it, without quotes: `mountain`,
    /// `yes`. Bytes that are not UTF-8 (a file in the game's own code page)
    /// come as U+FFFD.
    pub value: Cow<'a, str>,
    /// The scope's province ID; `None` when the scope has none.
    pub province: Option<i32>,
    /// The scope's country; `None` when the scope has none.
    pub country: Option<Tag>,
}

impl<'a> Call<'a> {
    /// The call anvil-script passes, `None` for a NULL pointer or a key.
    ///
    /// # Safety
    /// `raw` is NULL or points to a call whose strings are NUL-terminated and
    /// outlive `'a`.
    unsafe fn from_raw(raw: *const AnvilScriptCall) -> Option<Self> {
        // SAFETY: the caller's pointer, NULL or valid for the call.
        let raw = unsafe { raw.as_ref() }?;
        if raw.key.is_null() {
            return None;
        }
        // SAFETY: NUL-terminated strings anvil-script keeps for the call.
        let key = unsafe { CStr::from_ptr(raw.key) }.to_str().ok()?;
        let value = if raw.value.is_null() {
            Cow::Borrowed("")
        } else {
            // SAFETY: as above.
            unsafe { CStr::from_ptr(raw.value) }.to_string_lossy()
        };
        let province = (raw.scope.province != 0).then_some(raw.scope.province);
        let country = Tag::from_raw(&AnvilGameTag {
            text: raw.scope.country,
        });
        Some(Self {
            key,
            value,
            province,
            country,
        })
    }
}

/// The `anvil-script.effects` service.
#[derive(Clone, Copy)]
pub struct Effects {
    table: &'static AnvilScriptEffectsService,
}

impl fmt::Debug for Effects {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Effects")
            .field("size", &self.table.size)
            .finish()
    }
}

struct EffectAdapter<E, D> {
    execute: E,
    describe: Option<D>,
    execute_panicked: AtomicBool,
    describe_panicked: AtomicBool,
}

unsafe extern "C" fn execute_adapter<E, D>(context: *mut c_void, call: *const AnvilScriptCall)
where
    E: Fn(&Call<'_>) + Send + Sync + 'static,
    D: Fn(&Call<'_>) -> Option<String> + Send + Sync + 'static,
{
    // SAFETY: the adapter register leaked, never freed.
    let adapter = unsafe { &*context.cast::<EffectAdapter<E, D>>() };
    // SAFETY: anvil-script's call, valid for this call.
    let Some(call) = (unsafe { Call::from_raw(call) }) else {
        return;
    };
    run_callback("script effect", &adapter.execute_panicked, (), || {
        (adapter.execute)(&call)
    });
}

/// Writes the first line of `line` into the caller's buffer, cut at a whole
/// character, and gives the whole line's length in bytes, as `snprintf` does.
///
/// # Safety
/// `utf8` has room for `capacity` bytes, or `capacity` is 0.
unsafe fn write_line(line: &str, utf8: *mut c_char, capacity: u32) -> u32 {
    let end = line.find(['\n', '\r', '\0']).unwrap_or(line.len());
    let line = &line[..end];
    if capacity > 0 && !utf8.is_null() {
        let mut length = line.len().min(capacity as usize - 1);
        while !line.is_char_boundary(length) {
            length -= 1;
        }
        // SAFETY: `length` < `capacity` bytes into the caller's buffer.
        unsafe {
            ptr::copy_nonoverlapping(line.as_ptr(), utf8.cast::<u8>(), length);
            *utf8.add(length) = 0;
        }
    }
    u32::try_from(line.len()).unwrap_or(u32::MAX)
}

unsafe extern "C" fn describe_adapter<E, D>(
    context: *mut c_void,
    call: *const AnvilScriptCall,
    utf8: *mut c_char,
    capacity: u32,
) -> u32
where
    E: Fn(&Call<'_>) + Send + Sync + 'static,
    D: Fn(&Call<'_>) -> Option<String> + Send + Sync + 'static,
{
    // SAFETY: the adapter register leaked, never freed.
    let adapter = unsafe { &*context.cast::<EffectAdapter<E, D>>() };
    let Some(describe) = adapter.describe.as_ref() else {
        return 0;
    };
    // SAFETY: anvil-script's call, valid for this call.
    let Some(call) = (unsafe { Call::from_raw(call) }) else {
        return 0;
    };
    run_callback(
        "script effect tooltip",
        &adapter.describe_panicked,
        0,
        || {
            match describe(&call) {
                // SAFETY: anvil-script's buffer of `capacity` bytes.
                Some(line) if !line.is_empty() => unsafe { write_line(&line, utf8, capacity) },
                _ => 0,
            }
        },
    )
}

/// The type of the describe handler of an effect without a tooltip.
type NoDescribe = fn(&Call<'_>) -> Option<String>;

impl Effects {
    /// The service; `None` without anvil-script or on an Anvil older than
    /// 1.16.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-script.effects version 1.
        let table =
            unsafe { host.service::<AnvilScriptEffectsService>(EFFECTS_SERVICE, EFFECTS_VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilScriptEffectsService {
        self.table
    }

    /// Registers the effect `key` with no line of its own in tooltips.
    /// `execute` runs each time the game runs the effect. Results, in the order
    /// anvil-script checks them: `WrongKind` for a plugin with
    /// `affects_gameplay = false`; `WrongPhase` for a plugin that is not early,
    /// or once the game has parsed an effect; `InvalidArgument` for a key that
    /// is not `<owner>_<name>` (or holds a NUL); `AlreadyExists` for a taken
    /// key; `OutOfMemory` past 256 keys. The handler is kept for the process;
    /// a panic in it is logged once and the game's command goes on.
    pub fn register<E>(&self, host: &Host, key: &str, execute: E) -> Result<()>
    where
        E: Fn(&Call<'_>) + Send + Sync + 'static,
    {
        self.register_raw::<E, NoDescribe>(host, key, execute, None)
    }

    /// Registers the effect `key` as [`register`](Self::register) does, with
    /// a tooltip line: `describe` gives the line (UTF-8, its first line only)
    /// or `None` for none, for the scope the tooltip is built for. Text past
    /// anvil-script's buffer is cut at a whole character.
    pub fn register_with_tooltip<E, D>(
        &self,
        host: &Host,
        key: &str,
        execute: E,
        describe: D,
    ) -> Result<()>
    where
        E: Fn(&Call<'_>) + Send + Sync + 'static,
        D: Fn(&Call<'_>) -> Option<String> + Send + Sync + 'static,
    {
        self.register_raw(host, key, execute, Some(describe))
    }

    fn register_raw<E, D>(
        &self,
        host: &Host,
        key: &str,
        execute: E,
        describe: Option<D>,
    ) -> Result<()>
    where
        E: Fn(&Call<'_>) + Send + Sync + 'static,
        D: Fn(&Call<'_>) -> Option<String> + Send + Sync + 'static,
    {
        let f = function!(self.table, AnvilScriptEffectsService, register_effect)?;
        let c_key = CString::new(key).map_err(|_| {
            Error::new(
                ErrorCode::InvalidArgument,
                format!("register effect {key:?}: the key holds a NUL"),
            )
        })?;
        let has_describe = describe.is_some();
        // Never freed: anvil-script keeps the callbacks and the context for the
        // process (it copies the key and the table's fields).
        let adapter: &'static EffectAdapter<E, D> = Box::leak(Box::new(EffectAdapter {
            execute,
            describe,
            execute_panicked: AtomicBool::new(false),
            describe_panicked: AtomicBool::new(false),
        }));
        let effect = AnvilScriptEffect {
            size: std::mem::size_of::<AnvilScriptEffect>() as u32,
            flags: 0,
            key: c_key.as_ptr(),
            execute: Some(execute_adapter::<E, D>),
            describe: if has_describe {
                Some(describe_adapter::<E, D>)
            } else {
                None
            },
            context: ptr::from_ref(adapter).cast_mut().cast(),
        };
        // SAFETY: this plugin's handle and an effect whose key lives until the
        // call returns, with callbacks and a context that live for the process.
        check(unsafe { f(host.raw_plugin(), &effect) }, || {
            format!("register effect {key:?}")
        })
    }
}

// ---- the Day end --------------------------------------------------------------

/// The day that just ended: the date the game computed it for, which
/// `anvil-game.day` reports after the frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Day {
    /// The year.
    pub year: i32,
    /// 1–12.
    pub month: i32,
    /// 1–31.
    pub day: i32,
    /// The 1st of a month: the game ran its monthly update in this day.
    pub new_month: bool,
    /// 1 January: the game also ran its yearly update.
    pub new_year: bool,
}

impl Day {
    fn from_raw(raw: &AnvilScriptDay) -> Self {
        Self {
            year: raw.year,
            month: raw.month,
            day: raw.day,
            new_month: raw.flags & ANVIL_SCRIPT_DAY_NEW_MONTH != 0,
            new_year: raw.flags & ANVIL_SCRIPT_DAY_NEW_YEAR != 0,
        }
    }
}

/// The `anvil-script.day_end` service.
#[derive(Clone, Copy)]
pub struct DayEnd {
    table: &'static AnvilScriptDayEndService,
}

impl fmt::Debug for DayEnd {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DayEnd")
            .field("size", &self.table.size)
            .finish()
    }
}

struct Adapter<F> {
    handler: F,
    panicked: AtomicBool,
}

unsafe extern "C" fn day_end_adapter<F>(context: *mut c_void, day: *const AnvilScriptDay)
where
    F: Fn(&Day) + Send + Sync + 'static,
{
    // SAFETY: the adapter subscribe leaked, never freed.
    let adapter = unsafe { &*context.cast::<Adapter<F>>() };
    // SAFETY: anvil-script passes its day for the call (or NULL, never seen).
    let Some(raw) = (unsafe { day.as_ref() }) else {
        return;
    };
    let day = Day::from_raw(raw);
    run_callback("day end", &adapter.panicked, (), || (adapter.handler)(&day));
}

impl DayEnd {
    /// The service; `None` without anvil-script, on an Anvil older than 1.18,
    /// or on a build without the Day end's symbols.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-script.day_end version 1.
        let table = unsafe { host.service::<AnvilScriptDayEndService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilScriptDayEndService {
        self.table
    }

    /// Calls `handler` at every Day end, after the handlers that subscribed
    /// before it. `WrongKind` for a plugin with `affects_gameplay = false`,
    /// `WrongPhase` once the game has ended a day. The handler is kept for
    /// the process; a panic in it is logged once and that Day end goes on.
    pub fn subscribe<F>(&self, host: &Host, handler: F) -> Result<()>
    where
        F: Fn(&Day) + Send + Sync + 'static,
    {
        let f = function!(self.table, AnvilScriptDayEndService, subscribe)?;
        // Never freed: anvil-script keeps the subscription for the process.
        let adapter: &'static Adapter<F> = Box::leak(Box::new(Adapter {
            handler,
            panicked: AtomicBool::new(false),
        }));
        // SAFETY: this plugin's handle, a callback of the C type and its
        // context, which live for the process.
        check(
            unsafe {
                f(
                    host.raw_plugin(),
                    Some(day_end_adapter::<F>),
                    ptr::from_ref(adapter).cast_mut().cast(),
                )
            },
            || "day end subscribe".to_string(),
        )
    }
}
