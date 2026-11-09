//! `anvil-map.modes`: map modes of the plugin's own, each a button on the
//! minimap's list, a colour for each province and lines in the province's
//! tooltip (`anvil_map.h`).
//!
//! Depend on anvil-map in plugin.toml (`[[dependencies]] id = "anvil-map"`,
//! `version = "^1"`) and look the service up with [`MapModes::get`]. A mode
//! changes nothing in the game's state, so it needs no
//! `affects_gameplay = true`.
//!
//! The game paints the map and builds tooltips also in frames drawn inside a
//! game update, where [`World`](crate::world::World) may not be read. So the
//! plugin computes its colours in a step of its own ([`SHOWN`], a
//! [`game`](crate::game) event) and hands them over with
//! [`MapModes::set_colors`]; the map shows the last buffer. Text is UTF-8;
//! anvil-map writes it in the game's encoding and shows it as written.

use std::ffi::{c_char, c_void, CStr};
use std::fmt;
use std::ptr::{self, NonNull};
use std::sync::atomic::AtomicBool;

use crate::error::{check, Error, ErrorCode, Result};
use crate::event::{read_sized, Event, Payload};
use crate::host::Host;
use crate::panic::run_callback;
use crate::sys::{
    AnvilMapMode, AnvilMapModeChange, AnvilMapModeInfo, AnvilMapModesService,
    ANVIL_MAP_MODES_SERVICE_VERSION, ANVIL_MAP_NO_COLOR, ANVIL_MAP_TAB_1, ANVIL_MAP_TAB_2,
    ANVIL_MAP_TAB_3, ANVIL_MAP_TAB_4, ANVIL_MAP_TAB_DEFAULT,
};
use crate::text::{c_string, static_str};

/// The service's name.
pub const SERVICE: &CStr = c"anvil-map.modes";
/// The service's major version.
pub const VERSION: u32 = ANVIL_MAP_MODES_SERVICE_VERSION;

/// The colour that leaves a province as the political map mode shows it
/// (sea, wasteland and empty land look as players know them).
pub const NO_COLOR: u32 = ANVIL_MAP_NO_COLOR;

/// A colour, `0x00RRGGBB`.
pub const fn rgb(r: u8, g: u8, b: u8) -> u32 {
    ((r as u32) << 16) | ((g as u32) << 8) | b as u32
}

/// The tab of the minimap's list where the mode's button goes, named by the
/// game's modes it starts with. The buttons of the tabs stand two by two:
/// political and diplomacy above, trade and terrain below.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Tab {
    /// anvil-map's choice: [`Tab::Diplomacy`].
    #[default]
    Default,
    /// Tab 1, from the political mode.
    Political,
    /// Tab 2, from the terrain mode.
    Terrain,
    /// Tab 3, from the trade mode.
    Trade,
    /// Tab 4, from the diplomacy mode.
    Diplomacy,
}

impl Tab {
    const fn raw(self) -> u32 {
        (match self {
            Self::Default => ANVIL_MAP_TAB_DEFAULT,
            Self::Political => ANVIL_MAP_TAB_1,
            Self::Terrain => ANVIL_MAP_TAB_2,
            Self::Trade => ANVIL_MAP_TAB_3,
            Self::Diplomacy => ANVIL_MAP_TAB_4,
        }) as u32
    }
}

/// What [`MapModes::register`] takes. The strings are copied.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeInfo<'a> {
    /// The tab.
    pub tab: Tab,
    /// `<plugin-id>.<name>`: the plugin's own id, `.`, then `[a-z0-9_-]{1,32}`.
    pub id: &'a str,
    /// The button's name, 1 to 64 bytes.
    pub label: &'a str,
    /// The button's tooltip, at most 512 bytes.
    pub description: Option<&'a str>,
    /// A sprite (`GFX_...`) of the game or of the plugin's mod; `None`: an
    /// empty button frame.
    pub sprite: Option<&'a str>,
}

impl<'a> ModeInfo<'a> {
    /// A mode on the default tab with no description and no sprite.
    pub const fn new(id: &'a str, label: &'a str) -> Self {
        Self {
            tab: Tab::Default,
            id,
            label,
            description: None,
            sprite: None,
        }
    }
}

/// A registered mode; anvil-map keeps it until the process ends.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Mode {
    raw: NonNull<AnvilMapMode>,
}

// SAFETY: a mode is never freed, and every service call may come from any
// thread (anvil_map.h).
unsafe impl Send for Mode {}
// SAFETY: as above.
unsafe impl Sync for Mode {}

impl fmt::Debug for Mode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Mode").field(&self.raw).finish()
    }
}

impl Mode {
    fn from_raw(raw: *const AnvilMapMode) -> Option<Self> {
        NonNull::new(raw.cast_mut()).map(|raw| Self { raw })
    }

    /// The raw mode, for the C table.
    pub fn as_raw(&self) -> *const AnvilMapMode {
        self.raw.as_ptr()
    }
}

/// The payload of [`SHOWN`] and [`HIDDEN`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeChange {
    /// The mode.
    pub mode: Option<Mode>,
    /// Its id.
    pub id: Option<&'static str>,
}

// SAFETY: anvil-map publishes both events with an AnvilMapModeChange whose
// id lives for the process.
unsafe impl Payload for ModeChange {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the payload of anvil-map.shown or anvil-map.hidden.
        let change = unsafe { read_sized::<AnvilMapModeChange>(raw) }?;
        Some(Self {
            mode: Mode::from_raw(change.mode),
            // SAFETY: the mode's id, kept with the mode.
            id: unsafe { static_str(change.id) },
        })
    }
}

/// `anvil-map.shown`: a plugin mode became the map mode shown. On the main
/// thread after a frame of the main loop, where `World` may be read.
// SAFETY: anvil-map publishes it with an AnvilMapModeChange.
pub const SHOWN: Event<ModeChange> = unsafe { Event::new(c"anvil-map.shown") };
/// `anvil-map.hidden`: a plugin mode stopped being the map mode shown (also at
/// the end of a session). Published before `shown` of the next one.
// SAFETY: as above.
pub const HIDDEN: Event<ModeChange> = unsafe { Event::new(c"anvil-map.hidden") };

/// The `anvil-map.modes` service.
#[derive(Clone, Copy)]
pub struct MapModes {
    table: &'static AnvilMapModesService,
}

impl fmt::Debug for MapModes {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MapModes")
            .field("size", &self.table.size)
            .finish()
    }
}

struct DescribeAdapter<F> {
    handler: F,
    panicked: AtomicBool,
}

/// `lines` into the C buffer: at most `capacity - 1` bytes, cut at a whole
/// character and at a NUL, then a terminating zero. Returns the length.
///
/// # Safety
/// `text` has room for `capacity` bytes, `capacity > 0`.
unsafe fn write_lines(lines: &str, text: *mut c_char, capacity: usize) -> usize {
    let end = lines.find('\0').unwrap_or(lines.len());
    let mut length = end.min(capacity - 1);
    while !lines.is_char_boundary(length) {
        length -= 1;
    }
    // SAFETY: `length` < `capacity` bytes into the caller's buffer.
    unsafe {
        ptr::copy_nonoverlapping(lines.as_ptr(), text.cast::<u8>(), length);
        *text.add(length) = 0;
    }
    length
}

unsafe extern "C" fn describe_adapter<F>(
    context: *mut c_void,
    mode: *const AnvilMapMode,
    province: i32,
    text: *mut c_char,
    capacity: usize,
) -> usize
where
    F: Fn(Mode, i32) -> Option<String> + Send + Sync + 'static,
{
    // SAFETY: the adapter register_with_tooltip leaked, never freed.
    let adapter = unsafe { &*context.cast::<DescribeAdapter<F>>() };
    let Some(mode) = Mode::from_raw(mode) else {
        return 0;
    };
    if text.is_null() || capacity == 0 {
        return 0;
    }
    run_callback("map mode tooltip", &adapter.panicked, 0, || {
        match (adapter.handler)(mode, province) {
            // SAFETY: anvil-map's buffer of `capacity` bytes.
            Some(lines) => unsafe { write_lines(&lines, text, capacity) },
            None => 0,
        }
    })
}

impl MapModes {
    /// The service; `None` without anvil-map.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-map.modes version 1.
        let table = unsafe { host.service::<AnvilMapModesService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilMapModesService {
        self.table
    }

    /// Registers a mode with no lines of its own in the tooltip, which then
    /// shows the province's name and owner only. A pool of 13 numbers is lent
    /// in the order modes register: a fourteenth is `Unsupported`, and so is
    /// any mode when another loader of map modes runs. A taken id is
    /// `AlreadyExists`; a bad id, label, description or sprite is
    /// `InvalidArgument`.
    pub fn register(&self, info: &ModeInfo<'_>) -> Result<Mode> {
        self.register_raw(info, None, ptr::null_mut())
    }

    /// Registers a mode whose tooltip lines `describe` gives: for a province
    /// ID, the lines (separated by `\n`), or `None` for none. It runs on the
    /// main thread after a frame of the main loop, where `World` may be read,
    /// for the province under the cursor while the mode is shown; keep it
    /// short. Text past the buffer anvil-map gives is cut at a whole
    /// character.
    pub fn register_with_tooltip<F>(&self, info: &ModeInfo<'_>, describe: F) -> Result<Mode>
    where
        F: Fn(Mode, i32) -> Option<String> + Send + Sync + 'static,
    {
        // Never freed: anvil-map keeps the mode, and may call it, for the process.
        let adapter: &'static DescribeAdapter<F> = Box::leak(Box::new(DescribeAdapter {
            handler: describe,
            panicked: AtomicBool::new(false),
        }));
        self.register_raw(
            info,
            Some(describe_adapter::<F>),
            ptr::from_ref(adapter).cast_mut().cast(),
        )
    }

    fn register_raw(
        &self,
        info: &ModeInfo<'_>,
        describe: crate::sys::AnvilMapDescribe,
        context: *mut c_void,
    ) -> Result<Mode> {
        let f = function!(self.table, AnvilMapModesService, register_mode)?;
        let id = c_string(info.id);
        let label = c_string(info.label);
        let description = info.description.map(c_string);
        let sprite = info.sprite.map(c_string);
        let raw_info = AnvilMapModeInfo {
            size: std::mem::size_of::<AnvilMapModeInfo>() as u32,
            tab: info.tab.raw(),
            id: id.as_ptr(),
            label: label.as_ptr(),
            description: description
                .as_ref()
                .map_or(ptr::null(), |text| text.as_ptr()),
            sprite: sprite.as_ref().map_or(ptr::null(), |text| text.as_ptr()),
            describe,
            context,
        };
        let mut out: *const AnvilMapMode = ptr::null();
        // SAFETY: an info whose strings live until the call returns, and an
        // out pointer; anvil-map copies the strings.
        check(unsafe { f(&raw_info, &mut out) }, || {
            format!("map mode register {:?}", info.id)
        })?;
        Mode::from_raw(out).ok_or_else(|| {
            Error::new(
                ErrorCode::Internal,
                format!("map mode register {:?}: no mode", info.id),
            )
        })
    }

    /// Hands over the colours of `mode`: `colors[i]` for the province with
    /// ID `i` (see [`rgb`] and [`NO_COLOR`]); provinces past the slice and
    /// province 0 keep the political colour. The slice is copied. Only while
    /// a session runs (`WrongPhase` otherwise); more entries than the
    /// session's province limit, or a colour with a top byte other than 0 or
    /// 0xFF, is `InvalidArgument`.
    pub fn set_colors(&self, mode: Mode, colors: &[u32]) -> Result<()> {
        let f = function!(self.table, AnvilMapModesService, set_colors)?;
        let count = u32::try_from(colors.len()).map_err(|_| {
            Error::new(
                ErrorCode::InvalidArgument,
                "map mode set_colors: too many colours",
            )
        })?;
        // SAFETY: the mode and `count` colours valid during the call.
        check(unsafe { f(mode.as_raw(), colors.as_ptr(), count) }, || {
            "map mode set_colors"
        })
    }

    /// The id of the plugin mode that borrowed `number`, a value of the
    /// game's map mode enum; `None` for the game's own modes.
    pub fn mode_id(&self, number: i32) -> Option<&'static str> {
        let f = function!(self.table, AnvilMapModesService, mode_id).ok()?;
        // SAFETY: the service's function; the id lives for the process.
        unsafe { static_str(f(number)) }
    }

    /// The plugin mode shown on the map as of anvil-map's last step.
    pub fn shown(&self) -> Option<Mode> {
        let f = function!(self.table, AnvilMapModesService, shown).ok()?;
        // SAFETY: the service's function.
        Mode::from_raw(unsafe { f() })
    }

    /// The id of `mode`.
    pub fn id(&self, mode: Mode) -> Option<&'static str> {
        let f = function!(self.table, AnvilMapModesService, id).ok()?;
        // SAFETY: the service's function with its mode; the id lives for the process.
        unsafe { static_str(f(mode.as_raw())) }
    }
}
