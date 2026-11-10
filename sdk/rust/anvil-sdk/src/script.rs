//! `anvil-script.day_end`: a callback at the Day end, once a day after the
//! game has finished the day (with its month and year), on the main thread,
//! when no worker thread of the day runs, before the game compares checksums
//! (`anvil_script.h`, ADR 0009).
//!
//! Only a plugin with `affects_gameplay = true` subscribes, and only before
//! the first Day end of the process: in its [`Plugin::load`](crate::Plugin).
//! Depend on anvil-script in plugin.toml (`[[dependencies]] id =
//! "anvil-script"`, `version = "^1.18"`) and look the service up with
//! [`DayEnd::get`]. The handler runs inside the game's day command:
//! [`World`](crate::world::World) answers `WrongPhase` there. In multiplayer
//! every client runs the same Day ends in the same order, so the handler must
//! be deterministic. While Anvil suspends the plugin, the handler is not
//! called.
//!
//! The script effects of the same plugin (`anvil-script.effects`) are not
//! wrapped yet; [`sys`](crate::sys) has their C types.

use std::ffi::{c_void, CStr};
use std::fmt;
use std::ptr;
use std::sync::atomic::AtomicBool;

use crate::error::{check, Result};
use crate::host::Host;
use crate::panic::run_callback;
use crate::sys::{
    AnvilScriptDay, AnvilScriptDayEndService, ANVIL_SCRIPT_DAY_END_SERVICE_VERSION,
    ANVIL_SCRIPT_DAY_NEW_MONTH, ANVIL_SCRIPT_DAY_NEW_YEAR,
};

/// The service's name.
pub const SERVICE: &CStr = c"anvil-script.day_end";
/// The service's major version.
pub const VERSION: u32 = ANVIL_SCRIPT_DAY_END_SERVICE_VERSION;

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
