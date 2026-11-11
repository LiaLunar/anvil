//! The service of the standard plugin anvil-log-throttle (`anvil_log_throttle.h`).
//!
//! **`anvil-log-throttle.messages`** ([`Messages`], 1.20): the table of the
//! messages of the throttled logs (error.log by default) with their counts as
//! they happen, rather than when the throttle's next report reaches the file.
//! A message is its log, the game's source file and line (the game's C++ code,
//! not a mod's file) and the text; its count is the lines written plus the
//! lines the throttle only counted. The table is kept from [`Messages::keep`]
//! on (the plugin's script errors panel starts it at load when it is on), and
//! nothing leaves it until the process ends. Any thread.
//!
//! Depend on anvil-log-throttle in plugin.toml (`[[dependencies]] id =
//! "anvil-log-throttle"`, `version = "^1.20"`, `optional = true` when the
//! plugin also works without it) and look the service up with
//! [`Messages::get`].
//!
//! ```no_run
//! use anvil::log_throttle::Messages;
//! # fn load(host: &anvil::Host) -> anvil::Result<()> {
//! let messages = Messages::get(host).expect("anvil-log-throttle.messages");
//! messages.keep()?;
//! // Later: what changed since the last look.
//! let mut seen = 0;
//! let totals = messages.totals()?;
//! for m in messages.changed_since(seen)? {
//!     host.log(anvil::LogLevel::Info, format_args!("{} x{}", m.text, m.count()));
//! }
//! seen = totals.version;
//! # let _ = seen;
//! # Ok(())
//! # }
//! ```

use std::ffi::{c_char, CStr};
use std::fmt;
use std::mem::size_of;
use std::ptr;

use crate::error::{check, Result};
use crate::host::Host;
use crate::sys::{
    AnvilLogMessage, AnvilLogMessagesService, AnvilLogTotals, ANVIL_LOG_MESSAGES_SERVICE_VERSION,
};

/// The service's name.
pub const SERVICE: &CStr = c"anvil-log-throttle.messages";
/// The service's major version.
pub const VERSION: u32 = ANVIL_LOG_MESSAGES_SERVICE_VERSION;

/// The table's totals.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Totals {
    /// Whether the table is kept; before [`Messages::keep`] everything is 0.
    pub keeping: bool,
    /// Grows with every line the table takes: the same value, nothing changed.
    pub version: u64,
    /// `GetTickCount64()` when the table started.
    pub since_ms: u64,
    /// Messages in the table.
    pub messages: u64,
    /// Lines of the throttled logs since then that reached the log.
    pub written: u64,
    /// Lines the throttle counted instead.
    pub counted: u64,
    /// Of those, lines of messages the full table (32768 messages) does not take.
    pub untracked: u64,
}

/// A message of the table and its counts at the time of the read.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Message {
    /// Its position in the table, from 0, in the order first seen.
    pub index: u64,
    /// The table's version at its last line.
    pub version: u64,
    /// Lines that reached the log.
    pub written: u64,
    /// Lines the throttle counted instead.
    pub counted: u64,
    /// The log's file name, `error.log`.
    pub log: String,
    /// The game's C++ source file name, `triggerimplementation.cpp`.
    pub source: String,
    /// Its line.
    pub source_line: i32,
    /// The text, without the line end.
    pub text: String,
}

impl Message {
    /// Every line of the message: written and counted.
    pub fn count(&self) -> u64 {
        self.written + self.counted
    }
}

fn owned(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: a NUL-terminated string the service keeps until the process ends.
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// The `anvil-log-throttle.messages` service.
#[derive(Clone, Copy)]
pub struct Messages {
    table: &'static AnvilLogMessagesService,
}

impl fmt::Debug for Messages {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Messages")
            .field("size", &self.table.size)
            .finish()
    }
}

impl Messages {
    /// The service; `None` without anvil-log-throttle 1.20.0.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-log-throttle.messages version 1.
        let table = unsafe { host.service::<AnvilLogMessagesService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilLogMessagesService {
        self.table
    }

    /// Starts the table, if it is not kept yet. Lines before it are not in it.
    pub fn keep(&self) -> Result<()> {
        let f = function!(self.table, AnvilLogMessagesService, keep)?;
        // SAFETY: no arguments.
        check(unsafe { f() }, || "log messages keep".to_owned())
    }

    /// The table's totals.
    pub fn totals(&self) -> Result<Totals> {
        let f = function!(self.table, AnvilLogMessagesService, totals)?;
        let mut out = AnvilLogTotals {
            size: size_of::<AnvilLogTotals>() as u32,
            keeping: 0,
            version: 0,
            since_ms: 0,
            messages: 0,
            written: 0,
            counted: 0,
            untracked: 0,
        };
        // SAFETY: a struct the service fills as far as its size says.
        check(unsafe { f(&mut out) }, || "log messages totals".to_owned())?;
        Ok(Totals {
            keeping: out.keeping != 0,
            version: out.version,
            since_ms: out.since_ms,
            messages: out.messages,
            written: out.written,
            counted: out.counted,
            untracked: out.untracked,
        })
    }

    /// At most `capacity` messages from position `first` on whose version is
    /// above `changed_after` (0: every one), and the position to read on from:
    /// the table's size once the read got to its end.
    pub fn read(
        &self,
        first: u64,
        changed_after: u64,
        capacity: u32,
    ) -> Result<(Vec<Message>, u64)> {
        let f = function!(self.table, AnvilLogMessagesService, read)?;
        let empty = AnvilLogMessage {
            index: 0,
            version: 0,
            written: 0,
            counted: 0,
            log: ptr::null(),
            source: ptr::null(),
            source_line: 0,
            reserved: 0,
            text: ptr::null(),
        };
        let mut raw = vec![empty; capacity as usize];
        let mut count = 0u32;
        let mut next = 0u64;
        let out = if raw.is_empty() {
            ptr::null_mut()
        } else {
            raw.as_mut_ptr()
        };
        check(
            // SAFETY: room for `capacity` messages of this size, and two outputs.
            unsafe {
                f(
                    first,
                    changed_after,
                    out,
                    capacity,
                    size_of::<AnvilLogMessage>() as u32,
                    &mut count,
                    &mut next,
                )
            },
            || format!("log messages read from {first}"),
        )?;
        raw.truncate(count as usize);
        let messages = raw
            .iter()
            .map(|m| Message {
                index: m.index,
                version: m.version,
                written: m.written,
                counted: m.counted,
                log: owned(m.log),
                source: owned(m.source),
                source_line: m.source_line,
                text: owned(m.text),
            })
            .collect();
        Ok((messages, next))
    }

    /// Every message whose version is above `version` (0: the whole table),
    /// read in pieces of 1024. Pass the `version` of the last [`Totals`] to
    /// get what changed since.
    pub fn changed_since(&self, version: u64) -> Result<Vec<Message>> {
        let mut all = Vec::new();
        let mut first = 0;
        loop {
            let (part, next) = self.read(first, version, 1024)?;
            let full = part.len() == 1024;
            all.extend(part);
            if !full || next <= first {
                return Ok(all);
            }
            first = next;
        }
    }
}
