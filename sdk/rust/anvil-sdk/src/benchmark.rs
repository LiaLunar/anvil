//! `anvil.benchmark`: the core's service through which a plugin joins the
//! start-up benchmark and reports its own fields (`anvil_benchmark.h`).
//!
//! Declaring fields inside `load`, even zero of them, makes the plugin a
//! participant, which promises one [`done`](Benchmark::done) call: in a
//! benchmark start the game quits once it is ready and every participant is
//! done, or 30 s after ready ([`QUIT`]). Outside a benchmark start
//! ([`is_active`](Benchmark::is_active) is false) every call checks its
//! arguments the same way and records nothing, so a plugin may call it always.
//! A refused call also writes one warning to the plugin's log.

use std::ffi::{c_char, CString};
use std::fmt;
use std::ptr;

use crate::error::{check, Result};
use crate::event::Event;
use crate::host::Host;
use crate::sys::{
    AnvilBenchmarkField, AnvilBenchmarkService, AnvilPlugin, ANVIL_BENCHMARK_ENUM,
    ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_NUMBER,
    ANVIL_BENCHMARK_SERVICE_VERSION, ANVIL_BENCHMARK_UNIT_BYTES, ANVIL_BENCHMARK_UNIT_COUNT,
    ANVIL_BENCHMARK_UNIT_MB, ANVIL_BENCHMARK_UNIT_MS, ANVIL_BENCHMARK_UNIT_NONE,
    ANVIL_BENCHMARK_UNIT_PERCENT,
};
use crate::text::c_string;

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil.benchmark";
/// The service's major version.
pub const VERSION: u32 = ANVIL_BENCHMARK_SERVICE_VERSION;

/// Published once in a benchmark start, on a thread of its own, when the game
/// is ready and every participant is done, or 30 s after ready. Payload none.
// SAFETY: the core publishes it with a NULL payload.
pub const QUIT: Event<()> = unsafe { Event::new(c"anvil.benchmark_quit") };

/// The unit of a number field.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Unit {
    /// No unit.
    None,
    /// Milliseconds.
    Ms,
    /// A count.
    Count,
    /// Bytes.
    Bytes,
    /// Megabytes.
    Mb,
    /// Percent.
    Percent,
}

impl Unit {
    const fn raw(self) -> u32 {
        (match self {
            Self::None => ANVIL_BENCHMARK_UNIT_NONE,
            Self::Ms => ANVIL_BENCHMARK_UNIT_MS,
            Self::Count => ANVIL_BENCHMARK_UNIT_COUNT,
            Self::Bytes => ANVIL_BENCHMARK_UNIT_BYTES,
            Self::Mb => ANVIL_BENCHMARK_UNIT_MB,
            Self::Percent => ANVIL_BENCHMARK_UNIT_PERCENT,
        }) as u32
    }
}

/// The kind of a field. There is no string kind: a report carries no paths or text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldKind<'a> {
    /// An `i64`.
    Int(Unit),
    /// An `f64`; NaN and infinity are refused.
    Number(Unit),
    /// One of the options (1 to 32), by name.
    Enum(&'a [&'a str]),
    /// False or true.
    Flag,
}

/// One field of the plugin's benchmark record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Field<'a> {
    /// `group.field` or `field`, from `[a-z0-9_]`.
    pub name: &'a str,
    /// Its kind.
    pub kind: FieldKind<'a>,
    /// Short English text for the player's preview only.
    pub label: Option<&'a str>,
}

/// The `anvil.benchmark` service.
#[derive(Clone, Copy)]
pub struct Benchmark {
    table: &'static AnvilBenchmarkService,
    plugin: *mut AnvilPlugin,
}

// SAFETY: every function may be called from any thread; the plugin handle is an id.
unsafe impl Send for Benchmark {}
// SAFETY: as above.
unsafe impl Sync for Benchmark {}

impl fmt::Debug for Benchmark {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Benchmark")
            .field("active", &self.is_active())
            .finish()
    }
}

impl Benchmark {
    /// The service; `None` with an Anvil older than 1.4.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil.benchmark version 1.
        let table = unsafe { host.service::<AnvilBenchmarkService>(SERVICE, VERSION) }?;
        Some(Self {
            table,
            plugin: host.raw_plugin(),
        })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilBenchmarkService {
        self.table
    }

    /// Whether this is a benchmark start.
    pub fn is_active(&self) -> bool {
        // SAFETY: the service's function.
        function!(self.table, AnvilBenchmarkService, active).is_ok_and(|f| unsafe { f() } != 0)
    }

    /// Declares fields, all or none of them; inside `load` only. Any call,
    /// a refused one included, makes the plugin a participant.
    pub fn declare(&self, fields: &[Field<'_>]) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, declare)?;
        // Everything the raw fields point to lives until the call returns; Anvil copies it.
        let mut texts: Vec<CString> = Vec::new();
        let mut option_lists: Vec<Vec<*const c_char>> = Vec::new();
        let mut raw: Vec<AnvilBenchmarkField> = Vec::with_capacity(fields.len());
        for field in fields {
            let name = c_string(field.name);
            let label = field.label.map(c_string);
            let (kind, unit, options, option_count) = match field.kind {
                FieldKind::Int(unit) => (ANVIL_BENCHMARK_INT, unit.raw(), ptr::null(), 0),
                FieldKind::Number(unit) => (ANVIL_BENCHMARK_NUMBER, unit.raw(), ptr::null(), 0),
                FieldKind::Flag => (ANVIL_BENCHMARK_FLAG, Unit::None.raw(), ptr::null(), 0),
                FieldKind::Enum(names) => {
                    let list: Vec<*const c_char> = names
                        .iter()
                        .map(|option| {
                            let c = c_string(option);
                            let pointer = c.as_ptr();
                            texts.push(c);
                            pointer
                        })
                        .collect();
                    let pointer = list.as_ptr();
                    option_lists.push(list);
                    (
                        ANVIL_BENCHMARK_ENUM,
                        Unit::None.raw(),
                        pointer,
                        u32::try_from(names.len()).unwrap_or(u32::MAX),
                    )
                }
            };
            raw.push(AnvilBenchmarkField {
                name: name.as_ptr(),
                kind: kind as u32,
                unit,
                label: label.as_ref().map_or(ptr::null(), |l| l.as_ptr()),
                options,
                option_count,
            });
            texts.push(name);
            texts.extend(label);
        }
        let count = u32::try_from(raw.len()).unwrap_or(u32::MAX);
        // SAFETY: `count` fields whose texts and option lists live until the call returns.
        let result = unsafe { f(self.plugin, raw.as_ptr(), count) };
        drop((texts, option_lists));
        check(result, || "benchmark declare")
    }

    /// Sets an int field; the last value wins.
    pub fn set_int(&self, field: &str, value: i64) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, set_int)?;
        let name = c_string(field);
        // SAFETY: a NUL-terminated name.
        check(unsafe { f(self.plugin, name.as_ptr(), value) }, || {
            format!("benchmark set_int {field}")
        })
    }

    /// Sets a number field (finite).
    pub fn set_number(&self, field: &str, value: f64) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, set_number)?;
        let name = c_string(field);
        // SAFETY: a NUL-terminated name.
        check(unsafe { f(self.plugin, name.as_ptr(), value) }, || {
            format!("benchmark set_number {field}")
        })
    }

    /// Sets an enum field to one of its options.
    pub fn set_enum(&self, field: &str, option: &str) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, set_enum)?;
        let (name, option_c) = (c_string(field), c_string(option));
        // SAFETY: NUL-terminated names.
        check(
            unsafe { f(self.plugin, name.as_ptr(), option_c.as_ptr()) },
            || format!("benchmark set_enum {field}"),
        )
    }

    /// Sets a flag field.
    pub fn set_flag(&self, field: &str, value: bool) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, set_flag)?;
        let name = c_string(field);
        // SAFETY: a NUL-terminated name.
        check(
            unsafe { f(self.plugin, name.as_ptr(), i32::from(value)) },
            || format!("benchmark set_flag {field}"),
        )
    }

    /// The participant's work after ready is finished; later values are
    /// refused. Calling it again does nothing.
    pub fn done(&self) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, done)?;
        // SAFETY: the service's function with this plugin.
        check(unsafe { f(self.plugin) }, || "benchmark done")
    }

    /// This start is not representative for the plugin (e.g. a cache still
    /// being built): it is discarded and replaced. `reason` is a name like a
    /// field's, e.g. `new_mod_set`. Before `done` only.
    pub fn learning(&self, reason: &str) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, learning)?;
        let reason_c = c_string(reason);
        // SAFETY: a NUL-terminated reason.
        check(unsafe { f(self.plugin, reason_c.as_ptr()) }, || {
            format!("benchmark learning {reason}")
        })
    }

    /// The participant opened `files` of the game's files ahead of the game,
    /// which took `ms` in total. Before `done` only; the last call wins.
    pub fn open_cost(&self, files: u64, ms: f64) -> Result<()> {
        let f = function!(self.table, AnvilBenchmarkService, open_cost)?;
        // SAFETY: the service's function with this plugin.
        check(unsafe { f(self.plugin, files, ms) }, || {
            "benchmark open_cost"
        })
    }
}
