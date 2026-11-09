//! `anvil-overlay.panel`: a window of the plugin in Anvil's overlay. The
//! plugin hands over rows of text and buttons and the overlay draws them
//! (`anvil_panel.h`).
//!
//! Depend on the overlay in plugin.toml (`[[dependencies]] id =
//! "anvil-overlay"`, `optional = true` for a plugin that also works without a
//! window) and look the service up with [`Panels::get`]. A panel changes
//! nothing in the game, so it needs no `affects_gameplay = true`.
//!
//! Text is UTF-8, and Anvil translates none of it: ask
//! [`Panels::language`]. Every call may come from any thread; rows are
//! copied. A panel lives until the process ends. Clicks run on the game's
//! main thread after an outer frame (as anvil-game work), so a click handler
//! may read [`World`](crate::world::World).

use std::ffi::{c_void, CString};
use std::fmt;
use std::ptr::{self, NonNull};
use std::sync::atomic::AtomicBool;

use crate::error::{check, Result};
use crate::host::Host;
use crate::panic::run_callback;
use crate::sys::{
    AnvilPanel, AnvilPanelRow, AnvilPanelService, ANVIL_PANEL_BUTTON, ANVIL_PANEL_SEPARATOR,
    ANVIL_PANEL_SERVICE_VERSION, ANVIL_PANEL_TEXT,
};
use crate::text::{c_string, static_str};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-overlay.panel";
/// The service's major version.
pub const VERSION: u32 = ANVIL_PANEL_SERVICE_VERSION;

/// One row of a panel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Row<'a> {
    /// `label`, and `value` beside it when there is one.
    Text {
        /// The text.
        label: &'a str,
        /// The value beside it.
        value: Option<&'a str>,
    },
    /// A button with `label`; a click gives back `id`.
    Button {
        /// The button's text.
        label: &'a str,
        /// What the click handler receives.
        id: u64,
    },
    /// A line.
    Separator,
}

impl<'a> Row<'a> {
    /// A line of text.
    pub const fn text(label: &'a str) -> Self {
        Self::Text { label, value: None }
    }

    /// A label with a value beside it.
    pub const fn value(label: &'a str, value: &'a str) -> Self {
        Self::Text {
            label,
            value: Some(value),
        }
    }

    /// A button.
    pub const fn button(label: &'a str, id: u64) -> Self {
        Self::Button { label, id }
    }
}

/// The `anvil-overlay.panel` service.
#[derive(Clone, Copy)]
pub struct Panels {
    table: &'static AnvilPanelService,
}

impl fmt::Debug for Panels {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Panels")
            .field("size", &self.table.size)
            .finish()
    }
}

impl Panels {
    /// The service; `None` without the overlay (or with one older than 1.11).
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-overlay.panel version 1.
        let table = unsafe { host.service::<AnvilPanelService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilPanelService {
        self.table
    }

    /// A hidden panel with a title, which also names it in the overlay's
    /// status tab, where the player can show and hide it. At most 64 panels
    /// in a process (`OutOfMemory` after that).
    pub fn create(&self, title: &str) -> Result<Panel> {
        let f = function!(self.table, AnvilPanelService, create)?;
        let title_c = c_string(title);
        let mut raw: *mut AnvilPanel = ptr::null_mut();
        // SAFETY: a NUL-terminated title and an out pointer.
        check(unsafe { f(title_c.as_ptr(), &mut raw) }, || {
            format!("panel create {title:?}")
        })?;
        let raw = NonNull::new(raw).ok_or_else(|| {
            crate::Error::new(
                crate::ErrorCode::Internal,
                format!("panel create {title:?}: no panel"),
            )
        })?;
        Ok(Panel {
            table: self.table,
            raw,
        })
    }

    /// The player's language: "en", "fr", "de", "es", "zh-Hans", "zh-Hant",
    /// "ja" or "ko".
    pub fn language(&self) -> &'static str {
        let language = function!(self.table, AnvilPanelService, language).ok();
        // SAFETY: the service's function; the text lives for the process.
        language
            .and_then(|f| unsafe { static_str(f()) })
            .unwrap_or("en")
    }
}

/// A panel. It is `Copy`: the overlay keeps it until the process ends.
#[derive(Clone, Copy)]
pub struct Panel {
    table: &'static AnvilPanelService,
    raw: NonNull<AnvilPanel>,
}

// SAFETY: every panel call may come from any thread (anvil_panel.h, "Threads").
unsafe impl Send for Panel {}
// SAFETY: as above.
unsafe impl Sync for Panel {}

impl fmt::Debug for Panel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Panel").field(&self.raw).finish()
    }
}

struct ClickAdapter<F> {
    handler: F,
    panicked: AtomicBool,
}

unsafe extern "C" fn click_adapter<F: Fn(u64) + Send + Sync + 'static>(
    context: *mut c_void,
    id: u64,
) {
    // SAFETY: the adapter on_click registered, never freed.
    let adapter = unsafe { &*context.cast::<ClickAdapter<F>>() };
    run_callback("panel click handler", &adapter.panicked, (), || {
        (adapter.handler)(id)
    });
}

impl Panel {
    /// The raw panel, for the C table.
    pub fn as_raw(&self) -> *mut AnvilPanel {
        self.raw.as_ptr()
    }

    /// Replaces the rows: at most 1024, each text at most 4096 bytes, else
    /// `InvalidArgument`. A NUL inside a text is dropped.
    pub fn set_rows(&self, rows: &[Row<'_>]) -> Result<()> {
        let f = function!(self.table, AnvilPanelService, set_rows)?;
        // The texts live until the call returns; the overlay copies them.
        let mut texts: Vec<CString> = Vec::with_capacity(rows.len() * 2);
        let mut keep = |text: &str| {
            let c = c_string(text);
            let pointer = c.as_ptr();
            texts.push(c);
            pointer
        };
        let raw: Vec<AnvilPanelRow> = rows
            .iter()
            .map(|row| match *row {
                Row::Text { label, value } => AnvilPanelRow {
                    kind: ANVIL_PANEL_TEXT as u32,
                    reserved: 0,
                    label: keep(label),
                    value: value.map_or(ptr::null(), &mut keep),
                    id: 0,
                },
                Row::Button { label, id } => AnvilPanelRow {
                    kind: ANVIL_PANEL_BUTTON as u32,
                    reserved: 0,
                    label: keep(label),
                    value: ptr::null(),
                    id,
                },
                Row::Separator => AnvilPanelRow {
                    kind: ANVIL_PANEL_SEPARATOR as u32,
                    reserved: 0,
                    label: ptr::null(),
                    value: ptr::null(),
                    id: 0,
                },
            })
            .collect();
        let count = u32::try_from(raw.len()).unwrap_or(u32::MAX);
        // SAFETY: `count` rows of row_size bytes whose texts live until the call returns.
        let result = unsafe {
            f(
                self.raw.as_ptr(),
                raw.as_ptr(),
                count,
                std::mem::size_of::<AnvilPanelRow>() as u32,
            )
        };
        drop(texts);
        check(result, || "panel set_rows")
    }

    /// The handler of the panel's buttons (it gets the button's id),
    /// replacing the last one. Clicks run on the game's main thread; without
    /// anvil-game no click is delivered.
    pub fn on_click(&self, handler: impl Fn(u64) + Send + Sync + 'static) -> Result<()> {
        self.click(handler)
    }

    fn click<F: Fn(u64) + Send + Sync + 'static>(&self, handler: F) -> Result<()> {
        let f = function!(self.table, AnvilPanelService, on_click)?;
        // Never freed: a click may still run the replaced handler.
        let adapter: &'static ClickAdapter<F> = Box::leak(Box::new(ClickAdapter {
            handler,
            panicked: AtomicBool::new(false),
        }));
        // SAFETY: the adapter lives for the process and never unwinds.
        let result = unsafe {
            f(
                self.raw.as_ptr(),
                Some(click_adapter::<F>),
                ptr::from_ref(adapter).cast_mut().cast(),
            )
        };
        check(result, || "panel on_click")
    }

    /// Shows or hides the panel. The player may close it at any time: ask
    /// [`is_shown`](Panel::is_shown).
    pub fn show(&self, shown: bool) -> Result<()> {
        let f = function!(self.table, AnvilPanelService, show)?;
        // SAFETY: the service's function with its panel.
        check(unsafe { f(self.raw.as_ptr(), i32::from(shown)) }, || {
            "panel show"
        })
    }

    /// Whether the panel is shown.
    pub fn is_shown(&self) -> bool {
        // SAFETY: the service's function with its panel.
        function!(self.table, AnvilPanelService, shown)
            .is_ok_and(|f| unsafe { f(self.raw.as_ptr()) } != 0)
    }
}
